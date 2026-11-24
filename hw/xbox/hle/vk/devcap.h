/*
 * xbased HLE GPU — the dev control's frame capture (dev builds only,
 * XBASED_DEVCTL; native/app/win_devctl.c drives it).
 *
 * A capture lists every draw of the next whole frame (between two swaps) as
 * JSON: its index in the frame, primitive and count, vertex program or fixed
 * function, pixel stage, colour and depth targets, textures, the depth,
 * alpha, blend, cull, stencil and fog states, its pipeline key's hash, and
 * how it went (its own pipeline, the interpreters', skipped while it builds,
 * failed, dropped and why). detail: that draw also has its render and
 * texture-stage states whole, its transform, vertex layout and program
 * constants in <path>.draw<N>.txt, and its shaders' texts in
 * <path>.draw<N>.vert / .frag (as the build makes them).
 *
 * Skips: draws of each frame by index left out, "a-b,c,..." (none: ""), or
 * only those drawn with a leading '!': which draw breaks a picture.
 */
#ifndef HW_XBOX_HLE_VK_DEVCAP_H
#define HW_XBOX_HLE_VK_DEVCAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef XBASED_DEVCTL
/* (any thread) the next frame captured to path (JSON), detail: a draw's
 * index for its whole state and texts, -1 none. Written once whole (a
 * temporary file renamed). */
void hle_devcap_arm(const char *path, int detail);
/* (any thread) the skips, as above */
void hle_devcap_skip(const char *spec);
/* (any thread) the draw path's counters as JSON members (no braces) */
size_t hle_devcap_stats(char *buf, size_t cap);
/* (any thread) the per-frame record from row since on, as JSON arrays [row, swap time us, the render
 * thread's build time us, in-place builds, interpreter draws, draws skipped while building, new
 * pipelines, draws]; *next: the row after the last given */
size_t hle_devcap_frames(uint64_t since, char *buf, size_t cap, uint64_t *next);
#endif

#endif
