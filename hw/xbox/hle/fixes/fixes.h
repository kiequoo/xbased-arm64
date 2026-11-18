/*
 * xbased HLE GPU — title fixes: what a title, or the titles that use Direct3D
 * as a whole, need on top of the base. See docs/hle-gpu-layers.md.
 *
 * The base is the NV2A: every rule in it is a behaviour of the chip, measured
 * on a console (nxdk_pgraph_tests is the reference), and it is the same for
 * every title. It never asks which title is running. This layer is the only
 * place that does. A fix is a bit with a name and the titles it is on for by
 * default (fixes.c); the base code tests hle_fix() where the fix changes what
 * it does, and the fix's reason is written beside its row in the table.
 *
 * Two kinds of row, told apart by the name's prefix:
 *   d3d_*   what the titles with the D3D library (Direct3D 8 for the Xbox)
 *           need on top of the exact chip: a correction a game's content
 *           needs that the exact behaviour does not give it, or a strategy
 *           that is cheaper and equivalent while D3D stands between the
 *           title and the chip (its locks and fences order what the chip
 *           does against what the CPU reads).
 *   title_* one title's own fix (title ids in fixes.c), for a game that does
 *           something the chip allows and the base does not yet reproduce.
 *
 * A fix never changes the base's code path for the other titles, and a base
 * rule never moves into this table to make a pgraph picture pass: if the
 * chip does it, it is the base's.
 *
 * A/B on any title: XBASED_HLE_FIXES at start, or the live property
 * debug.xbased.fixes: names separated by commas or spaces, "+name" (or the
 * bare name) on and "-name" off on top of the title's defaults, "+all" /
 * "-all" every fix. Empty: the title's defaults.
 */
#ifndef HW_XBOX_HLE_FIXES_FIXES_H
#define HW_XBOX_HLE_FIXES_FIXES_H

#include <stdbool.h>
#include <stdint.h>

#include "hw/xbox/hle/core/xbe.h"

enum {
    /* A vertex position the rasterizer takes truncated to 1/16 pixel each
     * (the chip's: the base). D3D's own screen-space offset (0.53125) is the
     * truncation's rounding bias, which lands a position on a pixel centre
     * for D3D's content; here every position moves by that bias alone
     * instead (ff.h HLE_SUBPIXEL_GLSL). The host computes a pixel's depth
     * from the positions it is given and the console's does not from the
     * truncated ones, so with the truncation a decal and the surface under
     * it (other vertices, truncated apart by up to 1/16 pixel) tilt against
     * each other and the decal fights its surface (Burnout 3's road stains
     * and manhole covers, Dead or Alive 2's eaves shadows). To go once the
     * depth is made as the console's. */
    HLE_FIX_D3D_SNAP_BIAS = 1u << 0,
    /* Colour targets' memory: the base keeps every one of them, whatever its
     * size or use, as the chip's (a program that drives the chip reads any
     * target once the chip is idle, the back buffer it has yet to show
     * among them). D3D reaches a frame buffer's memory through a lock of the
     * frame on screen and a small target's after a fence, so with this fix
     * the front buffer's own path serves the frame buffers and the small or
     * CPU-used ones are written back at each unbind (pb/pbdraw.c coh_display,
     * writeback). Cheaper: a game's thousands of draws a frame pay no guard
     * on every target. */
    HLE_FIX_D3D_LAZY_TARGETS = 1u << 1,
    /* The base waits at each PUT until the decoder has read up to it: the
     * chip is close behind the push, and a program that writes the memory a
     * draw it just pushed reads, with no wait for the GPU, relies on it
     * (pbmode.c catch_up). D3D's fences order those writes. */
    HLE_FIX_D3D_LAZY_PUSHES = 1u << 2,
    /* The base waits for a visibility report at its GET_REPORT, as the chip
     * wrote it as it ran: a program that reads the report once the chip is
     * idle reads the one of the test before otherwise (pb/pbdraw.c). D3D
     * polls the report's status word and waits on its own. */
    HLE_FIX_D3D_LAZY_REPORTS = 1u << 3,
    /* Methods pushed in runs: D3D build 4928's push buffer carries long
     * non-incrementing runs (the vertex data, the constants) that the
     * decoder takes whole (pbmode.c k_count_runs). */
    HLE_FIX_D3D_METHOD_RUNS = 1u << 4,
    /* A colour target over a depth target's memory, the depth drawn after it:
     * the base has the colour image take the depth's words before its first
     * partial write (pb/pbdraw.c zcoh_newer_over: the chip's depth writes went
     * into those bytes), which lands the depth and waits for the GPU. D3D
     * places a target there once it is done with the depth buffer and draws
     * its contents anew (Fable's 128 x 128 target inside its 256 x 256 shadow
     * map, both swizzled, every frame: 0.586 of 1.0.104's fps with the wait).
     * To go once that copy is made on the GPU (a remapping ZREAD pass). */
    HLE_FIX_D3D_LAZY_ZALIAS = 1u << 5,
};

/* The fixes on for the title running now (0 before one starts) */
extern uint32_t g_hle_fixes;

static inline bool hle_fix(uint32_t f)
{
    return (g_hle_fixes & f) != 0;
}

/* A title starts: its defaults, then XBASED_HLE_FIXES and debug.xbased.fixes */
void hle_fixes_init(const HleXbeInfo *x);
/* Now and then (the draw path's property poll): debug.xbased.fixes changed */
void hle_fixes_poll(void);

/* The names of the fixes in mask, comma separated ("-" for none), for logs
 * and tests */
void hle_fixes_names(uint32_t mask, char *out, unsigned len);
/* The defaults for a title (tests: no state kept) */
uint32_t hle_fixes_defaults(const HleXbeInfo *x);
/* mask with spec's "+name" / "-name" applied (tests); unknown names are
 * counted in *bad, if given */
uint32_t hle_fixes_apply(uint32_t mask, const char *spec, unsigned *bad);

#endif
