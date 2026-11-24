/*
 * xbased HLE GPU — Kelvin values that come in a surface's own format.
 */
#ifndef HW_XBOX_HLE_PB_KVFMT_H
#define HW_XBOX_HLE_PB_KVFMT_H

#include <stdint.h>

/* SET_COLOR_CLEAR_VALUE as A8R8G8B8, for a colour surface of format cf
 * (SET_SURFACE_FORMAT 3:0). D3D pushes the value in the surface's format: a
 * 16-bit one's in its 5:5:5 or 5:6:5 bits, a 32-bit one's as it is. The
 * bits a format has no alpha for are written as the format says (its name's
 * Z: 0, O: 1): X1R5G5B5's one, X8R8G8B8's eight, X1A7R8G8B8's top one (its
 * low seven are the value's). */
uint32_t hle_kv_clear_color(uint32_t cf, uint32_t v);

/* A blend factor for a colour target whose format forces its alpha bits
 * (X1R5G5B5, X8R8G8B8, either spare-bit rule): the NV2A blends as if the
 * target's alpha were 1, so DST_ALPHA (0x304) is ONE and ONE_MINUS_DST_ALPHA
 * (0x305) ZERO (xemu's test on a console, its pull request 2478); any other
 * factor as it is. */
static inline uint32_t hle_kv_blend_factor_forced_alpha(uint32_t f)
{
    return f == 0x0304 ? 1 : f == 0x0305 ? 0 : f;
}

#endif
