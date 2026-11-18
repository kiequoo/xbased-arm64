/*
 * xbased HLE GPU — fixed-function shader generators (spec §5.8 FF; pure).
 * Keys are built by the back end from the draw state (render/texture-state
 * arrays + FVF); the generators emit GLSL 4.50 for glslang.
 *
 * Interface shared with vk/draw.c:
 * - vertex inputs: location 0 position, 1 normal, 2 diffuse, 3 specular,
 *   4..7 texcoord 0..3 (VIDS supplies the formats from the FVF);
 * - set 0: binding 0 sampled images[], binding 1 samplers[] (bindless);
 * - push constants: HleFfPush.
 */
#ifndef HW_XBOX_HLE_SHADER_FF_H
#define HW_XBOX_HLE_SHADER_FF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Xbox FVF bits (D3DFVF_*) */
#define HLE_FVF_POSITION_MASK 0x00E
#define HLE_FVF_XYZ           0x002
#define HLE_FVF_XYZRHW        0x004
#define HLE_FVF_NORMAL        0x010
#define HLE_FVF_PSIZE         0x020
#define HLE_FVF_DIFFUSE       0x040
#define HLE_FVF_SPECULAR      0x080
#define HLE_FVF_TEXCOUNT_MASK 0xF00
#define HLE_FVF_TEXCOUNT_SHIFT 8

/* Texture coordinate components for coordinate set i of an FVF (1-4). */
static inline unsigned hle_fvf_tex_size(uint32_t fvf, unsigned i)
{
    static const unsigned k_size[4] = { 2, 3, 4, 1 };
    return k_size[(fvf >> (16 + 2 * i)) & 3];
}

#define HLE_FF_STAGES 4
/* With POINTSPRITEENABLE the runtime builds the FF cascade from texture
 * stage 3 alone (4134 0x1e95e0 starts its stage loop at 3): the NV2A gives
 * unit 3 the point-sprite coordinates. The FF keys then describe that one
 * stage as their stage 0. */
#define HLE_FF_SPRITE_STAGE 3

/* Texture-coordinate generation (TEXCOORDINDEX >> 16) */
enum { HLE_TCI_PASSTHRU = 0, HLE_TCI_CAMERANORMAL, HLE_TCI_CAMERAPOSITION,
       HLE_TCI_CAMERAREFLECTION, HLE_TCI_SPHEREMAP };
/* Fog distance source (HleFfVsKey.fog_src), as D3D programs the NV2A fog
 * generator: FOGTABLEMODE NONE takes the vertex specular alpha as the fog
 * factor, RANGEFOGENABLE the eye distance, otherwise the eye depth. */
enum { HLE_FOG_SRC_OFF = 0, HLE_FOG_SRC_PLANAR, HLE_FOG_SRC_RADIAL, HLE_FOG_SRC_SPEC_ALPHA };
/* Light types (D3DLIGHTTYPE) */
enum { HLE_LIGHT_POINT = 1, HLE_LIGHT_SPOT = 2, HLE_LIGHT_DIRECTIONAL = 3 };
#define HLE_FF_LIGHTS 8

typedef struct HleFfVsKey {
    uint32_t fvf;
    uint8_t texcoord[HLE_FF_STAGES];  /* TSS TEXCOORDINDEX (low 3 bits) per stage */
    uint8_t texgen[HLE_FF_STAGES];    /* HLE_TCI_* per stage */
    uint8_t texxform[HLE_FF_STAGES];  /* texture transform: element count (0 off) |
                                         0x10 projected */
    uint8_t ntex;                     /* stages with a texture coordinate output */
    uint8_t flat;                     /* SHADEMODE FLAT */
    uint8_t lighting;                 /* LIGHTING (without a vertex normal N.L is 0) */
    uint8_t nlights;                  /* enabled lights */
    uint8_t light_type[HLE_FF_LIGHTS];
    uint8_t specular;                 /* SPECULARENABLE */
    uint8_t local_viewer;             /* LOCALVIEWER */
    uint8_t normalize;                /* NORMALIZENORMALS */
    uint8_t colorvertex;              /* COLORVERTEX */
    uint8_t src[4];                   /* diffuse, ambient, specular, emissive material
                                         source: 0 material, 1 color1, 2 color2 */
    uint8_t blend;                    /* vertex blending: matrices (0 = off, 2..4) */
    uint8_t blend_mix;                /* the last weight is 1 - the vertex's others
                                         (D3DRS_VERTEXBLEND 1 / 3 / 5); else every
                                         weight is the vertex's (2 / 4 / 6) */
    uint8_t fog_src;                  /* HLE_FOG_SRC_* (OFF: eye depth, no block) */
    uint8_t points;                   /* point lists: 1 POINTSIZE (or the vertex PSIZE),
                                         2 scaled by eye distance (POINTSCALEENABLE) */
    uint8_t wbuf;                     /* W-buffering (XYZRHW): vW = 1 / rhw */
    uint8_t cmp_normal;               /* the normal is NV2A CMP (11:11:10 signed), fetched
                                         as two 16-bit halves (a declaration's) */
    uint8_t zclamp;                   /* depth clamped, not clipped (the pipeline's: in
                                         the padding, as HleVshKey's) */
} HleFfVsKey;

/* The FF uniform block (set 1, binding 0: the dynamic window the vertex
 * programs use for their constants), vec4 slots: */
enum {
    HLE_FFU_WV = 0,                   /* world0 * view, column-major mat4 */
    HLE_FFU_PROJ = 4,                 /* projection, column-major mat4 */
    HLE_FFU_NORMAL = 8,               /* inverse transpose of WV (3 columns) */
    HLE_FFU_AMBIENT = 11,             /* AMBIENT render state */
    HLE_FFU_MATERIAL = 12,            /* diffuse, ambient, specular, emissive, (power) */
    HLE_FFU_LIGHTS = 20,              /* per light 7 vec4: diffuse, specular, ambient,
                                         position (eye), direction (eye),
                                         (range, falloff, att0, att1),
                                         (att2, cos theta/2, cos phi/2, 0) */
    HLE_FFU_TEXMAT = 76,              /* 4 texture matrices (column-major mat4) */
    HLE_FFU_BLEND = 92,               /* worldN * view for N = 1..3 (column-major) */
    HLE_FFU_POINT = 104,              /* (POINTSIZE, _MIN, _MAX, viewport height),
                                         (POINTSCALE_A, _B, _C, target pixel scale) */
    HLE_FFU_COUNT = 106,
};
#define HLE_FFU_LIGHT_VEC4 7

/* The FF VS reads the uniform block (lighting, texgen, texture transforms,
 * vertex blending or fog by eye distance); otherwise it only needs the push
 * constants. */
static inline int hle_ff_vs_needs_ubo(const HleFfVsKey *k)
{
    int n = k->lighting || k->blend || k->points || k->fog_src == HLE_FOG_SRC_PLANAR ||
            k->fog_src == HLE_FOG_SRC_RADIAL;
    for (unsigned s = 0; s < HLE_FF_STAGES; s++) {
        n |= k->texgen[s] || k->texxform[s];
    }
    return n;
}

typedef struct HleFfStage {
    uint32_t colorop, colorarg0, colorarg1, colorarg2;
    uint32_t alphaop, alphaarg0, alphaarg1, alphaarg2;
    uint32_t resultarg;
    uint8_t texture;                  /* a texture is bound to this stage; bit 1: one with its
                                         own border (its reported size in pc.scr[stage],
                                         cmd.h tex_border; programs only) */
    uint8_t tex_dim;                  /* 2 = 2D, 3 = cube, 4 = volume */
    uint8_t alpha_kill;               /* D3DTSS_ALPHAKILL: texel alpha 0 discards */
    uint8_t unset;                    /* no texture set (D3D's Textures[stage] NULL): an
                                         op reading the texture passes CURRENT through */
} HleFfStage;

typedef struct HleFfPsKey {
    HleFfStage st[HLE_FF_STAGES];
    uint8_t nstages;                  /* stages up to the first COLOROP DISABLE */
    uint8_t alpha_test;               /* ALPHATESTENABLE */
    uint8_t alpha_func;               /* D3DCMP_* - 0x200 (0..7) */
    uint8_t specular;                 /* SPECULARENABLE (add specular) */
    uint8_t flat;
    uint8_t fog;                      /* 0 off; FOGTABLEMODE + 1 (NONE 1 .. LINEAR 4) */
    uint8_t point_sprite;             /* POINTSPRITEENABLE on a point list: stage 0
                                         (texture stage 3, see HLE_FF_SPRITE_STAGE)
                                         samples at the point's coordinates */
    uint8_t out_bgra;                 /* 8-bit (B8) target: it stores blue */
    uint8_t wbuf;                     /* W-buffering: depth = vW (HLE_WBUF_*_GLSL) */
    uint8_t sblend;                   /* the NV2A's signed blend equations, ONE / ONE (the
                                         fixed-function blend is off): the output and
                                         the target's pixel (uFb, set 2) as two's-
                                         complement bytes, 1 dst + src (ADDSIGNED), 2
                                         dst - src (REVSUBTRACTSIGNED), saturated; 3 and
                                         4 the NV2A's, any factors: the
                                         target's byte as it is, 0 to 255 (draw.c
                                         signed_blend) */
    uint8_t fbfetch;                  /* stages (bits) whose texture is the colour target:
                                         its pixel as the earlier draws left it (uFb),
                                         as the NV2A's texture unit reads the memory
                                         the draw renders */
    uint8_t tex_affine;               /* texture coordinates without perspective (cmd.h) */
} HleFfPsKey;

/* Push constants (std430 layout, <= 256 bytes: 256 used); HLE_PUSH_GLSL
 * declares the same block for every generator. */
typedef struct HleFfPush {
    float mvp[16];                    /* world * view * projection, column-major */
    float vp[2];                      /* D3D screen pixels -> NDC: 2 aa / target w,
                                         -2 aa / target h, then + (-1, 1) (aa: the
                                         NV2A's antialiasing scale, else 1) */
    uint32_t bump3[2];                /* stage 3's BUMPENVMAT (as bump) */
    uint32_t tex[HLE_FF_STAGES];      /* image index | sampler index << 16 */
    uint32_t tfactor;                 /* TEXTUREFACTOR (D3DCOLOR) */
    uint32_t bumpl[3];                /* stages 1..3: BUMPENVLSCALE, LOFFSET (signed
                                         16-bit 4096ths, vk/draw.c fix12x2) */
    float alpha_ref;                  /* ALPHAREF / 255 */
    uint32_t flags;                   /* bit s: stage s samples a linear texture
                                         (texel coordinates); bit 8 + s: that
                                         texture is a surface rendered at the
                                         resolution scale (not a small target,
                                         at 1x: vk/draw.c stage_slot); bits
                                         16-23: the target's scale (point
                                         sizes), 24-31: the resolution scale
                                         (those surfaces' texels) */
    float zscale;                     /* vertex programs: 1 / depth maximum (2^-n for
                                         the integer formats: HLE_ZNORM_GLSL) */
    uint32_t fog_color;               /* FOGCOLOR (D3DCOLOR) */
    float fog[4];                     /* FOGSTART, FOGEND, FOGDENSITY, then (bits) the
                                         stage, size and address modes a linear
                                         texture is kept inside (HLE_NVRECT_GLSL) */
    uint32_t psc[20];                 /* pixel shaders: C0[8], C1[8], final C0, C1
                                         (D3DCOLOR), W-buffer offset and slope */
    float scr[4];                     /* fixed function: D3D screen pixels = xy *
                                         scr.xy + scr.zw (XYZRHW: the vertex's;
                                         transformed: NDC), the screen-space offset
                                         included */
    uint32_t bump[4];                 /* pixel shaders: BUMPENVMAT 00, 01 | 10, 11
                                         (half-float pairs) of stage 1 (xy) and
                                         stage 2 (zw); each stage has its own
                                         (Halo 2's water distorts its reflection
                                         a quarter as much as the rest) */
} HleFfPush;

#define HLE_PUSH_MEMBERS_GLSL \
    "  mat4 mvp;\n  vec2 vp;\n  uvec2 bump3;\n  uvec4 tex;\n  uint tfactor;\n" \
    "  uint bumpl[3];\n" \
    "  float alpha_ref;\n  uint flags;\n  float zscale;\n  uint fog_color;\n" \
    "  vec4 fog;\n  uint psc[20];\n  vec4 scr;\n  uvec4 bump;\n"
#define HLE_PUSH_GLSL \
    "layout(push_constant, std430) uniform Push {\n" HLE_PUSH_MEMBERS_GLSL "} pc;\n"
/* The same block where the push constants are smaller (Xclipse's 128 bytes):
 * a storage buffer, std430 as well (vk/draw.c, set 3) */
#define HLE_PUSH_SSBO_GLSL \
    "layout(set = 3, binding = 0, std430) readonly buffer Push {\n" HLE_PUSH_MEMBERS_GLSL "} pc;\n"

/* The block in a storage buffer (set by the back end before any shader is made) */
extern bool g_hle_push_ssbo;

/* A linear texture is addressed by texel within its own size (its image
 * rect), clamped, bordered or wrapped at its edges. One sampled from a larger
 * surface (a target's top-left part: Munch's Oddysee samples the top 128 rows
 * of its 128 x 2048 shadow target as a 128 x 128 texture, clamped) has its
 * texel coordinates kept inside that size first, as the stage's address mode
 * says: else the sampler's edges are the surface's, and the rows past the
 * texture's (black) drew a box over the ground. The push block's fog.w (bits,
 * vk/draw.c lin_rect; one stage a draw; vertex programs: a word a stage in
 * pc.scr, HLE_PSH_RECT4): width - 1 in 0-11, height - 1 in
 * 12-23 (the NV2A's are 4096 at most), U's and V's mode in 24-25 and 26-27 (0
 * clamp, 1 border, 2 wrap, 3 mirror), the stage's low bit in 28 and high bit
 * in 31, bit 29 set and 30 clear: as a float always a normal, finite number
 * (a driver may flush a denormal or quiet a NaN on its way through a float),
 * never 0. A coordinate outside the rect in border mode goes far outside the
 * image, to the sampler's border colour; every mode ends kept half a texel of
 * the image (e: 0.5 over the resolution scale for a scaled surface) inside the
 * edges, so filtering never reaches a texel past them (a wrap's seam blends
 * with its edge texel instead of the opposite one). Emitted only for the keys
 * that ask (HlePshKey.vol's HLE_PSH_RECT, HLE_PSH_RECT4). */
#define HLE_NVRECT_GLSL \
    "float nvaxis(float u, float n, uint m, float e) {\n" \
    "  if (m == 1u && (u < 0.0 || u > n)) return -65536.0;\n" \
    "  if (m == 2u) u -= n * floor(u / n);\n" \
    "  else if (m == 3u) { u -= 2.0 * n * floor(u / (2.0 * n)); u = u < n ? u : 2.0 * n - u; }\n" \
    "  return clamp(u, e, n - e);\n" \
    "}\n" \
    "vec2 nvrectr(vec2 uv, uint r, float e) {\n" \
    "  return vec2(nvaxis(uv.x, float((r & 0xFFFu) + 1u), (r >> 24) & 3u, e),\n" \
    "              nvaxis(uv.y, float(((r >> 12) & 0xFFFu) + 1u), (r >> 26) & 3u, e));\n" \
    "}\n" \
    "vec2 nvrect(vec2 uv, float e) { return nvrectr(uv, floatBitsToUint(pc.fog.w), e); }\n"

/* Mali (vk/quirks.h HLE_QUIRK_Z_EQ_W, set with g_hle_push_ssbo): a clip-space
 * z equal to w corrupts the depth buffer (PPSSPP's EQUAL_WZ_CORRUPTS_DEPTH,
 * ARMSX2): the vertex shaders end by moving such a vertex a hair inside. */
extern bool g_hle_glsl_zw;
#define HLE_ZW_NUDGE_GLSL "  if (gl_Position.z == gl_Position.w) gl_Position.z *= 0.999999;\n"

/* No depthClamp (vk/vkdev.h HleCaps.depth_clamp off: Mali's Bifrost): a
 * z-clamped draw's vertices (the key's zclamp) are clamped to the depth range
 * instead, so the near and far planes don't clip them; depth across a
 * primitive that crosses them comes from its clamped corners. */
extern bool g_hle_glsl_zclamp;
/* fast precision (vk/precision.h): the pixel stage without the per-pixel
 * steps xbased added after 1.0.94 (psh.c fast_cut) */
extern bool g_hle_psh_fast;
extern bool g_hle_glsl_f64;        /* the device has doubles in shaders (psh.c: a line's shares) */
extern bool g_hle_glsl_i64;        /* and 64-bit integers (psh.c nvzfast) */
#define HLE_ZCLAMP_GLSL \
    "  if (gl_Position.w > 0.0) gl_Position.z = clamp(gl_Position.z, 0.0, gl_Position.w);\n"

/* No descriptor indexing (HleCaps.bindless off: Mali's Bifrost, PowerVR's
 * Rogue): a pixel shader's bindless arrays become the draw's own set (vk/draw.c
 * fixed_bind): sized (each stage's images, then the default sampler and each
 * stage's), indexed without nonuniformEXT (the push block's indices, the same
 * for the whole draw). g_hle_glsl_fixed: hle_glsl_fixed_slots rewrites the
 * generated text; its new length, 0 if it doesn't fit in cap. */
extern bool g_hle_glsl_fixed;
size_t hle_glsl_fixed_slots(char *src, size_t cap);

static inline const char *hle_push_glsl(void)
{
    return g_hle_push_ssbo ? HLE_PUSH_SSBO_GLSL : HLE_PUSH_GLSL;
}

/* W-buffering (D3DRS_ZENABLE = D3DZB_USEW, NV2A z_perspective): the stored
 * depth is the perspective-correct w (in depth units, as the vertex program
 * left it) with the polygon offset in w: ZOFFSET + ZSLOPESCALE * max |dw|
 * (psc[18], psc[19], float bits; zero when SOLIDOFFSETENABLE is off). The
 * pixel generators declare vW (location 9, from the program vertex shader). */
#define HLE_WBUF_IN_GLSL "layout(location = 9) in float vW;\n"
/* Screen-space depth z (0 .. the buffer's maximum, 2^n - 1) to [0, 1] as
 * `out`: pc.zscale is 2^-n for the integer formats (pbdraw.c), and
 * z 2^-n + z 2^-2n is z / (2^n - 1) to float precision, both products exact
 * and summed once ("precise": not folded into one product, whose scale
 * float32 can't hold). The float nearest 1 / (2^24 - 1) is above it: a
 * vertex a step in front of the far plane (2^24 - 2) came out at 1.0, and
 * Super Monkey Ball Deluxe's movies, drawn there over a depth buffer cleared
 * to the far plane, failed their LESS test (black, the subtitles over it
 * drawn). Any other scale (a float depth buffer's, pbdraw.c zfloat_scale)
 * is the plain product. */
#define HLE_ZNORM_GLSL(z, out) \
    "  precise float " out " = " z " * pc.zscale + " z " * (pc.zscale == uintBitsToFloat(0x33800000u) ||\n" \
    "      pc.zscale == uintBitsToFloat(0x37800000u) ? pc.zscale * pc.zscale : 0.0);\n"
/* the integer formats store w truncated (nxdk_pgraph_tests' W_buffering: a
 * step below ours where w's fraction was over a half) */
#define HLE_WBUF_TRUNC_GLSL \
    "  float wCull = wRaw;\n" \
    "  if (pc.zscale == uintBitsToFloat(0x37800000u) || pc.zscale == uintBitsToFloat(0x33800000u))\n" \
    "    wRaw = floor(wRaw);\n"
/* first in main (derivatives before any discard), stored last */
#define HLE_WBUF_CALC_GLSL \
    "  float wRaw = vW + uintBitsToFloat(pc.psc[18]) + uintBitsToFloat(pc.psc[19]) *\n" \
    "      max(abs(dFdx(vW)), abs(dFdy(vW)));\n" \
    HLE_WBUF_TRUNC_GLSL \
    HLE_ZNORM_GLSL("wRaw", "wN") \
    "  float wDepth = clamp(wN, 0.0, 1.0);\n"
#define HLE_WBUF_STORE_GLSL "  gl_FragDepth = wDepth;\n"
/* The NV2A's slope term under W-buffering is one number for the polygon:
 * max |dw| at its first pixel in top-left order (xemu's notes; nxdk_pgraph_tests'
 * W_buffering prints the same ZSLOPESCALE * slope on every pixel of its floor,
 * 0x1B9F1 over w from 0xF5 to 0x3A). With the barycentrics (key wbuf 2) the
 * triangle's corners come back from their screen-space derivatives (the
 * inverse of the corners' matrix), 1/w is linear on the screen, and
 * |dw| = |d(1/w)| w^2 at that pixel; degenerate ones keep the pixel's own. */
#define HLE_WBUF_BARY_EXT_GLSL "#extension GL_EXT_fragment_shader_barycentric : require\n"
/* the corners' w as they are (their 1/w exact), the pixel's from the
 * perspective barycentrics */
#define HLE_WBUF_IN_BARY_GLSL "layout(location = 9) pervertexEXT in float vWv[3];\n"
#define HLE_WBUF_CALC_BARY_GLSL \
    "  vec3 bWv = vec3(vWv[0], vWv[1], vWv[2]);\n" \
    "#ifndef HLE_WBUF_VW\n#define HLE_WBUF_VW dot(gl_BaryCoordEXT, bWv)\n#endif\n" \
    "#ifdef HLE_WBUF_VWD\n" \
    "  double vWd = HLE_WBUF_VWD;\n" \
    "  float vW = float(vWd);\n" \
    "#else\n" \
    "  float vW = HLE_WBUF_VW;\n" \
    "#endif\n" \
    "  float wSl = max(abs(dFdx(vW)), abs(dFdy(vW)));\n" \
    "  {\n" \
    "    vec3 bL = gl_BaryCoordNoPerspEXT, bX = dFdxFine(bL), bY = dFdyFine(bL);\n" \
    "    vec2 bp = gl_FragCoord.xy;\n" \
    "    mat3 bMi = mat3(bX, bY, bL - bX * bp.x - bY * bp.y);\n" \
    "    float bdt = determinant(bMi);\n" \
    "    vec3 rv = 1.0 / bWv;\n" \
    "    if (abs(bdt) > 1.0e-30 && !isinf(bdt) && !isnan(bdt) && all(greaterThan(bWv, vec3(0.0)))) {\n" \
    "      mat3 bM = inverse(bMi);\n" \
    "      vec2 c0 = bM[0].xy, c1 = bM[1].xy, c2 = bM[2].xy;\n" \
    /* (the corners themselves where the shader has them, psh.c nvcol: from the */ \
    /* derivatives a polygon with corners far off the screen changed its term */ \
    /* from quad to quad, FloorQuad's corners at x -1168 and 1808) */ \
    "#ifdef HLE_WBUF_SCRV\n" \
    "      c0 = vScrV[0].xy; c1 = vScrV[1].xy; c2 = vScrV[2].xy;\n" \
    "#endif\n" \
    /* 1/w by its differences from corner 0's, from the w's own (exact: a */ \
    /* far quad's corners' 1/w are equal to float precision) */ \
    "      vec2 dr = (bWv.x - bWv.yz) / (bWv.yz * bWv.x);\n" \
    "      float ra = dot(bX.yz, dr), rb = dot(bY.yz, dr);\n" \
    /* (the row pair: with the top corner on the screen, rows 2 and 3 of the first */ \
    /* row's 4-row block, even outside the triangle (TriH: k = 0 .. 3 all at rows */ \
    /* 2, 3); with it off the screen, the first row's 2-aligned pair (the floors, */ \
    /* the roof)) */ \
    "      vec2 tv = c0.y <= c1.y && c0.y <= c2.y ? c0 : (c1.y <= c2.y ? c1 : c2);\n" \
    /* (the clip's origin, vk/draw.c: the first row the polygon shows in it, the top */ \
    /* of its part right of the clip's left edge: RoofQuad's second triangle from */ \
    /* row 366, where its edge meets x 150; ClipW's walls by the clip's column) */ \
    "#ifdef HLE_WBUF_SCRV\n" \
    "      vec2 cl = vec2(float(pc.tfactor & 0xFFFFu), float(pc.tfactor >> 16));\n" \
    "#else\n" \
    "      vec2 cl = vec2(0.0);\n" \
    "#endif\n" \
    "      bool ton = tv.y >= cl.y && tv.x >= cl.x && tv.x < 2.0 / abs(pc.vp.x);\n" \
    "      float yt = 1.0e30;\n" \
    "      if (c0.x >= cl.x) yt = min(yt, c0.y);\n" \
    "      if (c1.x >= cl.x) yt = min(yt, c1.y);\n" \
    "      if (c2.x >= cl.x) yt = min(yt, c2.y);\n" \
    "      if ((c0.x - cl.x) * (c1.x - cl.x) < 0.0) yt = min(yt, c0.y + (cl.x - c0.x) * (c1.y - c0.y) / (c1.x - c0.x));\n" \
    "      if ((c1.x - cl.x) * (c2.x - cl.x) < 0.0) yt = min(yt, c1.y + (cl.x - c1.x) * (c2.y - c1.y) / (c2.x - c1.x));\n" \
    "      if ((c2.x - cl.x) * (c0.x - cl.x) < 0.0) yt = min(yt, c2.y + (cl.x - c2.x) * (c0.y - c2.y) / (c0.x - c2.x));\n" \
    "      if (yt > 1.0e29 || ton) yt = min(c0.y, min(c1.y, c2.y));\n" \
    "      float yf = ceil(max(floor(yt * 16.0 + 0.5) / 16.0, cl.y) - 0.5);\n" \
    /* (and under a window clip's top edge, C over 0, with the top corner above it: */ \
    /* rows 2 and 3 of the 4-row block too, C + 2 and C + 3, unless the triangle's */ \
    /* top is an edge (two corners at the top: the 2-aligned pair, C and C + 1): */ \
    /* ClipF at C 32, 128, 224, the floor's single-top triangle at C + 2 and its */ \
    /* flat-top one at C, the depth words exact; at C 0 the screen's top is the */ \
    /* floors' pair 0, 1 as before) */ \
    "      float ym = min(c0.y, min(c1.y, c2.y));\n" \
    "      bool fltop = int(c0.y == ym) + int(c1.y == ym) + int(c2.y == ym) >= 2;\n" \
    "      bool tab = cl.y > 0.0 && tv.y < cl.y && !fltop;\n" \
    "      float yr = ton || tab ? 4.0 * floor(yf * 0.25) + 2.5 : 2.0 * floor(yf * 0.5) + 0.5;\n" \
    "      float xl = 1.0e30;\n" \
    "      if ((c0.y - yr) * (c1.y - yr) <= 0.0 && c0.y != c1.y) xl = min(xl, c0.x + (yr - c0.y) * (c1.x - c0.x) / (c1.y - c0.y));\n" \
    "      if ((c1.y - yr) * (c2.y - yr) <= 0.0 && c1.y != c2.y) xl = min(xl, c1.x + (yr - c1.y) * (c2.x - c1.x) / (c2.y - c1.y));\n" \
    "      if ((c2.y - yr) * (c0.y - yr) <= 0.0 && c2.y != c0.y) xl = min(xl, c2.x + (yr - c2.y) * (c0.x - c2.x) / (c0.y - c2.y));\n" \
    "      if (xl > 1.0e29) xl = min(c0.x, min(c1.x, c2.x));\n" \
    "      float xc = ceil(max(floor(xl * 16.0 + 0.5) / 16.0, 0.0) - 0.5);\n" \
    /* (with the top corner off the screen, the 2-aligned pair of its own column on */ \
    /* the screen: WallQuad's top corners at x 637.3 and 150, pairs (636, 637) and */ \
    /* (150, 151), whatever column the first row starts at) */ \
    "      float xt = clamp(floor(floor(tv.x * 16.0 + 0.5) / 16.0), cl.x, 2.0 / abs(pc.vp.x) - 1.0);\n" \
    "      float xr = ton ? 4.0 * floor(xc * 0.25) + 2.5 : 2.0 * floor(xt * 0.5) + 0.5;\n" \
    "      float fx = ton ? 0.9 + 0.1 * min(bWv.x, min(bWv.y, bWv.z)) / max(bWv.x, max(bWv.y, bWv.z)) : 1.0;\n" \
    "      float r0 = rv.x + dot((bMi * vec3(xr, yr, 1.0)).yz, dr);\n" \
    /* (on the screen: the plane of 1/w from the corners on the rasterizer's 1/16, */ \
    /* not from the derivatives, a few parts in a million off pixel to pixel) */ \
    "#ifdef HLE_WBUF_SCRV\n" \
    "      if (true) {\n" \
    "#else\n" \
    "      if (ton) {\n" \
    "#endif\n" \
    "        vec2 s0 = floor(c0 * 16.0 + 0.5) / 16.0, s1 = floor(c1 * 16.0 + 0.5) / 16.0, s2 = floor(c2 * 16.0 + 0.5) / 16.0;\n" \
    "        float pd = (s1.x - s0.x) * (s2.y - s0.y) - (s2.x - s0.x) * (s1.y - s0.y);\n" \
    "        if (pd != 0.0) {\n" \
    "          ra = (dr.x * (s2.y - s0.y) - dr.y * (s1.y - s0.y)) / pd;\n" \
    "          rb = ((s1.x - s0.x) * dr.y - (s2.x - s0.x) * dr.x) / pd;\n" \
    "          r0 = rv.x + ra * (xr - s0.x) + rb * (yr - s0.y);\n" \
    "        }\n" \
    "      }\n" \
    "      if (r0 > 0.0 && r0 + ra > 0.0 && r0 + rb > 0.0)\n" \
    /* (the integer formats: the step's quotient exact and cut toward zero to a */ \
    /* float, as the word's sum is (nvwc24): the GPU's division came out a last */ \
    /* place over the console's, the term a word over: nxdk_pgraph_tests' */ \
    /* W_buffering WallQuad 24D ZS1 exact so, TriH 396 -> 114 pixels; the x step */ \
    /* under the on-screen factor fx stays as it was, TriV's fitted so) */ \
    "#ifdef HLE_WBUF_VWD\n" \
    "      { bool wfix = pc.zscale == uintBitsToFloat(0x37800000u) || pc.zscale == uintBitsToFloat(0x33800000u);\n" \
    "        wSl = max(ton || !wfix ? fx * abs(ra) / (r0 * (r0 + ra)) :\n" \
    "                        float(nvwc24(abs(double(ra)) / (double(r0) * (double(r0) + double(ra))))),\n" \
    "                  !wfix ? abs(rb) / (r0 * (r0 + rb)) :\n" \
    "                        float(nvwc24(abs(double(rb)) / (double(r0) * (double(r0) + double(rb)))))); }\n" \
    "#else\n" \
    "        wSl = max(fx * abs(ra) / (r0 * (r0 + ra)), abs(rb) / (r0 * (r0 + rb)));\n" \
    "#endif\n" \
    "    }\n" \
    "  }\n" \
    "  float wRaw = vW + uintBitsToFloat(pc.psc[18]) + uintBitsToFloat(pc.psc[19]) * wSl;\n" \
    HLE_WBUF_TRUNC_GLSL \
    /* (the integer formats' cut from the pixel's w in doubles: past 2^23 a float */ \
    /* has no fraction left, and 16777046.5 rounded up a word; nxdk_pgraph_tests' */ \
    /* W_buffering LargeZ, w from 16777046 to 16777047 down the screen, every word */ \
    /* 16777046 on the console) */ \
    "#ifdef HLE_WBUF_VWD\n" \
    "  double wRawD = vWd + double(uintBitsToFloat(pc.psc[18])) + double(uintBitsToFloat(pc.psc[19])) * double(wSl);\n" \
    "  if (pc.zscale == uintBitsToFloat(0x37800000u) || pc.zscale == uintBitsToFloat(0x33800000u)) {\n" \
    "    wRawD = nvwc24(nvwc24(nvwq(vWd)) + double(uintBitsToFloat(pc.psc[18])) + nvwc24(double(uintBitsToFloat(pc.psc[19])) * nvwc24(double(wSl))));\n" \
    "    wRaw = float(floor(wRawD)); }\n" \
    "#endif\n" \
    HLE_ZNORM_GLSL("wRaw", "wN") \
    "  float wDepth = clamp(wN, 0.0, 1.0);\n"

/* EQUAL depth passes that do not write (vk/draw.c eq_tol) test LESSEQUAL a
 * step toward the eye, which passes everything in front of the stored depth
 * too: these shaders discard what is more than a few steps in front of it,
 * read as an input attachment (set 2, binding 1). Bits of the keys' fbfetch,
 * by the depth buffer's step: D24, D16. The declaration's input attachment
 * index (HLE_IA_DEPTH) is stripped from the SPIR-V (glsl_compile.c): one
 * without an index reads the depth attachment (local read's default map). */
#define HLE_FB_ZEQ24 0x20u
#define HLE_FB_ZEQ16 0x40u
#define HLE_FB_ZEQ (HLE_FB_ZEQ24 | HLE_FB_ZEQ16)
#define HLE_IA_DEPTH 7
#define HLE_ZEQ_DECL_GLSL \
    "layout(input_attachment_index = 7, set = 2, binding = 1) uniform subpassInput uZ;\n"
/* after the alpha test (the derivatives' neighbours are still running) */
#define HLE_ZEQ_TEST_GLSL(fb) \
    (((fb) & HLE_FB_ZEQ16) ? \
     "  if (gl_FragCoord.z < subpassLoad(uZ).r - 2.0 / 65535.0) discard;\n" : \
     "  if (gl_FragCoord.z < subpassLoad(uZ).r - 4.0 / 16777215.0) discard;\n")

/* NV2A's projective texture division: coordinates times 1/q, with 1/0 the
 * largest float and 0 times it 0, so q = 0 projects (0, 0, 0) to 0, never
 * NaN. NGB writes oT0 = 0 to turn far shadow receivers' lookups off (lit);
 * 0/0 left NaN in the shadow filter weights and those receivers black.
 * The third coordinate keeps every float (an infinity as the largest): a
 * shadow stage compares it as a depth word, 24-bit fixed up to 16777215 (cut
 * at 1e7, every compare against a far depth failed in nxdk_pgraph_tests'
 * Texture_shadow_comparator 3F24/3P24) or a 24-bit float's (Texture_format's
 * DepthFmt_D_X8Y24_FLOAT against 0.5e30). */
#define HLE_NVPROJ_GLSL \
    "vec3 nvproj(vec4 t) {\n" \
    "  float iq = abs(t.w) > 1.0e-30 ? 1.0 / t.w : 3.0e38;\n" \
    "  vec3 p = t.xyz * iq;\n" \
    "  float zm = uintBitsToFloat(0x7f7fffffu);\n" \
    "  return vec3(clamp(p.xy, -1.0e7, 1.0e7), clamp(p.z, -zm, zm));\n" \
    "}\n"

/* Screen positions v (D3D screen pixels) to where the NV2A rasterizer puts
 * them. It truncates them to 1/16 pixel, and D3D's screen-space offset
 * (0.53125: half a pixel and 1/32, the truncation's rounding bias) lands the
 * positions it gives on pixel centres, which the edge rules then decide.
 * Here every position moves by that 1/32 alone: the same place for D3D's
 * positions, and every vertex by the same amount. Truncating each vertex
 * moved them apart by up to 1/16 pixel: a decal's quad and the surface under
 * it (other vertices) tilted against each other by up to 1/16 of the depth
 * slope per pixel, at a grazing angle more than the decal stands off, and it
 * flickered out as the camera moved (Burnout 3's road stains and manhole
 * covers, Dead or Alive 2's eaves shadows). debug.xbased.snap=trunc: the
 * truncation (HLE_SNAP_TRUNC_FLAG), for A/B. */
#define HLE_SNAP_TRUNC_FLAG 8192u
/* pc.flags: a quad list's triangles: a bump stage's coordinate on a texel edge (within 1/4096)
 * takes the texel before it in a quad's first triangle (0, 1, 2), the one
 * after in its second (0, 2, 3), as the console's do (nxdk_pgraph_tests'
 * Bump_map rows, the first triangle the one above the quad's 0 - 2
 * diagonal; psh.c sampx). Other stages' ties as the flag above says: as
 * a rule for all, Blend_surface and Pixel_shader's lost theirs. */
#define HLE_QUAD_TIE_FLAG 16384u
/* (flags bit 15: the lines drawn are a polygon's sides: psh.c nvcol1) */
#define HLE_OUTLINE_FLAG 32768u

/* ---- the NV2A's one-pixel lines ----
 * Measured on the console's pictures (nxdk_pgraph_tests; pgraph/tools/
 * linerule2.py is these rules in Python, none of their pixels off):
 * W_buffering's 15 pointer lines a picture and its line strip, Line_width's
 * and Smoothing_control's 16-vertex loops, end points known in screen pixels.
 * The end points truncated to 1/16 pixel. SET_SMOOTHING_CONTROL's bit 0
 * picks the rule (pbkit and D3D leave it set):
 * - set: the line's points whose major coordinate lies within 1/4 pixel of
 *   the pixel's centre light it when their minor one lies within 1/2: more
 *   pixels than one a step, a step and half the minor axis' length; the
 *   line, taken unbounded, ends square across its own direction at its end
 *   points: the centre counts when it projects onto the line between them,
 *   as a smoothed line's points do. SET_LINE_WIDTH scales that box, 1/4 and
 *   1/2 of the width (pgraph/tools/linewidth2.py: Line_width's loop from
 *   1/8 to 32 pixels, a pixel or two of thousands off, where ends 1/4 width
 *   inside left 149 of 6891 off at 9 pixels). A centre on the band's edge
 *   counts where that edge is a right one, or a level top one, and not on a
 *   left or a bottom one (pgraph/tools/linetie.py: 2300 such centres at
 *   eight widths); a centre on an end counts at the far end alone, not for
 *   a y-major line running down (ZPass_pixel_count's lines, 288.5 to 352.5:
 *   289 to 352 lit at every width; W_buffering's line strips). Width 0
 *   draws nothing. (The one-pixel line was first drawn with ends 1/4 pixel
 *   inside along the longer axis, nvl_at without cap: the same pixels on
 *   every loop and strip, but which of two overlapping lines owns a corner
 *   came out wrong on outlines: Shade_model's 16 pictures, Line_width at
 *   one pixel.)
 * - clear: one pixel a step: the line's point at the centre's major
 *   coordinate, from half a pixel before the start to half a pixel before
 *   the end.
 * A geometry stage (vsh.c hle_nvline_gs_glsl) draws each line as a quad
 * around it and hands the pixel stage the end points in 1/16 pixel
 * (location 12, the width in 1/8 pixel at 13); the pixel stage keeps the rule's pixels (HLE_NVLINE_FS_GLSL
 * after a "const bool nvl_thin", exact: 64-bit products). The pipeline key's
 * topo_class bit and the pixel key's (psh.h HLE_PSH_NVLINE, _THIN) go
 * together. Resolution scale 1 only. */
#define HLE_TOPO_NVLINE 0x1000u
/* with it on a triangle class (SET_FRONT_POLYGON_MODE GL_LINE: each
 * triangle's three sides are such lines, in its vertices' order;
 * nxdk_pgraph_tests' Line_width, pgraph/tools/outline.py), bits 13-14: the
 * winding culled (0 none, 1 clockwise on the screen, 2 the other, 3 both) */
#define HLE_TOPO_NVCULL_SHIFT 13
/* bit 15: each triangle stands for its side 1 - 2 alone, drawn with vertex
 * 0's flat colours (a flat-shaded quad's or polygon's outline, pb/pbres.h) */
#define HLE_TOPO_NVSIDE 0x8000u
/* bit 25: a smoothed polygon (SET_POLY_SMOOTH_ENABLE): every
 * pixel the triangle touches reaches the pixel stage, which counts the points of
 * the pixel inside it (psh.c nvp_cov); bit 26: and half a pixel further out (an
 * antialiased surface's passes, vk/draw.c aarep: a texel is half a pixel wide) */
#define HLE_TOPO_CONSERV 0x2000000u
/* bit 27, with HLE_TOPO_NVLINE on a triangle class: the triangles are a list's or a
 * strip's (not a fan's). Their outline's sides are drawn 1 - 2, 2 - 0, 0 - 1, the
 * last on top where they overlap; a fan's triangles 0 - 1, 1 - 2, 2 - 0, which is
 * the same order on the triangle taken as i + 1, 0, i (nxdk_pgraph_tests' Line_width
 * at 3 pixels: the two triangles' corners show side 0 - 1 over the others and 2 - 0
 * over 1 - 2, 18 pixels, and the fan's 600 overlapping pixels its own; Front_face's
 * four line-mode pictures; quads and polygons: pb/pbres.h). */
#define HLE_TOPO_NVTRIS 0x8000000u
#define HLE_TOPO_CONSERV_EXTRA 0x4000000u
#define HLE_NVLINE_FS_GLSL \
    "layout(location = 12) flat in ivec4 vLine;\n" \
    "layout(location = 13) flat in int vLineW;\n" \
    "int nvl_cmp(int a, int b, int c, int d) {\n" \
    "  int ah, al, ch, cl;\n" \
    "  imulExtended(a, b, ah, al);\n" \
    "  imulExtended(c, d, ch, cl);\n" \
    "  if (ah != ch) return ah < ch ? -1 : 1;\n" \
    "  return uint(al) < uint(cl) ? -1 : uint(al) > uint(cl) ? 1 : 0;\n" \
    "}\n" \
    "ivec2 nvl_sum(int a, int b, int c, int d) {\n" \
    "  int ah, al, ch, cl;\n" \
    "  imulExtended(a, b, ah, al);\n" \
    "  imulExtended(c, d, ch, cl);\n" \
    "  uint lo = uint(al) + uint(cl);\n" \
    "  return ivec2(ah + ch + (lo < uint(al) ? 1 : 0), int(lo));\n" \
    "}\n" \
    "int nvl_cmp2(int a, int b, int c, int d, int e, int f, int g, int h) {\n" \
    "  ivec2 p = nvl_sum(a, b, c, d), q = nvl_sum(e, f, g, h);\n" \
    "  if (p.x != q.x) return p.x < q.x ? -1 : 1;\n" \
    "  return uint(p.y) < uint(q.y) ? -1 : uint(p.y) > uint(q.y) ? 1 : 0;\n" \
    "}\n" \
    "bool nvl_at(ivec2 c, bool thin, bool cap, bool smp) {\n" \
    "  ivec4 ln = vLine * 2;\n" \
    "  ivec2 d = ln.zw - ln.xy;\n" \
    "  int w = vLineW;\n" \
    "  bool xm = abs(d.x) >= abs(d.y);\n" \
    "  int sM = xm ? ln.x : ln.y, dM = xm ? d.x : d.y, cM = xm ? c.x : c.y;\n" \
    "  int sN = xm ? ln.y : ln.x, dN = xm ? d.y : d.x, cN = xm ? c.y : c.x;\n" \
    "  if (dM == 0) return false;\n" \
    "  bool pos = dM > 0;\n" \
    "  int len = abs(dM);\n" \
    "  int lo, hi, ua, ub;\n" \
    "  bool loc, hic, uac, ubc;\n" \
    "  if (thin) {\n" \
    "    lo = cM; hi = cM; loc = true; hic = true;\n" \
    "    ua = -16; ub = -16; uac = xm ? !pos : pos; ubc = !uac;\n" \
    "  } else {\n" \
    "    lo = cM - w; hi = cM + w; loc = xm; hic = !xm;\n" \
    "    ua = w; ub = xm && !pos ? 0 : -w; uac = xm && pos; ubc = xm || !pos;\n" \
    "  }\n" \
    "  int slo = pos ? sM + ua : sM - len - ub, shi = pos ? sM + len + ub : sM - ua;\n" \
    "  bool sloc = pos ? uac : ubc, shic = pos ? ubc : uac;\n" \
    "  if (!cap) {\n" \
    "  if (slo > lo) { lo = slo; loc = sloc; } else if (slo == lo) loc = loc && sloc;\n" \
    "  if (shi < hi) { hi = shi; hic = shic; } else if (shi == hi) hic = hic && shic;\n" \
    "  }\n" \
    "  if (lo > hi || (lo == hi && !(loc && hic))) return false;\n" \
    "  bool inc = dN == 0 || ((dN > 0) == pos);\n" \
    "  int m1 = inc ? lo : hi, m2 = inc ? hi : lo;\n" \
    "  bool m1c = smp || dN == 0 || (inc ? loc : hic), m2c = smp || dN == 0 || (inc ? hic : loc);\n" \
    "  int sg = pos ? 1 : -1;\n" \
    "  int s1 = sg * nvl_cmp(m1 - sM, dN, cN + 2 * w - sN, dM);\n" \
    "  int s2 = sg * nvl_cmp(m2 - sM, dN, cN - 2 * w - sN, dM);\n" \
    "  bool t1 = m1c && xm, t2 = m2c && !xm;\n" \
    "  if (cap && !smp) { t1 = xm && inc; t2 = !xm || !inc; }\n" \
    "  if (!((s1 < 0 || (s1 == 0 && t1)) && (s2 > 0 || (s2 == 0 && t2)))) return false;\n" \
    "  if (!cap) return true;\n" \
    "  ivec2 e = c - ln.xy;\n" \
    "  int u = nvl_cmp2(e.x, d.x, e.y, d.y, 0, 0, 0, 0), v = nvl_cmp2(e.x, d.x, e.y, d.y, d.x, d.x, d.y, d.y);\n" \
    "  bool lf = d.x < 0 || (d.x == 0 && d.y < 0);\n" \
    "  bool sc = smp && lf, ec = smp ? !lf : xm || !pos;\n" \
    "  return (u > 0 || (u == 0 && sc)) && (v < 0 || (v == 0 && ec));\n" \
    "}\n" \
    "bool nvl_lit() {\n" \
    "  ivec2 c = ivec2(floor(gl_FragCoord.xy * 32.0 + 0.5));\n" \
    "  if (!nvl_thin && nvms()) c = ivec2(floor(gl_FragCoord.xy)) * 32 + nvsmp() * 2;\n" \
    "  return nvl_at(c, nvl_thin, !nvl_thin, false);\n" \
    "}\n" \
    "int nvl_cov() {\n" \
    "  ivec2 o = ivec2(floor(gl_FragCoord.xy)) * 32;\n" \
    "  int k = 0;\n" \
    "  for (int j = 0; j < 8; j++) if (nvl_at(o + ivec2(4 * j, 4 * ((3 * j) & 7)), false, true, true)) k++;\n" \
    "  return k;\n" \
    "}\n"
/* (a position past 2^100, beyond any sixteenth: as it is, the product overflowing
 * to infinity lost the triangle of a corner at the transform unit's saturated
 * 3.4e38, nxdk_pgraph_tests' W_param rcc pictures, which the console fills) */
#define HLE_SUBPIXEL_GLSL(v) \
    "  " v " = (pc.flags & 8192u) != 0u ? mix(trunc(" v " * 16.0) / 16.0, " v ", greaterThan(abs(" v "), vec2(1.0e30))) : " v " - 0.03125;\n"

/* Screen positions v (D3D screen pixels, after HLE_SUBPIXEL_GLSL) at a
 * resolution scale s > 1 (HleFfPush.flags bits 16-23; bit 12: the NV2A's
 * antialiasing, whose texels are its samples: not then). The NV2A covers a
 * texel an edge's line passes through the centre of when it is a left or
 * top edge and not when right or bottom; scaled, such an edge crosses the
 * middle of the texel's s x s image pixels and covers about half of them.
 * D3D's full-target quads start on the first texel's centre (its 0.53125
 * offset, to 0.5): scaled they left the image's first column and
 * row as an earlier pass left them, and Splinter Cell Chaos Theory's bloom
 * blurred the blue its EMF vision had left there into a band along the
 * frame's left and top. Positions on a texel's centre move to its first
 * image pixel's: the texel is covered, or not, whole. */
/* D3DTADDRESS_CLAMPTOEDGE (address mode 5, the NV2A's CLAMP_OGL: GL's CLAMP,
 * the XDK's texture addressing white paper): the border mode with the filter's
 * point held on the texture, [0, 1] for a bilinear filter (at the edge up to
 * half its weight from the border colour), [0, 1) for a point one (it never
 * reads the border: nxdk_pgraph_tests' Texture_border Clamp_OGL quads, point
 * magnified, show the edge texels as CLAMP's). The filter is the pixel's, by
 * the unclamped coordinate's steps (magnified: MAG's), and the texture is read
 * with those steps (the clamped ones would move its level). The stage's sampler
 * is a border one (vk/draw.c addr_mode); pc.tex bits 26-28: its axes u, v, w in
 * that mode; bit 25: MIN a point filter, bit 29: MAG one (vk/draw.c cte_bits).
 * Mode 5 was CLAMP's edge clamp: a bilinear stage's border-tinted half texel
 * at the texture's edge was missing. */
#define HLE_CTE_GLSL \
    "bool nvctept(uint t, vec2 sz, vec2 dx, vec2 dy) {\n" \
    "  return max(length(dx * sz), length(dy * sz)) <= 1.0 ? (t & 0x20000000u) != 0u : (t & 0x02000000u) != 0u;\n" \
    "}\n" \
    "vec2 nvcte2(uint t, vec2 uv, vec2 sz, vec2 dx, vec2 dy) {\n" \
    "  if ((t & 0x0C000000u) == 0u) return uv;\n" \
    "  vec2 hi = nvctept(t, sz, dx, dy) ? 1.0 - 0.5 / sz : vec2(1.0);\n" \
    "  return mix(uv, clamp(uv, vec2(0.0), hi), notEqual(uvec2(t >> 26, t >> 27) & 1u, uvec2(0u)));\n" \
    "}\n" \
    "vec3 nvcte3(uint t, vec3 uvw, vec3 sz, vec3 dx, vec3 dy) {\n" \
    "  if ((t & 0x1C000000u) == 0u) return uvw;\n" \
    "  bool pt = max(length(dx * sz), length(dy * sz)) <= 1.0 ? (t & 0x20000000u) != 0u : (t & 0x02000000u) != 0u;\n" \
    "  vec3 hi = pt ? 1.0 - 0.5 / sz : vec3(1.0);\n" \
    "  return mix(uvw, clamp(uvw, vec3(0.0), hi), notEqual(uvec3(t >> 26, t >> 27, t >> 28) & 1u, uvec3(0u)));\n" \
    "}\n" \
    /* (the texture's size asked only where the stage clamps: a size query is a \
     * sampler message of its own on Intel's GPUs, as dear as a read, #309) */ \
    "vec2 nvcte2i(uint t, vec2 uv, uint ti, vec2 dx, vec2 dy) {\n" \
    "  if ((t & 0x0C000000u) == 0u) return uv;\n" \
    "  return nvcte2(t, uv, vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)), dx, dy);\n" \
    "}\n"
#define HLE_CTE3I_GLSL \
    "vec3 nvcte3i(uint t, vec3 uvw, uint ti, vec3 dx, vec3 dy) {\n" \
    "  if ((t & 0x1C000000u) == 0u) return uvw;\n" \
    "  return nvcte3(t, uvw, vec3(textureSize(sampler3D(uTex3D[nonuniformEXT(ti)], uSmp[0]), 0)), dx, dy);\n" \
    "}\n"

#define HLE_TEXEL_EDGE_FLAG 4096u
#define HLE_TEXEL_EDGE_GLSL(v) \
    "  { uint rsc = (pc.flags >> 16) & 255u;\n" \
    "    if (rsc > 1u && (pc.flags & 4096u) == 0u)\n" \
    "      " v " -= vec2(equal(fract(" v "), vec2(0.5))) * (0.5 - 0.5 / float(rsc)); }\n"

/* GLSL source into buf (NUL-terminated); returns its length, or 0 if it did
 * not fit. */
size_t hle_ff_vs_glsl(const HleFfVsKey *k, char *buf, size_t cap);
size_t hle_ff_ps_glsl(const HleFfPsKey *k, char *buf, size_t cap);

#endif
