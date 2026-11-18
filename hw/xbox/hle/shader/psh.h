/*
 * xbased HLE GPU — Xbox pixel shaders (NV2A register combiners and texture
 * modes) -> GLSL 4.50 (spec §5.8; pure). The key is the pixel-shader
 * render-state block (5659 render states 0..56, which SetPixelShader loads
 * from the D3DPIXELSHADERDEF); the combiner constants are push constants.
 *
 * Interface: inputs location 0 vDiffuse, 1 vSpecular, 2..5 vTex0..3, 6 vFog
 * (as vsh.h / ff.h); set 0 bindless images and samplers; HleFfPush.
 */
#ifndef HW_XBOX_HLE_SHADER_PSH_H
#define HW_XBOX_HLE_SHADER_PSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* D3DPIXELSHADERDEF / render-state indices (5659: D3DRS_PSALPHAINPUTS0 = 0) */
enum {
    HLE_PS_ALPHA_IN = 0,                /* [8] */
    HLE_PS_FINAL_ABCD = 8,
    HLE_PS_FINAL_EFG = 9,
    HLE_PS_C0 = 10,                     /* [8] */
    HLE_PS_C1 = 18,                     /* [8] */
    HLE_PS_ALPHA_OUT = 26,              /* [8] */
    HLE_PS_RGB_IN = 34,                 /* [8] */
    HLE_PS_COMPARE = 42,
    HLE_PS_FINAL_C0 = 43,
    HLE_PS_FINAL_C1 = 44,
    HLE_PS_RGB_OUT = 45,                /* [8] */
    HLE_PS_COUNT = 53,
    HLE_PS_TEXMODES = 54,
    HLE_PS_DOTMAP = 55,
    HLE_PS_INPUTTEX = 56,
    HLE_PS_REGS = 57,
};

/* PSTextureModes values */
enum {
    HLE_PST_NONE = 0, HLE_PST_PROJECT2D, HLE_PST_PROJECT3D, HLE_PST_CUBEMAP,
    HLE_PST_PASSTHRU, HLE_PST_CLIPPLANE, HLE_PST_BUMPENVMAP, HLE_PST_BUMPENVMAP_LUM,
    HLE_PST_BRDF, HLE_PST_DOT_ST, HLE_PST_DOT_ZW, HLE_PST_DOT_RFLCT_DIFF,
    HLE_PST_DOT_RFLCT_SPEC, HLE_PST_DOT_STR_3D, HLE_PST_DOT_STR_CUBE, HLE_PST_DPNDNT_AR,
    HLE_PST_DPNDNT_GB, HLE_PST_DOTPRODUCT, HLE_PST_DOT_RFLCT_SPEC_CONST,
};

/* The stage (2 or 3) whose mode is DOT_ZW, 0 for none: the pixel's depth is
 * the previous stage's dot over its own, in the buffer's units */
static inline unsigned hle_psh_dotzw_stage(uint32_t tex_modes)
{
    return ((tex_modes >> 10) & 0x1Fu) == HLE_PST_DOT_ZW ? 2u :
           ((tex_modes >> 15) & 0x1Fu) == HLE_PST_DOT_ZW ? 3u : 0u;
}

typedef struct HlePshKey {
    uint32_t rgb_in[8], rgb_out[8], alpha_in[8], alpha_out[8];
    uint32_t final_abcd, final_efg;
    uint32_t count;                     /* PSCombinerCount */
    uint32_t tex_modes;                 /* PSTextureModes */
    uint32_t dot_map;                   /* PSDotMapping */
    uint32_t input_tex;                 /* PSInputTexture */
    uint32_t compare;                   /* PSCompareMode */
    uint32_t ckey[4];                   /* the stages' colour keys (A8R8G8B8), where one is on
                                           (HLE_PSH_CKEY): its alpha 0 where the texture's
                                           format has none */
    uint8_t alpha_test;                 /* ALPHATESTENABLE */
    uint8_t alpha_func;                 /* D3DCMP_* - 0x200 (0..7) */
    uint8_t flat;
    uint8_t fog;                        /* 0 off; FOGTABLEMODE + 1 (NONE 1 .. LINEAR 4) */
    uint8_t fog_abs;                    /* the factor of |distance|: an NV2A _ABS fog mode
                                           the title pushed (D3D's are never _ABS) */
    uint8_t debug;                      /* output instead: 1 t0, 2 v0, 3 r0, 4 fog, 5 v1,
                                           6 t3, 7 r0.a, 8 t2, 9 shadow bands, 10 v1.a,
                                           11 t0.a, 12 t3.a, 13 fract(T3 s/q, t/q),
                                           14 |T3 str| / 4, 15 t3 and 16 t0 as sampled,
                                           17 shadow-buffer draws: (stored depth, r,
                                           result) / range, unblended; 18 magenta,
                                           19 t1, 20 r1, 21 |T2 str| / 4, 22 T2 str
                                           direction, 23 T2 NaN / Inf, 24 stage 2's
                                           cube along (0.3, 0.5, 0.8) */
    uint8_t point_sprite;               /* POINTSPRITEENABLE on a point list: texture
                                           unit 3's coordinates are the point's */
    /* Shadow buffers: stages whose texture is a depth surface (bits 0-3),
     * D3DRS_SHADOWFUNC - 0x200 (bits 4-6); per stage (2 bits) the depth
     * range the r coordinate is in: 1 D24 (2^24 - 1), 2 D16, 3 float. */
    uint8_t shadow;                     /* (bit 7, HLE_PSH_SHADOW_WORDS: those textures are the
                                           title's own depth words, 16-bit ones in a red
                                           channel or 24 over 8 in four bytes) */
    uint8_t shadow_fmt;
    uint8_t out_bgra;                   /* 8-bit (B8) target: it stores blue */
    uint8_t alpha_kill;                 /* stages with D3DTSS_ALPHAKILL (bits 0-3); bits 4-7: the
                                           stage's shadow surface is a 24-bit float buffer */
    uint8_t vol;                        /* stages whose texture is a volume (bits 0-3):
                                           2D-coordinate modes fetch it in 3D, as the NV2A
                                           (LLE): PROJECT2D at r = 0, bump-env at the
                                           coordinates' r; HLE_PSH_RECT: the stage the
                                           push block's fog.w names has its linear
                                           texture kept to its own size, part of a
                                           larger surface (ff.h HLE_NVRECT_GLSL);
                                           HLE_PSH_RECT4: each stage by its word of
                                           pc.scr (vertex programs); bits 6-7 free */
    uint8_t wbuf;                       /* W-buffering: depth = vW (HLE_WBUF_*_GLSL) */
    uint8_t fbfetch;                    /* stages (bits 0-3) whose texture is the colour
                                           target drawn into: its pixel, read as the
                                           input attachment (set 2) */
    uint8_t specular;                   /* SPECULARENABLE: D3D's own final combiner adds
                                           v1 (a shader without one) */
    uint8_t sblend;                     /* the NV2A's signed blend equations (ff.h
                                           HleFfPsKey.sblend) */
    uint8_t stipple;                    /* a filled polygon under SET_STIPPLE_ENABLE:
                                           g_hle_stipple[stipple - 1] (in the
                                           key's padding: keys without one stay the same) */
    uint8_t tex16;                      /* per stage (bits 2s, 2s + 1) its texture's kind where a
                                           HILO_1 dot stage, a bump stage or the colour-space
                                           conversion needs it: 1 R16B16, 2 Y16 (16-bit
                                           channels, psh.c tex16), 3 YUY2 / UYVY (its texels
                                           (Y, Cb, Cr), converted after the filter: psh.c
                                           yuv_stage). In the padding too */
    uint8_t bordc;                      /* stages whose sampler is a border-colour trio (vk/draw.c
                                           sampler_index: the stage's own, then the same with the
                                           standard transparent-black and opaque-white borders):
                                           the border colour as its raw ARGB (psh.c HLE_BORDC);
                                           bits 4-7: stages under a convolution filter (psh.c
                                           nvconv, the kernel in pc.tex bit 29) */
    uint8_t zcons;                      /* the depth word's early test (with HLE_PSH_NVCOL,
                                           HLE_PSH_ZWORD and HLE_PSH_ZTRUNC, vk/draw.c): the
                                           word is declared at least the rasterizer's depth
                                           (depth_greater), which the draw has biased toward
                                           the eye past what the two can differ by */
    uint8_t linez;                      /* a line whose geometry stage hands over its ends'
                                           screen depths (vLineZ, psh.c nvlzd): its depth
                                           from them (in the padding too) */
    uint8_t dither;                     /* SET_DITHER_ENABLE into a 16-bit target: the colour
                                           dithered to its channels' steps as it is written,
                                           1 R5G6B5, 2 X1R5G5B5 / A1R5G5B5 (vk/draw.c
                                           dither_mode, HLE_DITHER_GLSL; in the padding, for
                                           the fixed-function stages' keys as well) */
} HlePshKey;
/* The NV2A's dither into a 16-bit target, a 4x4 ordered pattern added below
 * the channels' step before the colour is rounded to it: m the step's
 * fraction of 1 per channel (1/31, 1/63 or 1/31). Above 1x the pattern is the
 * scaled image's pixels', finer than the console's, as the steps it hides
 * (without it Fable's sky had its light in rings from 2x) */
#define HLE_DITHER_GLSL \
    "float nvbayer() {\n" \
    "  uvec2 q = uvec2(gl_FragCoord.xy) & 3u;\n" \
    "  const float b[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,\n" \
    "                                3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);\n" \
    "  return (b[q.y * 4u + q.x] + 0.5) / 16.0;\n" \
    "}\n" \
    "vec3 nvdither(vec3 c, vec3 m) { return clamp(c + (nvbayer() - 0.5) * m, 0.0, 1.0); }\n"
/* (nvdither: the GPU's own rounding to the nearest step after it; nvbayer
 * alone: a threshold under a cut, psh.c's 16-bit blend in the shader) */
/* SET_STIPPLE_PATTERN's 32 words, each pattern a title drew with
 * (pb/pbdraw.c adds them, the generator reads the ones keys name): a pixel is
 * drawn where bit (31 - x mod 32) ^ 24 of word 31 - (y mod 32) is set (the
 * words' bytes in memory order, each from its top bit; nxdk_pgraph_tests'
 * Stipple_tests, fitted on the console's Asymmetric_On with no pixel off) */
#define HLE_PSH_STIPPLES 255
extern uint32_t g_hle_stipple[HLE_PSH_STIPPLES][32];
/* HlePshKey.dot_map's bits 16-31 (PSDotMapping has 12): per stage, 4 bits each
 * (stage 0 lowest), the channels its texture holds as signed bytes,
 * SET_TEXTURE_FILTER 31:28 (bit 0 A, 1 R, 2 G, 3 B). There, a key without any
 * is the same bytes as before, and recorded pipelines stay valid. */
#define HLE_PSH_TSIGN(k) ((k)->dot_map >> 16)
/* bits 12-15: a signed stage (bit 12 + stage) whose texture is magnified
 * point-sampled: its four texels are not blended (psh.c samps) */
#define HLE_PSH_TPOINT(k) (((k)->dot_map >> 12) & 15)
/* HlePshKey.input_tex bits 0-3 (PSInputTexture uses 16, 20-21): a signed
 * stage (bit = stage) whose texture was defined with its signed bytes
 * flipped (hle_tex_sign_xor): read centred on 0x80, filtered as signed. */
#define HLE_PSH_TFLIP(k) ((k)->input_tex & 15)
/* HlePshKey.tex_modes bits 20-27 (PSTextureModes has 20): per stage, 2 bits
 * (stage 0 lowest), the colour key's operation, SET_TEXTURE_CONTROL0 1:0: a
 * fetched texel equal to the stage's SET_COLOR_KEY_COLOR (ckey) has its
 * alpha 0 (1), is (0, 0, 0, 0) (2), or the pixel is not drawn (3). Bits
 * 28-31: the stage's texture format has no alpha, and the key's alpha is
 * not compared (X1R5G5B5, X8R8G8B8). */
#define HLE_PSH_CKEY(k, s) (((k)->tex_modes >> (20 + 2 * (s))) & 3u)
#define HLE_PSH_CKEY_NOALPHA(k, s) (((k)->tex_modes >> (28 + (s))) & 1u)
#define HLE_PSH_CKEY_BITS(s, op, noalpha) \
    ((uint32_t)((op) & 3u) << (20 + 2 * (s)) | (uint32_t)((noalpha) ? 1u : 0u) << (28 + (s)))
/* HlePshKey.compare bits 16-19 (PSCompareMode has 16): the stage's texture is
 * of another kind than its mode fetches, and is read as the NV2A reads it
 * (psh.c): a 2D texture under a mode that fetches a cube (CUBEMAP,
 * DOT_STR_CUBE, DOT_RFLCT_DIFF, DOT_RFLCT_SPEC) or a volume (DOT_STR_3D), a
 * cube under PROJECT2D. */
#define HLE_PSH_TKIND(k, s) (((k)->compare >> (16 + (s))) & 1u)
#define HLE_PSH_TKIND_BIT(s) (1u << (16 + (s)))
/* HlePshKey.compare bits 20-23: the stage's texture has its own border
 * (cmd.h tex_border, its word in pc.scr: programs only) */
#define HLE_PSH_BORDER_BIT(s) (1u << (20 + (s)))
/* HlePshKey.input_tex bit 8 (PSInputTexture has 16 and 20-21; bits 0-3 the stages
 * flipped at definition): SET_CONTROL0's colour-space conversion (cmd.h csc), every
 * stage's texel taken as (Y, Cb, Cr) to RGB (a YUY2 / UYVY one's always is, tex16 3) */
#define HLE_PSH_CSC (1u << 8)
/* HlePshKey.input_tex bit 9: stage 3's DOT_STR_3D texture is a cube: read as its first
 * face, a 2D texture at (dot1, dot2) over their q, clamped to its edge (psh.c) */
#define HLE_PSH_CUBE3D (1u << 9)
/* HlePshKey.input_tex bits 22-29: stage s's cylinder wrap of u (22 + 2s) and v (23 + 2s),
 * SET_TEXTURE_ADDRESS's: its coordinates interpolated the short way round (psh.c) */
#define HLE_PSH_CYLWRAP (0xFFu << 22)
/* HlePshKey.compare bit 24: texture coordinates without perspective (cmd.h tex_affine) */
#define HLE_PSH_TEX_AFFINE (1u << 24)
/* HlePshKey.compare bit 25: a one-pixel line by the NV2A's rule (ff.h HLE_NVLINE) */
#define HLE_PSH_NVLINE (1u << 25)
/* HlePshKey.compare bit 26: that line with SET_SMOOTHING_CONTROL's bit 0 clear (a step a pixel) */
#define HLE_PSH_NVLINE_THIN (1u << 26)
#define HLE_PSH_SHADOW_WORDS 0x80u      /* HlePshKey.shadow */
/* HlePshKey.compare bit 27: the shadow stages are point filtered: one texel's
 * compare, no percentage-closer weights */
#define HLE_PSH_SHADOW_POINT (1u << 27)
/* HlePshKey.compare bit 28: the depth target is a 16-bit float one: a depth
 * below its smallest exponent is stored as 0 */
#define HLE_PSH_ZF16 (1u << 28)
/* HlePshKey.compare bit 29: a triangle's colours by the NV2A's weights (psh.c
 * nvcol: the corners' colours, pervertex) */
#define HLE_PSH_NVCOL (1u << 29)
/* HlePshKey.compare: the depth word from the corners' own depths (psh.c nvzword;
 * with HLE_PSH_NVCOL and HLE_PSH_ZTRUNC, a device with doubles) */
#define HLE_PSH_ZWORD (1u << 30)
/* HlePshKey.compare: a float depth buffer's image holds its words (with HLE_PSH_ZF16 the 16-bit format's, else the 24-bit one's): psh.c nvfword */
#define HLE_PSH_ZFWORD (1u << 31)
#define HLE_PSH_RECT 0x10u              /* HlePshKey.vol: a linear texture kept to its size */
#define HLE_PSH_RECT4 0x20u             /* HlePshKey.vol: each stage's in pc.scr (programs) */
#define HLE_PSH_TWO_SIDE 0x40u          /* HlePshKey.vol: SET_TWO_SIDE_LIGHT_EN: a back face's
                                           v0 / v1 are the program's oB0 / oB1 */
#define HLE_PSH_ZTRUNC 0x80u            /* HlePshKey.vol: an integer depth buffer's depth
                                           truncated to its steps */

/* Key from the render-state array (rs[0..56] = the pixel-shader block). */
void hle_psh_key(const uint32_t *rs, HlePshKey *k);
/* A line by the NV2A's rule whose pixel stage interpolates the colours itself
 * (nvcol1): its geometry stage hands both ends' (vsh.h). */
static inline bool hle_psh_line_ends(const HlePshKey *k)
{
    return (k->compare & HLE_PSH_NVLINE) && !(k->vol & HLE_PSH_TWO_SIDE) && !k->flat;
}

/* GLSL into buf; returns its length, 0 when it does not fit. Unsupported
 * texture modes sample as PROJECT2D and set *unsupported (may be NULL). */
size_t hle_psh_glsl(const HlePshKey *k, char *buf, size_t cap, unsigned *unsupported);

#endif
