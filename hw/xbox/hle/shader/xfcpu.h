/*
 * xbased HLE - the NV2A's transform unit on the CPU: the arithmetic of
 * xflib.h (the vertex programs' GLSL) in C, and a program run by it.
 *
 * The runs the GPU can't make go through here: a program that writes the
 * constants (CXT_WRITE_EN, pbdraw.c cxt_after), the state programs
 * (LAUNCH_TRANSFORM_PROGRAM), the fog a vertex left for the ones after it
 * (pbdraw.c g_fogc). They are the GPU's to the bit: the product cut toward
 * zero, the adder's 29-bit window cut (nxdk_low_level_nv2a_tests' vsh_test
 * measures both on the console: its "win_rz" model), RCP / RSQ by the
 * table and two steps; the rest as the GLSL's (vsh.c k_lib). Pure.
 */
#ifndef HLE_SHADER_XFCPU_H
#define HLE_SHADER_XFCPU_H

#include <stdbool.h>
#include <stdint.h>

#include "subprojects/nv2a_vsh_cpu/src/nv2a_vsh_disassembler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The unit's operations on the bits of floats (xflib.h's xfm, xfs, xfrcp, xfrsq) */
uint32_t hle_xf_mul(uint32_t a, uint32_t b);
uint32_t hle_xf_sum(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
uint32_t hle_xf_rcp(uint32_t x, bool rcc);
uint32_t hle_xf_rsq(uint32_t x);

/* One vertex through the parsed program: v the inputs (16 x 4), c the
 * constants (192 x 4, its writes made there and flagged in cwrote, 192),
 * out the outputs (13 x 4: oPos 0, oD0 3 .. oT3 12) as the program left
 * them, from what the caller set (oFog's place: the fog it carries). The
 * temporaries and A0 start at 0. */
void hle_xf_run(const Nv2aVshProgram *prog, const float *v, float *c, bool *cwrote, float *out);

/* The steps of prog (its first ns slots parsed) oFog.x depends on, in their
 * order, the other steps left out and the others' operations NOPs: hle_xf_run
 * on *slice leaves the oFog.x the whole program leaves, from the same
 * values (liveness by component back from it; both operations of a step read
 * before either writes). slice->steps NULL: no step changes it. False, and
 * nothing made, for a program writing a constant. Freed by
 * nv2a_vsh_program_destroy. */
bool hle_xf_fog_slice(const Nv2aVshProgram *prog, uint32_t ns, Nv2aVshProgram *slice);

#ifdef __cplusplus
}
#endif

#endif
