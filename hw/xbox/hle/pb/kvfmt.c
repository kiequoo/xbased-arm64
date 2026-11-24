/*
 * xbased HLE GPU — Kelvin values that come in a surface's own format (kvfmt.h).
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
