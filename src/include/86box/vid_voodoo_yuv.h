/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          SIMD-vectorized and scalar YUV422 (YUYV / UYVY) color space
 *          conversion routines for 3dfx Voodoo Banshee / 3 blitter and
 *          video processor overlay.
 *
 * Authors: 86Box Contributors.
 *
 *          Copyright 2008-2026 Sarah Walker and the 86Box Contributors.
 */
#ifndef VIDEO_VOODOO_YUV_H
#define VIDEO_VOODOO_YUV_H

#include <stdint.h>

#ifndef CLAMP
#    define CLAMP(x) (((x) < 0) ? 0 : (((x) > 0xff) ? 0xff : (x)))
#endif

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#    define VOODOO_YUV_SSE2 1
#    include <emmintrin.h>
#endif

/*
 * Scalar decode for a single 4-byte YUYV chunk (2 pixels) to 32bpp (0x00BBGGRR).
 */
static inline void
voodoo_decode_yuyv422_pair_32bpp(uint32_t *buf, const uint8_t *src)
{
    uint8_t y1 = src[0];
    int16_t Cr = (int8_t) (src[1] - 0x80);
    uint8_t y2 = src[2];
    int16_t Cb = (int8_t) (src[3] - 0x80);

    int16_t dR = Cr + ((103 * Cr) >> 8);
    int16_t dG = Cr + ((88 * Cb - 73 * Cr) >> 8);
    int16_t dB = Cb + ((197 * Cb) >> 8);

    int r1 = CLAMP(y1 + dR);
    int g1 = CLAMP(y1 - dG);
    int b1 = CLAMP(y1 + dB);
    buf[0] = (uint32_t) (r1 | (g1 << 8) | (b1 << 16));

    int r2 = CLAMP(y2 + dR);
    int g2 = CLAMP(y2 - dG);
    int b2 = CLAMP(y2 + dB);
    buf[1] = (uint32_t) (r2 | (g2 << 8) | (b2 << 16));
}

/*
 * Scalar decode for a single 4-byte UYVY/UYUV chunk (2 pixels) to 32bpp (0x00BBGGRR).
 */
static inline void
voodoo_decode_uyuv422_pair_32bpp(uint32_t *buf, const uint8_t *src)
{
    int16_t Cr = (int8_t) (src[0] - 0x80);
    uint8_t y1 = src[1];
    int16_t Cb = (int8_t) (src[2] - 0x80);
    uint8_t y2 = src[3];

    int16_t dR = Cr + ((103 * Cr) >> 8);
    int16_t dG = Cr + ((88 * Cb - 73 * Cr) >> 8);
    int16_t dB = Cb + ((197 * Cb) >> 8);

    int r1 = CLAMP(y1 + dR);
    int g1 = CLAMP(y1 - dG);
    int b1 = CLAMP(y1 + dB);
    buf[0] = (uint32_t) (r1 | (g1 << 8) | (b1 << 16));

    int r2 = CLAMP(y2 + dR);
    int g2 = CLAMP(y2 - dG);
    int b2 = CLAMP(y2 + dB);
    buf[1] = (uint32_t) (r2 | (g2 << 8) | (b2 << 16));
}

/*
 * Scalar decode for a single 4-byte YUYV chunk (2 pixels) to 16bpp (RGB565).
 */
static inline void
voodoo_decode_yuyv422_pair_16bpp(uint16_t *buf, const uint8_t *src)
{
    uint8_t y1 = src[0];
    int16_t Cr = (int8_t) (src[1] - 0x80);
    uint8_t y2 = src[2];
    int16_t Cb = (int8_t) (src[3] - 0x80);

    int16_t dR = Cr + ((103 * Cr) >> 8);
    int16_t dG = Cr + ((88 * Cb - 73 * Cr) >> 8);
    int16_t dB = Cb + ((197 * Cb) >> 8);

    int r1 = CLAMP(y1 + dR) >> 3;
    int g1 = CLAMP(y1 - dG) >> 2;
    int b1 = CLAMP(y1 + dB) >> 3;
    buf[0] = (uint16_t) (r1 | (g1 << 5) | (b1 << 11));

    int r2 = CLAMP(y2 + dR) >> 3;
    int g2 = CLAMP(y2 - dG) >> 2;
    int b2 = CLAMP(y2 + dB) >> 3;
    buf[1] = (uint16_t) (r2 | (g2 << 5) | (b2 << 11));
}

/*
 * Scalar decode for a single 4-byte UYVY/UYUV chunk (2 pixels) to 16bpp (RGB565).
 */
static inline void
voodoo_decode_uyuv422_pair_16bpp(uint16_t *buf, const uint8_t *src)
{
    int16_t Cr = (int8_t) (src[0] - 0x80);
    uint8_t y1 = src[1];
    int16_t Cb = (int8_t) (src[2] - 0x80);
    uint8_t y2 = src[3];

    int16_t dR = Cr + ((103 * Cr) >> 8);
    int16_t dG = Cr + ((88 * Cb - 73 * Cr) >> 8);
    int16_t dB = Cb + ((197 * Cb) >> 8);

    int r1 = CLAMP(y1 + dR) >> 3;
    int g1 = CLAMP(y1 - dG) >> 2;
    int b1 = CLAMP(y1 + dB) >> 3;
    buf[0] = (uint16_t) (r1 | (g1 << 5) | (b1 << 11));

    int r2 = CLAMP(y2 + dR) >> 3;
    int g2 = CLAMP(y2 - dG) >> 2;
    int b2 = CLAMP(y2 + dB) >> 3;
    buf[1] = (uint16_t) (r2 | (g2 << 5) | (b2 << 11));
}

/*
 * Decode a span of YUYV422 bytes to 32bpp (0x00BBGGRR), vectorized with SSE2.
 * byte_count must be a multiple of 4; produces (byte_count / 2) 32-bit pixels.
 */
static inline void
voodoo_decode_yuyv422_span_32bpp(uint32_t *buf, const uint8_t *src, int byte_count)
{
    int c  = 0;
    int wp = 0;

#if defined(VOODOO_YUV_SSE2)
    const __m128i mask_y = _mm_set1_epi16(0x00FF);
    const __m128i c128   = _mm_set1_epi16(128);
    const __m128i c103   = _mm_set1_epi16(103);
    const __m128i c88    = _mm_set1_epi16(88);
    const __m128i c73    = _mm_set1_epi16(73);
    const __m128i c197   = _mm_set1_epi16(197);
    const __m128i zero   = _mm_setzero_si128();

    for (; c <= byte_count - 16; c += 16) {
        __m128i raw = _mm_loadu_si128((const __m128i *) (src + c));

        __m128i Y  = _mm_and_si128(raw, mask_y);
        __m128i uv = _mm_sub_epi16(_mm_srli_epi16(raw, 8), c128);

        __m128i uv_lo = _mm_unpacklo_epi16(uv, uv);
        __m128i uv_hi = _mm_unpackhi_epi16(uv, uv);

        __m128i Cr = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(uv_lo), _mm_castsi128_ps(uv_hi), _MM_SHUFFLE(2, 0, 2, 0)));
        __m128i Cb = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(uv_lo), _mm_castsi128_ps(uv_hi), _MM_SHUFFLE(3, 1, 3, 1)));

        __m128i dR      = _mm_add_epi16(Cr, _mm_srai_epi16(_mm_mullo_epi16(Cr, c103), 8));
        __m128i dG_term = _mm_sub_epi16(_mm_mullo_epi16(Cb, c88), _mm_mullo_epi16(Cr, c73));
        __m128i dG      = _mm_add_epi16(Cr, _mm_srai_epi16(dG_term, 8));
        __m128i dB      = _mm_add_epi16(Cb, _mm_srai_epi16(_mm_mullo_epi16(Cb, c197), 8));

        __m128i R16 = _mm_add_epi16(Y, dR);
        __m128i G16 = _mm_sub_epi16(Y, dG);
        __m128i B16 = _mm_add_epi16(Y, dB);

        __m128i R8 = _mm_packus_epi16(R16, zero);
        __m128i G8 = _mm_packus_epi16(G16, zero);
        __m128i B8 = _mm_packus_epi16(B16, zero);

        __m128i RG = _mm_unpacklo_epi8(R8, G8);
        __m128i BA = _mm_unpacklo_epi8(B8, zero);

        __m128i pix0_3 = _mm_unpacklo_epi16(RG, BA);
        __m128i pix4_7 = _mm_unpackhi_epi16(RG, BA);

        _mm_storeu_si128((__m128i *) (buf + wp + 0), pix0_3);
        _mm_storeu_si128((__m128i *) (buf + wp + 4), pix4_7);
        wp += 8;
    }
#endif

    for (; c <= byte_count - 4; c += 4) {
        voodoo_decode_yuyv422_pair_32bpp(buf + wp, src + c);
        wp += 2;
    }
}

/*
 * Decode a span of UYVY/UYUV bytes to 32bpp (0x00BBGGRR), vectorized with SSE2.
 * byte_count must be a multiple of 4; produces (byte_count / 2) 32-bit pixels.
 */
static inline void
voodoo_decode_uyuv422_span_32bpp(uint32_t *buf, const uint8_t *src, int byte_count)
{
    int c  = 0;
    int wp = 0;

#if defined(VOODOO_YUV_SSE2)
    const __m128i mask_y = _mm_set1_epi16(0x00FF);
    const __m128i c128   = _mm_set1_epi16(128);
    const __m128i c103   = _mm_set1_epi16(103);
    const __m128i c88    = _mm_set1_epi16(88);
    const __m128i c73    = _mm_set1_epi16(73);
    const __m128i c197   = _mm_set1_epi16(197);
    const __m128i zero   = _mm_setzero_si128();

    for (; c <= byte_count - 16; c += 16) {
        __m128i raw = _mm_loadu_si128((const __m128i *) (src + c));

        __m128i Y  = _mm_srli_epi16(raw, 8);
        __m128i uv = _mm_sub_epi16(_mm_and_si128(raw, mask_y), c128);

        __m128i uv_lo = _mm_unpacklo_epi16(uv, uv);
        __m128i uv_hi = _mm_unpackhi_epi16(uv, uv);

        __m128i Cr = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(uv_lo), _mm_castsi128_ps(uv_hi), _MM_SHUFFLE(2, 0, 2, 0)));
        __m128i Cb = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(uv_lo), _mm_castsi128_ps(uv_hi), _MM_SHUFFLE(3, 1, 3, 1)));

        __m128i dR      = _mm_add_epi16(Cr, _mm_srai_epi16(_mm_mullo_epi16(Cr, c103), 8));
        __m128i dG_term = _mm_sub_epi16(_mm_mullo_epi16(Cb, c88), _mm_mullo_epi16(Cr, c73));
        __m128i dG      = _mm_add_epi16(Cr, _mm_srai_epi16(dG_term, 8));
        __m128i dB      = _mm_add_epi16(Cb, _mm_srai_epi16(_mm_mullo_epi16(Cb, c197), 8));

        __m128i R16 = _mm_add_epi16(Y, dR);
        __m128i G16 = _mm_sub_epi16(Y, dG);
        __m128i B16 = _mm_add_epi16(Y, dB);

        __m128i R8 = _mm_packus_epi16(R16, zero);
        __m128i G8 = _mm_packus_epi16(G16, zero);
        __m128i B8 = _mm_packus_epi16(B16, zero);

        __m128i RG = _mm_unpacklo_epi8(R8, G8);
        __m128i BA = _mm_unpacklo_epi8(B8, zero);

        __m128i pix0_3 = _mm_unpacklo_epi16(RG, BA);
        __m128i pix4_7 = _mm_unpackhi_epi16(RG, BA);

        _mm_storeu_si128((__m128i *) (buf + wp + 0), pix0_3);
        _mm_storeu_si128((__m128i *) (buf + wp + 4), pix4_7);
        wp += 8;
    }
#endif

    for (; c <= byte_count - 4; c += 4) {
        voodoo_decode_uyuv422_pair_32bpp(buf + wp, src + c);
        wp += 2;
    }
}

/*
 * Decode a span of YUYV422 bytes to 16bpp (RGB565), vectorized with SSE2.
 * byte_count must be a multiple of 4; produces (byte_count / 2) 16-bit pixels.
 */
static inline void
voodoo_decode_yuyv422_span_16bpp(uint16_t *buf, const uint8_t *src, int byte_count)
{
    int c  = 0;
    int wp = 0;

#if defined(VOODOO_YUV_SSE2)
    const __m128i mask_y = _mm_set1_epi16(0x00FF);
    const __m128i c128   = _mm_set1_epi16(128);
    const __m128i c103   = _mm_set1_epi16(103);
    const __m128i c88    = _mm_set1_epi16(88);
    const __m128i c73    = _mm_set1_epi16(73);
    const __m128i c197   = _mm_set1_epi16(197);
    const __m128i zero   = _mm_setzero_si128();

    for (; c <= byte_count - 16; c += 16) {
        __m128i raw = _mm_loadu_si128((const __m128i *) (src + c));

        __m128i Y  = _mm_and_si128(raw, mask_y);
        __m128i uv = _mm_sub_epi16(_mm_srli_epi16(raw, 8), c128);

        __m128i uv_lo = _mm_unpacklo_epi16(uv, uv);
        __m128i uv_hi = _mm_unpackhi_epi16(uv, uv);

        __m128i Cr = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(uv_lo), _mm_castsi128_ps(uv_hi), _MM_SHUFFLE(2, 0, 2, 0)));
        __m128i Cb = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(uv_lo), _mm_castsi128_ps(uv_hi), _MM_SHUFFLE(3, 1, 3, 1)));

        __m128i dR      = _mm_add_epi16(Cr, _mm_srai_epi16(_mm_mullo_epi16(Cr, c103), 8));
        __m128i dG_term = _mm_sub_epi16(_mm_mullo_epi16(Cb, c88), _mm_mullo_epi16(Cr, c73));
        __m128i dG      = _mm_add_epi16(Cr, _mm_srai_epi16(dG_term, 8));
        __m128i dB      = _mm_add_epi16(Cb, _mm_srai_epi16(_mm_mullo_epi16(Cb, c197), 8));

        __m128i R16 = _mm_add_epi16(Y, dR);
        __m128i G16 = _mm_sub_epi16(Y, dG);
        __m128i B16 = _mm_add_epi16(Y, dB);

        __m128i R8 = _mm_packus_epi16(R16, zero);
        __m128i G8 = _mm_packus_epi16(G16, zero);
        __m128i B8 = _mm_packus_epi16(B16, zero);

        __m128i R_word = _mm_unpacklo_epi8(R8, zero);
        __m128i G_word = _mm_unpacklo_epi8(G8, zero);
        __m128i B_word = _mm_unpacklo_epi8(B8, zero);

        __m128i R5 = _mm_srli_epi16(R_word, 3);
        __m128i G6 = _mm_and_si128(_mm_slli_epi16(G_word, 3), _mm_set1_epi16(0x07e0));
        __m128i B5 = _mm_and_si128(_mm_slli_epi16(B_word, 8), _mm_set1_epi16(0xf800));

        __m128i RGB16 = _mm_or_si128(R5, _mm_or_si128(G6, B5));

        _mm_storeu_si128((__m128i *) (buf + wp), RGB16);
        wp += 8;
    }
#endif

    for (; c <= byte_count - 4; c += 4) {
        voodoo_decode_yuyv422_pair_16bpp(buf + wp, src + c);
        wp += 2;
    }
}

/*
 * Decode a span of UYVY/UYUV bytes to 16bpp (RGB565), vectorized with SSE2.
 * byte_count must be a multiple of 4; produces (byte_count / 2) 16-bit pixels.
 */
static inline void
voodoo_decode_uyuv422_span_16bpp(uint16_t *buf, const uint8_t *src, int byte_count)
{
    int c  = 0;
    int wp = 0;

#if defined(VOODOO_YUV_SSE2)
    const __m128i mask_y = _mm_set1_epi16(0x00FF);
    const __m128i c128   = _mm_set1_epi16(128);
    const __m128i c103   = _mm_set1_epi16(103);
    const __m128i c88    = _mm_set1_epi16(88);
    const __m128i c73    = _mm_set1_epi16(73);
    const __m128i c197   = _mm_set1_epi16(197);
    const __m128i zero   = _mm_setzero_si128();

    for (; c <= byte_count - 16; c += 16) {
        __m128i raw = _mm_loadu_si128((const __m128i *) (src + c));

        __m128i Y  = _mm_srli_epi16(raw, 8);
        __m128i uv = _mm_sub_epi16(_mm_and_si128(raw, mask_y), c128);

        __m128i uv_lo = _mm_unpacklo_epi16(uv, uv);
        __m128i uv_hi = _mm_unpackhi_epi16(uv, uv);

        __m128i Cr = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(uv_lo), _mm_castsi128_ps(uv_hi), _MM_SHUFFLE(2, 0, 2, 0)));
        __m128i Cb = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(uv_lo), _mm_castsi128_ps(uv_hi), _MM_SHUFFLE(3, 1, 3, 1)));

        __m128i dR      = _mm_add_epi16(Cr, _mm_srai_epi16(_mm_mullo_epi16(Cr, c103), 8));
        __m128i dG_term = _mm_sub_epi16(_mm_mullo_epi16(Cb, c88), _mm_mullo_epi16(Cr, c73));
        __m128i dG      = _mm_add_epi16(Cr, _mm_srai_epi16(dG_term, 8));
        __m128i dB      = _mm_add_epi16(Cb, _mm_srai_epi16(_mm_mullo_epi16(Cb, c197), 8));

        __m128i R16 = _mm_add_epi16(Y, dR);
        __m128i G16 = _mm_sub_epi16(Y, dG);
        __m128i B16 = _mm_add_epi16(Y, dB);

        __m128i R8 = _mm_packus_epi16(R16, zero);
        __m128i G8 = _mm_packus_epi16(G16, zero);
        __m128i B8 = _mm_packus_epi16(B16, zero);

        __m128i R_word = _mm_unpacklo_epi8(R8, zero);
        __m128i G_word = _mm_unpacklo_epi8(G8, zero);
        __m128i B_word = _mm_unpacklo_epi8(B8, zero);

        __m128i R5 = _mm_srli_epi16(R_word, 3);
        __m128i G6 = _mm_and_si128(_mm_slli_epi16(G_word, 3), _mm_set1_epi16(0x07e0));
        __m128i B5 = _mm_and_si128(_mm_slli_epi16(B_word, 8), _mm_set1_epi16(0xf800));

        __m128i RGB16 = _mm_or_si128(R5, _mm_or_si128(G6, B5));

        _mm_storeu_si128((__m128i *) (buf + wp), RGB16);
        wp += 8;
    }
#endif

    for (; c <= byte_count - 4; c += 4) {
        voodoo_decode_uyuv422_pair_16bpp(buf + wp, src + c);
        wp += 2;
    }
}

#endif /*VIDEO_VOODOO_YUV_H*/
