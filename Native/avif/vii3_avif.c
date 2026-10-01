/*
 * vii3_avif.c — libavif 解码封装（导出 vii3_avif_*）。
 *
 * 职责：
 *   1. 打开 / 解析 AVIF，把 libavif 的属性按官方字段名整包填进 Vii3AvifInfo
 *      （几何、位深、YUV 格式与 range、CICP、alpha、帧数与时长、ICC 长度、
 *       CLLI / CLAP、旋转与镜像、avifResult）。不做字段裁剪，上层自己挑。
 *   2. 逐帧解出 BGRA / RGBA16 / RGBA_F16（alpha 由 libavif 合成，直通非预乘）。
 *
 * 与 FFmpeg 的 shim 一起编进 vii3_media.dll：两边都要 dav1d，合在一个 dll 里只进一份。
 *
 * 本文件必须自包含：只用 libc 的 malloc/free/string，不碰 FFmpeg 的 av_malloc。
 * 授权：libavif 与 dav1d 均为 BSD-2-Clause。
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <avif/avif.h>
#include "../v3_hdr.h"
#include "../v3_pool.h"

/* 被 vii3_media.c include 进来时 V3_API 已经定义过了。 */
#ifndef V3_API
#if defined(_WIN32)
#define V3_API __declspec(dllexport)
#else
#define V3_API __attribute__((visibility("default")))
#endif
#endif

/*
 * libavif 属性快照。字段名与 avifImage / avifDecoder 一一对应，取值用它的枚举。
 *
 * 约定：
 *   - struct_size 用于 ABI 演进：托管侧先看它，再决定哪些字段可用。
 *   - result 只在「打开/解析」失败时被填（此时句柄为 NULL，其它字段无意义）。
 */
typedef struct Vii3AvifInfo
{
    int struct_size; /* = sizeof(Vii3AvifInfo) */

    /* --- avifImage：几何与格式 --- */
    int width; /* avifImage.width */
    int height; /* avifImage.height */
    int depth; /* avifImage.depth（8/10/12） */
    int yuv_format; /* avifImage.yuvFormat（AVIF_PIXEL_FORMAT_*） */
    int yuv_range; /* avifImage.yuvRange（AVIF_RANGE_*） */
    int yuv_chroma_sample_position; /* avifImage.yuvChromaSamplePosition */

    /* --- avifImage：CICP（没写 colr 时三项都是 2 = AVIF_*_UNSPECIFIED） --- */
    int color_primaries; /* avifImage.colorPrimaries */
    int transfer_characteristics; /* avifImage.transferCharacteristics */
    int matrix_coefficients; /* avifImage.matrixCoefficients */

    /* --- alpha --- */
    int alpha_present; /* avifDecoder.alphaPresent */
    int alpha_premultiplied; /* avifImage.alphaPremultiplied */

    /* --- 图像序列（动画） --- */
    int image_count; /* avifDecoder.imageCount（静态 = 1） */
    int image_sequence_track_present; /* avifDecoder.imageSequenceTrackPresent */
    int timescale; /* avifDecoder.timescale */
    long long duration_in_timescales; /* avifDecoder.durationInTimescales */

    /* --- ICC 轮廓：这里只给长度，字节用 vii3_avif_icc_profile() 取 --- */
    int icc_size;

    /* --- HDR 内容亮度（avifImage.clli） --- */
    int has_clli;
    int clli_max_content_light_level; /* avifImage.clli.maxCLL */
    int clli_max_pic_average_light_level; /* avifImage.clli.maxPALL */

    /* --- 干净孔径（avifImage.clap，宽高已换算成像素） --- */
    int has_clap;
    int clap_width;
    int clap_height;

    /* --- 打开/解析结果（avifResult；AVIF_RESULT_OK = 0） --- */
    int result;

    /* --- HDR（后加字段：托管侧先看 struct_size 再决定是否可用） --- */
    int is_hdr;       /* depth>8，或 transfer 是 PQ(SMPTE2084)/HLG */
    int has_gain_map; /* avifImage.gainMap != NULL（ISO 21496-1 gain map） */

    /* --- 旋转 / 镜像（后加字段：同样看 struct_size）
     * 与 libavif 字段一一对应、原样给出，不做换算：libavif 不把这两个变换应用到像素
     * （avif.h 明写 images won't be pre-cropped or mirrored upon decode），
     * 所以解出的位图是未变换的，必须由上层自己变换。语义见 avif.h
     * （ISO/IEC 23008-12 6.5.10 / 6.5.12）：
     *   transform_flags : avifImage.transformFlags 原样。
     *                     AVIF_TRANSFORM_PASP=1 / _CLAP=2 / _IROT=4 / _IMIR=8。
     *   irot_angle      : avifImage.irot.angle，[0,3]；真实角度 = angle × 90°，
     *                     方向为逆时针。仅当 flags 含 AVIF_TRANSFORM_IROT(4) 时有效。
     *   imir_axis       : avifImage.imir.axis，[0,1]；0 = 上下镜像，1 = 左右镜像。
     *                     仅当 flags 含 AVIF_TRANSFORM_IMIR(8) 时有效。
     * 两者同时生效时先 irot、后 imir（MIAF ISO/IEC 28002-12:2021 7.3.6.7）。 */
    int transform_flags;
    int irot_angle;
    int imir_axis;
} Vii3AvifInfo;

typedef struct Vii3Avif
{
    avifDecoder *dec;
    int width;
    int height;

    /* 单帧解码线程数，见 vii3_avif_set_max_threads。1 = 单线程（缺省）。 */
    int max_threads;

    /* HDR->SDR 色调映射的并行池（懒建，见 v3_pool.h）。 */
    V3Pool pool;
} Vii3Avif;

V3_API const char *vii3_avif_version(void)
{
    return avifVersion();
}

/* 把 decoder 上报的属性整包填进 out（成功后调用，dec->image 已就绪）。 */
static void v3_avif_fill_info(const avifDecoder *dec, Vii3AvifInfo *out)
{
    const avifImage *img = dec->image;

    memset(out, 0, sizeof(*out));
    out->struct_size = (int)sizeof(Vii3AvifInfo);
    out->result = AVIF_RESULT_OK;

    out->width = img->width;
    out->height = img->height;
    out->depth = img->depth;
    out->yuv_format = (int)img->yuvFormat;
    out->yuv_range = (int)img->yuvRange;
    out->yuv_chroma_sample_position = (int)img->yuvChromaSamplePosition;

    out->color_primaries = (int)img->colorPrimaries;
    out->transfer_characteristics = (int)img->transferCharacteristics;
    out->matrix_coefficients = (int)img->matrixCoefficients;

    out->alpha_present = dec->alphaPresent ? 1 : 0;
    out->alpha_premultiplied = img->alphaPremultiplied ? 1 : 0;

    out->image_count = (int)dec->imageCount;
    out->image_sequence_track_present = dec->imageSequenceTrackPresent ? 1 : 0;
    out->timescale = dec->timescale;
    out->duration_in_timescales = (long long)dec->durationInTimescales;

    out->icc_size = (int)img->icc.size;

    out->has_clli = (img->clli.maxCLL != 0 || img->clli.maxPALL != 0) ? 1 : 0;
    out->clli_max_content_light_level = img->clli.maxCLL;
    out->clli_max_pic_average_light_level = img->clli.maxPALL;

    if (img->clap.widthD > 0 && img->clap.heightD > 0)
    {
        out->has_clap = 1;
        out->clap_width = (int)(img->clap.widthN / img->clap.widthD);
        out->clap_height = (int)(img->clap.heightN / img->clap.heightD);
    }

    out->is_hdr = (img->depth > 8 ||
                   img->transferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084 ||
                   img->transferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_HLG)
                      ? 1
                      : 0;
    out->has_gain_map = (img->gainMap != NULL) ? 1 : 0;

    /* 旋转 / 镜像：原样给出，由上层按 avif.h 语义变换 */
    out->transform_flags = (int)img->transformFlags;
    out->irot_angle = img->irot.angle;
    out->imir_axis = img->imir.axis;
}

/* 把解析后的 decoder 包成会话并填 info。失败时销毁 decoder。 */
static Vii3Avif *vii3_avif_make(avifDecoder *dec, Vii3AvifInfo *out_info)
{
    Vii3Avif *a = (Vii3Avif *)calloc(1, sizeof(Vii3Avif)); /* 必须清零：后加字段依赖初值 0 */
    if (a == NULL)
    {
        avifDecoderDestroy(dec);
        if (out_info != NULL)
        {
            memset(out_info, 0, sizeof(*out_info));
            out_info->result = AVIF_RESULT_OUT_OF_MEMORY;
        }
        return NULL;
    }

    a->dec = dec;
    a->width = dec->image->width;
    a->height = dec->image->height;
    a->max_threads = 1; /* 单线程：要开就显式调 vii3_avif_set_max_threads */

    if (out_info != NULL)
    {
        v3_avif_fill_info(dec, out_info);
    }
    return a;
}

V3_API Vii3Avif *vii3_avif_open_memory(const uint8_t *data, size_t size, Vii3AvifInfo *out_info)
{
    if (out_info != NULL)
    {
        memset(out_info, 0, sizeof(*out_info));
    }
    if (data == NULL || size == 0)
    {
        if (out_info != NULL)
        {
            out_info->result = AVIF_RESULT_INVALID_ARGUMENT;
        }
        return NULL;
    }

    avifDecoder *dec = avifDecoderCreate();
    if (dec == NULL)
    {
        if (out_info != NULL)
        {
            out_info->result = AVIF_RESULT_OUT_OF_MEMORY;
        }
        return NULL;
    }

    avifResult r = avifDecoderSetIOMemory(dec, data, size);
    if (r == AVIF_RESULT_OK)
    {
        r = avifDecoderParse(dec);
    }
    if (r != AVIF_RESULT_OK)
    {
        avifDecoderDestroy(dec);
        if (out_info != NULL)
        {
            out_info->result = (int)r;
        }
        return NULL;
    }

    return vii3_avif_make(dec, out_info);
}

V3_API Vii3Avif *vii3_avif_open_file(const char *path, Vii3AvifInfo *out_info)
{
    if (out_info != NULL)
    {
        memset(out_info, 0, sizeof(*out_info));
    }
    if (path == NULL)
    {
        if (out_info != NULL)
        {
            out_info->result = AVIF_RESULT_INVALID_ARGUMENT;
        }
        return NULL;
    }

    avifDecoder *dec = avifDecoderCreate();
    if (dec == NULL)
    {
        if (out_info != NULL)
        {
            out_info->result = AVIF_RESULT_OUT_OF_MEMORY;
        }
        return NULL;
    }

    avifResult r = avifDecoderSetIOFile(dec, path);
    if (r == AVIF_RESULT_OK)
    {
        r = avifDecoderParse(dec);
    }
    if (r != AVIF_RESULT_OK)
    {
        avifDecoderDestroy(dec);
        if (out_info != NULL)
        {
            out_info->result = (int)r;
        }
        return NULL;
    }

    return vii3_avif_make(dec, out_info);
}

/*
 * 设置「单帧解码」用的线程数。默认 1 = 单线程。
 *
 * 影响的是**一次 vii3_avif_next_frame 调用内部**的两段工作：
 *   1. dav1d 解这一帧 AV1。并行度取决于编码时有没有分 tile，单 tile 收益有限。
 *   2. libavif 把这一帧 YUV 转 BGRA（最多 8 路按行切）。但 YUV420 + 自动/双线性色度上采样时，
 *      libavif 会强制单线程（reformat.c 里 jobs = 1），此时这一半设了也不生效。
 *
 * 必须在第一次 vii3_avif_next_frame 之前调用：dav1d 上下文在首次解码时创建，
 * 之后再改不会重建（第 2 点那种每次调用现读的部分不受影响）。
 *
 * 线程数是每会话的：同时解多张图时会与文件级并发相乘，别两个都拉满。
 * 返回生效的线程数（>= 1）；句柄为空返回 -1。
 */
V3_API int vii3_avif_set_max_threads(Vii3Avif *a, int max_threads)
{
    if (a == NULL)
    {
        return -1;
    }

    if (max_threads < 1)
    {
        max_threads = 1; /* 0 / 负数当单线程：调用方用 0 表示「不开启」 */
    }

    a->max_threads = max_threads;
    /* libavif 在每次 avifDecoderNextImage 之前把它转交给 codec（read.c），
       真正生效取决于 dav1d 上下文建好之前有没有设过。 */
    a->dec->maxThreads = max_threads;
    return max_threads;
}

/*
 * 把 ICC 轮廓字节复制到调用方缓冲。
 * 返回：>0 = 写入的字节数；0 = 这张图没有 ICC；<0 = 缓冲太小（绝对值 = 需要的字节数）。
 */
V3_API int vii3_avif_icc_profile(const Vii3Avif *a, uint8_t *dst, int capacity)
{
    if (a == NULL || a->dec == NULL)
    {
        return -1;
    }

    const avifRWData *icc = &a->dec->image->icc;
    if (icc->size == 0)
    {
        return 0;
    }

    if (dst == NULL || capacity < (int)icc->size)
    {
        return -(int)icc->size;
    }

    memcpy(dst, icc->data, icc->size);
    return (int)icc->size;
}

/*
 * 输出像素格式（vii3_avif_next_frame_ex 的 fmt 参数）。
 *
 * libavif 只做 YUV→RGB 的矩阵/range 转换，不做传递函数（PQ/HLG）或色域转换，
 * 所以三种输出都保留源色彩空间（CICP 由 Vii3AvifInfo 带出），交给下游做色彩管理：
 *   BGRA8    4 字节/像素。SDR 直出（10/12bit 会降到 8-bit）。
 *   RGBA16   8 字节/像素，每通道 uint16。HDR 直通（12bit 无损）。
 *   RGBA_F16 8 字节/像素，每通道 half-float。HDR 直通（约 11bit 有效精度，12bit 丢约 1bit）。
 */
#define V3_AVIF_BGRA8 0
#define V3_AVIF_RGBA16 1
#define V3_AVIF_RGBA_F16 2

/* HDR->SDR 色调映射的分带并行参数（池懒建：SDR 图与小图用不到）。 */
typedef struct
{
    uint8_t *dst;
    int w;
    int primaries;
    int is_pq;
    const uint16_t *src;
} V3AvifTmCtx;

static void v3_avif_tm_band(void *param, int y0, int y1)
{
    V3AvifTmCtx *c = (V3AvifTmCtx *)param;
    v3_tone_map_rgba16_rows(c->dst, c->w, c->primaries, c->is_pq, c->src, y0, y1);
}

static void v3_avif_pool_ensure(Vii3Avif *a)
{
    if (a->pool.sem != NULL || a->pool.nworker > 0)
    {
        return;
    }
    /* 色调映射是计算密集（LUT + 色域矩阵），用逻辑核；封顶 8（内存带宽受限）。 */
    int n = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (n < 1)
    {
        n = 1;
    }
    if (n > 8)
    {
        n = 8;
    }
    v3_pool_create(&a->pool, n);
}

/*
 * 解出下一帧到指定像素格式。
 * 返回：>0 = 写入字节数；0 = 序列结束（含静态图只解过一次）；<0 = 错误（-2 = 缓冲太小）。
 *
 * 缓冲需 >= width * height * (BGRA8 ? 4 : 8) 字节。
 */
V3_API int vii3_avif_next_frame_ex(Vii3Avif *a, uint8_t *dst, int dst_capacity, int fmt,
                                   double *out_duration_ms)
{
    if (a == NULL || dst == NULL || out_duration_ms == NULL)
    {
        return -1;
    }

    const int w = a->width, h = a->height;
    const int bytes_per_px = (fmt == V3_AVIF_BGRA8) ? 4 : 8;
    if (dst_capacity < w * h * bytes_per_px)
    {
        return -2; /* 缓冲太小 */
    }

    if (avifDecoderNextImage(a->dec) != AVIF_RESULT_OK)
    {
        return 0; /* 序列结束 */
    }

    const int trc = (int)a->dec->image->transferCharacteristics;
    const int pri = (int)a->dec->image->colorPrimaries;
    const int is_hdr_trc = (trc == AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084 ||
                            trc == AVIF_TRANSFER_CHARACTERISTICS_HLG);

    /* SDR 直出 BGRA8（10/12bit 会被 libavif 降到 8-bit） */
    if (fmt == V3_AVIF_BGRA8 && !is_hdr_trc)
    {
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, a->dec->image);
        rgb.format = AVIF_RGB_FORMAT_BGRA;
        rgb.depth = 8;
        rgb.isFloat = AVIF_FALSE;
        rgb.maxThreads = a->max_threads;
        if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK)
        {
            return -3;
        }
        int rc = -3;
        if (avifImageYUVToRGB(a->dec->image, &rgb) == AVIF_RESULT_OK)
        {
            const int bytes = w * h * 4;
            memcpy(dst, rgb.pixels, (size_t)bytes);
            *out_duration_ms = a->dec->imageTiming.duration * 1000.0;
            rc = bytes;
        }
        avifRGBImageFreePixels(&rgb);
        return rc;
    }

    /* 其余：总是先进 16-bit（uint16 或 f16）；HDR 的 BGRA8 也走这里（先解 16 位再色调映射） */
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, a->dec->image);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = 16;
    rgb.isFloat = (fmt == V3_AVIF_RGBA_F16) ? AVIF_TRUE : AVIF_FALSE;
    rgb.maxThreads = a->max_threads;
    if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK)
    {
        return -3;
    }

    int rc = -3;
    if (avifImageYUVToRGB(a->dec->image, &rgb) == AVIF_RESULT_OK)
    {
        if (fmt == V3_AVIF_BGRA8)
        {
            /* HDR（PQ/HLG 编码的 RGBA16）-> sRGB BGRA8 色调映射 */
            const int is_pq = (trc == AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084);
            v3_ensure_hdr_luts(); /* 并行前先建表，避免多线程同时惰性初始化 */
            v3_ensure_tm_lut(v3_nominal_peak_rw(is_pq)); /* 出表按源峰值重建 */
            v3_avif_pool_ensure(a);
            V3AvifTmCtx tm;
            tm.dst = dst;
            tm.w = w;
            tm.primaries = pri;
            tm.is_pq = is_pq;
            tm.src = (const uint16_t *)rgb.pixels;
            v3_pool_run(&a->pool, h, w, v3_avif_tm_band, &tm);
            *out_duration_ms = a->dec->imageTiming.duration * 1000.0;
            rc = w * h * 4;
        }
        else
        {
            const int bytes = w * h * 8;
            memcpy(dst, rgb.pixels, (size_t)bytes);
            *out_duration_ms = a->dec->imageTiming.duration * 1000.0;
            rc = bytes;
        }
    }

    avifRGBImageFreePixels(&rgb);
    return rc;
}

/* 旧入口：等价于 BGRA8（SDR 直出）。 */
V3_API int vii3_avif_next_frame(Vii3Avif *a, uint8_t *dst, int dst_capacity, double *out_duration_ms)
{
    return vii3_avif_next_frame_ex(a, dst, dst_capacity, V3_AVIF_BGRA8, out_duration_ms);
}

V3_API void vii3_avif_close(Vii3Avif *a)
{
    if (a == NULL)
    {
        return;
    }
    if (a->dec != NULL)
    {
        avifDecoderDestroy(a->dec);
    }
    v3_pool_destroy(&a->pool);
    free(a);
}
