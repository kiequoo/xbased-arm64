/*
 * hakuX HLE GPU — Kelvin values that come in a surface's own format (kvfmt.h).
 */
#include "hw/xbox/hle/pb/kvfmt.h"

static uint32_t wide(uint32_t x, uint32_t max)
{
    return x * 255 / max;
}

uint32_t hle_kv_clear_color(uint32_t cf, uint32_t v)
{
    switch (cf & 0xF) {
    case 1:     /* X1R5G5B5_Z1R5G5B5 */
    case 2:     /* X1R5G5B5_O1R5G5B5 */
        return ((cf & 0xF) == 2 ? 0xFF000000u : 0) | wide((v >> 10) & 0x1F, 31) << 16 |
               wide((v >> 5) & 0x1F, 31) << 8 | wide(v & 0x1F, 31);
    case 3:     /* R5G6B5 */
        return 0xFF000000u | wide((v >> 11) & 0x1F, 31) << 16 | wide((v >> 5) & 0x3F, 63) << 8 |
               wide(v & 0x1F, 31);
    case 4:     /* X8R8G8B8_Z8R8G8B8 */
        return v & 0x00FFFFFFu;
    case 5:     /* X8R8G8B8_O8R8G8B8 */
        return v | 0xFF000000u;
    case 6:     /* X1A7R8G8B8_Z1A7R8G8B8 */
        return v & 0x7FFFFFFFu;
    case 7:     /* X1A7R8G8B8_O1A7R8G8B8 */
        return v | 0x80000000u;
    default:    /* A8R8G8B8, B8, G8B8 */
        return v;
    }
}

bool hle_kv_takes_listed(uint32_t m, uint32_t v)
{
    switch (m) {
    case 0x0344:        /* SET_BLEND_FUNC_SFACTOR */
    case 0x0348:        /* SET_BLEND_FUNC_DFACTOR */
        return v <= 1 || (v >= 0x0300 && v <= 0x0308) || (v >= 0x8001 && v <= 0x8004);
    case 0x0350:        /* SET_BLEND_EQUATION: add, min, max, subtract, reverse subtract, and the
                         * signed reverse subtract and add */
        return (v >= 0x8006 && v <= 0x8008) || v == 0x800A || v == 0x800B || v == 0xF005 || v == 0xF006;
    case 0x0354:        /* SET_DEPTH_FUNC */
        return v >= 0x0200 && v <= 0x0207;
    case 0x037C:        /* SET_SHADE_MODE: flat, smooth */
        return v == 0x1D00 || v == 0x1D01;
    case 0x03A0:        /* SET_FRONT_FACE: clockwise, counter-clockwise */
        return v == 0x0900 || v == 0x0901;
    case 0x043C:        /* SET_POINT_SIZE: 9 bits, in eighths */
        return v <= 0x1FF;
    default:
        return true;
    }
}

bool hle_kv_window_clip(const uint32_t hr[8], const uint32_t vr[8], bool exclusive, uint32_t w,
                        uint32_t h, int32_t r[4])
{
    int32_t b[4] = { INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
    if (exclusive) {
        return false;
    }
    for (unsigned i = 0; i < 8; i++) {
        const uint32_t x = hr[i], y = vr[i];
        const int32_t x1 = x & 0xFFF, x2 = ((x >> 16) & 0xFFF) + 1, y1 = y & 0xFFF, y2 = ((y >> 16) & 0xFFF) + 1;
        if (!x && !y) {
            continue;
        }
        if (x1 == 0 && y1 == 0 && x2 >= (int32_t)w && y2 >= (int32_t)h) {
            return false;
        }
        b[0] = x1 < b[0] ? x1 : b[0];
        b[1] = y1 < b[1] ? y1 : b[1];
        b[2] = x2 > b[2] ? x2 : b[2];
        b[3] = y2 > b[3] ? y2 : b[3];
    }
    if (b[0] == INT32_MAX) {
        return false;
    }
    for (unsigned i = 0; i < 4; i++) {
        r[i] = b[i];
    }
    return true;
}
