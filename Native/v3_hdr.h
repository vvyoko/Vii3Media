#ifndef V3_HDR_H
#define V3_HDR_H

#include <stdint.h>
#include <math.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h> /* 近似倒数（_mm_rcp_ps），x64 恒定可用 */
#endif

/*
 * HDR 色调映射 / 高位深工具的共享实现，两个 shim 各自 #include "../v3_hdr.h"。
 * 全部 static inline，不依赖 FFmpeg 或 libavif。
 *
 * libavif / swscale 只做 YUV->RGB 的矩阵与 range 转换，不做传递函数（PQ/HLG）与色域转换，
 * 所以直通输出保留源色彩空间，交给下游做色彩管理。这里提供 CPU 端的 HDR->SDR 兜底：
 * EOTF 到线性光 -> HLG 按 BT.2100 做公共增益 OOTF -> 归一化到参考白 203nit ->
 * BT.2020->BT.709 色域压缩 -> BT.2390 EETF -> sRGB。
 * 给「当前不是 HDR 显示器」的场景用；HDR 显示器走直通的 RGBA16/RGBA_F16。
 */

/* 传输函数 / 原色枚举（AVCOL_TRC_* 与 AVIF_TRANSFER_CHARACTERISTICS_* 共用同一套编号） */
#define V3_TRC_PQ 16           /* SMPTE ST 2084 (PQ) */
#define V3_TRC_HLG 18          /* ARIB STD-B67 (HLG) */
#define V3_PRI_BT2020 9        /* ITU-R BT.2020 原色 */
#define V3_PRI_UNSPECIFIED 2   /* AVCOL_PRI_UNSPECIFIED / AVIF_COLOR_PRIMARIES_UNSPECIFIED */

/* --- PQ (SMPTE ST 2084) EOTF：encoded E in [0,1] -> 绝对亮度（nit，0..10000） --- */
static inline float v3_pq_eotf(float e)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f;
    const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    if (e <= 0.0f)
    {
        return 0.0f;
    }
    float p = powf(e, 1.0f / m2);
    float num = p - c1;
    if (num < 0.0f)
    {
        num = 0.0f;
    }
    float den = c2 - c3 * p;
    float L = powf(num / den, 1.0f / m1);
    return L * 10000.0f;
}

/* --- HLG 逆 OETF：encoded E in [0,1] -> scene linear Ls in [0,1] ---
 * 不含 OOTF：BT.2100 的 OOTF 是三通道乘同一个亮度增益，不能折进逐通道查表，
 * 放到色调映射里按像素做（见 v3_tone_map_linear_lut）。 */
static inline float v3_hlg_inverse_oetf(float e)
{
    const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    if (e <= 0.5f)
    {
        return (e * e) / 3.0f;
    }
    return (expf((e - c) / a) + b) / 12.0f;
}

/* --- sRGB / BT.709 OETF：线性 -> 编码 --- */
static inline float v3_srgb_oetf(float l)
{
    if (l <= 0.0031308f)
    {
        return 12.92f * l;
    }
    return 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
}

/* --- BT.2020 线性 -> BT.709 线性（色域压缩） --- */
static inline void v3_gamut_bt2020_to_bt709(float r, float g, float b, float *out)
{
    out[0] =  1.660491f * r - 0.587641f * g - 0.072750f * b;
    out[1] = -0.124550f * r + 1.132899f * g - 0.008349f * b;
    out[2] = -0.018151f * r - 0.100579f * g + 1.118730f * b;
}

/* --- float32 -> IEEE half（uint16，小端） --- */
static inline uint16_t v3_float_to_half(float v)
{
    uint32_t x;
    memcpy(&x, &v, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = (int32_t)((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffffu;
    if (exp <= 0)
    {
        if (exp < -10)
        {
            return (uint16_t)sign;
        }
        mant |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t h = mant >> shift;
        if ((mant >> (shift - 1)) & 1u)
        {
            h += 1u;
        }
        return (uint16_t)(sign | h);
    }
    else if (exp >= 31)
    {
        return (uint16_t)(sign | 0x7c00u);
    }
    else
    {
        uint32_t h = ((uint32_t)exp << 10) | (mant >> 13);
        if ((mant >> 12) & 1u)
        {
            h += 1u;
        }
        return (uint16_t)(sign | h);
    }
}

/* --- HDR->SDR 查表（参考白 = BT.2408 的 203 nit）
 *   v3_eotf_lut    : 逆传递函数。PQ -> 绝对亮度/203；HLG -> scene linear（OOTF 另做）
 *   v3_hlg_ootf_lut: HLG 的 OOTF 公共增益 (1000/203)·Y^0.2，按亮度 Y 索引
 *   v3_tm_out_lut  : 线性光（经 BT.2390 EETF 压缩）-> sRGB 字节，按源峰值重建；
 *                    索引是 Reinhard 非线性（nr = t/(1-t)），暗部精度高、
 *                    且定义域覆盖到峰值之上（避免高光被夹成同一个值）
 * 表在 .bss 运行时填充，不占库体积；头文件被多个编译单元 include，各持一份 static 副本。 --- */
static float v3_eotf_lut[2][4096];    /* [0]=PQ(绝对亮度/203), [1]=HLG(scene linear)；索引 = 16bit 码值>>4 */
static float v3_hlg_ootf_lut[1024];   /* (1000/203)·(i/1023)^0.2（γ-1=0.2 @ L_W=1000） */
static uint8_t v3_tm_out_lut[1024];   /* 索引 t=i/1024 <-> nr=t/(1-t) -> sRGB 字节（按源峰值重建） */
static int v3_hdr_lut_ready = 0;
static float v3_tm_lut_peak = -1.0f;  /* v3_tm_out_lut 是按这个源峰值（参考白倍数）建的 */

/* --- PQ 逆 EOTF（线性 -> PQ 编码）。x 归一化到 10000 nit。 --- */
static inline float v3_pq_encode(float x)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f;
    const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    if (x < 0.0f)
    {
        x = 0.0f;
    }
    float xp = powf(x, m1);
    return powf((c1 + c2 * xp) / (1.0f + c3 * xp), m2);
}

/* 「源峰值」的标称值（参考白倍数）：PQ 10000 nit、HLG 1000 nit，用于没有 MDCV/CLLI 时。 */
static inline float v3_nominal_peak_rw(int is_pq)
{
    return (is_pq ? 10000.0f : 1000.0f) / 203.0f;
}

/* --- BT.2390 EETF 的分段 Hermite 膝点（ITU-R BT.2390）。
 * 输入输出都在「相对源峰值」的 PQ 空间：膝点以下原样保留，以上平滑压缩到 maxLum
 * （= 目标峰值在该空间的位置）。 */
static inline float v3_bt2390_eetf_rel(float rel, float maxLum)
{
    if (rel <= 0.0f)
    {
        return 0.0f;
    }
    if (rel > 1.0f)
    {
        rel = 1.0f;
    }
    float ks = 1.5f * maxLum - 0.5f;
    if (ks < 0.0f)
    {
        ks = 0.0f;
    }
    if (rel < ks || ks >= 1.0f)
    {
        return rel; /* 膝点以下不动 */
    }
    float t = (rel - ks) / (1.0f - ks);
    float t2 = t * t, t3 = t2 * t;
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * ks
         + (t3 - 2.0f * t2 + t) * (1.0f - ks)
         + (-2.0f * t3 + 3.0f * t2) * maxLum;
}

static inline void v3_ensure_hdr_luts(void)
{
    if (v3_hdr_lut_ready)
    {
        return;
    }
    /* 4096 项即覆盖 8/10/12bit 源的全部码值（16bit 码值 >>4 后一一对应）；两张表共 32KB。 */
    for (int i = 0; i < 4096; i++)
    {
        float e = (float)i / 4095.0f;
        v3_eotf_lut[0][i] = v3_pq_eotf(e) / 203.0f; /* PQ：绝对亮度归一化到参考白 */
        v3_eotf_lut[1][i] = v3_hlg_inverse_oetf(e); /* HLG：只到 scene linear */
    }
    for (int i = 0; i < 1024; i++)
    {
        float e = (float)i / 1023.0f;
        v3_hlg_ootf_lut[i] = (1000.0f / 203.0f) * powf(e, 0.2f);
    }
    v3_hdr_lut_ready = 1;
}

/* 出表（BT.2390 EETF + sRGB OETF 折叠成一张表）依赖源峰值，峰值变了就重建；
 * 1024 项含少量 powf，只在峰值变化时发生。 */
static inline void v3_ensure_tm_lut(float src_peak_rw)
{
    if (src_peak_rw < 1.0f)
    {
        src_peak_rw = 1.0f;
    }
    /* 量化到 1/16，避免峰值微抖动导致每帧重建 */
    src_peak_rw = (float)((int)(src_peak_rw * 16.0f + 0.5f)) / 16.0f;
    if (v3_tm_lut_peak == src_peak_rw)
    {
        return;
    }
    /* 公共量只算一次 */
    const float ref_code = v3_pq_encode(203.0f / 10000.0f);
    float ep = v3_pq_encode(src_peak_rw * 203.0f / 10000.0f);
    if (ep < 1e-6f)
    {
        ep = 1e-6f;
    }
    const float maxLum = ref_code / ep;
    for (int i = 0; i < 1024; i++)
    {
        float t = (float)i / 1024.0f; /* t ∈ [0, 0.999]，避开 t=1 除零 */
        float nr = t / (1.0f - t);    /* t=0.5 -> nr=1（203 nit） */
        if (nr > src_peak_rw)
        {
            nr = src_peak_rw;
        }
        float rel = v3_pq_encode(nr * 203.0f / 10000.0f) / ep;
        float a = v3_pq_eotf(v3_bt2390_eetf_rel(rel, maxLum) * ep) / 203.0f;
        if (a < 0.0f)
        {
            a = 0.0f;
        }
        else if (a > 1.0f)
        {
            a = 1.0f;
        }
        v3_tm_out_lut[i] = (uint8_t)(v3_srgb_oetf(a) * 255.0f + 0.5f);
    }
    v3_tm_lut_peak = src_peak_rw;
}

/* 单值倒数：x64 上用 SSE 的 12bit 近似倒数，非 SSE2 目标回退普通除法。
 * 只用于对精度不敏感的系数（如去饱和系数），别用于需要精确的换算。 */
static inline float v3_rcp1(float x)
{
#if defined(__SSE2__)
    return _mm_cvtss_f32(_mm_rcp_ps(_mm_set_ss(x)));
#else
    return 1.0f / x;
#endif
}

/* 出表索引：Reinhard 压缩 t = nr/(nr+1)（nr>=0）量化到 0..1023。
 * 只用于定位查表，±1 无影响，故用近似倒数一次算三通道。 */
static inline void v3_index3(float nr, float ng, float nb, int *ri, int *gi, int *bi)
{
#if defined(__SSE2__)
    __m128 n = _mm_set_ps(0.0f, nb, ng, nr);
    __m128 t = _mm_mul_ps(n, _mm_rcp_ps(_mm_add_ps(n, _mm_set1_ps(1.0f))));
    __m128 s = _mm_add_ps(_mm_mul_ps(t, _mm_set1_ps(1024.0f)), _mm_set1_ps(0.5f));
    s = _mm_max_ps(s, _mm_setzero_ps());
    s = _mm_min_ps(s, _mm_set1_ps(1023.0f));
    int idx[4];
    _mm_storeu_si128((__m128i *)idx, _mm_cvttps_epi32(s));
    *ri = idx[0];
    *gi = idx[1];
    *bi = idx[2];
#else
    int i;
    *ri = (int)((nr / (nr + 1.0f)) * 1024.0f + 0.5f);
    *gi = (int)((ng / (ng + 1.0f)) * 1024.0f + 0.5f);
    *bi = (int)((nb / (nb + 1.0f)) * 1024.0f + 0.5f);
    i = *ri > 1023 ? 1023 : *ri; *ri = i;
    i = *gi > 1023 ? 1023 : *gi; *gi = i;
    i = *bi > 1023 ? 1023 : *bi; *bi = i;
#endif
}

/* 三个通道「已过 EOTF 的值」-> sRGB BGRA8（不含 alpha）。热路径调用。
 *   is_pq    : 1=PQ，0=HLG（决定要不要做 OOTF；两者 EOTF 表的单位不同）
 *   is_bt2020: 源原色为 BT.2020（含「未标记原色但为 PQ/HLG」的推定）时做色域压缩 */
static inline void v3_tone_map_linear_lut(float r, float g, float b, int is_pq,
                                          int is_bt2020, uint8_t *out)
{
    float nr, ng, nb;
    if (is_pq)
    {
        nr = r; /* PQ 的 EOTF 输出已是绝对亮度/参考白，不需要 OOTF */
        ng = g;
        nb = b;
    }
    else
    {
        /* HLG OOTF：三通道乘同一个亮度增益（γ=1.2 @ L_W=1000），用按 Y 索引的小表代替每像素 powf */
        float Y = 0.2627f * r + 0.6780f * g + 0.0593f * b;
        if (Y < 0.0f)
        {
            Y = 0.0f;
        }
        else if (Y > 1.0f)
        {
            Y = 1.0f;
        }
        float k = v3_hlg_ootf_lut[(int)(Y * 1023.0f + 0.5f)];
        nr = k * r;
        ng = k * g;
        nb = k * b;
    }

    if (is_bt2020)
    {
        float t[3];
        v3_gamut_bt2020_to_bt709(nr, ng, nb, t);
        nr = t[0];
        ng = t[1];
        nb = t[2];

        /* 色域越界 soft-clip：BT.2020 的饱和色在 BT.709 里装不下，表现为矩阵后出现负通道。
         * 逐通道硬夹会色相偏移，所以按 BT.709 亮度向白点去饱和（把负通道拉到 0），
         * 只对这类像素生效。
         * 触发条件只认负通道：这里的值作用在色调映射**之前**、单位是 203nit 的倍数，
         * >1 只表示比漫反射白更亮（HDR 的正常情况），不是越界。
         * 上端不需要处理：EETF 输出恒 ≤ 目标峰值（参考白）。 */
        float Y = 0.2126f * nr + 0.7152f * ng + 0.0722f * nb;
        float mn = nr < ng ? nr : ng;
        if (nb < mn) mn = nb;
        if (mn < 0.0f && mn < Y)
        {
            /* (0-Y)/(mn-Y) 是去饱和系数，用近似倒数（精度足够） */
            float t = (0.0f - Y) * v3_rcp1(mn - Y);
            if (t < 0.0f)
            {
                t = 0.0f; /* 亮度本身为负：该像素无法表示，退化为灰(黑) */
            }
            else if (t > 1.0f)
            {
                t = 1.0f;
            }
            nr = Y + t * (nr - Y);
            ng = Y + t * (ng - Y);
            nb = Y + t * (nb - Y);
        }
    }

    /* 兜底：仍为负则按黑处理，否则 Reinhard 索引会翻到亮部。 */
    if (nr < 0.0f) nr = 0.0f;
    if (ng < 0.0f) ng = 0.0f;
    if (nb < 0.0f) nb = 0.0f;

    int ri, gi, bi;
    v3_index3(nr, ng, nb, &ri, &gi, &bi);
    out[0] = v3_tm_out_lut[bi];
    out[1] = v3_tm_out_lut[gi];
    out[2] = v3_tm_out_lut[ri];
}

/* --- SSE2：一次算 4 个像素的「OOTF(可选) + 色域矩阵 + 去饱和 + 索引换算」。
 * 查表在 SSE2 下没有 gather，只能标量，所以只向量化纯算术部分；
 * 算法与 v3_tone_map_linear_lut 逐步对应。
 * 输入 lr/lg/lb：4 个像素的 EOTF 后线性光；输出 out16：4 组 BGRA（alpha 由调用方写）。 --- */
#if defined(__SSE2__)
static inline void v3_tone_map4_sse2(const float *lr, const float *lg, const float *lb,
                                     int is_pq, int is_bt2020, uint8_t *out16)
{
    const __m128 z = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    const __m128 s1024 = _mm_set1_ps(1024.0f), s1023 = _mm_set1_ps(1023.0f), s05 = _mm_set1_ps(0.5f);
    __m128 R = _mm_loadu_ps(lr), G = _mm_loadu_ps(lg), B = _mm_loadu_ps(lb);

    if (!is_pq)
    {
        /* HLG OOTF：k = ootf_lut[Y]，三通道乘同一个增益 */
        const __m128 my = _mm_set1_ps(0.2627f), mg_ = _mm_set1_ps(0.6780f), mb_ = _mm_set1_ps(0.0593f);
        __m128 Y = _mm_add_ps(_mm_add_ps(_mm_mul_ps(R, my), _mm_mul_ps(G, mg_)), _mm_mul_ps(B, mb_));
        Y = _mm_min_ps(_mm_max_ps(Y, z), one);
        int yi[4];
        _mm_storeu_si128((__m128i *)yi, _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(Y, s1023), s05)));
        float k4[4];
        for (int k = 0; k < 4; k++)
        {
            int i = yi[k] < 0 ? 0 : (yi[k] > 1023 ? 1023 : yi[k]);
            k4[k] = v3_hlg_ootf_lut[i];
        }
        const __m128 K = _mm_loadu_ps(k4);
        R = _mm_mul_ps(R, K);
        G = _mm_mul_ps(G, K);
        B = _mm_mul_ps(B, K);
    }

    if (is_bt2020)
    {
        const __m128 m00 = _mm_set1_ps(1.660491f), m01 = _mm_set1_ps(-0.587641f), m02 = _mm_set1_ps(-0.072750f);
        const __m128 m10 = _mm_set1_ps(-0.124550f), m11 = _mm_set1_ps(1.132899f), m12 = _mm_set1_ps(-0.008349f);
        const __m128 m20 = _mm_set1_ps(-0.018151f), m21 = _mm_set1_ps(-0.100579f), m22 = _mm_set1_ps(1.118730f);
        __m128 r = _mm_add_ps(_mm_add_ps(_mm_mul_ps(R, m00), _mm_mul_ps(G, m01)), _mm_mul_ps(B, m02));
        __m128 g = _mm_add_ps(_mm_add_ps(_mm_mul_ps(R, m10), _mm_mul_ps(G, m11)), _mm_mul_ps(B, m12));
        __m128 b = _mm_add_ps(_mm_add_ps(_mm_mul_ps(R, m20), _mm_mul_ps(G, m21)), _mm_mul_ps(B, m22));

        const __m128 cy = _mm_set1_ps(0.2126f), cg2 = _mm_set1_ps(0.7152f), cb2 = _mm_set1_ps(0.0722f);
        const __m128 Y = _mm_add_ps(_mm_add_ps(_mm_mul_ps(r, cy), _mm_mul_ps(g, cg2)), _mm_mul_ps(b, cb2));
        const __m128 mn = _mm_min_ps(_mm_min_ps(r, g), b);
        const __m128 mask = _mm_and_ps(_mm_cmplt_ps(mn, z), _mm_cmplt_ps(mn, Y));
        if (_mm_movemask_ps(mask) != 0)
        {
            __m128 t = _mm_mul_ps(_mm_sub_ps(z, Y), _mm_rcp_ps(_mm_sub_ps(mn, Y)));
            t = _mm_min_ps(_mm_max_ps(t, z), one);
            r = _mm_add_ps(Y, _mm_mul_ps(t, _mm_sub_ps(r, Y)));
            g = _mm_add_ps(Y, _mm_mul_ps(t, _mm_sub_ps(g, Y)));
            b = _mm_add_ps(Y, _mm_mul_ps(t, _mm_sub_ps(b, Y)));
        }
        R = _mm_max_ps(r, z);
        G = _mm_max_ps(g, z);
        B = _mm_max_ps(b, z);
    }
    R = _mm_max_ps(R, z);
    G = _mm_max_ps(G, z);
    B = _mm_max_ps(B, z);

    __m128 ir = _mm_add_ps(_mm_mul_ps(_mm_mul_ps(R, _mm_rcp_ps(_mm_add_ps(R, one))), s1024), s05);
    __m128 ig = _mm_add_ps(_mm_mul_ps(_mm_mul_ps(G, _mm_rcp_ps(_mm_add_ps(G, one))), s1024), s05);
    __m128 ib = _mm_add_ps(_mm_mul_ps(_mm_mul_ps(B, _mm_rcp_ps(_mm_add_ps(B, one))), s1024), s05);
    int ri[4], gi[4], bi[4];
    _mm_storeu_si128((__m128i *)ri, _mm_cvttps_epi32(_mm_min_ps(_mm_max_ps(ir, z), s1023)));
    _mm_storeu_si128((__m128i *)gi, _mm_cvttps_epi32(_mm_min_ps(_mm_max_ps(ig, z), s1023)));
    _mm_storeu_si128((__m128i *)bi, _mm_cvttps_epi32(_mm_min_ps(_mm_max_ps(ib, z), s1023)));
    for (int k = 0; k < 4; k++)
    {
        out16[k * 4 + 0] = v3_tm_out_lut[bi[k]];
        out16[k * 4 + 1] = v3_tm_out_lut[gi[k]];
        out16[k * 4 + 2] = v3_tm_out_lut[ri[k]];
        out16[k * 4 + 3] = 0; /* alpha 由调用方按源 alpha 写 */
    }
}
#endif

/* --- RGBA16（源空间，PQ/HLG 编码，无旋转）-> sRGB BGRA8。
 *     用于 AVIF 的 HDR->SDR 兜底；视频侧的带旋转版本在 vii3_media.c 里。
 *     分带版本 v3_tone_map_rgba16_rows 只处理源行 [y0,y1)，供线程池并行调用。 --- */
static inline void v3_tone_map_rgba16_rows(uint8_t *dst, int w, int primaries, int is_pq,
                                           const uint16_t *src, int y0, int y1)
{
    const float inv = 1.0f / 65535.0f;
    const float *eotf = v3_eotf_lut[is_pq ? 0 : 1];
    /* 原色未标记（2）但传递函数是 PQ/HLG 时按 BT.2020 推定（H.273 惯例） */
    const int bt2020 = (primaries == V3_PRI_BT2020 || primaries == V3_PRI_UNSPECIFIED);
    for (int y = y0; y < y1; y++)
    {
        int x = 0;
#if defined(__SSE2__)
        /* 4 像素一批：算术向量化，查表仍标量；输出连续，可整块写 */
        for (; x + 4 <= w; x += 4)
        {
            const uint16_t *p = src + ((size_t)y * w + x) * 4;
            uint8_t *o = dst + ((size_t)y * w + x) * 4;
            float lr[4], lg4[4], lb4[4];
            for (int k = 0; k < 4; k++)
            {
                lr[k] = eotf[p[k * 4 + 0] >> 4];
                lg4[k] = eotf[p[k * 4 + 1] >> 4];
                lb4[k] = eotf[p[k * 4 + 2] >> 4];
            }
            v3_tone_map4_sse2(lr, lg4, lb4, is_pq, bt2020, o);
            for (int k = 0; k < 4; k++)
            {
                o[k * 4 + 3] = (uint8_t)(p[k * 4 + 3] * inv * 255.0f + 0.5f);
            }
        }
#endif
        for (; x < w; x++)
        {
            const uint16_t *p = src + ((size_t)y * w + x) * 4;
            uint8_t *o = dst + ((size_t)y * w + x) * 4;
            v3_tone_map_linear_lut(eotf[p[0] >> 4], eotf[p[1] >> 4], eotf[p[2] >> 4], is_pq,
                                   bt2020, o);
            o[3] = (uint8_t)(p[3] * inv * 255.0f + 0.5f);
        }
    }
}

static inline void v3_tone_map_rgba16(uint8_t *dst, int w, int h, int primaries, int is_pq,
                                      float src_peak_rw, const uint16_t *src)
{
    v3_ensure_hdr_luts();
    v3_ensure_tm_lut(src_peak_rw);
    v3_tone_map_rgba16_rows(dst, w, primaries, is_pq, src, 0, h);
}

#endif /* V3_HDR_H */
