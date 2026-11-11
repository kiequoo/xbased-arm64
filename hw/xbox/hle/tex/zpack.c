/*
 * hakuX HLE GPU — a depth target's image as the NV2A's words (zpack.h).
 */
#include "hw/xbox/hle/tex/zpack.h"

#include <math.h>
#include <string.h>

static uint32_t depth24(const uint8_t *p, int src)
{
    if (src == HLE_ZSRC_D16) {
        uint16_t v;
        memcpy(&v, p, 2);
        return (uint32_t)v << 8 | v >> 8;
    }
    if (src == HLE_ZSRC_D24) {
        uint32_t v;
        memcpy(&v, p, 4);
        return v & 0x00FFFFFFu;
    }
    float f;
    memcpy(&f, p, 4);
    if (!(f > 0.0f)) {                  /* (a NaN too) */
        return 0;
    }
    return f >= 1.0f ? 0x00FFFFFFu : (uint32_t)lrint((double)f * 16777215.0);
}

static uint32_t depth16(const uint8_t *p, int src)
{
    if (src == HLE_ZSRC_D16) {
        uint16_t v;
        memcpy(&v, p, 2);
        return v;
    }
    if (src == HLE_ZSRC_D24) {
        return depth24(p, src) >> 8;
    }
    float f;
    memcpy(&f, p, 4);
    if (!(f > 0.0f)) {
        return 0;
    }
    return f >= 1.0f ? 0xFFFFu : (uint32_t)lrint((double)f * 65535.0);
}

uint32_t hle_zf24(float z)
{
    if (!(z > 0.0f)) {
        return 0;
    }
    uint32_t b;
    memcpy(&b, &z, 4);
    b >>= 7;
    return b > 0x00FEFFFFu ? 0x00FEFFFFu : b;      /* (no infinity: the largest) */
}

uint32_t hle_zf16(float z)
{
    if (!(z > 0.0f)) {
        return 0;
    }
    if (z >= 511.9375f) {
        return 0xFFFFu;
    }
    int e;
    const float m = frexpf(z, &e);                  /* z = m 2^e, m in [0.5, 1) */
    int E = e - 1 + 7;
    if (E >= 1) {
        return (uint32_t)E << 12 | (uint32_t)((m * 2.0f - 1.0f) * 4096.0f);
    }
    return (uint32_t)(z * 64.0f * 4096.0f);         /* below 2^-6: no implied bit */
}

void hle_zpack(const uint8_t *depth, const uint8_t *stencil, int src, uint32_t n, uint32_t gbpp,
               uint8_t *out, float zf)
{
    const uint32_t sb = hle_zsrc_bytes(src);
    if (!sb || (gbpp != 2 && gbpp != 4)) {
        return;
    }
    if (zf > 0.0f && src == HLE_ZSRC_F32) {
        for (uint32_t i = 0; i < n; i++) {
            float f;
            memcpy(&f, depth + (size_t)i * 4, 4);
            /* the image's top is the buffer's: a float clear's all-ones word
             * (D3D's and nxdk's far clear, not a number the chip draws) */
            const float z = f / zf;
            if (gbpp == 2) {
                const uint16_t v = f >= 1.0f ? 0xFFFFu : (uint16_t)hle_zf16(z);
                memcpy(out + (size_t)i * 2, &v, 2);
            } else {
                const uint32_t v = (f >= 1.0f ? 0x00FFFFFFu : hle_zf24(z)) << 8 | (stencil ? stencil[i] : 0);
                memcpy(out + (size_t)i * 4, &v, 4);
            }
        }
        return;
    }
    if (gbpp == 2 && src == HLE_ZSRC_D16) {
        memcpy(out, depth, (size_t)n * 2);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *d = depth + (size_t)i * sb;
        if (gbpp == 2) {
            const uint16_t v = (uint16_t)depth16(d, src);
            memcpy(out + (size_t)i * 2, &v, 2);
        } else {
            const uint32_t v = depth24(d, src) << 8 | (stencil ? stencil[i] : 0);
            memcpy(out + (size_t)i * 4, &v, 4);
        }
    }
}
