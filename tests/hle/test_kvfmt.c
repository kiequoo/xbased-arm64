/*
 * hakuX HLE GPU — Kelvin values in a surface's format (hw/xbox/hle/pb/kvfmt.c).
 */
#include "hle_test.h"

#include "hw/xbox/hle/pb/kvfmt.h"

/* A clear's colour: D3D pushes it in the target's format. */
static void test_clear_color(void)
{
    /* R5G6B5 */
    HT_EQ_U32(hle_kv_clear_color(3, 0xFFFF), 0xFFFFFFFFu);
    HT_EQ_U32(hle_kv_clear_color(3, 0xF800), 0xFFFF0000u);
    HT_EQ_U32(hle_kv_clear_color(3, 0x07E0), 0xFF00FF00u);
    HT_EQ_U32(hle_kv_clear_color(3, 0x001F), 0xFF0000FFu);
    HT_EQ_U32(hle_kv_clear_color(3, 0x8410), 0xFF838183u);      /* 16 of 31, 32 of 63 */
    /* X1R5G5B5: five bits a channel from bit 10 down; read as R5G6B5 or as 32-bit, white (0x7FFF)
     * was a blue-green */
    HT_EQ_U32(hle_kv_clear_color(1, 0x7FFF), 0x00FFFFFFu);
    HT_EQ_U32(hle_kv_clear_color(2, 0x7FFF), 0xFFFFFFFFu);
    HT_EQ_U32(hle_kv_clear_color(2, 0x7C00), 0xFFFF0000u);
    HT_EQ_U32(hle_kv_clear_color(2, 0x03E0), 0xFF00FF00u);
    HT_EQ_U32(hle_kv_clear_color(2, 0x001F), 0xFF0000FFu);
    HT_EQ_U32(hle_kv_clear_color(1, 0x4210), 0x00838383u);
    HT_EQ_U32(hle_kv_clear_color(2, 0xFFFF8000), 0xFF000000u);     /* the bits above: not colour */
    /* A8R8G8B8, B8 and G8B8: the value as it is */
    for (uint32_t cf = 8; cf <= 10; cf++) {
        HT_EQ_U32(hle_kv_clear_color(cf, 0x12345678), 0x12345678u);
        HT_EQ_U32(hle_kv_clear_color(cf, 0xFEDCBA98), 0xFEDCBA98u);
    }
    HT_EQ_U32(hle_kv_clear_color(0, 0x12345678), 0x12345678u);
    /* the 32-bit formats without a whole alpha: the spare bits as the format writes them */
    HT_EQ_U32(hle_kv_clear_color(4, 0xFF345678), 0x00345678u);      /* X8, Z: 0 */
    HT_EQ_U32(hle_kv_clear_color(4, 0x12345678), 0x00345678u);
    HT_EQ_U32(hle_kv_clear_color(5, 0x00345678), 0xFF345678u);      /* X8, O: 1 */
    HT_EQ_U32(hle_kv_clear_color(5, 0x12345678), 0xFF345678u);
    HT_EQ_U32(hle_kv_clear_color(6, 0xFF345678), 0x7F345678u);      /* X1A7, Z: the top bit 0 */
    HT_EQ_U32(hle_kv_clear_color(6, 0x12345678), 0x12345678u);
    HT_EQ_U32(hle_kv_clear_color(7, 0x00345678), 0x80345678u);      /* X1A7, O: the top bit 1 */
    HT_EQ_U32(hle_kv_clear_color(7, 0x7F345678), 0xFF345678u);
    /* blending into a target with forced alpha: its alpha counts as 1 */
    HT_EQ_U32(hle_kv_blend_factor_forced_alpha(0x304), 1);
    HT_EQ_U32(hle_kv_blend_factor_forced_alpha(0x305), 0);
    static const uint32_t same[] = { 0, 1, 0x300, 0x301, 0x302, 0x303, 0x306, 0x307, 0x308, 0x8001, 0x8004 };
    for (unsigned i = 0; i < sizeof(same) / sizeof(same[0]); i++) {
        HT_EQ_U32(hle_kv_blend_factor_forced_alpha(same[i]), same[i]);
    }
}

/* The state methods that name one of a list keep their state for anything else. */
static void test_takes(void)
{
    static const uint32_t factors[] = { 0, 1, 0x300, 0x301, 0x302, 0x303, 0x304, 0x305, 0x306, 0x307, 0x308,
                                        0x8001, 0x8002, 0x8003, 0x8004 };
    for (unsigned i = 0; i < sizeof(factors) / sizeof(factors[0]); i++) {
        HT_CHECK(hle_kv_takes(0x0344, factors[i]));
        HT_CHECK(hle_kv_takes(0x0348, factors[i]));
    }
    static const uint32_t not_factors[] = { 2, 0x2FF, 0x309, 0x8000, 0x8005, 0xFFFFFFFFu, 0x10000 };
    for (unsigned i = 0; i < sizeof(not_factors) / sizeof(not_factors[0]); i++) {
        HT_CHECK(!hle_kv_takes(0x0344, not_factors[i]));
        HT_CHECK(!hle_kv_takes(0x0348, not_factors[i]));
    }
    static const uint32_t eqs[] = { 0x8006, 0x8007, 0x8008, 0x800A, 0x800B, 0xF005, 0xF006 };
    for (unsigned i = 0; i < sizeof(eqs) / sizeof(eqs[0]); i++) {
        HT_CHECK(hle_kv_takes(0x0350, eqs[i]));
    }
    HT_CHECK(!hle_kv_takes(0x0350, 0));
    HT_CHECK(!hle_kv_takes(0x0350, 0x8005));
    HT_CHECK(!hle_kv_takes(0x0350, 0x8009));
    HT_CHECK(!hle_kv_takes(0x0350, 0x800C));
    HT_CHECK(!hle_kv_takes(0x0350, 0xF004));
    HT_CHECK(!hle_kv_takes(0x0350, 0xF007));
    for (uint32_t v = 0x1F0; v < 0x220; v++) {
        HT_CHECK(hle_kv_takes(0x0354, v) == (v >= 0x200 && v <= 0x207));
    }
    HT_CHECK(!hle_kv_takes(0x0354, 0));
    HT_CHECK(hle_kv_takes(0x037C, 0x1D00));
    HT_CHECK(hle_kv_takes(0x037C, 0x1D01));
    HT_CHECK(!hle_kv_takes(0x037C, 0));
    HT_CHECK(!hle_kv_takes(0x037C, 0x1D02));
    HT_CHECK(hle_kv_takes(0x03A0, 0x900));
    HT_CHECK(hle_kv_takes(0x03A0, 0x901));
    HT_CHECK(!hle_kv_takes(0x03A0, 0));             /* (read as clockwise before: the state stays) */
    HT_CHECK(!hle_kv_takes(0x03A0, 0x902));
    HT_CHECK(hle_kv_takes(0x043C, 0));
    HT_CHECK(hle_kv_takes(0x043C, 0x1FF));
    HT_CHECK(!hle_kv_takes(0x043C, 0x200));
    HT_CHECK(!hle_kv_takes(0x043C, 0x80000008u));
    /* every other method takes anything */
    for (uint32_t m = 0; m < 0x2000; m += 4) {
        if (m == 0x0344 || m == 0x0348 || m == 0x0350 || m == 0x0354 || m == 0x037C || m == 0x03A0 ||
            m == 0x043C) {
            continue;
        }
        HT_CHECK(hle_kv_takes(m, 0xDEADBEEFu));
        HT_CHECK(hle_kv_takes(m, 0));
    }
}

#define WC(lo, hi) ((uint32_t)(hi) << 16 | (lo))

/* The window clip: an inclusive set draws inside any of its rectangles. */
static void test_window_clip(void)
{
    int32_t r[4] = { -1, -1, -1, -1 };
    uint32_t hr[8] = { 0 }, vr[8] = { 0 };
    HT_CHECK(!hle_kv_window_clip(hr, vr, false, 640, 480, r));     /* none in use */
    hr[0] = WC(0, 639);
    vr[0] = WC(0, 479);
    HT_CHECK(!hle_kv_window_clip(hr, vr, false, 640, 480, r));     /* D3D's "no clip" */
    /* a portrait's frame */
    hr[0] = WC(100, 199);
    vr[0] = WC(50, 149);
    HT_CHECK(hle_kv_window_clip(hr, vr, false, 640, 480, r));
    HT_EQ_INT(r[0], 100);
    HT_EQ_INT(r[1], 50);
    HT_EQ_INT(r[2], 200);
    HT_EQ_INT(r[3], 150);
    /* two apart: their box */
    hr[3] = WC(300, 319);
    vr[3] = WC(10, 19);
    HT_CHECK(hle_kv_window_clip(hr, vr, false, 640, 480, r));
    HT_EQ_INT(r[0], 100);
    HT_EQ_INT(r[1], 10);
    HT_EQ_INT(r[2], 320);
    HT_EQ_INT(r[3], 150);
    /* the first set back over the whole surface, another left in its slot
     * (nxdk_pgraph_tests' window clip suite ends so): nothing is clipped.
     * It clipped to the one left, and every draw of the suites after it
     * outside that rectangle was gone. */
    hr[0] = WC(0, 640);
    vr[0] = WC(0, 480);
    r[0] = -7;
    HT_CHECK(!hle_kv_window_clip(hr, vr, false, 640, 480, r));
    HT_EQ_INT(r[0], -7);
    /* ... wherever in the eight the whole-surface one is */
    hr[0] = WC(100, 199);
    vr[0] = WC(50, 149);
    hr[7] = WC(0, 639);
    vr[7] = WC(0, 479);
    HT_CHECK(!hle_kv_window_clip(hr, vr, false, 640, 480, r));
    /* over a smaller surface the same rectangle is the whole of it */
    hr[7] = vr[7] = 0;
    hr[3] = vr[3] = 0;
    hr[0] = WC(0, 255);
    vr[0] = WC(0, 255);
    HT_CHECK(hle_kv_window_clip(hr, vr, false, 640, 480, r));
    HT_CHECK(!hle_kv_window_clip(hr, vr, false, 256, 256, r));
    /* an exclusive set: not a box */
    hr[0] = WC(100, 199);
    vr[0] = WC(50, 149);
    HT_CHECK(!hle_kv_window_clip(hr, vr, true, 640, 480, r));
}

/* A rectangle's word is its slot's and every later slot's. */
static void test_window_clip_spread(void)
{
    uint32_t first = 0;
    HT_EQ_U32(hle_kv_window_clip_spread(0x02C0, &first), 8);       /* horizontal, slot 0: all eight */
    HT_EQ_U32(first, 0x02C0);
    HT_EQ_U32(hle_kv_window_clip_spread(0x02C4, &first), 7);
    HT_EQ_U32(first, 0x02C4);
    HT_EQ_U32(hle_kv_window_clip_spread(0x02DC, &first), 1);       /* slot 7: itself */
    HT_EQ_U32(hle_kv_window_clip_spread(0x02E0, &first), 8);       /* vertical, slot 0 */
    HT_EQ_U32(first, 0x02E0);
    HT_EQ_U32(hle_kv_window_clip_spread(0x02F4, &first), 3);
    HT_EQ_U32(first, 0x02F4);
    HT_EQ_U32(hle_kv_window_clip_spread(0x02BC, &first), 0);       /* the type, the method after: not one */
    HT_EQ_U32(hle_kv_window_clip_spread(0x0300, &first), 0);
    /* the console's two cases: slot 1 then slot 0 leaves slot 0's everywhere; slot 0 then slot 1,
     * slot 0's in the first and slot 1's in the rest, and the box is both */
    uint32_t hr[8] = { 0 }, vr[8] = { 0 };
    int32_t r[4];
    const uint32_t r0h = WC(60, 123), r0v = WC(60, 123), r1h = WC(0, 63), r1v = WC(0, 63);
    for (unsigned i = 1; i < 8; i++) { hr[i] = r1h; vr[i] = r1v; }
    for (unsigned i = 0; i < 8; i++) { hr[i] = r0h; vr[i] = r0v; }
    HT_CHECK(hle_kv_window_clip(hr, vr, false, 640, 480, r));
    HT_EQ_INT(r[0], 60);
    HT_EQ_INT(r[2], 124);
    for (unsigned i = 1; i < 8; i++) { hr[i] = r1h; vr[i] = r1v; }
    HT_CHECK(hle_kv_window_clip(hr, vr, false, 640, 480, r));
    HT_EQ_INT(r[0], 0);
    HT_EQ_INT(r[2], 124);
}

int main(void)
{
    HT_RUN(test_window_clip_spread);
    HT_RUN(test_window_clip);
    HT_RUN(test_clear_color);
    HT_RUN(test_takes);
    HT_DONE();
}
