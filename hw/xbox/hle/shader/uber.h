/*
 * xbased HLE GPU — interpreter shaders ("uber"): one vertex stage and one pixel
 * stage that read the draw's vertex program and pixel-stage key from a buffer
 * (HleUberCfg) instead of having them compiled in, for the draws whose own
 * pipeline (the stand-in, vk/draw.c) is still being built: they draw at once,
 * the specific pipeline replaces them when it is there. The arithmetic of their
 * own shaders (vsh.c, psh.c); the subset they cover is hle_uber_vs_ok /
 * hle_uber_ps_ok's, the other keys wait for their own pipeline as before. Pure:
 * text and words.
 */
#ifndef HW_XBOX_HLE_SHADER_UBER_H
#define HW_XBOX_HLE_SHADER_UBER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hw/xbox/hle/shader/psh.h"
#include "hw/xbox/hle/shader/vsh.h"

/* The config, words of a UBO both stages read (binding 0 of a set of its own,
 * after the draws' layout's: uvec4 u[HLE_UBER_WORDS / 4]): the vertex key's,
 * the pixel key's, the program's steps. */
#define HLE_UBER_STEP_WORDS 12
#define HLE_UBER_STEPS_AT   64
#define HLE_UBER_WORDS      (HLE_UBER_STEPS_AT + HLE_VSH_MAX_SLOTS * HLE_UBER_STEP_WORDS)
#define HLE_UBER_BYTES      (HLE_UBER_WORDS * 4)

/* word 4's switches */
#define HLE_UBER_V_FLAT     1u
#define HLE_UBER_V_SPECOFF  2u      /* spec_off: oD1 / oB1 read (0, 0, 0, 1) */
#define HLE_UBER_V_WBUF     8u
#define HLE_UBER_V_ZFREE    16u

/* GLSL of the two stages into buf (NUL-terminated); their length, 0 if it did not fit.
 * set: the config's. The pixel stage's depth: 0 the rasterizer's, 1 cut to a D24
 * buffer's steps (HLE_PSH_ZCUT), 3 W-buffering's (the others the rasterizer's):
 * hle_uber_fs_depth. */
size_t hle_uber_vs_glsl(char *buf, size_t cap, unsigned set);
size_t hle_uber_fs_glsl(char *buf, size_t cap, unsigned set, unsigned depth, bool full);
unsigned hle_uber_fs_depth(const HlePshKey *k);
/* The pixel stage's full form: shadow buffers, bump-env and dot-product stages,
 * signed channels (the key has one); without them it builds in half the
 * driver's time, first */
bool hle_uber_fs_full(const HlePshKey *k);

/* The program's steps as the vertex stage reads them, from word HLE_UBER_STEPS_AT:
 * how many (0: a slot that does not parse, or no final one) */
unsigned hle_uber_prog(const uint32_t *tokens, unsigned nslots, uint32_t *cfg);

/* The keys the interpreters draw as their own stages would (plain floats) */
bool hle_uber_vs_ok(const HleVshKey *k, const uint32_t *tokens, unsigned nslots);
bool hle_uber_ps_ok(const HlePshKey *k);

/* The fixed function's vertex stage (ffhw.h, push-buffer mode's key as a
 * program's tokens): its own text, its key from word HLE_UBER_STEPS_AT
 * (false: tokens are no such key), the keys it draws (none on this base: its
 * text is the program interpreter's) */
size_t hle_uber_ff_vs_glsl(char *buf, size_t cap, unsigned set);
bool hle_uber_ff(const uint32_t *tokens, unsigned nslots, uint32_t *cfg);
bool hle_uber_ff_ok(const HleVshKey *k, const uint32_t *tokens, unsigned nslots);

/* The keys' words (cfg[0 .. HLE_UBER_STEPS_AT)) */
void hle_uber_keys(const HleVshKey *vk, const HlePshKey *pk, uint32_t nsteps, uint32_t *cfg);

#endif
