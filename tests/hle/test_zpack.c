/*
 * hakuX HLE GPU — a depth image as the NV2A's words (hw/xbox/hle/tex/zpack.c).
 */
#include "hle_test.h"

#include <math.h>
#include <string.h>

#include "hw/xbox/hle/tex/zpack.h"

static uint32_t w32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint32_t w16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

/* Z24S8: the depth in the top 24 bits, the stencil in the low byte; what the
 * host leaves above its 24 bits is not the depth's. */
static void test_d24s8(void)
{
    const uint32_t d[4] = { 0x00000000u, 0x00FFFFFFu, 0xAB123456u, 0xFF000001u };
    const uint8_t s[4] = { 0x00, 0xFF, 0x7E, 0x01 };
    uint8_t out[16];
    hle_zpack((const uint8_t *)d, s, HLE_ZSRC_D24, 4, 4, out, 0.0f);
    HT_EQ_U32(w32(out), 0x00000000u);
    HT_EQ_U32(w32(out + 4), 0xFFFFFFFFu);
    HT_EQ_U32(w32(out + 8), 0x1234567Eu);
    HT_EQ_U32(w32(out + 12), 0x00000101u);
    /* no stencil in the image: 0 */
    hle_zpack((const uint8_t *)d, NULL, HLE_ZSRC_D24, 4, 4, out, 0.0f);
    HT_EQ_U32(w32(out + 4), 0xFFFFFF00u);
    HT_EQ_U32(w32(out + 8), 0x12345600u);
}

/* Z16 from a 16-bit image: the words as they are. */
static void test_d16(void)
{
    const uint16_t d[5] = { 0x0000, 0xFFFF, 0xF044, 0x0EC4, 0x8000 };
    uint8_t out[12] = { 0 };
    out[10] = 0x5A;                             /* (past the five: untouched) */
    hle_zpack((const uint8_t *)d, NULL, HLE_ZSRC_D16, 5, 2, out, 0.0f);
    HT_CHECK(memcmp(out, d, 10) == 0);
    HT_EQ_U32(out[10], 0x5A);
}

/* A depth of another width: 24 bits keep their top 16, 16 bits repeat into
 * 24 (0xFFFF is the far plane either way). */
static void test_widths(void)
{
    const uint32_t d24[3] = { 0x00FFFFFFu, 0x00123456u, 0x000000FFu };
    uint8_t out[12];
    hle_zpack((const uint8_t *)d24, NULL, HLE_ZSRC_D24, 3, 2, out, 0.0f);
    HT_EQ_U32(w16(out), 0xFFFF);
    HT_EQ_U32(w16(out + 2), 0x1234);
    HT_EQ_U32(w16(out + 4), 0x0000);
    const uint16_t d16[3] = { 0xFFFF, 0x1234, 0x0001 };
    const uint8_t s[3] = { 1, 2, 3 };
    hle_zpack((const uint8_t *)d16, s, HLE_ZSRC_D16, 3, 4, out, 0.0f);
    HT_EQ_U32(w32(out), 0xFFFFFF01u);
    HT_EQ_U32(w32(out + 4), 0x12341202u);
    HT_EQ_U32(w32(out + 8), 0x00010003u);
}

/* A float image (a host without a 24-bit format): 0 .. 1 of the range, to
 * nearest, clamped; a NaN is the near plane. */
static void test_float(void)
{
    const float f[6] = { 0.0f, 1.0f, 0.5f, -3.0f, 7.0f, 0.0f / 0.0f };
    const uint8_t s[6] = { 0, 0, 9, 0, 0, 0 };
    uint8_t out[24];
    hle_zpack((const uint8_t *)f, s, HLE_ZSRC_F32, 6, 4, out, 0.0f);
    HT_EQ_U32(w32(out), 0x00000000u);
    HT_EQ_U32(w32(out + 4), 0xFFFFFF00u);
    HT_EQ_U32(w32(out + 8), 0x80000009u);       /* 8388607.5 to nearest even: 0x800000 */
    HT_EQ_U32(w32(out + 12), 0x00000000u);
    HT_EQ_U32(w32(out + 16), 0xFFFFFF00u);
    HT_EQ_U32(w32(out + 20), 0x00000000u);
    hle_zpack((const uint8_t *)f, NULL, HLE_ZSRC_F32, 6, 2, out, 0.0f);
    HT_EQ_U32(w16(out), 0x0000);
    HT_EQ_U32(w16(out + 2), 0xFFFF);
    HT_EQ_U32(w16(out + 4), 0x8000);            /* 32767.5 to nearest even */
    HT_EQ_U32(w16(out + 8), 0xFFFF);
}

/* Not a source or not a depth word: nothing written. */
static void test_rejects(void)
{
    const uint32_t d = 0x00123456u;
    uint8_t out[8];
    memset(out, 0x5A, sizeof(out));
    hle_zpack((const uint8_t *)&d, NULL, HLE_ZSRC_NONE, 1, 4, out, 0.0f);
    hle_zpack((const uint8_t *)&d, NULL, HLE_ZSRC_D24, 1, 3, out, 0.0f);
    hle_zpack((const uint8_t *)&d, NULL, 9, 1, 4, out, 0.0f);
    for (unsigned i = 0; i < sizeof(out); i++) {
        HT_EQ_U32(out[i], 0x5A);
    }
    HT_EQ_U32(hle_zsrc_bytes(HLE_ZSRC_D16), 2);
    HT_EQ_U32(hle_zsrc_bytes(HLE_ZSRC_D24), 4);
    HT_EQ_U32(hle_zsrc_bytes(HLE_ZSRC_F32), 4);
    HT_EQ_U32(hle_zsrc_bytes(HLE_ZSRC_NONE), 0);
}

static void test_zfloat(void)
{
    HT_EQ_U32(hle_zf24(1.0f), 0x3F800000u >> 7);
    HT_EQ_U32(hle_zf24(0.0f), 0);
    HT_EQ_U32(hle_zf24(HUGE_VALF), 0x00FEFFFFu);
    HT_EQ_U32(hle_zf16(511.9375f), 0xFFFFu);
    HT_EQ_U32(hle_zf16(1.0f), 7u << 12);
    HT_EQ_U32(hle_zf16(1.5f), 7u << 12 | 2048u);
}

int main(void)
{
    HT_RUN(test_d24s8);
    HT_RUN(test_d16);
    HT_RUN(test_widths);
    HT_RUN(test_float);
    HT_RUN(test_rejects);
    HT_RUN(test_zfloat);
    HT_DONE();
}
