/*
 * hakuX HLE GPU — Kelvin values that come in a surface's own format.
 */
#ifndef HW_XBOX_HLE_PB_KVFMT_H
#define HW_XBOX_HLE_PB_KVFMT_H

#include <stdbool.h>
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

/* Whether Kelvin method m takes the value v. A state method that names one
 * of a list (the blend factors and equation, the depth function, the shade
 * mode, the front face) or has a field's width (the point size) keeps its
 * state when given anything else: the NV2A reports the write as an error and
 * changes nothing (xemu's tests on a console, its pull requests 2230, 497,
 * 489, 557, 2527). True for every other method. */
bool hle_kv_takes_listed(uint32_t m, uint32_t v);

static inline bool hle_kv_takes(uint32_t m, uint32_t v)
{
    /* (the listed ones lie in 0x0344..0x043C: the rest without a call) */
    return m - 0x0344u > 0x043Cu - 0x0344u || hle_kv_takes_listed(m, v);
}

/* A window clip rectangle's word goes into its slot and every slot after it
 * (xemu's rule, from tests on a console: nxdk_pgraph_tests' window clip
 * suite sets slot 1 and then slot 0, and the chip clips to slot 0's alone;
 * slot 0 and then slot 1, to both): the first of SET_WINDOW_CLIP_HORIZONTAL
 * / _VERTICAL's methods m writes and how many, or 0 if m is not one. Setting
 * the first rectangle so sets all eight, which is how one rectangle clips
 * though the set is "inside any of the eight". */
static inline unsigned hle_kv_window_clip_spread(uint32_t m, uint32_t *first)
{
    if (m < 0x02C0 || m >= 0x0300) {
        return 0;
    }
    *first = m;
    return 8 - ((m >> 2) & 7);
}

/* The window clip's box: SET_WINDOW_CLIP_HORIZONTAL / _VERTICAL's 8
 * rectangles (corners in pixels, both included: low word the first, high
 * word the last) over a w x h surface, as [x1, y1, x2, y2). An inclusive set
 * draws inside any of them: one that covers the whole surface means nothing
 * is clipped, whatever the others hold (a rectangle left in another slot by
 * whoever set them last); those all zero are not in use; the rest's box
 * clips. False: no clip (none in use, one over everything, or an exclusive
 * set, which a box can't do). */
bool hle_kv_window_clip(const uint32_t hr[8], const uint32_t vr[8], bool exclusive, uint32_t w,
                        uint32_t h, int32_t r[4]);

#endif
