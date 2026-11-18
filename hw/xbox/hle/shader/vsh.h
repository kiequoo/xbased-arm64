/*
 * xbased HLE GPU — NV2A vertex program (XDK xvs microcode) -> GLSL 4.50
 * (spec §5.8; pure). Decoding uses subprojects/nv2a_vsh_cpu; the GLSL is
 * HLE's own (independent of the LLE render path).
 *
 * Interface (shared with vk/draw.c and the pixel generators):
 * - inputs: layout(location = N) in vN_in for each bit of in_mask (vec4;
 *   ivec4 for int_mask: S32K shorts; uvec2 for cmp_mask: the two 16-bit
 *   halves of a packed 11:11:10 dword, unpacked here); unbound registers
 *   read their current values vcur[N] (SetVertexData*);
 * - constants: set 1, binding 0, std140 { vec4 c[192]; vec4 vcur[16]; };
 * - push constants: HleFfPush (vp = screen -> NDC mapping, zscale);
 * - outputs: location 0 oD0, 1 oD1, 2..5 oT0..oT3, 6 fog (x), 7 oB0, 8 oB1.
 */
#ifndef HW_XBOX_HLE_SHADER_VSH_H
#define HW_XBOX_HLE_SHADER_VSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HLE_VSH_MAX_SLOTS 136
#define HLE_VSH_CONSTANTS 192
#define HLE_VSH_CUR_REGS  16    /* current attribute values after the constants */

#define HLE_VSH_XF 4u
/* HleVshKey.spec_off with HLE_VSH_XF: a program's stand-in while its exact
 * form builds (vk/draw.c): its operations in plain floats, the outputs the
 * pixel stage takes from the exact form written all the same. It builds as
 * fast as a program did before HLE_VSH_XF: the exact operations took the
 * driver up to 2 s a program (Prince of Persia - Warrior Within's loading
 * screens and first scenes, a stall each time one was first drawn), and
 * still 0.7 s with only those oPos comes from (a skinned program's
 * fifty). */
#define HLE_VSH_XF_FAST 8u
/* HleVshKey.spec_off, set for the text alone (vk/draw.c pipeline_build, not in
 * the key): the pipeline draws no points, the point rules (pc.mvp[2].y 0 there)
 * left out of the shader: ~3 ms of the driver's build each */
#define HLE_VSH_NO_POINTS 16u
/* HleVshKey.fog_table: the NV2A's fog value, K0 + K1 d by the lighting unit
 * (ltlib.h) cut to 12 bits of fraction, in vFog for the pixel stage's table
 * (psh.c fog 5 .. 8); _ABS: of the coordinate's magnitude (LINEAR_ABS) */
#define HLE_VSH_FOG_NV     2u
#define HLE_VSH_FOG_NV_ABS 4u
#define HLE_VSH_FOG_NV_LIN 8u      /* a LINEAR mode: an infinite coordinate is no fog, whatever K1 */
#define HLE_VSH_FOG_NV_RAD 16u     /* SET_FOG_GEN_MODE RADIAL under a program (vsh.c) */
#define HLE_VSH_FOG_NV_CARRY 32u   /* a program writing no oFog: the fog a vertex 6 before
                                      left (vsh.c nvcarry, vk/draw.c, pbdraw.c g_fogc) */

typedef struct HleVshKey {
    uint32_t in_mask;           /* vertex attribute registers bound (bit per vN); bits
                                   16-31: of those, "3W" arrays (size 7, D3D's
                                   FLOAT2H): their three components x, y and w, z 0
                                   (the XDK's d3d8types.h; read as x, y, z they put
                                   a projective q in r, and w was 1) */
    uint16_t int_mask;          /* of those, fetched as signed integers */
    uint16_t cmp_mask;          /* of those, packed 11:11:10 dwords */
    uint8_t flat;               /* SHADEMODE FLAT */
    uint8_t spec_off;           /* bit 0, specular off: oD1 / oB1 read as (0, 0, 0, 1); bit 1,
                                   SET_LIGHT_CONTROL without ALPHA_FROM_MATERIAL: their
                                   alpha 1, a program's too; bit 2 (HLE_VSH_XF): the
                                   program by the transform unit's own arithmetic
                                   (xflib.h) */
    uint8_t wbuf;               /* W-buffering: oPos.w to the pixel stage (vW) */
    uint8_t zfree;              /* no depth test: the program's z clips nothing */
    uint8_t fog_table;          /* FOGTABLEMODE other than NONE (a program writing
                                   no oFog: distance 0, else factor 1); HLE_VSH_FOG_NV:
                                   the fog value as the NV2A's lighting unit makes it
                                   from the coordinate */
    uint8_t zclamp;             /* depth clamped, not clipped (the pipeline's: in the
                                   padding, the key's size is the pipeline record's) */
    uint16_t w1_mask;           /* of in_mask, three-component arrays fetched with
                                   their four-component format (the driver's lacks
                                   vertex support: AMD's for 8 and 16 bits), the
                                   fourth read past them: w is 1 (in the padding too) */
    uint16_t pt_size;           /* a point list without point parameters
                                   (SET_POINT_PARAMS_ENABLE off): HLE_VSH_PT_FIXED |
                                   SET_POINT_SIZE, the size in eighths of a pixel,
                                   whatever the program leaves in oPts; 0: oPts */
    uint16_t s1_mask;           /* of in_mask, S1 arrays (normalized shorts), fetched as
                                   integers: (2x + 1) / 65535, the console's, on their
                                   s1_cnt components (nxdk_vsh_tests' Vertex Data Array
                                   Format S1: 0 reads 1 / 65535, -32767 -65533 / 65535;
                                   the host's SNORM was x / 32767) */
    uint32_t s1_cnt;            /* 2 bits an attribute: its components - 1 */
} HleVshKey;
#define HLE_VSH_PT_FIXED 0x8000u

/* The operations by the transform unit's arithmetic: HLE_VSH_XF, not its stand-in */
static inline bool hle_vsh_xf_exact(const HleVshKey *k)
{
    return (k->spec_off & (HLE_VSH_XF | HLE_VSH_XF_FAST)) == HLE_VSH_XF;
}

/* Parses nslots 4-dword instructions (stopping at the FINAL one) and writes
 * the GLSL into buf. Returns the length, or 0 when a slot does not decode,
 * no instruction is FINAL, or the text does not fit. */
size_t hle_vsh_glsl(const uint32_t *tokens, unsigned nslots, const HleVshKey *k,
                    char *buf, size_t cap);
/* The same shader around body, GLSL statements in main that set the
 * registers (v0..v15 read, R0..R11 scratch; oPos in screen space as a
 * program leaves it, oD0, oD1, oB0, oB1, oT0..oT3, oFog, oPts); fogw: body
 * writes oFog. Returns the length, 0 when it does not fit. */
size_t hle_vsh_glsl_wrap(const HleVshKey *k, bool lt, const char *body, bool fogw, char *buf, size_t cap);
/* The geometry stage of a one-pixel line by the NV2A's rule (ff.h HLE_NVLINE)
 * after a program of key k: a quad around the line, the program's outputs
 * along it (affine: without perspective), the end points in 1/16 pixel at
 * location 12. tri: 0 lines in; 1 + the winding culled (ff.h
 * HLE_TOPO_NVCULL_SHIFT) triangles in, their three sides drawn so; 5 triangles
 * in, their side 1 - 2 alone (HLE_TOPO_NVSIDE). width:
 * SET_LINE_WIDTH's 1/8 pixels (location 13; 0 draws nothing). ends: the
 * diffuse and specular colours of the line's two ends, flat (its start's at
 * 0 and 1, its end's at 14 and 15), for the pixel stage's nvcol1. Returns
 * the length, 0 when it does not fit. */
size_t hle_nvline_gs_glsl(const HleVshKey *k, bool affine, unsigned tri, unsigned width, bool ends,
                         char *buf, size_t cap);
/* The geometry stage of a flat-shaded quad (four vertices in, lines with
 * adjacency): its two triangles, the colours those of its last vertex. */
size_t hle_quadflat_gs_glsl(const HleVshKey *k, bool affine, char *buf, size_t cap);

#endif
