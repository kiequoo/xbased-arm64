/*
 * xbased HLE GPU — Kelvin values in a surface's format (hw/xbox/hle/pb/kvfmt.c).
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

int main(void)
{
    HT_RUN(test_clear_color);
    HT_DONE();
}
