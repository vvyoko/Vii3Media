/*
 * vii3_media.c — 媒体解码封装的 FFmpeg shim。
 *
 * 把一份裁剪过的 LGPL FFmpeg 静态链进来，对外只暴露一个小而稳定的 C ABI
 * （vii3_open / vii3_video_next_ex / vii3_audio_next / vii3_seek / vii3_close / vii3_version），
 * 托管侧不接触 FFmpeg 的结构体与版本相关布局。
 *
 * 解码范围按动态照片规范定（mov 容器 + h264/hevc/av1 + aac），
 * 具体开关见 Build/native/build-vii3-media.sh。
 *
 * HDR / 高位深：输出路径由 vii3_video_next_ex 的 fmt 选择：
 *   1/2 = 直通（RGBA64 / RGBA_F16），保留源色彩空间交给下游色彩管理；
 *   0   = CPU 色调映射成 sRGB BGRA8（PQ/HLG -> SDR，见 v3_hdr.h）；
 *   3   = 不做色调映射的 BGRA8 直出（旧版行为）。
 * 位深（8/10/12）、CICP、MDCV、CLLI 经 Vii3MediaInfo 上报。
 *
 * 音频只吐 PCM，不在这里播放：WASAPI 输出在同一 dll 里，由托管侧按需驱动（见下文音频一节）。
 *
 * 旋转取自容器的 display matrix，只应用「容器旋转 − 外部已解决角度」的差值（见 vii3_open）。
 *
 * 线程模型：一个 session 同一时刻只能被一个线程使用；视频与音频可在不同线程交替调用
 * （读包时不属于本流的包会进各自的 FIFO）。
 *
 * 授权：本文件与静态链入的 FFmpeg 均为 LGPL；构建时不得加 --enable-gpl / --enable-nonfree。
 * AVIF 是同一 dll 里的另一个编译单元：Native/avif/vii3_avif.c。
 *
 * 调试：加 -DV3_DEBUG 打开 native 告警（见 V3_DBG）。
 */

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/buffer.h>
#include <libavutil/display.h>
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/cpu.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "../v3_hdr.h"
#include "../v3_pool.h"
/* AVIF 在同一个 dll 的另一个编译单元里：Native/avif/vii3_avif.c（见文件末尾） */

/*
 * WASAPI（音频输出）。initguid.h 必须在声明 GUID 的头之前引入，这样 GUID 的定义
 * 在本编译单元生成，链接时只需 -lole32。
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <initguid.h>
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>

#ifdef V3_DEBUG
#include <stdio.h>
#define V3_DBG(...) fprintf(stderr, "[vii3] " __VA_ARGS__)
#else
#define V3_DBG(...) ((void)0)
#endif

#if defined(_WIN32)
#define V3_API __declspec(dllexport)
#else
#define V3_API __attribute__((visibility("default")))
#endif

/* ============================ 对外 ABI ============================ */

#define V3_AUDIO_S16 0 /* int16 交错 */
#define V3_AUDIO_F32 1 /* float32 交错 */

#define V3_ERR_BUFFER_TOO_SMALL (-2)

/* 并行度上限（解码与后处理共用），同时作为持久线程池的数组大小与信号量上限。 */
#define V3_MAX_THREADS 16

typedef struct Vii3MediaInfo
{
    int width;    /* 显示宽（已含旋转，直接拿来建位图即可） */
    int height;   /* 显示高（已含旋转） */
    int rotation; /* native 实际补转进像素的逆时针角度（= 容器 display matrix − 外部已解决角度），仅供诊断 */
    int has_video;
    int has_audio;
    double duration_sec;
    double fps;
    int audio_sample_rate; /* 源采样率 */
    int audio_channels;    /* 源声道数 */
    char video_codec[32];  /* 编解码器名（"h264"/"hevc"/"av1"），仅供诊断；规范只允许这三种 */

    /* --- HDR / 高位深（后加的尾部字段；托管侧按 ABI 大小判断是否可用） --- */
    int video_bit_depth;          /* 实际位深（8/10/12），由首帧像素格式推出 */
    int color_primaries;          /* AVCOL_PRI_* */
    int transfer_characteristics; /* AVCOL_TRC_* */
    int matrix_coefficients;      /* AVCOL_SPC_* */
    int color_range;              /* AVCOL_RANGE_*（0=unspecified） */
    int is_hdr;                   /* trc 是 PQ/HLG（与 vii3_video_next_ex 的色调映射判定同源） */

    int has_mdcv;                 /* 主显示元数据有效（SMPTE 2086） */
    int mdcv_red_x, mdcv_red_y;
    int mdcv_green_x, mdcv_green_y;
    int mdcv_blue_x, mdcv_blue_y;
    int mdcv_white_x, mdcv_white_y;
    int mdcv_min_luminance;       /* 0.0001 cd/m^2 */
    int mdcv_max_luminance;       /* 0.0001 cd/m^2 */

    int has_clli;                       /* CTA-861.3 内容亮度 */
    int clli_max_content_light_level;   /* cd/m^2 */
    int clli_max_frame_avg_light_level; /* cd/m^2 */

    /* = sizeof(Vii3MediaInfo)：托管侧据此判断尾部 HDR 字段是否可信（旧 dll 不写这一项）。 */
    int struct_size;
} Vii3MediaInfo;

/* 音频重采样目标。三个字段都要有效；传 NULL 给 vii3_open 表示由 native 选默认值。 */
typedef struct Vii3AudioOut
{
    int sample_rate;   /* 目标采样率 */
    int channels;      /* 目标声道数 */
    int sample_format; /* V3_AUDIO_S16 / V3_AUDIO_F32 */
} Vii3AudioOut;

/* 默认渲染设备的混音格式（共享模式下必须以它打开流）。 */
typedef struct Vii3AudioDeviceFormat
{
    int sample_rate;
    int channels;
    int sample_format; /* V3_AUDIO_F32 / V3_AUDIO_S16 */
    int valid;
} Vii3AudioDeviceFormat;

/* 返回已读字节数(>0)，0 表示 EOF，负值为错误。 */
typedef int (*V3ReadCb)(void *opaque, uint8_t *buf, int size);

/* whence 为 SEEK_SET/SEEK_CUR/SEEK_END，或 AVSEEK_SIZE(0x10000) 需返回总长度。 */
typedef int64_t (*V3SeekCb)(void *opaque, int64_t offset, int whence);

typedef struct Vii3Session Vii3Session;

V3_API const char *vii3_version(void);

V3_API Vii3Session *vii3_open(void *opaque, V3ReadCb read_cb, V3SeekCb seek_cb,
                             const Vii3AudioOut *audio_out, int external_rotation_cw,
                             Vii3MediaInfo *out_info);

/* 返回写入的帧字节数(>0)，0 表示 EOF，<0 为错误（V3_ERR_BUFFER_TOO_SMALL 表示 dst 太小）。 */
V3_API int vii3_video_next(Vii3Session *s, uint8_t *dst, int dst_capacity, double *out_pts);

/* 设置输出目标尺寸（画布缩小显示时用它少算像素）。w/h < 1 表示按原始分辨率。
 * 尺寸是**位图（旋转后）**坐标系，与 info.width/height 一致。
 * 两条上限：不会放大（目标 >= 视频原始尺寸时按原始分辨率）、长边 ≤ 4096。
 * 实际输出用 vii3_get_output_size 查询。
 * 尺寸变化会重建 sws 与中间缓冲（下次取帧时惰性重建），所以别每帧传不同的值。 */
V3_API int vii3_set_target_size(Vii3Session *s, int w, int h);

/* 当前输出（位图）尺寸，旋转后坐标系；受 vii3_set_target_size 影响，长边 ≤ 4096。 */
V3_API void vii3_get_output_size(const Vii3Session *s, int *w, int *h);

/* 同上，但可指定像素格式：
 *   0 = BGRA8（SDR；HDR 会被色调映射成 sRGB）
 *   1 = RGBA64 直通（HDR/高位深，源色彩空间原样；12bit 无损）
 *   2 = RGBA_F16 直通（同 1，half-float；约 11bit 有效精度，12bit 会丢约 1bit）
 *   3 = BGRA8 直出（不做色调映射——HDR 也原样输出，等价旧版行为，最快但偏灰） */
V3_API int vii3_video_next_ex(Vii3Session *s, uint8_t *dst, int dst_capacity, int fmt, double *out_pts);

/* 返回写入的 PCM 字节数(>0)，0 表示 EOF，<0 为错误。 */
V3_API int vii3_audio_next(Vii3Session *s, uint8_t *dst, int dst_capacity, double *out_pts);

V3_API int vii3_seek(Vii3Session *s, double seconds);

V3_API void vii3_close(Vii3Session *s);

/* ---------------------------- 音频输出（WASAPI 共享模式） ----------------------------
 *
 * 用法：
 *   1. vii3_audio_device_format(&fmt)        查默认渲染设备的混音格式
 *   2. vii3_open(..., audioOut = fmt, ...)   用同一个格式打开媒体
 *   3. vii3_audio_start(session)             起渲染线程
 *   4. 应用线程循环 vii3_audio_next() -> vii3_audio_push()
 *   5. vii3_audio_played() 作为 A/V 同步主时钟
 *   6. vii3_audio_stop(session)              停止（vii3_close 里也会自动调）
 */

/* 查默认渲染设备（eRender/eMultimedia）的混音格式。成功返回 0。 */
V3_API int vii3_audio_device_format(Vii3AudioDeviceFormat *out);

/* 打开设备并启动渲染线程；要求会话的音频输出格式与设备混音格式一致。成功返回 0。 */
V3_API int vii3_audio_start(Vii3Session *s);

/* 停止渲染线程并释放设备。可重复调用。 */
V3_API void vii3_audio_stop(Vii3Session *s);

/* 把 PCM 推入内部环形缓冲（应用线程调用）。返回写入字节数；队列满时可能小于 len，且总是整帧对齐。 */
V3_API int vii3_audio_push(Vii3Session *s, const uint8_t *pcm, int len);

/* 环形缓冲里还没交给设备的字节数（用于判断要不要继续推）。 */
V3_API int vii3_audio_buffered(Vii3Session *s);

/* 已播放的媒体时长（秒）——只计真实媒体帧、不含欠载补的静音，可直接当主时钟。 */
V3_API double vii3_audio_played(Vii3Session *s);

/* 静音（走 ISimpleAudioVolume，只影响本进程这一路，不动系统混音总线）。 */
V3_API void vii3_audio_set_mute(Vii3Session *s, int mute);

/* ============================ 内部实现 ============================ */

#define V3_AVIO_BUFFER_SIZE (64 * 1024)
#define V3_FIFO_CAP 512

typedef struct V3Fifo
{
    AVPacket *items[V3_FIFO_CAP];
    int head;
    int count;
} V3Fifo;

/* 音频输出的环形缓冲时长（毫秒）。 */
#define V3_AUDIO_RING_MS 500

/*
 * 音频输出状态。
 *
 * COM 对象（IAudioClient / IAudioRenderClient / ISimpleAudioVolume）只在渲染线程
 * 里创建、使用、释放，不放进这个结构体 —— 避免跨线程访问同一次 COM 调用。
 */
typedef struct V3AudioOut
{
    volatile LONG started;
    volatile LONG stop;
    volatile LONG setup_result; /* 0 = 设备已就绪，-1 = 初始化失败，-2 = 尚未开始 */
    volatile LONG mute;

    HANDLE thread;
    HANDLE wake_event;  /* 交给 IAudioClient 的事件句柄（自动重置） */
    HANDLE ready_event; /* 渲染线程完成设备初始化后置位，供 start 同步等待 */

    int sample_rate;
    int channels;
    int bytes_per_sample;
    int frame_bytes;

    uint32_t device_buffer_frames;
    LONG mute_applied;

    uint8_t *ring;
    int ring_capacity; /* 字节 */
    int ring_read;
    int ring_write;
    int ring_available; /* 已写入、尚未交给设备的字节 */

    long long frames_rendered; /* 累计真正交给设备的媒体帧（不含欠载补的静音） */

    CRITICAL_SECTION lock;
    int lock_ready;
} V3AudioOut;

struct Vii3Session
{
    AVFormatContext *fmt;
    AVIOContext *avio;
    void *user_opaque;
    V3ReadCb read_cb;
    V3SeekCb seek_cb;

    int video_index;
    AVCodecContext *vdec;
    AVFrame *vframe;
    AVPacket *pending_v;
    struct SwsContext *sws;
    int src_w;
    int src_h;
    enum AVPixelFormat src_fmt;
    int out_w; /* 缩放后、旋转前 */
    int out_h;
    int disp_w; /* 旋转后 = 输出位图尺寸（打开时未设目标尺寸，所以 = info.width/height） */
    int disp_h;
    /* 目标缩放尺寸（0 = 按原始分辨率输出）。由 vii3_set_target_size 设置，
     * 用于画布缩小显示时少算像素（见 v3_apply_out_size）。 */
    int tgt_w;
    int tgt_h;
    int rotation;
    uint8_t *scratch;
    int scratch_capacity;
    int v_flushed;
    double v_time_base;
    int64_t v_start_ts;

    int audio_index;
    AVCodecContext *adec;
    AVFrame *aframe;
    AVPacket *pending_a;
    struct SwrContext *swr;
    int in_sr;
    int in_ch;
    enum AVSampleFormat in_fmt;
    int out_sr;
    int out_ch;
    enum AVSampleFormat out_fmt;
    int out_bytes_per_sample;
    int a_flushed;
    double a_time_base;
    int64_t a_start_ts;

    V3Fifo fifo_v;
    V3Fifo fifo_a;

    int has_prepared_frame; /* 打开时已解出并备好的首帧（存在 vframe 里） */
    float peak_smooth_rw;   /* 实测内容峰值的平滑值（参考白倍数，见 v3_frame_peak_rw） */

    int v_threads;          /* 解码与后处理的并行度（打开时按逻辑核数定，见 vii3_open） */
    int sws_dst_fmt;        /* 当前 sws 输出像素格式（缓存键的一部分） */
    AVFrame *sws_dst_frame; /* sws_scale_frame() 的输出帧（复用；buf[0] 每帧包住目标缓冲） */

    /* 后处理线程池（共享实现见 v3_pool.h；打开时建、关闭时收） */
    V3Pool pool;

    V3AudioOut audio_out;
};

V3_API void vii3_close(Vii3Session *s);

/* ---------------------------- FIFO ---------------------------- */

static int v3_fifo_push(V3Fifo *f, AVPacket *pkt)
{
    if (f->count >= V3_FIFO_CAP)
    {
        return -1;
    }
    f->items[(f->head + f->count) % V3_FIFO_CAP] = pkt;
    f->count++;
    return 0;
}

static AVPacket *v3_fifo_pop(V3Fifo *f)
{
    if (f->count == 0)
    {
        return NULL;
    }
    AVPacket *pkt = f->items[f->head];
    f->head = (f->head + 1) % V3_FIFO_CAP;
    f->count--;
    return pkt;
}

static void v3_fifo_clear(V3Fifo *f)
{
    AVPacket *pkt;
    while ((pkt = v3_fifo_pop(f)) != NULL)
    {
        av_packet_free(&pkt);
    }
}

/*
 * 取属于 stream_index 的下一个包。
 *
 * 视频和音频共用同一个 AVFormatContext，而 vii3_video_next / vii3_audio_next 是
 * 被独立调用的，所以读包时要把不属于本流的包缓存进它自己的 FIFO，不能丢。
 */
static AVPacket *v3_pkt_next(Vii3Session *s, int stream_index)
{
    V3Fifo *own = stream_index == s->video_index ? &s->fifo_v : &s->fifo_a;
    V3Fifo *other = stream_index == s->video_index ? &s->fifo_a : &s->fifo_v;

    AVPacket *pkt = v3_fifo_pop(own);
    if (pkt != NULL)
    {
        return pkt;
    }

    for (;;)
    {
        AVPacket *fresh = av_packet_alloc();
        if (fresh == NULL)
        {
            return NULL;
        }
        int r = av_read_frame(s->fmt, fresh);
        if (r < 0)
        {
            av_packet_free(&fresh);
            return NULL;
        }

        if (fresh->stream_index == stream_index)
        {
            return fresh;
        }
        if (fresh->stream_index == s->video_index || fresh->stream_index == s->audio_index)
        {
            if (v3_fifo_push(other, fresh) < 0)
            {
                av_packet_free(&fresh); /* 溢出，只能丢 */
            }
        }
        else
        {
            av_packet_free(&fresh); /* 其余流（字幕等）忽略 */
        }
    }
}

/* ---------------------------- AVIO 回调 ---------------------------- */

static int v3_read_trampoline(void *opaque, uint8_t *buf, int size)
{
    Vii3Session *s = (Vii3Session *)opaque;
    int r = s->read_cb(s->user_opaque, buf, size);
    /* FFmpeg 的 AVIO 约定：流末尾必须返回负的 AVERROR_EOF；
     * 返回 0 会被当成「零字节数据」，探测格式时可能死循环。 */
    return r == 0 ? AVERROR_EOF : r;
}

/* FFmpeg 可能在 whence 里或上内部标志（AVSEEK_FORCE），先归一化再交给调用方。 */
static int64_t v3_seek_trampoline(void *opaque, int64_t offset, int whence)
{
    Vii3Session *s = (Vii3Session *)opaque;
    if (whence == AVSEEK_SIZE)
    {
        return s->seek_cb(s->user_opaque, offset, AVSEEK_SIZE);
    }
    whence &= ~AVSEEK_FORCE;
    return s->seek_cb(s->user_opaque, offset, whence);
}

static void v3_log_silence(void *avcl, int level, const char *fmt, va_list vl)
{
    (void)avcl;
    (void)level;
    (void)fmt;
    (void)vl;
}

/* ---------------------------- 旋转 ---------------------------- */

/*
 * 读容器的显示旋转（display matrix，手机竖录时会写）。
 *
 * 语义：av_display_rotation_get() 返回「矩阵把画面逆时针旋转的角度」，范围 [-180, 180]；
 * 这里原样取用（不取负），四舍五入成整数度并归一化到 [0, 360)。
 *
 * 与 mpv 的关系（不必再猜，两者是同一个旋转）：
 *   mpv 的 demux-rotation = (((int)(-r) % 360) + 360) % 360，是「顺时针为正」；
 *   而 mpv 的 --video-rotate / Avalonia 的 RotateTransform 都是顺时针为正。
 *   所以  顺时针旋转 (-r)  ==  逆时针旋转 r  ==  本函数返回值。
 *   两种实现转出来的像素完全相同，区别只是「逆时针」还是「顺时针」的表述。
 *
 * 因此这里不需要任何按编码器/容器的特判：motion photo 的 display matrix 本身就是
 * 权威值（Pixel / Samsung / Apple 的样本都验证过，见 Tests/Vii3Media.Tests）。
 */
static int v3_read_display_rotation(const AVStream *stream)
{
    const AVPacketSideData *sd = av_packet_side_data_get(
        stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX);
    if (sd == NULL || sd->data == NULL)
    {
        return 0;
    }

    double theta = av_display_rotation_get((const int32_t *)sd->data);
    if (isnan(theta))
    {
        return 0;
    }

    int rotation = (int)llround(theta);
    rotation %= 360;
    if (rotation < 0)
    {
        rotation += 360;
    }
    return rotation;
}

/*
 * 把紧凑的 BGRA 帧逆时针旋转 90/180/270 到 dst（dst 尺寸须为旋转后的尺寸）。
 * rotation 的语义与 v3_read_display_rotation() 一致：rotation==90 是逆时针 90°。
 */
/* 旋转按 32x32 分块：90/270 是「转置」式写入——按源顺序遍历时目标地址每次跨 h 个
 * 元素（换 cache line）。分块后一块的目标区域只有 32 行、能留在 cache 里，读写的
 * cache 命中率都大幅提高（180 是行内倒序，本来就局部，不必分块）。 */
#define V3_ROT_TILE 32

static void v3_rotate_bgra(const uint8_t *src, int w, int h, uint8_t *dst, int rotation,
                           int y0, int y1)
{
    const uint32_t *s = (const uint32_t *)src;
    uint32_t *d = (uint32_t *)dst;

    switch (rotation)
    {
    case 90:
        for (int by = y0; by < y1; by += V3_ROT_TILE)
            for (int bx = 0; bx < w; bx += V3_ROT_TILE)
                for (int y = by; y < by + V3_ROT_TILE && y < y1; y++)
                    for (int x = bx; x < bx + V3_ROT_TILE && x < w; x++)
                        d[(size_t)(w - 1 - x) * h + y] = s[(size_t)y * w + x];
        break;
    case 180:
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < w; x++)
                d[(size_t)(h - 1 - y) * w + (w - 1 - x)] = s[(size_t)y * w + x];
        break;
    case 270:
        for (int by = y0; by < y1; by += V3_ROT_TILE)
            for (int bx = 0; bx < w; bx += V3_ROT_TILE)
                for (int y = by; y < by + V3_ROT_TILE && y < y1; y++)
                    for (int x = bx; x < bx + V3_ROT_TILE && x < w; x++)
                        d[(size_t)x * h + (h - 1 - y)] = s[(size_t)y * w + x];
        break;
    default:
        break;
    }
}

/* 帧的矩阵 -> sws 的 SWS_CS_*（sws 只做 YUV<->RGB 矩阵，不做跨色域，
 * 所以输出 RGB 落在源原色里；BT.709/601 内容也各自正确）。 */
static int v3_map_sws_cs(const AVFrame *frame)
{
    enum AVColorSpace spc = frame->colorspace;
    if (spc == AVCOL_SPC_UNSPECIFIED)
    {
        /* H.273 惯例：矩阵未标记时，若传递函数是 PQ/HLG 或原色是 BT.2020，按 BT.2020 处理
         * —— 不少手机/编码器产出的 HDR 流会漏标矩阵，按默认 601 解会双重出错
         * （颜色偏 + 少做色域压缩）。 */
        if (frame->color_trc == AVCOL_TRC_SMPTE2084 ||
            frame->color_trc == AVCOL_TRC_ARIB_STD_B67 ||
            frame->color_primaries == AVCOL_PRI_BT2020)
        {
            return SWS_CS_BT2020;
        }
        return SWS_CS_DEFAULT; /* 普通 SDR 未标记：维持 BT.601（与旧版一致） */
    }
    switch (spc)
    {
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:  return SWS_CS_BT2020;
        case AVCOL_SPC_BT709:      return SWS_CS_ITU709;
        default:                   return SWS_CS_DEFAULT; /* BT.601 */
    }
}

/* 把位深 / CICP / is_hdr 填进 out_info。
 * 首帧已预解时用「首帧」的实际像素格式与色彩参数（AV1/libdav1d 等解码器要到解码
 * 才确定位深，codecpar 未必准），否则退回解码器声明值 / 容器参数。 */
static void v3_fill_info_basic(Vii3MediaInfo *out, const Vii3Session *s)
{
    const AVCodecParameters *cp = s->fmt->streams[s->video_index]->codecpar;
    int have_frame = s->has_prepared_frame;
    enum AVPixelFormat pf = have_frame ? (enum AVPixelFormat)s->vframe->format
                                       : s->vdec->pix_fmt;
    int depth = av_pix_fmt_desc_get(pf) ? (int)av_pix_fmt_desc_get(pf)->comp[0].depth : 8;
    int trc = have_frame ? s->vframe->color_trc : cp->color_trc;
    int pri = have_frame ? s->vframe->color_primaries : cp->color_primaries;
    int spc = have_frame ? s->vframe->colorspace : cp->color_space;
    int rng = have_frame ? s->vframe->color_range : cp->color_range;
    if (trc == AVCOL_TRC_UNSPECIFIED) trc = cp->color_trc;
    if (pri == AVCOL_PRI_UNSPECIFIED) pri = cp->color_primaries;
    if (spc == AVCOL_SPC_UNSPECIFIED) spc = cp->color_space;
    if (rng == AVCOL_RANGE_UNSPECIFIED) rng = cp->color_range;

    out->video_bit_depth = depth;
    out->color_primaries = pri;
    out->transfer_characteristics = trc;
    out->matrix_coefficients = spc;
    out->color_range = rng;
    /* is_hdr 只认传递函数（PQ/HLG）——与 v3_emit_video 里「要不要色调映射」的判定
     * 完全同源，避免出现「上报 HDR 却不做色调映射」的错位。高位深另见 video_bit_depth。 */
    out->is_hdr = (trc == AVCOL_TRC_SMPTE2084 || trc == AVCOL_TRC_ARIB_STD_B67) ? 1 : 0;
}

/* 把首帧的 HDR side data（MDCV / CLLI）回填（带不出就留 0）。 */
static int v3_rational_i(AVRational r, float scale)
{
    if (r.den == 0)
    {
        return 0;
    }
    return (int)lroundf((float)r.num / r.den * scale);
}

static void v3_fill_hdr_side_data(Vii3MediaInfo *out, const AVFrame *frame)
{
    const AVFrameSideData *sd =
        av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (sd != NULL && sd->data != NULL)
    {
        const AVMasteringDisplayMetadata *m = (const AVMasteringDisplayMetadata *)sd->data;
        out->has_mdcv = (m->has_primaries || m->has_luminance) ? 1 : 0;
        out->mdcv_red_x   = v3_rational_i(m->display_primaries[0][0], 100000.0f);
        out->mdcv_red_y   = v3_rational_i(m->display_primaries[0][1], 100000.0f);
        out->mdcv_green_x = v3_rational_i(m->display_primaries[1][0], 100000.0f);
        out->mdcv_green_y = v3_rational_i(m->display_primaries[1][1], 100000.0f);
        out->mdcv_blue_x  = v3_rational_i(m->display_primaries[2][0], 100000.0f);
        out->mdcv_blue_y  = v3_rational_i(m->display_primaries[2][1], 100000.0f);
        out->mdcv_white_x = v3_rational_i(m->white_point[0], 100000.0f);
        out->mdcv_white_y = v3_rational_i(m->white_point[1], 100000.0f);
        out->mdcv_min_luminance = v3_rational_i(m->min_luminance, 10000.0f);
        out->mdcv_max_luminance = v3_rational_i(m->max_luminance, 10000.0f);
    }

    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (sd != NULL && sd->data != NULL)
    {
        const AVContentLightMetadata *c = (const AVContentLightMetadata *)sd->data;
        out->has_clli = 1;
        out->clli_max_content_light_level = (int)c->MaxCLL;
        out->clli_max_frame_avg_light_level = (int)c->MaxFALL;
    }
}

/* 保证 sws 上下文匹配当前帧（尺寸/像素格式/目标格式变化时重建），并按源矩阵设好色彩空间。 */
static int v3_ensure_sws_ex(Vii3Session *s, const AVFrame *frame, enum AVPixelFormat dst_fmt)
{
    enum AVPixelFormat fmt = (enum AVPixelFormat)frame->format;
    if (s->sws != NULL && fmt == s->src_fmt && frame->width == s->src_w &&
        frame->height == s->src_h && (int)dst_fmt == s->sws_dst_fmt)
    {
        return 0;
    }

    if (s->sws != NULL)
    {
        sws_freeContext(s->sws);
        s->sws = NULL;
    }

    {
        /* 用显式属性路径（sws_getContext 不支持设线程数）。注意 libswscale 的 slice threading
         * 只被 sws_scale_frame()（新 API）使用，经典 sws_scale() 会无视 "threads"。 */
        SwsContext *sws = sws_alloc_context();
        if (sws == NULL)
        {
            return -1;
        }
        /* 线程数按**物理核**（不是逻辑核）：超线程对逐像素转换无增益，且要避免与解码线程、
         * 后处理池三方超订（见 v3_pool.h 的说明与基准结论）。检测失败退回 0 = auto。 */
        {
            int pc = v3_cpu_physical_cores();
            av_opt_set_int(sws, "threads", pc > 1 ? pc : 0, 0);
        }
        av_opt_set_int(sws, "srcw", frame->width, 0);
        av_opt_set_int(sws, "srch", frame->height, 0);
        av_opt_set_int(sws, "src_format", fmt, 0);
        av_opt_set_int(sws, "dstw", s->out_w, 0);
        av_opt_set_int(sws, "dsth", s->out_h, 0);
        av_opt_set_int(sws, "dst_format", dst_fmt, 0);
        av_opt_set_int(sws, "sws_flags", SWS_BILINEAR, 0);
        if (sws_init_context(sws, NULL, NULL) < 0)
        {
            sws_freeContext(sws);
            return -1;
        }
        s->sws = sws;
    }

    s->src_fmt = fmt;
    s->src_w = frame->width;
    s->src_h = frame->height;
    s->sws_dst_fmt = (int)dst_fmt;

    int cs = v3_map_sws_cs(frame);
    int src_range = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    /* 输出 RGB 恒 full-range（RGB 没有 range 概念）；src/dst 用同一矩阵系数。 */
    sws_setColorspaceDetails(s->sws, sws_getCoefficients(cs), src_range,
                             sws_getCoefficients(cs), 1, 0, 1 << 16, 1 << 16);
    return 0;
}

/* sws_scale_frame 用 dst->buf[0] 判断「调用方已分配缓冲」（只设 data[0] 会被它重新分配，
 * 输出跑到别处 → 全黑）。这里把外部缓冲包成 AVBufferRef；free 回调空实现，
 * 保证 unref 时不会释放我们的内存。 */
static void v3_sws_ext_free(void *opaque, uint8_t *data)
{
    (void)opaque;
    (void)data;
}

/* 8 字节/像素的旋转（RGBA64 / RGBA_F16），语义同 v3_rotate_bgra。 */
static void v3_rotate_rgba64(const uint8_t *src, int w, int h, uint8_t *dst, int rotation,
                             int y0, int y1)
{
    const uint64_t *s = (const uint64_t *)src;
    uint64_t *d = (uint64_t *)dst;
    switch (rotation)
    {
    case 90:
        for (int by = y0; by < y1; by += V3_ROT_TILE)
            for (int bx = 0; bx < w; bx += V3_ROT_TILE)
                for (int y = by; y < by + V3_ROT_TILE && y < y1; y++)
                    for (int x = bx; x < bx + V3_ROT_TILE && x < w; x++)
                        d[(size_t)(w - 1 - x) * h + y] = s[(size_t)y * w + x];
        break;
    case 180:
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < w; x++)
                d[(size_t)(h - 1 - y) * w + (w - 1 - x)] = s[(size_t)y * w + x];
        break;
    case 270:
        for (int by = y0; by < y1; by += V3_ROT_TILE)
            for (int bx = 0; bx < w; bx += V3_ROT_TILE)
                for (int y = by; y < by + V3_ROT_TILE && y < y1; y++)
                    for (int x = bx; x < bx + V3_ROT_TILE && x < w; x++)
                        d[(size_t)x * h + (h - 1 - y)] = s[(size_t)y * w + x];
        break;
    default:
        break;
    }
}

/*
 * RGBA64（源色彩空间，PQ/HLG 编码）-> sRGB BGRA8，含旋转。
 * EOTF / ACES / sRGB OETF 的查表在共享头 v3_hdr.h（v3_eotf_lut / v3_tm_out_lut）。
 * 按「源顺序」遍历（顺序读源），旋转只决定目标写入位置；再按 32x32 分块，
 * 让 90/270 转置式写入的目标区域留在 cache 内（与 v3_rotate_bgra 同一手法）。
 */
static void v3_tone_map_rgba64_rotated(const uint16_t *src, int sw, int sh,
                                       int is_pq, int is_bt2020, int rotation,
                                       uint8_t *dst, int dw, int dh, int y0, int y1)
{
    v3_ensure_hdr_luts();
    const float inv = 1.0f / 65535.0f;
    const float *eotf = v3_eotf_lut[is_pq ? 0 : 1];
    for (int by = y0; by < y1; by += V3_ROT_TILE)
    {
        const int yend = (by + V3_ROT_TILE < y1) ? by + V3_ROT_TILE : y1;
        for (int bx = 0; bx < sw; bx += V3_ROT_TILE)
        {
            const int xend = (bx + V3_ROT_TILE < sw) ? bx + V3_ROT_TILE : sw;
            for (int sy = by; sy < yend; sy++)
            {
                int sx = bx;
#if defined(__SSE2__)
                /* 4 像素一批：只把算术向量化（查表在 SSE2 下没有 gather，仍标量）。
                 * 90/270 的目标落在同一列上、地址不连续，所以逐个写回 —— 写回不是瓶颈，
                 * 算术才是（实测这趟从约 14ns/px 降到约 8-9ns/px，且与标量版逐字节一致）。 */
                for (; sx + 4 <= xend; sx += 4)
                {
                    const uint16_t *p = src + ((size_t)sy * sw + sx) * 4;
                    float lr[4], lg4[4], lb4[4];
                    for (int k = 0; k < 4; k++)
                    {
                        lr[k] = eotf[p[k * 4 + 0] >> 4];
                        lg4[k] = eotf[p[k * 4 + 1] >> 4];
                        lb4[k] = eotf[p[k * 4 + 2] >> 4];
                    }
                    uint8_t tmp[16];
                    v3_tone_map4_sse2(lr, lg4, lb4, is_pq, is_bt2020, tmp);
                    for (int k = 0; k < 4; k++)
                    {
                        const int sxk = sx + k;
                        int dx, dy;
                        switch (rotation)
                        {
                        case 90:  dx = sy;           dy = sw - 1 - sxk; break;
                        case 180: dx = sw - 1 - sxk; dy = sh - 1 - sy;  break;
                        case 270: dx = sh - 1 - sy;  dy = sxk;          break;
                        default:  dx = sxk;          dy = sy;           break;
                        }
                        uint8_t *o = dst + ((size_t)dy * dw + dx) * 4;
                        o[0] = tmp[k * 4 + 0];
                        o[1] = tmp[k * 4 + 1];
                        o[2] = tmp[k * 4 + 2];
                        o[3] = (uint8_t)(p[k * 4 + 3] * inv * 255.0f + 0.5f);
                    }
                }
#endif
                for (; sx < xend; sx++)
                {
                    const uint16_t *p = src + ((size_t)sy * sw + sx) * 4;
                    int dx, dy;
                    switch (rotation)
                    {
                    case 90:  dx = sy;           dy = sw - 1 - sx; break;
                    case 180: dx = sw - 1 - sx;  dy = sh - 1 - sy; break;
                    case 270: dx = sh - 1 - sy;  dy = sx;          break;
                    default:  dx = sx;           dy = sy;          break;
                    }
                    uint8_t *o = dst + ((size_t)dy * dw + dx) * 4;
                    v3_tone_map_linear_lut(eotf[p[0] >> 4], eotf[p[1] >> 4], eotf[p[2] >> 4],
                                           is_pq, is_bt2020, o);
                    o[3] = (uint8_t)(p[3] * inv * 255.0f + 0.5f);
                }
            }
        }
    }
}

/* RGBA64 -> RGBA_F16（half），含旋转。
 * 与色调映射同一手法：按源顺序遍历（顺序读源）+ 32x32 分块，避免 90/270 的目标写入
 * 每次跨行换 cache line（旧版按目标遍历，读源也是乱序的，双重不友好）。 */
static void v3_rgba64_to_f16_rotated(const uint16_t *src, int sw, int sh,
                                     int rotation, uint8_t *dst, int dw, int dh,
                                     int y0, int y1)
{
    const float inv = 1.0f / 65535.0f;
    for (int by = y0; by < y1; by += V3_ROT_TILE)
    {
        const int yend = (by + V3_ROT_TILE < y1) ? by + V3_ROT_TILE : y1;
        for (int bx = 0; bx < sw; bx += V3_ROT_TILE)
        {
            const int xend = (bx + V3_ROT_TILE < sw) ? bx + V3_ROT_TILE : sw;
            for (int sy = by; sy < yend; sy++)
            {
                for (int sx = bx; sx < xend; sx++)
                {
                    const uint16_t *p = src + ((size_t)sy * sw + sx) * 4;
                    int dx, dy;
                    switch (rotation)
                    {
                    case 90:  dx = sy;           dy = sw - 1 - sx; break;
                    case 180: dx = sw - 1 - sx;  dy = sh - 1 - sy; break;
                    case 270: dx = sh - 1 - sy;  dy = sx;          break;
                    default:  dx = sx;           dy = sy;          break;
                    }
                    uint16_t *o = (uint16_t *)(dst + ((size_t)dy * dw + dx) * 8);
                    for (int k = 0; k < 4; k++)
                    {
                        o[k] = v3_float_to_half(p[k] * inv);
                    }
                }
            }
        }
    }
}

/* 后处理的逐行线程池用共享实现：见 v3_pool.h（视频与 AVIF shim 同一套）。 */



/* 后处理分派：按源行区间 [y0,y1) 执行。 */
typedef struct
{
    const uint8_t *scratch; /* sws 输出（out_w x out_h） */
    uint8_t *dst;
    int sw, sh, dw, dh;
    int rotation;
    int sws_bpp;
    int mode; /* 0=色调映射, 1=half 转换, 2=旋转 */
    int is_pq;
    int is_bt2020;
} V3PostCtx;

static void v3_post_band(void *param, int y0, int y1)
{
    V3PostCtx *c = (V3PostCtx *)param;
    switch (c->mode)
    {
    case 0:
        v3_tone_map_rgba64_rotated((const uint16_t *)c->scratch, c->sw, c->sh,
                                   c->is_pq, c->is_bt2020, c->rotation, c->dst,
                                   c->dw, c->dh, y0, y1);
        break;
    case 1:
        v3_rgba64_to_f16_rotated((const uint16_t *)c->scratch, c->sw, c->sh,
                                 c->rotation, c->dst, c->dw, c->dh, y0, y1);
        break;
    default:
        if (c->sws_bpp == 4)
        {
            v3_rotate_bgra(c->scratch, c->sw, c->sh, c->dst, c->rotation, y0, y1);
        }
        else
        {
            v3_rotate_rgba64(c->scratch, c->sw, c->sh, c->dst, c->rotation, y0, y1);
        }
        break;
    }
}

/* 没有 MDCV/CLLI 时按 4x4 子采样实测内容峰值（在 Y 平面上取最大值）。
 * 比按传递函数标称（PQ 10000 nit）靠谱：否则未标记的 PQ 内容会被当成 10000 nit 而整体压暗。 */
static float v3_measure_peak_rw(const AVFrame *frame, int is_pq)
{
    if (frame->data[0] == NULL || frame->width <= 0 || frame->height <= 0)
    {
        return v3_nominal_peak_rw(is_pq);
    }
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get((enum AVPixelFormat)frame->format);
    const int depth = (d != NULL) ? (int)d->comp[0].depth : 8;
    const int step = 4; /* 1/16 采样：足够稳，代价可忽略 */
    uint32_t mx = 0;
    if (depth > 8)
    {
        for (int y = 0; y < frame->height; y += step)
        {
            const uint16_t *row =
                (const uint16_t *)(frame->data[0] + (size_t)y * frame->linesize[0]);
            for (int x = 0; x < frame->width; x += step)
            {
                uint32_t v = row[x];
                if (v > mx)
                {
                    mx = v;
                }
            }
        }
    }
    else
    {
        for (int y = 0; y < frame->height; y += step)
        {
            const uint8_t *row = frame->data[0] + (size_t)y * frame->linesize[0];
            for (int x = 0; x < frame->width; x += step)
            {
                uint32_t v = row[x];
                if (v > mx)
                {
                    mx = v;
                }
            }
        }
    }
    if (mx == 0)
    {
        return v3_nominal_peak_rw(is_pq);
    }
    /* EOTF 表按「16bit 码值 >> 4」索引（见 v3_hdr.h），而 mx 是源位深的原始码值，
     * 必须先归一到 16bit：idx = mx * 4095 / (2^depth - 1)。 */
    int idx = (int)((uint32_t)mx * 4095u / (uint32_t)((1u << depth) - 1u));
    if (idx > 4095)
    {
        idx = 4095;
    }
    float peak;
    if (is_pq)
    {
        peak = v3_eotf_lut[0][idx]; /* PQ 表值就是「绝对亮度/203」 */
    }
    else
    {
        /* HLG：OOTF 是公共增益，峰值亮度 ≈ 增益 × scene linear（与 v3_tone_map* 同一算法） */
        float ls = v3_eotf_lut[1][idx];
        int oi = (int)(ls * 1023.0f + 0.5f);
        if (oi < 0)
        {
            oi = 0;
        }
        else if (oi > 1023)
        {
            oi = 1023;
        }
        peak = v3_hlg_ootf_lut[oi] * ls;
    }
    return peak < 1.0f ? 1.0f : peak;
}

/* 源内容峰值（参考白倍数 = 203nit 的倍数）。优先级：
 * MDCV 主显示峰值 -> CLLI 的 maxCLL -> 实测 -> 标称值。
 * 色调映射的出表按它重建；实测路径做「立即跟涨、缓慢回落」的平滑，避免亮斑进出画面时抖动。 */
static float v3_frame_peak_rw(Vii3Session *s, const AVFrame *frame, int is_pq)
{
    const AVFrameSideData *sd =
        av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (sd != NULL && sd->size >= (int)sizeof(AVMasteringDisplayMetadata))
    {
        const AVMasteringDisplayMetadata *m = (const AVMasteringDisplayMetadata *)sd->data;
        if (m->has_luminance && m->max_luminance.den > 0)
        {
            double nit = av_q2d(m->max_luminance);
            if (nit > 1.0)
            {
                return (float)(nit / 203.0);
            }
        }
    }
    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (sd != NULL && sd->size >= (int)sizeof(AVContentLightMetadata))
    {
        const AVContentLightMetadata *c = (const AVContentLightMetadata *)sd->data;
        if (c->MaxCLL > 1)
        {
            return (float)(c->MaxCLL / 203.0);
        }
    }

    float m = v3_measure_peak_rw(frame, is_pq);
    if (s->peak_smooth_rw <= 0.0f || m >= s->peak_smooth_rw)
    {
        s->peak_smooth_rw = m; /* 变亮立刻跟 */
    }
    else
    {
        s->peak_smooth_rw = s->peak_smooth_rw * 0.90f + m * 0.10f; /* 变暗缓慢回落 */
    }
    return s->peak_smooth_rw;
}

/*
 * 把已解码帧 produce 到 dst：用 sws 转成中间 RGBA64（或 BGRA8），再按 fmt 后处理
 * （色调映射 / half 转换 / 旋转）。scratch 存放 sws 输出（out_w x out_h）。
 * 无旋转且无需后处理时（SDR 的 BGRA8、或 fmt 1/3 且无旋转），sws 直接写 dst，
 * 省掉一次中间缓冲的写+读；后处理按源行区间多线程并行。
 *
 * YUV->RGB 交给 libswscale：自己写矩阵 + 色度上采样并融进这一趟做过（并实测更慢），
 * 所以保持现状。手写标量矩阵拼不过 libswscale 的 SIMD，而融合省下的中间缓冲流量
 * （约 36MB/帧）并不是瓶颈 —— 解码与转换在同一调用内串行，不存在与解码线程争带宽。
 */
static int v3_emit_video(Vii3Session *s, const AVFrame *frame, uint8_t *dst, int fmt)
{
    const int is_hdr = (frame->color_trc == AVCOL_TRC_SMPTE2084 ||
                        frame->color_trc == AVCOL_TRC_ARIB_STD_B67);
    const int tone_map = (fmt == 0 && is_hdr);
    /* fmt==3 与 fmt==0 不同：始终 sws 直出 BGRA8，不做色调映射（HDR 也原样输出，等价旧版）。
     * 下方 sws_dst 对 fmt!=0 一律走 BGRA，输出阶段落到 rotate/copy 分支。 */
    enum AVPixelFormat sws_dst =
        (tone_map || fmt == 1 || fmt == 2) ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_BGRA;

    if (v3_ensure_sws_ex(s, frame, sws_dst) < 0)
    {
        return -1;
    }

    const int sws_bpp = (sws_dst == AV_PIX_FMT_BGRA) ? 4 : 8;
    const int rotation = s->rotation;
    const int sw = s->out_w, sh = s->out_h;
    const int dw = s->disp_w, dh = s->disp_h;

    /* 无需后处理（非色调映射、非 half、无旋转）→ sws 直接写 dst。 */
    const int direct = (rotation == 0) && !tone_map && (fmt != 2);

    uint8_t *sws_out;
    if (direct)
    {
        sws_out = dst;
    }
    else
    {
        int need = sw * sh * sws_bpp;
        if (s->scratch_capacity < need)
        {
            av_free(s->scratch);
            s->scratch = (uint8_t *)av_malloc((size_t)need);
            s->scratch_capacity = s->scratch != NULL ? need : 0;
        }
        if (s->scratch == NULL)
        {
            return -1;
        }
        sws_out = s->scratch;
    }

    /* 走 sws_scale_frame（新 API）：只有它走 libswscale 的 slicethread。
     * 必须把外部缓冲包成 dst->buf[0]，否则它按「未分配」处理、自己再分配一块。 */
    AVFrame *dstf = s->sws_dst_frame;
    av_frame_unref(dstf);
    AVBufferRef *ref = av_buffer_create(sws_out, (size_t)sw * sh * sws_bpp,
                                        v3_sws_ext_free, NULL, 0);
    if (ref == NULL)
    {
        return -1;
    }
    dstf->buf[0] = ref;
    dstf->data[0] = ref->data;
    dstf->linesize[0] = sw * sws_bpp;
    dstf->format = (int)sws_dst;
    dstf->width = sw;
    dstf->height = sh;
    if (sws_scale_frame(s->sws, dstf, frame) < 0)
    {
        return -1;
    }

    if (direct)
    {
        return dw * dh * sws_bpp;
    }

    V3PostCtx c;
    memset(&c, 0, sizeof(c));
    c.scratch = s->scratch;
    c.dst = dst;
    c.sw = sw;
    c.sh = sh;
    c.dw = dw;
    c.dh = dh;
    c.rotation = rotation;
    c.sws_bpp = sws_bpp;

    if (tone_map)
    {
        c.mode = 0;
        c.is_pq = (frame->color_trc == AVCOL_TRC_SMPTE2084);
        /* 原色未标记时按 BT.2020 推定（色调映射只在 PQ/HLG 下发生，见 v3_hdr.h）。 */
        c.is_bt2020 = (frame->color_primaries == AVCOL_PRI_BT2020 ||
                       frame->color_primaries == AVCOL_PRI_UNSPECIFIED);
        v3_ensure_hdr_luts(); /* 并行前先建好查表，避免多线程同时惰性初始化 */
        v3_ensure_tm_lut(v3_frame_peak_rw(s, frame, c.is_pq)); /* 出表按内容峰值重建 */
        v3_pool_run(&s->pool, sh, sw, v3_post_band, &c);
        return dw * dh * 4;
    }
    if (fmt == 2)
    {
        c.mode = 1;
        v3_pool_run(&s->pool, sh, sw, v3_post_band, &c);
        return dw * dh * 8;
    }
    c.mode = 2;
    v3_pool_run(&s->pool, sh, sw, v3_post_band, &c);
    return dw * dh * sws_bpp;
}

/* 帧的呈现时间（秒，相对流起点）；无时间戳时返回 0。 */
static double v3_frame_pts(int64_t ts, double time_base, int64_t start_ts)
{
    if (ts != AV_NOPTS_VALUE && time_base > 0)
    {
        double t = (double)(ts - start_ts) * time_base;
        if (t >= 0)
        {
            return t;
        }
    }
    return 0;
}

/* ---------------------------- 视频 ---------------------------- */

/* 送下一个视频包。返回 false 只表示解码器报错；EOF（无包可送/已 flush）返回 true，
 * 让调用方再 receive 一次把残帧取干净。 */
static bool v3_send_next_video(Vii3Session *s)
{
    if (s->pending_v == NULL)
    {
        s->pending_v = v3_pkt_next(s, s->video_index);
        if (s->pending_v == NULL)
        {
            if (!s->v_flushed)
            {
                s->v_flushed = 1;
                avcodec_send_packet(s->vdec, NULL);
            }
            return true;
        }
    }

    int r = avcodec_send_packet(s->vdec, s->pending_v);
    if (r == AVERROR(EAGAIN))
    {
        return true; /* 解码器暂时吃不下，保留 pending 下轮再送 */
    }
    av_packet_free(&s->pending_v);
    if (r < 0 && r != AVERROR_EOF)
    {
        V3_DBG("video send_packet -> %d\n", r);
        return false;
    }
    return true;
}

V3_API int vii3_video_next(Vii3Session *s, uint8_t *dst, int dst_capacity, double *out_pts)
{
    return vii3_video_next_ex(s, dst, dst_capacity, 0, out_pts);
}

/*
 * 解出下一帧到指定像素格式：
 *   0 = BGRA8：SDR 直出；HDR(PQ/HLG) 经 CPU 色调映射成 sRGB（见 v3_hdr.h）。
 *   1 = RGBA64 直通：源色彩空间，12bit 无损。
 *   2 = RGBA_F16 直通：同 1，half-float（约 11bit 有效精度，12bit 丢约 1bit；要无损用 1）。
 *   3 = BGRA8 直出：不做色调映射（等价旧版行为，最快但偏灰）。
 *
 * 缓冲需 >= disp_w * disp_h * (0/3 为 4，其余为 8)。返回字节数；0 = EOF；<0 = 错误。
 *
 * 不要在这里加「提前解码 / 缓存后面若干帧」：实测过，缓存只能削抖、提不了吞吐
 * （上限由硬件决定），还要多占内存并把解码线程与后处理池挤成超订。
 */
V3_API int vii3_video_next_ex(Vii3Session *s, uint8_t *dst, int dst_capacity, int fmt,
                              double *out_pts)
{
    if (s == NULL || s->vdec == NULL)
    {
        return 0; /* 没有视频流 */
    }
    int bpp = (fmt == 0 || fmt == 3) ? 4 : 8;
    if (dst == NULL || out_pts == NULL || dst_capacity < s->disp_w * s->disp_h * bpp)
    {
        return V3_ERR_BUFFER_TOO_SMALL;
    }

    for (;;)
    {
        if (!s->has_prepared_frame)
        {
            int r = avcodec_receive_frame(s->vdec, s->vframe);
            if (r == 0)
            {
                /* s->vframe 已持有解码帧，落下继续处理 */
            }
            else if (r != AVERROR(EAGAIN))
            {
                return 0; /* 已排空 / 致命错误 */
            }
            else if (!v3_send_next_video(s))
            {
                return -1;
            }
            else
            {
                continue;
            }
        }
        else
        {
            s->has_prepared_frame = 0; /* s->vframe 是打开时备好的首帧 */
        }

        int written = v3_emit_video(s, s->vframe, dst, fmt);
        int64_t ts = s->vframe->best_effort_timestamp != AV_NOPTS_VALUE
                         ? s->vframe->best_effort_timestamp
                         : s->vframe->pts;
        *out_pts = v3_frame_pts(ts, s->v_time_base, s->v_start_ts);
        av_frame_unref(s->vframe);
        return written < 0 ? -1 : written;
    }
}

static void v3_apply_out_size(Vii3Session *s); /* 定义见文件后部「生命周期」之前 */

/* 设置输出目标尺寸（见头文件处的说明）。 */
V3_API int vii3_set_target_size(Vii3Session *s, int w, int h)
{
    if (s == NULL || s->vdec == NULL)
    {
        return -1;
    }
    if (w < 1 || h < 1)
    {
        w = 0;
        h = 0;
    }
    if (w == s->tgt_w && h == s->tgt_h)
    {
        return 0; /* 没变：什么都不用重建 */
    }
    s->tgt_w = w;
    s->tgt_h = h;
    v3_apply_out_size(s);

    /* 输出尺寸变了 → sws 目标尺寸与中间缓冲都要换（下次取帧时惰性重建）。 */
    if (s->sws != NULL)
    {
        sws_freeContext(s->sws);
        s->sws = NULL;
    }
    av_free(s->scratch);
    s->scratch = NULL;
    s->scratch_capacity = 0;
    return 0;
}

V3_API void vii3_get_output_size(const Vii3Session *s, int *w, int *h)
{
    if (w != NULL)
    {
        *w = (s != NULL) ? s->disp_w : 0;
    }
    if (h != NULL)
    {
        *h = (s != NULL) ? s->disp_h : 0;
    }
}

/* ---------------------------- 音频 ---------------------------- */

static int v3_audio_reinit_swr(Vii3Session *s, const AVFrame *f)
{
    if (s->swr != NULL && f->sample_rate == s->in_sr &&
        f->ch_layout.nb_channels == s->in_ch && (enum AVSampleFormat)f->format == s->in_fmt)
    {
        return 0;
    }

    if (s->swr != NULL)
    {
        swr_free(&s->swr);
    }

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, s->out_ch);

    AVChannelLayout in_layout;
    av_channel_layout_copy(&in_layout, &f->ch_layout);

    int r = swr_alloc_set_opts2(&s->swr, &out_layout, s->out_fmt, s->out_sr,
                                &in_layout, (enum AVSampleFormat)f->format, f->sample_rate,
                                0, NULL);
    av_channel_layout_uninit(&in_layout);
    if (r < 0 || s->swr == NULL)
    {
        V3_DBG("swr_alloc_set_opts2 -> %d\n", r);
        return -1;
    }

    r = swr_init(s->swr);
    if (r < 0)
    {
        swr_free(&s->swr);
        return -1;
    }

    s->in_sr = f->sample_rate;
    s->in_ch = f->ch_layout.nb_channels;
    s->in_fmt = (enum AVSampleFormat)f->format;
    return 0;
}

/* 送下一个音频包。语义同 v3_send_next_video。 */
static bool v3_send_next_audio(Vii3Session *s)
{
    if (s->pending_a == NULL)
    {
        s->pending_a = v3_pkt_next(s, s->audio_index);
        if (s->pending_a == NULL)
        {
            if (!s->a_flushed)
            {
                s->a_flushed = 1;
                avcodec_send_packet(s->adec, NULL);
            }
            return true;
        }
    }

    int r = avcodec_send_packet(s->adec, s->pending_a);
    if (r == AVERROR(EAGAIN))
    {
        return true;
    }
    av_packet_free(&s->pending_a);
    if (r < 0 && r != AVERROR_EOF)
    {
        V3_DBG("audio send_packet -> %d\n", r);
        return false;
    }
    return true;
}

V3_API int vii3_audio_next(Vii3Session *s, uint8_t *dst, int dst_capacity, double *out_pts)
{
    if (s == NULL || s->adec == NULL || dst == NULL || out_pts == NULL || dst_capacity <= 0)
    {
        return V3_ERR_BUFFER_TOO_SMALL;
    }

    for (;;)
    {
        int r = avcodec_receive_frame(s->adec, s->aframe);
        if (r == 0)
        {
            if (v3_audio_reinit_swr(s, s->aframe) < 0)
            {
                av_frame_unref(s->aframe);
                return -1;
            }

            int out_max = (int)av_rescale_rnd(
                swr_get_delay(s->swr, s->aframe->sample_rate) + s->aframe->nb_samples,
                s->out_sr, s->aframe->sample_rate, AV_ROUND_UP);
            if (out_max <= 0)
            {
                av_frame_unref(s->aframe);
                continue;
            }

            int need = out_max * s->out_ch * s->out_bytes_per_sample;
            if (need > dst_capacity)
            {
                av_frame_unref(s->aframe);
                return V3_ERR_BUFFER_TOO_SMALL;
            }

            uint8_t *out = dst;
            int got = swr_convert(s->swr, &out, out_max,
                                  (const uint8_t **)s->aframe->extended_data,
                                  s->aframe->nb_samples);
            if (got < 0)
            {
                av_frame_unref(s->aframe);
                return -1;
            }

            *out_pts = v3_frame_pts(s->aframe->best_effort_timestamp, s->a_time_base,
                                    s->a_start_ts);
            av_frame_unref(s->aframe);

            if (got == 0)
            {
                continue; /* swr 还在补延迟，继续取下一帧 */
            }
            return got * s->out_ch * s->out_bytes_per_sample;
        }

        if (r != AVERROR(EAGAIN))
        {
            return 0;
        }

        if (!v3_send_next_audio(s))
        {
            return -1;
        }
    }
}

/* ============================ 音频输出（WASAPI 共享模式） ============================ */

/*
 * 线程模型
 *
 * session 内部共用同一个 AVFormatContext，是单线程设计，所以设备线程绝不能直接调
 * 解码接口。这里的分工是：
 *
 *   应用线程：vii3_audio_next() 解出 PCM -> vii3_audio_push() 写进环形缓冲
 *   渲染线程：只从环形缓冲取数据喂 WASAPI（不够就补静音）
 *
 * 主时钟 vii3_audio_played() 只累计「真正交给设备的媒体帧」，欠载补进去的静音不计入，
 * 所以掉帧时媒体时间不会虚增 —— 正是 A/V 同步需要的语义。
 *
 * 所有 MMDevice / WASAPI 调用都只发生在渲染线程里（COM 按线程初始化），不必在调用方
 * 线程上强行 CoInitializeEx(COINIT_MULTITHREADED)，也就不会撞上调用方的 STA。
 */

#define V3_AUDIO_REQUEST_HNS (50 * 10000) /* 50ms（100ns 单位） */

/* KSDATAFORMAT_SUBTYPE_*，避免为了两个 GUID 引入 ksmedia.h */
static const GUID V3_SUBTYPE_PCM =
    {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID V3_SUBTYPE_IEEE_FLOAT =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

/* 从混音格式里解出采样率/声道/采样格式；不支持的格式返回 -1。 */
static int v3_audio_format_from_mix(const WAVEFORMATEX *mix, int *out_sr, int *out_ch, int *out_fmt)
{
    WORD tag = mix->wFormatTag;
    WORD bits = mix->wBitsPerSample;
    int fmt = -1;

    if (tag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
    {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)mix;
        if (memcmp(&ext->SubFormat, &V3_SUBTYPE_IEEE_FLOAT, sizeof(GUID)) == 0 && bits == 32)
        {
            fmt = V3_AUDIO_F32;
        }
        else if (memcmp(&ext->SubFormat, &V3_SUBTYPE_PCM, sizeof(GUID)) == 0 && bits == 16)
        {
            fmt = V3_AUDIO_S16;
        }
    }
    else if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32)
    {
        fmt = V3_AUDIO_F32;
    }
    else if (tag == WAVE_FORMAT_PCM && bits == 16)
    {
        fmt = V3_AUDIO_S16;
    }

    if (fmt < 0 || mix->nSamplesPerSec == 0 || mix->nChannels == 0)
    {
        return -1;
    }

    *out_sr = (int)mix->nSamplesPerSec;
    *out_ch = (int)mix->nChannels;
    *out_fmt = fmt;
    return 0;
}

/* 独立的短命线程里查询设备格式，用完即退 —— 避免在调用方线程上做 COM 初始化。 */
static DWORD WINAPI v3_audio_query_proc(LPVOID param)
{
    Vii3AudioDeviceFormat *out = (Vii3AudioDeviceFormat *)param;

    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
    {
        return 0;
    }

    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    IAudioClient *client = NULL;
    WAVEFORMATEX *mix = NULL;

    if (SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                   &IID_IMMDeviceEnumerator, (void **)&enumerator))
        && SUCCEEDED(enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender, eMultimedia, &device))
        && SUCCEEDED(device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client))
        && SUCCEEDED(client->lpVtbl->GetMixFormat(client, &mix)))
    {
        int sr, ch, fmt;
        if (v3_audio_format_from_mix(mix, &sr, &ch, &fmt) == 0)
        {
            out->sample_rate = sr;
            out->channels = ch;
            out->sample_format = fmt;
            out->valid = 1;
        }
    }

    if (mix != NULL)
    {
        CoTaskMemFree(mix);
    }
    if (client != NULL)
    {
        client->lpVtbl->Release(client);
    }
    if (device != NULL)
    {
        device->lpVtbl->Release(device);
    }
    if (enumerator != NULL)
    {
        enumerator->lpVtbl->Release(enumerator);
    }
    CoUninitialize();
    return 0;
}

V3_API int vii3_audio_device_format(Vii3AudioDeviceFormat *out)
{
    if (out == NULL)
    {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    HANDLE thread = CreateThread(NULL, 0, v3_audio_query_proc, out, 0, NULL);
    if (thread == NULL)
    {
        return -1;
    }
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);

    return out->valid ? 0 : -1;
}

/* 只写整帧的数据；返回写入字节数。调用方须持锁。 */
static int v3_ring_write(V3AudioOut *ao, const uint8_t *src, int len)
{
    const int fb = ao->frame_bytes;
    len -= len % fb;
    if (len <= 0)
    {
        return 0;
    }

    int space = ao->ring_capacity - ao->ring_available;
    space -= space % fb;
    if (len > space)
    {
        len = space;
    }
    if (len <= 0)
    {
        return 0;
    }

    int first = ao->ring_capacity - ao->ring_write;
    if (first > len)
    {
        first = len;
    }
    memcpy(ao->ring + ao->ring_write, src, (size_t)first);
    if (len > first)
    {
        memcpy(ao->ring, src + first, (size_t)(len - first));
    }

    ao->ring_write = (ao->ring_write + len) % ao->ring_capacity;
    ao->ring_available += len;
    return len;
}

/* 返回实际读到的字节数（整帧对齐）。调用方须持锁。 */
static int v3_ring_read(V3AudioOut *ao, uint8_t *dst, int len)
{
    const int fb = ao->frame_bytes;
    len -= len % fb;
    if (len > ao->ring_available)
    {
        len = ao->ring_available;
    }
    if (len <= 0)
    {
        return 0;
    }

    int first = ao->ring_capacity - ao->ring_read;
    if (first > len)
    {
        first = len;
    }
    memcpy(dst, ao->ring + ao->ring_read, (size_t)first);
    if (len > first)
    {
        memcpy(dst + first, ao->ring, (size_t)(len - first));
    }

    ao->ring_read = (ao->ring_read + len) % ao->ring_capacity;
    ao->ring_available -= len;
    return len;
}

/* 渲染线程：建设备、跑事件驱动的共享模式渲染循环、退出时释放全部 COM 对象。 */
static DWORD WINAPI v3_audio_render_proc(LPVOID param)
{
    Vii3Session *s = (Vii3Session *)param;
    V3AudioOut *ao = &s->audio_out;

    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    IAudioClient *client = NULL;
    IAudioRenderClient *render = NULL;
    ISimpleAudioVolume *volume = NULL;
    WAVEFORMATEX *mix = NULL;
    int com_ok = 0;
    int ready = 0;

    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
    {
        goto done;
    }
    com_ok = 1;

    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void **)&enumerator)))
    {
        goto done;
    }
    if (FAILED(enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender, eMultimedia, &device)))
    {
        goto done;
    }
    if (FAILED(device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client)))
    {
        goto done;
    }
    if (FAILED(client->lpVtbl->GetMixFormat(client, &mix)))
    {
        goto done;
    }

    /* 设备混音格式必须与会话的音频输出格式一致：应用侧是按设备格式打开媒体的。 */
    {
        int sr, ch, fmt;
        enum AVSampleFormat want =
            (s->out_fmt == AV_SAMPLE_FMT_S16) ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_FLT;

        if (v3_audio_format_from_mix(mix, &sr, &ch, &fmt) != 0
            || sr != ao->sample_rate
            || ch != ao->channels
            || (fmt == V3_AUDIO_F32 ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16) != want)
        {
            V3_DBG("audio: device mix format mismatch\n");
            goto done;
        }
    }

    if (FAILED(client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                          AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                          V3_AUDIO_REQUEST_HNS, 0, mix, NULL)))
    {
        goto done;
    }
    if (FAILED(client->lpVtbl->SetEventHandle(client, ao->wake_event)))
    {
        goto done;
    }
    if (FAILED(client->lpVtbl->GetBufferSize(client, &ao->device_buffer_frames)))
    {
        goto done;
    }
    if (FAILED(client->lpVtbl->GetService(client, &IID_IAudioRenderClient, (void **)&render)))
    {
        goto done;
    }
    /* 静音走这一路；拿不到也不影响播放 */
    client->lpVtbl->GetService(client, &IID_ISimpleAudioVolume, (void **)&volume);

    if (FAILED(client->lpVtbl->Start(client)))
    {
        goto done;
    }

    ready = 1;
    InterlockedExchange(&ao->setup_result, 0);
    SetEvent(ao->ready_event);

    /* 启动时立即断言一次静音状态（不能等「有数据」的唤醒才下发） */
    if (volume != NULL)
    {
        /* 只读**一次**（原子快照）。这段与调用方的 vii3_audio_set_mute 并发：
         * vii3_audio_start 返回后调用方马上就会下发状态，若在"读给 SetMute"和
         * "读给自己记"之间状态翻转，就会**按旧值下发、却把新值记为已应用** ——
         * 之后渲染循环认为无需下发，就再也不静音（现象：图标是静音、声音照出）。 */
        const LONG mute_want = InterlockedCompareExchange(&ao->mute, 0, 0);
        volume->lpVtbl->SetMute(volume, mute_want ? TRUE : FALSE, NULL);
        ao->mute_applied = mute_want;
    }

    while (!ao->stop)
    {
        /* 每轮（含超时）都先同步一次静音：这段原来在「等唤醒」之后，而唤醒要靠调用方 push
         * 数据 —— 暂停/停止喂数据时静音变更就一直不下发、stop 时被丢掉，而 mute_applied
         * 仍记旧值，导致下一个会话以为无需下发，WASAPI 的进程级会话卡在静音上。 */
        /* 同样只读一次：条件、下发、记账必须用同一个快照，否则三读之间翻转会记错账
         * （记成"已应用新值"而实际下发的是旧值），后面就再也不补下发了。 */
        const LONG mute_want = InterlockedCompareExchange(&ao->mute, 0, 0);
        if (volume != NULL && mute_want != ao->mute_applied)
        {
            volume->lpVtbl->SetMute(volume, mute_want ? TRUE : FALSE, NULL);
            ao->mute_applied = mute_want;
        }

        if (WaitForSingleObject(ao->wake_event, 200) != WAIT_OBJECT_0)
        {
            continue;
        }
        if (ao->stop)
        {
            break;
        }

        UINT32 padding = 0;
        if (FAILED(client->lpVtbl->GetCurrentPadding(client, &padding)))
        {
            break;
        }

        UINT32 available = ao->device_buffer_frames - padding;
        if (available == 0)
        {
            continue;
        }

        BYTE *data = NULL;
        if (FAILED(render->lpVtbl->GetBuffer(render, available, &data)))
        {
            break;
        }

        const int want = (int)available * ao->frame_bytes;
        EnterCriticalSection(&ao->lock);
        const int got = v3_ring_read(ao, data, want);
        ao->frames_rendered += got / ao->frame_bytes;
        LeaveCriticalSection(&ao->lock);

        if (got < want)
        {
            memset(data + got, 0, (size_t)(want - got)); /* 欠载补静音 */
        }

        render->lpVtbl->ReleaseBuffer(render, available, 0);
    }

    client->lpVtbl->Stop(client);

done:
    if (!ready)
    {
        InterlockedExchange(&ao->setup_result, -1);
        SetEvent(ao->ready_event);
    }
    if (mix != NULL)
    {
        CoTaskMemFree(mix);
    }
    if (volume != NULL)
    {
        volume->lpVtbl->Release(volume);
    }
    if (render != NULL)
    {
        render->lpVtbl->Release(render);
    }
    if (client != NULL)
    {
        client->lpVtbl->Release(client);
    }
    if (device != NULL)
    {
        device->lpVtbl->Release(device);
    }
    if (enumerator != NULL)
    {
        enumerator->lpVtbl->Release(enumerator);
    }
    if (com_ok)
    {
        CoUninitialize();
    }
    return 0;
}

V3_API int vii3_audio_start(Vii3Session *s)
{
    if (s == NULL)
    {
        return -1;
    }

    V3AudioOut *ao = &s->audio_out;
    if (ao->started)
    {
        return 0;
    }

    ao->sample_rate = s->out_sr;
    ao->channels = s->out_ch;
    ao->bytes_per_sample = s->out_bytes_per_sample;
    ao->frame_bytes = ao->channels * ao->bytes_per_sample;
    if (ao->sample_rate <= 0 || ao->frame_bytes <= 0)
    {
        return -1;
    }

    int ring = (int)((long long)ao->sample_rate * ao->frame_bytes * V3_AUDIO_RING_MS / 1000);
    ring -= ring % ao->frame_bytes;
    if (ring < ao->frame_bytes * 4)
    {
        ring = ao->frame_bytes * 4;
    }

    ao->ring = (uint8_t *)av_malloc((size_t)ring);
    if (ao->ring == NULL)
    {
        return -1;
    }
    ao->ring_capacity = ring;
    ao->ring_read = ao->ring_write = ao->ring_available = 0;
    ao->frames_rendered = 0;
    /* mute_applied 用 -1 表示「从未下发过」：渲染线程只在 mute != mute_applied 时才下发，
     * 若两者都置 0，新会话永远不会声明一次「取消静音」。而 WASAPI 音频会话默认按进程共享，
     * 上一路留下的静音会被继承（表现为「打开声音后切下一个视频无声」）。
     * 置 -1 保证首轮必定下发一次 SetMute(FALSE)。 */
    ao->mute = 0;
    ao->mute_applied = -1;
    ao->device_buffer_frames = 0;
    ao->setup_result = -2;

    InitializeCriticalSection(&ao->lock);
    ao->lock_ready = 1;

    ao->wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ao->ready_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (ao->wake_event == NULL || ao->ready_event == NULL)
    {
        vii3_audio_stop(s);
        return -1;
    }

    ao->thread = CreateThread(NULL, 0, v3_audio_render_proc, s, 0, NULL);
    if (ao->thread == NULL)
    {
        vii3_audio_stop(s);
        return -1;
    }

    /* 等设备初始化结果（最多 3 秒） */
    if (WaitForSingleObject(ao->ready_event, 3000) != WAIT_OBJECT_0 || ao->setup_result != 0)
    {
        V3_DBG("audio: device init failed (%ld)\n", ao->setup_result);
        vii3_audio_stop(s);
        return -1;
    }

    InterlockedExchange(&ao->started, 1);
    return 0;
}

V3_API void vii3_audio_stop(Vii3Session *s)
{
    if (s == NULL)
    {
        return;
    }

    V3AudioOut *ao = &s->audio_out;

    if (ao->thread != NULL)
    {
        InterlockedExchange(&ao->stop, 1);
        if (ao->wake_event != NULL)
        {
            SetEvent(ao->wake_event);
        }
        WaitForSingleObject(ao->thread, 5000);
        CloseHandle(ao->thread);
        ao->thread = NULL;
    }

    if (ao->wake_event != NULL)
    {
        CloseHandle(ao->wake_event);
        ao->wake_event = NULL;
    }
    if (ao->ready_event != NULL)
    {
        CloseHandle(ao->ready_event);
        ao->ready_event = NULL;
    }
    if (ao->ring != NULL)
    {
        av_free(ao->ring);
        ao->ring = NULL;
    }
    if (ao->lock_ready)
    {
        DeleteCriticalSection(&ao->lock);
        ao->lock_ready = 0;
    }

    ao->started = 0;
    ao->stop = 0;
    ao->setup_result = -2;
    ao->device_buffer_frames = 0;
    ao->ring_capacity = 0;
    ao->ring_read = ao->ring_write = ao->ring_available = 0;
    ao->frames_rendered = 0;
}

V3_API int vii3_audio_push(Vii3Session *s, const uint8_t *pcm, int len)
{
    if (s == NULL || pcm == NULL || len <= 0)
    {
        return -1;
    }

    V3AudioOut *ao = &s->audio_out;
    if (!ao->started || !ao->lock_ready)
    {
        return -1;
    }

    EnterCriticalSection(&ao->lock);
    const int written = v3_ring_write(ao, pcm, len);
    LeaveCriticalSection(&ao->lock);
    return written;
}

V3_API int vii3_audio_buffered(Vii3Session *s)
{
    if (s == NULL)
    {
        return 0;
    }

    V3AudioOut *ao = &s->audio_out;
    if (!ao->lock_ready)
    {
        return 0;
    }

    EnterCriticalSection(&ao->lock);
    const int n = ao->ring_available;
    LeaveCriticalSection(&ao->lock);
    return n;
}

V3_API double vii3_audio_played(Vii3Session *s)
{
    if (s == NULL)
    {
        return 0;
    }

    V3AudioOut *ao = &s->audio_out;
    if (!ao->lock_ready || ao->sample_rate <= 0)
    {
        return 0;
    }

    EnterCriticalSection(&ao->lock);
    const double t = (double)ao->frames_rendered / (double)ao->sample_rate;
    LeaveCriticalSection(&ao->lock);
    return t;
}

V3_API void vii3_audio_set_mute(Vii3Session *s, int mute)
{
    if (s == NULL)
    {
        return;
    }
    /* 只置标志，真正的 SetMute 由渲染线程执行（COM 对象只属于那个线程）；顺手唤醒它，
     * 因为渲染线程平时在等「有数据」的唤醒，暂停时没有数据就不会执行。 */
    InterlockedExchange(&s->audio_out.mute, mute ? 1 : 0);
    if (s->audio_out.wake_event != NULL)
    {
        SetEvent(s->audio_out.wake_event);
    }
}

/* 按目标尺寸算出 disp_w/disp_h（= 输出位图尺寸，旋转后）与 out_w/out_h（缩放后、旋转前）。
 *
 * 坐标系：tgt_w/tgt_h 与 disp_w/disp_h 都是**位图（旋转后）**尺寸 —— 调用方拿到的
 * info.width/height 也是这个坐标系，直接用，不必自己交换宽高。
 *
 * 两条上限（与旧版软件渲染路径的 UpdateBitmapSize 一致）：
 *   1. 目标尺寸 >= 视频原始尺寸时按原始分辨率输出（放大不传进去）；
 *   2. 位图长边不超过 4096。
 * 目标尺寸变化后必须重建 sws 上下文与 scratch（调用方负责，见 vii3_set_target_size）。 */
static void v3_apply_out_size(Vii3Session *s)
{
    const int w = s->vdec->width;
    const int h = s->vdec->height;
    const int rot90 = (s->rotation == 90 || s->rotation == 270);
    const int full_dw = rot90 ? h : w; /* 视频的显示尺寸（旋转后） */
    const int full_dh = rot90 ? w : h;

    int dw = full_dw;
    int dh = full_dh;
    if (s->tgt_w > 0 && s->tgt_h > 0 && !(s->tgt_w >= full_dw && s->tgt_h >= full_dh))
    {
        dw = s->tgt_w;
        dh = s->tgt_h;
    }
    /* 位图上限 4096（旧版行为）。这里与旧版不同的一点：按同一比例缩，避免长边超限
     * 时把画面拉变形（旧版两个维度各自 clamp）。只影响 >4096 的视频。 */
    const int lim = 4096;
    if (dw > lim || dh > lim)
    {
        const double k = (double)lim / (double)(dw > dh ? dw : dh);
        dw = (int)(dw * k);
        dh = (int)(dh * k);
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    s->disp_w = dw;
    s->disp_h = dh;
    s->out_w = rot90 ? dh : dw; /* 反推旋转前的缩放目标 */
    s->out_h = rot90 ? dw : dh;
}

/* ---------------------------- 生命周期 ---------------------------- */

V3_API const char *vii3_version(void)
{
    return av_version_info();
}

V3_API Vii3Session *vii3_open(void *opaque, V3ReadCb read_cb, V3SeekCb seek_cb,
                             const Vii3AudioOut *audio_out, int external_rotation_cw,
                             Vii3MediaInfo *out_info)
{
    if (read_cb == NULL || seek_cb == NULL || out_info == NULL)
    {
        return NULL;
    }

    av_log_set_callback(v3_log_silence);
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = (int)sizeof(Vii3MediaInfo);

    Vii3Session *s = (Vii3Session *)av_mallocz(sizeof(Vii3Session));
    if (s == NULL)
    {
        return NULL;
    }
    s->video_index = -1;
    s->audio_index = -1;
    s->v_start_ts = AV_NOPTS_VALUE;
    s->a_start_ts = AV_NOPTS_VALUE;
    s->user_opaque = opaque;
    s->read_cb = read_cb;
    s->seek_cb = seek_cb;
    int r;

    uint8_t *avio_buffer = (uint8_t *)av_malloc(V3_AVIO_BUFFER_SIZE);
    if (avio_buffer == NULL)
    {
        goto fail;
    }

    /* 缓冲区所有权交给 AVIOContext；avio_context_free 会释放当前那一块。 */
    s->avio = avio_alloc_context(avio_buffer, V3_AVIO_BUFFER_SIZE, 0, s,
                                 v3_read_trampoline, NULL, v3_seek_trampoline);
    if (s->avio == NULL)
    {
        av_free(avio_buffer);
        goto fail;
    }

    s->fmt = avformat_alloc_context();
    if (s->fmt == NULL)
    {
        goto fail;
    }
    s->fmt->pb = s->avio;

    r = avformat_open_input(&s->fmt, NULL, NULL, NULL);
    if (r < 0)
    {
        V3_DBG("avformat_open_input -> %d\n", r);
        s->fmt = NULL; /* 失败时由 avformat_open_input 释放 */
        goto fail;
    }

    r = avformat_find_stream_info(s->fmt, NULL);
    if (r < 0)
    {
        V3_DBG("avformat_find_stream_info -> %d\n", r);
        goto fail;
    }

    /* ---- 视频 ---- */
    /*
     * 规范允许一条可选的「更高分辨率次视频轨」（Motion Photo Format 1.0：主轨必须存在、
     * 次轨可选且 track index 更大）。这里交给 av_find_best_stream 的启发式来选择
     * （disposition → 探测到的帧数 → 码率 → 序号），它并不比较分辨率，所以带次轨的文件
     * 选到哪条是不确定的。目前手上没有带次轨的样本；真遇到时要显式选轨 —— 要么按规范选
     * 主轨（track index 更小），要么按分辨率选更高那条（次轨本来就是给静态图替换用的）。
     */
    const AVCodec *vcodec = NULL;
    int vindex = av_find_best_stream(s->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 0);
    if (vindex >= 0 && vcodec != NULL)
    {
        AVStream *stream = s->fmt->streams[vindex];
        s->vdec = avcodec_alloc_context3(vcodec);
        if (s->vdec == NULL)
        {
            goto fail;
        }
        if (avcodec_parameters_to_context(s->vdec, stream->codecpar) < 0)
        {
            goto fail;
        }
        /*
         * 多线程软解：帧级并行，做法与 mpv 的 mp_set_avcodec_threads 一致 ——
         * 0 会被 libavcodec 当默认单线程，这里显式按逻辑核数设置，核数 >1 时 +1
         * 便于负载均衡，并封顶 16（部分版本超过 16 线程会异常/告警）。
         * 必须在 avcodec_open2 之前设置；帧级并行会带来 thread_count 帧的回帧延迟，
         * 打开时的预解首帧已按此做了「收帧→喂包」循环（见 vii3_open）。
         */
        {
            /* 解码线程数按**物理核**（不是逻辑核）。解码是计算密集，超线程几乎无收益；
             * 而这一段还要给后处理池（逻辑核、封顶 8）和 sws（物理核）留位置 ——
             * 按逻辑核开（本机 = 9）会和它们一起把 8 个逻辑核挤爆（超订），实测更慢。
             * 这条与后处理池相反是有实测依据的：把**池**压到物理核会让 fmt0 慢 ~2.5ms。
             * V3_DEC_THREADS=n 可覆盖，便于本机 A/B。 */
            /* 按**逻辑核**（+1 便于负载均衡）。曾经改成物理核，实测**更慢**
             * （本样本：逻辑核 8+1 → 19.4ms；物理核 4+1 → 21~25ms）——
             * 因为帧级并行本来就能让解码自己跑在前面，压线程只会降低它的吞吐。
             * 注意这与 sws（按物理核）相反：sws 那边 MT 超订的代价更大。 */
            int threads = av_cpu_count();
            const char *dt = getenv("V3_DEC_THREADS");
            if (dt != NULL && dt[0] >= '0' && dt[0] <= '9')
            {
                int v = atoi(dt);
                if (v >= 1)
                {
                    threads = v;
                }
            }
            if (threads < 1)
            {
                threads = 1;
            }
            else if (threads > 1)
            {
                threads += 1; /* +1 便于负载均衡（同 mpv 的做法） */
            }
            s->vdec->thread_count = threads > 16 ? 16 : threads;
            s->v_threads = s->vdec->thread_count;
            /* 后处理池与解码线程解耦：后处理是内存带宽受限，堆到 16 路无线性收益，还会与
             * 上层「并行解多张图」相乘造成线程超订，故封顶 8。 */
            {
                /* 后处理池用**逻辑核**（av_cpu_count）：色调映射是计算密集（LUT + 色域矩阵），
                 * 超线程有效 —— 实测把它压到物理核（4）会让 fmt0 慢 ~2.5ms。与 sws 不同。 */
                int pool_threads = av_cpu_count();
                if (pool_threads < 1)
                {
                    pool_threads = 1;
                }
                if (pool_threads > 8)
                {
                    pool_threads = 8;
                }
                v3_pool_create(&s->pool, pool_threads);
            }
        }
        if (avcodec_open2(s->vdec, vcodec, NULL) < 0)
        {
            goto fail;
        }

        s->vframe = av_frame_alloc();
        if (s->vframe == NULL)
        {
            goto fail;
        }
        s->sws_dst_frame = av_frame_alloc();
        if (s->sws_dst_frame == NULL)
        {
            goto fail;
        }

        s->video_index = vindex;
        s->rotation = v3_read_display_rotation(stream);
        s->v_time_base = av_q2d(stream->time_base);
        s->v_start_ts = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;

        /*
         * 扣掉「外部已经解决的顺时针角度」。
         *
         * 动态照片的静态图方向由 EXIF 决定，而画布是用一个共享变换（RotateTransform）
         * 把静态图转正的 —— 视频帧走同一个控件，所以只能补「容器旋转 − 静态图已抵消」
         * 的差值，否则会被画布重复旋转一次。
         *
         * 实测两个样本正好代表两种情形：
         *   三星 A34：EXIF=6 需 +90°CW，视频 display matrix 也是 +90°CW → 差值 0，
         *            视频保持原始编码方向，和静态图一起被画布转正；
         *   Pixel 9：EXIF=1 无需转，但视频需要 +90°CW → 差值 90°，由 native 转掉。
         */
        {
            int container_cw = (360 - s->rotation) % 360;
            int external_cw = ((external_rotation_cw % 360) + 360) % 360;
            int bake_cw = ((container_cw - external_cw) % 360 + 360) % 360;
            s->rotation = (360 - bake_cw) % 360;
        }

        /* 输出尺寸：out_w/out_h 是「缩放后、旋转前」，disp_w/disp_h 是旋转后（= 位图尺寸）。
         * 打开时还没设目标尺寸，所以这里就是完整分辨率；缩放由 vii3_set_target_size 控制。 */
        v3_apply_out_size(s);

        out_info->has_video = 1;
        out_info->width = s->disp_w;
        out_info->height = s->disp_h;
        out_info->rotation = s->rotation;

        /* 对外只报编解码器名，不暴露具体实现：AV1 实际走的是 libdav1d
         * （FFmpeg 自带的 av1 解码器只支持硬解），报成 "libdav1d" 会让上层
         * 耦合到实现选择 —— 换实现后名字就变了。规范只认 h264 / hevc / av1。 */
        const char *codec_name = vcodec->name;
        if (strcmp(codec_name, "libdav1d") == 0)
        {
            codec_name = "av1";
        }
        strncpy(out_info->video_codec, codec_name, sizeof(out_info->video_codec) - 1);

        AVRational rate = av_guess_frame_rate(s->fmt, stream, NULL);
        out_info->fps = (rate.den > 0 && rate.num > 0) ? (double)rate.num / rate.den : 30.0;

        if (stream->duration > 0 && s->v_time_base > 0)
        {
            out_info->duration_sec = stream->duration * s->v_time_base;
        }
        else if (s->fmt->duration > 0)
        {
            out_info->duration_sec = (double)s->fmt->duration / AV_TIME_BASE;
        }

        /* 预解首帧：取 HDR side data（MDCV/CLLI）回填，并把首帧备好给首个
         * vii3_video_next（避免再解一次）。解不出也不影响会话有效性。
         * 注意：帧级多线程下首帧要等若干包才吐出来，所以这里「收帧→喂包」循环到
         * 拿到一帧或到流尾，保证 HDR side data 不因线程延迟而丢失。 */
        for (;;)
        {
            int rr = avcodec_receive_frame(s->vdec, s->vframe);
            if (rr == 0)
            {
                v3_fill_hdr_side_data(out_info, s->vframe);
                s->has_prepared_frame = 1;
                break;
            }
            if (rr != AVERROR(EAGAIN))
            {
                break; /* EOF 或错误：没有可用首帧 */
            }

            AVPacket *p = s->pending_v != NULL ? s->pending_v
                                               : v3_pkt_next(s, s->video_index);
            if (p == NULL)
            {
                if (!s->v_flushed)
                {
                    s->v_flushed = 1;
                    avcodec_send_packet(s->vdec, NULL); /* 冲刷，逼出延迟的帧 */
                    continue;
                }
                break;
            }
            if (p == s->pending_v)
            {
                s->pending_v = NULL;
            }
            int sr = avcodec_send_packet(s->vdec, p);
            if (sr == AVERROR(EAGAIN))
            {
                s->pending_v = p; /* 解码器暂时吃不下，下一轮先收帧 */
            }
            else
            {
                av_packet_free(&p);
                if (sr < 0 && sr != AVERROR_EOF)
                {
                    break;
                }
            }
        }

        /* 位深 / CICP / is_hdr：放在预解之后，让首帧的真实像素格式/色彩参数参与判定。 */
        v3_fill_info_basic(out_info, s);
    }

    /* ---- 音频 ---- */
    const AVCodec *acodec = NULL;
    int aindex = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);
    if (aindex >= 0 && acodec != NULL)
    {
        AVStream *stream = s->fmt->streams[aindex];
        s->adec = avcodec_alloc_context3(acodec);
        if (s->adec == NULL)
        {
            goto fail;
        }
        if (avcodec_parameters_to_context(s->adec, stream->codecpar) < 0)
        {
            goto fail;
        }
        if (avcodec_open2(s->adec, acodec, NULL) < 0)
        {
            goto fail;
        }

        s->aframe = av_frame_alloc();
        if (s->aframe == NULL)
        {
            goto fail;
        }

        s->audio_index = aindex;
        s->a_time_base = av_q2d(stream->time_base);
        s->a_start_ts = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;

        int src_sr = s->adec->sample_rate;
        int src_ch = s->adec->ch_layout.nb_channels > 0 ? s->adec->ch_layout.nb_channels : 2;
        out_info->has_audio = 1;
        out_info->audio_sample_rate = src_sr;
        out_info->audio_channels = src_ch;

        /* 目标格式：调用方给了就用它的（三个字段都当作有效），NULL 才走默认值。 */
        s->out_sr = (audio_out != NULL) ? audio_out->sample_rate : src_sr;
        s->out_ch = (audio_out != NULL) ? audio_out->channels : (src_ch > 2 ? 2 : src_ch);
        s->out_fmt = (audio_out != NULL && audio_out->sample_format == V3_AUDIO_S16)
                         ? AV_SAMPLE_FMT_S16
                         : AV_SAMPLE_FMT_FLT;
        s->out_bytes_per_sample = av_get_bytes_per_sample(s->out_fmt);
        s->audio_out.frame_bytes = s->out_ch * s->out_bytes_per_sample;
    }

    if (!out_info->has_video && !out_info->has_audio)
    {
        V3_DBG("no decodable stream\n");
        goto fail;
    }

    return s;

fail:
    vii3_close(s);
    return NULL;
}

V3_API int vii3_seek(Vii3Session *s, double seconds)
{
    if (s == NULL || s->fmt == NULL)
    {
        return -1;
    }

    int64_t ts = (int64_t)(seconds * AV_TIME_BASE);
    if (ts < 0)
    {
        ts = 0;
    }

    int r = av_seek_frame(s->fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
    if (r < 0)
    {
        V3_DBG("av_seek_frame -> %d\n", r);
        return -1;
    }

    /* 丢弃所有缓存包并冲洗两个解码器，避免 seek 后吐出旧帧。 */
    v3_fifo_clear(&s->fifo_v);
    v3_fifo_clear(&s->fifo_a);
    if (s->pending_v != NULL)
    {
        av_packet_free(&s->pending_v);
    }
    if (s->pending_a != NULL)
    {
        av_packet_free(&s->pending_a);
    }

    if (s->vdec != NULL)
    {
        avcodec_flush_buffers(s->vdec);
        s->v_flushed = 0;
    }
    /* 打开时预解的首帧也要丢弃：否则「先 seek 再取帧」会把 seek 之前的那一帧吐出来
     * （画面与 pts 都对不上）。 */
    if (s->has_prepared_frame)
    {
        s->has_prepared_frame = 0;
        av_frame_unref(s->vframe);
    }
    if (s->adec != NULL)
    {
        avcodec_flush_buffers(s->adec);
        s->a_flushed = 0;
    }
    if (s->swr != NULL)
    {
        /* 丢掉重采样器里残留的延迟样本 */
        swr_close(s->swr);
        swr_init(s->swr);
    }

    return 0;
}

V3_API void vii3_close(Vii3Session *s)
{
    if (s == NULL)
    {
        return;
    }

    /* 先停音频输出：渲染线程要在 session 还完整的时候退出 */
    vii3_audio_stop(s);


    /* 停后处理线程池（此刻不会再有帧在处理中） */
    v3_pool_destroy(&s->pool);

    v3_fifo_clear(&s->fifo_v);
    v3_fifo_clear(&s->fifo_a);
    if (s->pending_v != NULL)
    {
        av_packet_free(&s->pending_v);
    }
    if (s->pending_a != NULL)
    {
        av_packet_free(&s->pending_a);
    }

    if (s->swr != NULL)
    {
        swr_free(&s->swr);
    }
    if (s->sws != NULL)
    {
        sws_freeContext(s->sws);
    }
    av_free(s->scratch);

    if (s->vframe != NULL)
    {
        av_frame_free(&s->vframe);
    }
    if (s->sws_dst_frame != NULL)
    {
        av_frame_free(&s->sws_dst_frame);
    }
    if (s->aframe != NULL)
    {
        av_frame_free(&s->aframe);
    }
    if (s->vdec != NULL)
    {
        avcodec_free_context(&s->vdec);
    }
    if (s->adec != NULL)
    {
        avcodec_free_context(&s->adec);
    }
    if (s->fmt != NULL)
    {
        /* 不释放外部提供的 AVIOContext。 */
        avformat_close_input(&s->fmt);
    }
    if (s->avio != NULL)
    {
        avio_context_free(&s->avio);
    }

    av_free(s);
}

/* ============================ AVIF：同一个 dll 的另一套导出 ============================
 *
 * AVIF 的封装在另一个编译单元：Native/avif/vii3_avif.c（导出 vii3_avif_*），
 * 和本文件一起链进 vii3_media.dll —— dav1d 因此只进一份（libavif 与 FFmpeg 都要它）。
 * 托管侧封装：Libraries/Vii3Media/Native/AvifNative.cs。
 *
 * 两边的分工仍然是「AVIF 只走 libavif」：它的透明是独立辅助图层，FFmpeg 认不出来
 * （mov 解复用器会把 alpha 图层当成另一条 AV1 视频流），所以别把 AVIF 交给本文件这套。
 */
