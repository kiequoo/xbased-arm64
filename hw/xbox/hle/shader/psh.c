/*
 * xbased HLE GPU — Xbox pixel shaders -> GLSL (pure). See psh.h.
 *
 * Register combiners (D3DPIXELSHADERDEF, NV_register_combiners on NV2A):
 * - input byte: bits 0-3 register, bit 4 alpha channel (RGB portion: .aaa;
 *   alpha portion: .a, else .b), bits 5-7 input mapping;
 * - output word: bits 0-3 CD, 4-7 AB, 8-11 SUM/MUX register, bit 12 CD dot,
 *   13 AB dot, 14 MUX, 15-17 output mapping, 18 CD blue-to-alpha, 19 AB;
 * - every input of a stage is read before any output is written; stage
 *   results are clamped to [-1, 1]; r0.a starts as t0.a;
 * - PSCombinerCount: bits 0-7 stages, bit 8 MUX on r0.a's MSB (else LSB),
 *   bit 12 a C0 per stage (else stage 0's), bit 16 the same for C1;
 * - final combiner: rgb = A*B + (1-A)*C + D, alpha = G, inputs clamped to
 *   [0, 1]; EF_PROD = E*F, V1R0_SUM = v1 + r0 (EFG flags: 0x80 clamp,
 *   0x40 complement v1, 0x20 complement r0). Without one, the output is r0.
 */
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "hw/xbox/hle/shader/ff.h"
#include "hw/xbox/hle/shader/psh.h"

uint32_t g_hle_stipple[HLE_PSH_STIPPLES][32];

typedef struct Buf {
    char *p;
    size_t cap, len;
    bool overflow;
    const char *half;                   /* the bias mappings' 0.5 (hle_psh_glsl) */
} Buf;

static void put(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->len < b->cap ? b->cap - b->len : 0, fmt, ap);
    va_end(ap);
    if (n < 0 || b->len + (size_t)n >= b->cap) {
        b->overflow = true;
        b->len = b->cap;
        return;
    }
    b->len += (size_t)n;
}

void hle_psh_key(const uint32_t *rs, HlePshKey *k)
{
    memset(k, 0, sizeof(*k));
    unsigned n = rs[HLE_PS_COUNT] & 0xFF;
    n = n > 8 ? 8 : n;
    for (unsigned s = 0; s < n; s++) {      /* stages past the count do not split keys */
        k->rgb_in[s] = rs[HLE_PS_RGB_IN + s];
        k->rgb_out[s] = rs[HLE_PS_RGB_OUT + s];
        k->alpha_in[s] = rs[HLE_PS_ALPHA_IN + s];
        k->alpha_out[s] = rs[HLE_PS_ALPHA_OUT + s];
    }
    k->final_abcd = rs[HLE_PS_FINAL_ABCD];
    k->final_efg = rs[HLE_PS_FINAL_EFG];
    k->count = rs[HLE_PS_COUNT] & 0x111FF;
    k->tex_modes = rs[HLE_PS_TEXMODES] & 0xFFFFF;
    k->dot_map = rs[HLE_PS_DOTMAP] & 0xFFF;
    k->input_tex = rs[HLE_PS_INPUTTEX] & 0x310000;
    k->compare = rs[HLE_PS_COMPARE] & 0xFFFF;
}

/* Register as a vec4 expression; final = the final combiner. */
static const char *reg(unsigned r, bool final)
{
    static const char *const k_reg[16] = {
        "vec4(0.0)", "C0", "C1", "fogr", "v0", "v1", "vec4(0.0)", "vec4(0.0)",
        "t0", "t1", "t2", "t3", "r0", "r1", "v1r0", "efp",
    };
    if (!final && r >= 14) {
        return "vec4(0.0)";
    }
    return k_reg[r & 15];
}

/* Input of a combiner: alpha = the alpha portion (float), else vec3. */
static void in_expr(Buf *b, uint8_t in, bool alpha, bool final)
{
    char x[48];
    const char *r = reg(in & 0xF, final);
    bool a = (in & 0x10) != 0;
    if (alpha) {
        snprintf(x, sizeof(x), "%s.%c", r, a ? 'a' : 'b');
    } else {
        snprintf(x, sizeof(x), a ? "%s.aaa" : "%s.rgb", r);
    }
    switch ((in >> 5) & 7) {
    case 0: put(b, "max(%s, 0.0)", x); break;                    /* UNSIGNED_IDENTITY */
    case 1: put(b, "(1.0 - clamp(%s, 0.0, 1.0))", x); break;     /* UNSIGNED_INVERT */
    case 2: put(b, "(2.0 * max(%s, 0.0) - 1.0)", x); break;      /* EXPAND_NORMAL */
    case 3: put(b, "(-2.0 * max(%s, 0.0) + 1.0)", x); break;     /* EXPAND_NEGATE */
    case 4: put(b, "(max(%s, 0.0) - %s)", x, b->half); break;    /* HALFBIAS_NORMAL */
    case 5: put(b, "(-max(%s, 0.0) + %s)", x, b->half); break;   /* HALFBIAS_NEGATE */
    case 6: put(b, "(%s)", x); break;                            /* SIGNED_IDENTITY */
    default: put(b, "(-(%s))", x); break;                        /* SIGNED_NEGATE */
    }
}

/* A stage's result v through its output mapping (half: the bias's 0.5). */
static void out_expr(char *e, size_t n, uint32_t out, const char *v, const char *half)
{
    switch ((out >> 15) & 7) {
    case 1: snprintf(e, n, "(%s - %s)", v, half); break;            /* BIAS */
    case 2: snprintf(e, n, "(%s * 2.0)", v); break;                 /* SHIFTLEFT_1 */
    case 3: snprintf(e, n, "((%s - %s) * 2.0)", v, half); break;    /* SHIFTLEFT_1_BIAS */
    case 4: snprintf(e, n, "(%s * 4.0)", v); break;                 /* SHIFTLEFT_2 */
    case 6: snprintf(e, n, "(%s * 0.5)", v); break;                 /* SHIFTRIGHT_1 */
    default: snprintf(e, n, "(%s)", v); break;
    }
}

/* Writable combiner outputs. */
static const char *dst(unsigned r)
{
    switch (r & 15) {
    case 4: return "v0";
    case 5: return "v1";
    case 8: return "t0";
    case 9: return "t1";
    case 10: return "t2";
    case 11: return "t3";
    case 12: return "r0";
    case 13: return "r1";
    default: return NULL;                               /* DISCARD and read-only */
    }
}

static void stage(Buf *b, const HlePshKey *k, unsigned s)
{
    uint32_t ri = k->rgb_in[s], ro = k->rgb_out[s], ai = k->alpha_in[s], ao = k->alpha_out[s];
    unsigned c0 = (k->count & 0x1000) ? s : 0, c1 = 8 + ((k->count & 0x10000) ? s : 0);
    put(b, "  { /* combiner stage %u */\n"
           "    vec4 C0 = col(pc.psc[%u]), C1 = col(pc.psc[%u]);\n", s, c0, c1);
    static const char k_v[4] = { 'A', 'B', 'C', 'D' };
    for (unsigned i = 0; i < 4; i++) {
        put(b, "    vec3 %c = ", k_v[i]);
        in_expr(b, (ri >> (24 - 8 * i)) & 0xFF, false, false);
        put(b, ";\n    float %ca = ", k_v[i]);
        in_expr(b, (ai >> (24 - 8 * i)) & 0xFF, true, false);
        put(b, ";\n");
    }
    /* MSB: bit 7 of the 8-bit value (D3D cnd: r0.a > 0.5); 1.0 * 0.5 is
     * 0xFF >> 1 = 0x7F, clear, and a bias's 0.5 is 0x80, set (b->half) */
    put(b, "    bool mx = %s;\n", (k->count & 0x100) ? "r0.a > 0.5"
                                                   : "(uint(r0.a * 255.0) & 1u) == 1u");
    put(b, "    vec3 ab = %s, cd = %s;\n", (ro & 0x2000) ? "vec3(dot(A, B))" : "A * B",
        (ro & 0x1000) ? "vec3(dot(C, D))" : "C * D");
    put(b, "    vec3 ms = %s;\n", (ro & 0x4000) ? "mx ? cd : ab" : "ab + cd");
    put(b, "    float aab = Aa * Ba, acd = Ca * Da, ams = %s;\n",
        (ao & 0x4000) ? "mx ? acd : aab" : "aab + acd");
    char e[64];
    static const char *const k_rv[3] = { "ab", "cd", "ms" };
    static const char *const k_av[3] = { "aab", "acd", "ams" };
    for (unsigned i = 0; i < 3; i++) {
        out_expr(e, sizeof(e), ro, k_rv[i], b->half);
        put(b, "    %s = clamp(%s, -1.0, 1.0);\n", k_rv[i], e);
        out_expr(e, sizeof(e), ao, k_av[i], b->half);
        put(b, "    %s = clamp(%s, -1.0, 1.0);\n", k_av[i], e);
        /* (a register holds steps of 1 / 255 (1.0 = 0xFF): each output to its nearest,
         * a half up: nxdk_pgraph_tests' Antialiasing_tests CompositeBlur*, Rugby 2005's
         * four-tap blur, 0x2A t0 + 0x55 t1 of a texel 128 is 64 (63.75) in r0 and in r1
         * and 128 their sum, 127 unrounded; SignedCombinerOps, -15.5 / 255 is -15 / 255) */
        put(b, "    %s = floor(%s * 255.0 + (0.5 + 1.0 / 1024.0)) / 255.0;\n"
               "    %s = floor(%s * 255.0 + (0.5 + 1.0 / 1024.0)) / 255.0;\n",
            k_rv[i], k_rv[i], k_av[i], k_av[i]);
    }
    /* outputs: AB bits 4-7, CD bits 0-3, SUM bits 8-11 */
    static const unsigned k_shift[3] = { 4, 0, 8 };
    for (unsigned i = 0; i < 3; i++) {
        const char *d = dst(ro >> k_shift[i]);
        if (d) {
            put(b, "    %s.rgb = %s;\n", d, k_rv[i]);
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        const char *d = dst(ao >> k_shift[i]);
        if (d) {
            put(b, "    %s.a = %s;\n", d, k_av[i]);
        }
    }
    if (ro & 0x80000) {                                 /* AB_BLUE_TO_ALPHA */
        const char *d = dst(ro >> 4);
        if (d) {
            put(b, "    %s.a = ab.b;\n", d);
        }
    }
    if (ro & 0x40000) {                                 /* CD_BLUE_TO_ALPHA */
        const char *d = dst(ro);
        if (d) {
            put(b, "    %s.a = cd.b;\n", d);
        }
    }
    put(b, "  }\n");
}

static void final_combiner(Buf *b, const HlePshKey *k)
{
    uint32_t abcd = k->final_abcd, efg = k->final_efg;
    put(b, "  vec4 outc;\n");
    if (!abcd && !efg) {
        /* No final combiner in the shader: the one D3D's fog setters push
         * (Otogi 0xfc362 / 0xfc3ce): A = fog.a, B = r0 or, with
         * SPECULARENABLE, v1 + r0 (clamped), C = fog.rgb, G = r0.a; without
         * fog, D = B (fogf is 1 then) */
        put(b, "  outc = vec4(mix(fogr.rgb, clamp(%s, 0.0, 1.0), fogf), clamp(r0.a, 0.0, 1.0));\n",
            k->specular ? "v1.rgb + r0.rgb" : "r0.rgb");
        return;
    }
    put(b, "  {\n    vec4 C0 = col(pc.psc[16]), C1 = col(pc.psc[17]);\n");
    put(b, "    vec3 E = clamp(");
    in_expr(b, (efg >> 24) & 0xFF, false, false);
    put(b, ", 0.0, 1.0);\n    vec3 F = clamp(");
    in_expr(b, (efg >> 16) & 0xFF, false, false);
    put(b, ", 0.0, 1.0);\n    vec4 efp = vec4(E * F, 0.0);\n");
    /* (v1 and r0 in it as the final combiner reads registers, [0, 1], then
     * complemented; unclamped (EFG 0x80 clear) the sum is read past 1 as
     * an input too: nxdk_pgraph_tests' Combiner SpecularR0SumFlags, 0.5 x
     * (1 + 1) is 1, a negative r0 adds 0) */
    put(b, "    vec4 v1r0 = vec4(%s + %s, 0.0);\n",
        (efg & 0x40) ? "(1.0 - clamp(v1.rgb, 0.0, 1.0))" : "clamp(v1.rgb, 0.0, 1.0)",
        (efg & 0x20) ? "(1.0 - clamp(r0.rgb, 0.0, 1.0))" : "clamp(r0.rgb, 0.0, 1.0)");
    if (efg & 0x80) {
        put(b, "    v1r0 = clamp(v1r0, 0.0, 1.0);\n");
    }
    static const char k_v[4] = { 'A', 'B', 'C', 'D' };
    for (unsigned i = 0; i < 4; i++) {
        const uint8_t in = (abcd >> (24 - 8 * i)) & 0xFF;
        put(b, "    vec3 %c = clamp(", k_v[i]);
        in_expr(b, in, false, true);
        put(b, ", 0.0, %s);\n", (in & 0xF) == 0xE && !(efg & 0x80) ? "2.0" : "1.0");
    }
    put(b, "    float G = clamp(");
    in_expr(b, (efg >> 8) & 0xFF, true, true);
    put(b, ", 0.0, 1.0);\n"
           "    outc = vec4(A * B + (1.0 - A) * C + D, G);\n  }\n");
}

static const char *k_cmp[8] = {
    "false", "a < r", "a == r", "a <= r", "a > r", "a != r", "a >= r", "true",
};

/* Shadow buffer at stage s: the stored depth compared with the r coordinate
 * (depth FUNC r, both in the format's range; PROJECT2D compares with 0),
 * the four results bilinearly weighted (percentage-closer filtering). */
static void shadow_sample(Buf *b, const HlePshKey *k, unsigned s, bool project3d)
{
    static const char *const k_max[4] = { "1.0", "16777215.0", "65535.0", "511.9375" };
    static const char *const k_op[8] = { "", "lessThan", "equal", "lessThanEqual", "greaterThan",
                                         "notEqual", "greaterThanEqual", "" };
    unsigned func = (k->shadow >> 4) & 7, fmt = (k->shadow_fmt >> (2 * s)) & 3;
    /* a 24-bit float depth: r up to the largest float (its word 0xfeffff,
     * nvfw24), the 16-bit one's 511.9375 (Texture_format's DepthFmt_D_X8Y24_FLOAT
     * compares against 0.5e30) */
    const bool f24 = fmt == 3 && ((k->alpha_kill >> (4 + s)) & 1);
    if (func == 0 || func == 7) {
        put(b, "  vec4 t%u = vec4(%s);\n", s, func ? "1.0" : "0.0");
        return;
    }
    const char *r = project3d ? "clamp(pq.z, 0.0, m)" : "0.0";
    const bool point = (k->compare & HLE_PSH_SHADOW_POINT) != 0;
    /* (the stage, for the corners' texel ties: an edge a coordinate lands on goes to
     * the texel before it where those say so, as sampx's; nxdk_pgraph_tests'
     * Texture_shadow_comparator 3F* pictures, a box's top row) */
    put(b, "#define SHS %uu\n", s);
    put(b, "  vec4 t%u;\n  { uint ti = pc.tex[%u] & 0xFFFFu, si = (pc.tex[%u] >> 16) & 0x1FFu;\n"
           "    vec3 pq = nvproj(vTex%u);\n"
           "    vec2 uv = pq.xy;\n"
           "    vec2 sz = vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0));\n"
           "    if ((pc.flags & (1u << %uu)) != 0u)\n"
           /* a scaled surface's texel coordinates: sz / the resolution scale */
           "      uv /= sz / ((pc.flags & (256u << %uu)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
           "    float m = %s, r = %s;\n"
           "%s"
           "%s"
           "    vec4 d = %s;\n"
           "#undef SHG\n"
           "    vec4 p = vec4(%s(d, vec4(r)));\n"
           "    vec2 f = fract(uv * sz - 0.5);\n"
           "    t%u = vec4(%s);\n",
        s, s, s, s, s, s, f24 ? "uintBitsToFloat(0x7f7fffffu)" : k_max[fmt], r,
        /* a fixed-point depth: r as a whole word, cut (the
         * chip compares words: nxdk_pgraph_tests' Texture_shadow_comparator, a box
         * drawn at the reference depth passes EQUAL over all of it) */
        fmt == 1 || fmt == 2 ? "    r = floor(r);\n" :
        /* (and a float depth surface's image is its words there: r as one) */
        fmt == 3 ? f24 ? "    r = nvfw24(r);\n" : "    r = nvfw16(r);\n" : "",
        /* point filtered: the texel the coordinates fall in, alone */
        /* (a coordinate a hair short of a texel edge, without perspective: on
         * it, as sampx's) */
        point ? "    vec2 uq = uv * sz, ur = floor(uq + 0.5);\n"
                "    if (dFdx(gl_FragCoord.w) == 0.0 && dFdy(gl_FragCoord.w) == 0.0)\n"
                "      uq = mix(uq, ur - ((pc.flags & (16u << SHS)) != 0u ? HLE_NVTIE_AT(SHS) / 4096.0 : vec2(0.0)),\n"
                "               lessThan(abs(uq - ur), vec2(1.0 / 4096.0)));\n"
                "#define SHG(c) vec4(texelFetch(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), "
                "clamp(ivec2(floor(uq)), ivec2(0), ivec2(sz) - 1), 0)[c])\n"
              : "#define SHG(c) textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], "
                "uSmp[nonuniformEXT(si)]), uv, c)\n",
        /* the title's own words: whole numbers, as stored (a float format's
         * words compare as r's, nvfw16 / nvfw24) */
        !(k->shadow & HLE_PSH_SHADOW_WORDS) ?
            fmt != 3 ? "SHG(0) * m" :
            f24 ? "round(SHG(0) * 16777216.0)" : "round(SHG(0) * 65536.0)" :
        fmt == 2 || (fmt == 3 && !f24) ? "round(SHG(0) * 65535.0)" :
        "round(SHG(3) * 255.0) * 65536.0 + round(SHG(2) * 255.0) * 256.0 + round(SHG(1) * 255.0)",
        k_op[func], s, point ? "p.x" : "mix(mix(p.w, p.z, f.x), mix(p.x, p.y, f.x), f.y)");
    if (k->debug == 17) {               /* debug view 17: stored depth, r, result */
        put(b, "    shdbg = vec4(d.x / m, r / m, t%u.x, 1.0);\n", s);
    }
    put(b, "  }\n#undef SHS\n");
}

/* D3DTSS_ALPHAKILL: a fetched texel with alpha 0 discards the fragment. A
 * shadow-buffer stage's result is its alpha too: projected shadow receivers
 * (PGR2's car shadow) draw only where the compare passed. */
static void alpha_kill(Buf *b, const HlePshKey *k, unsigned s, unsigned m)
{
    if (((k->alpha_kill >> s) & 1) && m != HLE_PST_NONE && m != HLE_PST_PASSTHRU &&
        m != HLE_PST_CLIPPLANE && m != HLE_PST_DOTPRODUCT) {
        put(b, "  if (t%u.a == 0.0) discard;\n", s);
    }
}

/* The colour key (SET_COLOR_KEY_COLOR, the mode in SET_TEXTURE_CONTROL0): a
 * stage's fetched texel, as eight bits a channel, equal to the stage's key
 * is made transparent or is not drawn at all. The titles that use it cut
 * sprites out of a flat background with it (Top Spin's crowd, each
 * spectator a quad in a block of the key's colour). After the fetch and the
 * alpha kill, for the modes that fetch (as the NV2A, by xemu's tests on a
 * console). */
static bool signed_stage(const HlePshKey *k, unsigned s);

static void color_key(Buf *b, const HlePshKey *k, unsigned s, unsigned m)
{
    static const char *const k_act[4] = { "", "t%u.a = 0.0;", "t%u = vec4(0.0);", "discard;" };
    const unsigned op = HLE_PSH_CKEY(k, s);
    if (!op || m == HLE_PST_NONE || m == HLE_PST_PASSTHRU || m == HLE_PST_CLIPPLANE ||
        m == HLE_PST_DOTPRODUCT) {
        return;
    }
    /* A filtered 2D stage: the key tested on each texel the filter weighs,
     * before the filter (the XDK's D3DTEXTURECOLORKEYOP: KILL when any texel
     * of non-zero weight is the key's; ALPHA and RGBA zero those texels and
     * filter the rest). The filtered texel against the key let a keyed sprite's
     * edge through as a halo of the key's colour (Angelic Concert's sprites,
     * ALPHA under a bilinear filter). psh.c ckeyt: point-sampled and mipmapped
     * stages, and pixels whose texels miss the key, as before. */
    if (m == HLE_PST_PROJECT2D && !HLE_PSH_TKIND(k, s) && !((k->vol >> s) & 1) && !((k->shadow >> s) & 1) &&
        !((k->fbfetch >> s) & 1) && !signed_stage(k, s)) {
        put(b, "  t%u = ckeyt(%uu, nvproj(vTex%u).xy, t%u, 0x%08xu, 0x%08xu, %uu);\n", s, s, s, s,
            (unsigned)k->ckey[s], HLE_PSH_CKEY_NOALPHA(k, s) ? 0x00FFFFFFu : 0xFFFFFFFFu, op);
        return;
    }
    put(b, "  if (ckey(t%u, 0x%08xu, 0x%08xu)) { ", s, (unsigned)k->ckey[s],
        HLE_PSH_CKEY_NOALPHA(k, s) ? 0x00FFFFFFu : 0xFFFFFFFFu);
    put(b, k_act[op], s);
    put(b, " }\n");
}

/* Stage s's texture has channels the texture unit gives as signed bytes
 * (SET_TEXTURE_FILTER 31:28) and its mode samples one: the combiners take
 * them signed, -1..1. A bump-env, dot-product or dependent stage that reads
 * it addresses by the texel's bytes as they are (sgn8, dmap): tr<s>, the
 * texel as sampled; a bump stage whose input's b and g are signed takes
 * those (bump_in). Fable's pond and Burnout 3's lake read their ripple maps
 * this way, the pond's in the combiners too. */
static bool signed_stage(const HlePshKey *k, unsigned s)
{
    const unsigned m = (k->tex_modes >> (5 * s)) & 0x1F;
    if (!((HLE_PSH_TSIGN(k) >> (4 * s)) & 15) || ((k->fbfetch >> s) & 1) ||
        (((k->shadow >> s) & 1) && (m == HLE_PST_PROJECT2D || m == HLE_PST_PROJECT3D))) {
        return false;
    }
    return m != HLE_PST_NONE && m != HLE_PST_PASSTHRU && m != HLE_PST_CLIPPLANE &&
           m != HLE_PST_DOTPRODUCT;
}

/* A stage that fetches a texture (not the target drawn into) */
static bool fetch_stage(const HlePshKey *k, unsigned s)
{
    const unsigned m = (k->tex_modes >> (5 * s)) & 0x1F;
    return s < 4 && !((k->fbfetch >> s) & 1) && m != HLE_PST_NONE && m != HLE_PST_PASSTHRU &&
           m != HLE_PST_CLIPPLANE && m != HLE_PST_DOTPRODUCT;
}

/* Stage s fetches a YUY2 / UYVY texture, kept as its (Y, Cb, Cr) texels (pb/pbres.c
 * yuv_define, HlePshKey.tex16 3): filtered as they are, then converted to RGB for
 * its colour (nvcsc), as the console's texture unit does; the stages that address
 * by it read it unconverted (ty<s>), unsigned whatever its flags. nxdk_pgraph_tests'
 * Bump_map YUY2 / UYVY: the console's offsets are its (Cr, Cb), its four rectangles
 * alike */
static bool yuv_stage(const HlePshKey *k, unsigned s)
{
    return fetch_stage(k, s) && ((k->tex16 >> (2 * s)) & 3) == 3;
}

/* The name of stage in's texel for a stage that addresses by it */
static const char *tex_in(const HlePshKey *k, unsigned in, char out[24])
{
    snprintf(out, 24, "%s%u", yuv_stage(k, in) ? "ty" : signed_stage(k, in) ? "tr" : "t", in);
    return out;
}

/* Stage in's texture, a 2D one its coordinates fetch, has 16-bit channels
 * (HlePshKey.tex16): 1 R16B16, 2 Y16; 0 neither. The texture unit filters
 * their words whole and gives its bytes from the result: a HILO_1 dot stage
 * reads an R16B16's two words (hl16), a bump stage the bytes (tb16: (du, dv)
 * the low word's low and high bytes, the luminance the high word's low byte).
 * nxdk_pgraph_tests' Bump_map R16B16: the low word's ramp from 0x0101 to
 * 0x0303 tilts the console's bands smoothly, as bytes in three steps; Y16: from
 * 0x5252 to 0x5353 the low byte runs once through all 256 values, a band of
 * fine stripes; Bump_env_lum R16B16: from 0x7F7F to 0x8080 the luminance
 * climbs from 0x7F through 0xFF to 0x80, whatever the sign flags. A word is
 * signed by its high byte's channel's flag (R16B16's high word A, low word G;
 * Y16's G): Bump_map R16B16's As, its high word from 0x0202 to 0xFFFF a ramp
 * from +514 to -1, not through 0x8000. */
static unsigned tex16(const HlePshKey *k, unsigned in)
{
    const unsigned m = (k->tex_modes >> (5 * in)) & 0x1F;
    if (in >= 4 || ((k->vol >> in) & 1) || ((k->fbfetch >> in) & 1) || m != HLE_PST_PROJECT2D) {
        return 0;
    }
    const unsigned c = (k->tex16 >> (2 * in)) & 3;
    return c == 3 ? 0 : c;
}

/* Stage s's texel taken through SET_CONTROL0's colour-space conversion (HLE_PSH_CSC):
 * a stage that fetches a texture other than YUY2 / UYVY (those always are, yuv_stage),
 * its (r, g, b) as (Y, Cb, Cr). nxdk_pgraph_tests' Bump_map YUY2 / UYVY: the test sets
 * the conversion for its YUY2 stage 0, and the console's red A8R8G8B8 checker of
 * stage 1, (254, 0, 0), comes out (72, 255, 18); its grey one, (34, 33, 32) at alpha
 * 0x7F, (0, 136, 0) before the blend */
static bool csc_stage(const HlePshKey *k, unsigned s)
{
    return (k->input_tex & HLE_PSH_CSC) && fetch_stage(k, s) && ((k->tex16 >> (2 * s)) & 3) != 3;
}

/* The arguments of stage in's 16-bit functions (hl16, tb16) */
static const char *w16_args(const HlePshKey *k, unsigned in, char out[160])
{
    const unsigned g = (HLE_PSH_TSIGN(k) >> (4 * in)) & 15, t = tex16(k, in);
    char tn[24];
    snprintf(out, 160, "%uu, nvproj(vTex%u).xy, %s, bvec2(%s, %s), %s, %s", in, in, tex_in(k, in, tn),
             (t == 1 ? g & 1 : g & 4) ? "true" : "false", t == 1 && (g & 4) ? "true" : "false",
             (HLE_PSH_TPOINT(k) >> in) & 1 ? "true" : "false", t == 2 ? "true" : "false");
    return out;
}

/* A dot stage's vector from stage in's texel through mapping dm, and the line's end */
static void dot_vec(Buf *b, const HlePshKey *k, unsigned in, unsigned dm)
{
    char tn[160];
    if (dm == 4 && tex16(k, in) == 1) {
        put(b, "hl16(%s));\n", w16_args(k, in, tn));
    } else {
        put(b, "dmap(%uu, %s));\n", dm, tex_in(k, in, tn));
    }
}

/* A bump stage's (du, dv): its input stage's (b, g) as signed bytes over 128
 * (nxdk_pgraph_tests' Bump_map: the offsets of the console's pictures, whether
 * the texture unit gives the channels signed or not; +127 is 127 / 128 and -128
 * is -1). Each channel the texture unit gives signed is the signed filtered one
 * (samps, over 127.5, over 128 here: its band ramps from +127 to -128), the
 * others the texel's bytes as sampled read as two's complement (a step). */
static const char *bump_in(const HlePshKey *k, unsigned in, char out[192])
{
    const unsigned m = signed_stage(k, in) ? (HLE_PSH_TSIGN(k) >> (4 * in)) & 12 : 0;
    if (!m) {
        char tn[24];
        snprintf(out, 192, "sgn8(%s.bg)", tex_in(k, in, tn));
    } else if (m == 12) {
        snprintf(out, 192, "mix(t%u.bg * 0.99609375, vec2(-1.0), lessThanEqual(t%u.bg, vec2(-1.0)))", in, in);
    } else {
        snprintf(out, 192, "mix(sgn8(tr%u.bg), mix(t%u.bg * 0.99609375, vec2(-1.0), lessThanEqual(t%u.bg, vec2(-1.0))), "
                 "bvec2(%s, %s))", in, in, in, m & 8 ? "true" : "false", m & 4 ? "true" : "false");
    }
    return out;
}

/* A bump-env luminance stage's luminance: its input's red byte, as the texture
 * unit gives it: a signed red its signed filtered value's bits (A8R8G8B8's,
 * flipped at definition, as sampled: tr), a 16-bit texture's from its words
 * (tw, tex16). nxdk_pgraph_tests' Bump_env_lum R8B8 with R signed: from 0x7F to
 * 0x80 the console's luminance falls through 0x00 and 0xFF, as A8R8G8B8's; as
 * sampled it stays near 0x80 */
static const char *lum_in(const HlePshKey *k, unsigned in, bool b16, char out[192])
{
    if (b16) {
        snprintf(out, 192, "tw.r");
    } else if (signed_stage(k, in) && !((k->input_tex >> in) & 1) &&
               ((HLE_PSH_TSIGN(k) >> (4 * in)) & 2)) {
        snprintf(out, 192, "sbit(t%u.r)", in);
    } else if (((k->tex_modes >> (5 * in)) & 0x1F) == HLE_PST_PROJECT2D && !((k->vol >> in) & 1) &&
               !yuv_stage(k, in) && !((k->fbfetch >> in) & 1) && !((k->shadow >> in) & 1) &&
               !(signed_stage(k, in) && ((HLE_PSH_TSIGN(k) >> (4 * in)) & 2))) {
        /* (an unsigned red's bytes blended with the weights cut: lum8; a signed one's
         * are tr's, flipped at definition) */
        char tn[24];
        snprintf(out, 192, "lum8(%uu, nvproj(vTex%u).xy, %s.r, %s)", in, in, tex_in(k, in, tn),
                 (HLE_PSH_TPOINT(k) >> in) & 1 ? "true" : "false");
    } else {
        char tn[24];
        snprintf(out, 192, "%s.r", tex_in(k, in, tn));
    }
    return out;
}

/* The sign of rcp(t) t - 1 for a whole t: the table reciprocal of t as a float, times t,
 * against 1 (the rasterizer's weights are short where it is under: nvcol, nvcol1). */
static const char k_nvrsign[] =
    "const uint nvrcp_lut[64] = uint[64](\n"
    "  0x7fu, 0x7du, 0x7bu, 0x79u, 0x78u, 0x76u, 0x74u, 0x73u, 0x71u, 0x6fu, 0x6eu, 0x6du, 0x6bu, 0x6au, 0x68u, 0x67u,\n"
    "  0x66u, 0x65u, 0x63u, 0x62u, 0x61u, 0x60u, 0x5fu, 0x5eu, 0x5du, 0x5cu, 0x5bu, 0x5au, 0x59u, 0x58u, 0x57u, 0x56u,\n"
    "  0x55u, 0x54u, 0x53u, 0x52u, 0x52u, 0x51u, 0x50u, 0x4fu, 0x4eu, 0x4eu, 0x4du, 0x4cu, 0x4cu, 0x4bu, 0x4au, 0x49u,\n"
    "  0x49u, 0x48u, 0x48u, 0x47u, 0x46u, 0x46u, 0x45u, 0x45u, 0x44u, 0x43u, 0x43u, 0x42u, 0x42u, 0x41u, 0x41u, 0x40u);\n"
    /* the sign of rcp(t) t - 1: the table reciprocal (vsh's xfrcp) of the
     * area as a float, times the area, against 1, in whole numbers */
    /* (nvrmant: that reciprocal's mantissa, bit 23 set, for a mantissa fx, bit 23 set) */
    "uint nvrmant(uint fx) {\n"
    "  uint s0 = nvrcp_lut[(fx >> 17) & 0x3fu];\n"
    "  uint h, l;\n"
    "  umulExtended(0x80000000u - s0 * fx, s0, h, l);\n"
    "  uint s1 = (h << 8) | (l >> 24);\n"
    "  umulExtended(s1, fx, h, l);\n"
    "  uint bl = 0u - l, bh = 0x20u - h - (l != 0u ? 1u : 0u);\n"
    "  uint h0, w0, h1, l1;\n"
    "  umulExtended(bl, s1, h0, w0);\n"
    "  umulExtended(bh, s1, h1, l1);\n"
    "  return ((l1 + h0) << 7) | (w0 >> 25);\n"
    "}\n"
    /* 256 e rcp(t), cut: a corner's share as the rasterizer has it, the edge value
     * times the area's table reciprocal. A share a hair over a whole number comes
     * out under it where that reciprocal is short (nxdk_pgraph_tests' 3D_primitive
     * quad strip, pixel (250, 199): 76.0000023 as 75, the reciprocal 3.3e-8 short;
     * the whole ones' drop is this too). The favoured corner's is what the other
     * two's leave of 256 (up: a whole number more than the cut), so a share of
     * its own a hair over a whole number stays there (the same strip, (306, 310)
     * and (516, 329): 220.0000005 and 217.000004, favoured, as 220 and 217). */
    "int nvdivr(int e, int t, bool up) {\n"
    "  uint x = floatBitsToUint(float(t));\n"
    "  uint fx = x & 0x7fffffu;\n"
    "  int sh = int((x >> 23) & 0xffu) - 127 + 16;\n"
    "  uint h, l;\n"
    "  if (fx == 0u) { h = uint(e) >> 9; l = uint(e) << 23; sh -= 1; }\n"
    "  else umulExtended(uint(e), nvrmant(fx + 0x800000u), h, l);\n"
    "  int q = sh >= 32 ? int(h >> (sh - 32)) : int((h << (32 - sh)) | (l >> sh));\n"
    /* (the favoured corner's share is what the other two leave of 256: their total
     * counts as a whole number more only when its first 16 fractional bits are
     * not all zero, so 117.0000061 is 117 and the favoured corner has 139;
     * 35.9999995 and 38.999996 are a whole number more: nxdk_pgraph_tests' Line_width
     * quad strip, pixel (228, 269), and the 3D_primitive strip's (306, 310), (516, 329);
     * Shade_model's (290, 183), (415, 207), (345, 268) go with it: 1.4e-5 over a whole
     * number is no more; Overlapping_draw_modes' SquashOpt (390, 263), 1.45e-5 over 175,
     * is one more and the one picture pair this costs) */
    "  uint fr = sh > 32 ? ((h << (64 - sh)) | (l >> (sh - 32))) : sh == 32 ? l : (l << (32 - sh));\n"
    "  bool rem = (fr & 0xFFFF0000u) != 0u;\n"
    "  return up && rem ? q + 1 : q;\n"
    "}\n"
    "int nvrsign(int t) {\n"
    "  uint x = floatBitsToUint(float(t));\n"
    "  uint fx = x & 0x7fffffu;\n"
    "  if (fx == 0u) return 0;\n"
    "  int ex = int((x >> 23) & 0xffu);\n"
    "  fx += 0x800000u;\n"
    "  uint s0 = nvrcp_lut[(fx >> 17) & 0x3fu];\n"
    "  uint h, l;\n"
    "  umulExtended(0x80000000u - s0 * fx, s0, h, l);\n"
    "  uint s1 = (h << 8) | (l >> 24);\n"
    "  umulExtended(s1, fx, h, l);\n"
    "  uint bl = 0u - l, bh = 0x20u - h - (l != 0u ? 1u : 0u);\n"
    "  uint h0, w0, h1, l1;\n"
    "  umulExtended(bl, s1, h0, w0);\n"
    "  umulExtended(bh, s1, h1, l1);\n"
    "  uint mr = ((l1 + h0) << 7) | (w0 >> 25);\n"       /* the reciprocal's mantissa, bit 23 set */
    "  umulExtended(mr, fx, h, l);\n"                    /* against 2^47: (2^23 m)(2^-24 r) = 1 */
    "  return h > 0x8000u || (h == 0x8000u && l != 0u) ? 1 : h == 0x8000u ? 0 : -1;\n"
    "}\n";

/* text's every copy of pat cut out of buf (len its length); the new length */
static size_t cut_all(char *buf, size_t len, const char *pat)
{
    const size_t n = strlen(pat);
    for (char *f = buf; (f = strstr(f, pat)) != NULL; ) {
        memmove(f, f + n, len - (size_t)(f - buf) - n + 1);
        len -= n;
    }
    return len;
}

/* Fast precision (g_hle_psh_fast): the per-pixel steps added after 1.0.94
 * for the console's exact bytes cut from the text, as 1.0.94 drew: each
 * combiner output's and the colour's rounding to a byte, the vertex
 * colours' bytes, the samples' own passes of an antialiased surface (their
 * discard kept the GPU's early depth test off in every shader) and a 2D
 * stage's coordinate held on a texel edge (a texture size and its steps on
 * every flat draw: the full-screen passes of Midnight Club 3's blur cost
 * half as much again, #309). */
static size_t fast_cut(char *buf, size_t len)
{
    static const char *const k_cut[] = {
        "  if ((pc.flags & 4096u) != 0u && pc.mvp[1].x >= 1.0) {\n"
        "    ivec2 ms = ivec2(pc.mvp[1].xy), mp = ivec2(nvFC.xy);\n"
        "    nvAAk = mp - (mp / ms) * ms != ivec2(pc.mvp[1].zw);\n"
        "    nvFC.xy = vec2(mp / ms) + 0.5;\n"
        "  }\n",
        "  if (nvAAk) discard;\n",
        "  v0 = NVC8(v0) / 255.0; v1 = NVC8(v1) / 255.0;\n",
        "  outc = floor(outc * 255.0 + 0.5) / 255.0;\n",
    };
    for (unsigned i = 0; i < sizeof(k_cut) / sizeof(k_cut[0]); i++) {
        len = cut_all(buf, len, k_cut[i]);
    }
    /* the combiners' outputs to their nearest 1 / 255 */
    static const char k_round[] = " * 255.0 + (0.5 + 1.0 / 1024.0)) / 255.0;\n";
    for (char *f; (f = strstr(buf, k_round)) != NULL; ) {
        char *ls = f;
        while (ls > buf && ls[-1] != '\n') {
            ls--;
        }
        char *le = f + sizeof(k_round) - 1;
        memmove(ls, le, len - (size_t)(le - buf) + 1);
        len -= (size_t)(le - ls);
    }
    /* a cube stage read by the GPU's own level, as 1.0.104 did (sampc: its
     * edge hold cut, the level from the face coordinates and the size query
     * then unused) */
    static const char k_ctie[] = "  if ((pc.flags & (16u << s)) != 0u && dFdx(nvFC        .w) == 0.0 && dFdy(nvFC        .w) == 0.0) {\n"
                                 "    vec2 q = (f0 + 1.0) * hs";
    for (char *f; (f = strstr(buf, k_ctie)) != NULL; ) {
        char *e = strstr(f, "\n  }\n");
        if (!e) {
            break;
        }
        e += 5;
        memmove(f, e, len - (size_t)(e - buf) + 1);
        len -= (size_t)(e - f);
    }
    static const char k_clod[] = "  return textureLod(samplerCube(uTexCube[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), dir, "
                                 "log2(max(rho, 1.0e-6)));\n";
    static const char k_ctex[] = "  return texture(samplerCube(uTexCube[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), dir);\n";
    for (char *f; (f = strstr(buf, k_clod)) != NULL; ) {
        const size_t a = sizeof(k_clod) - 1, b = sizeof(k_ctex) - 1;
        memmove(f + b, f + a, len - (size_t)(f - buf) - a + 1);
        memcpy(f, k_ctex, b);
        len -= a - b;
    }
    /* sampx's texel-edge hold (2D stages) */
    static const char k_tie[] = "  if (dFdx(nvFC        .w) == 0.0 && dFdy(nvFC        .w) == 0.0) {\n"
                                "    vec2 qs = vec2(textureSize(sampler2D(";
    for (char *f; (f = strstr(buf, k_tie)) != NULL; ) {
        char *e = strstr(f, "\n  }\n");
        if (!e) {
            break;
        }
        e += 5;
        memmove(f, e, len - (size_t)(e - buf) + 1);
        len -= (size_t)(e - f);
    }
    return len;
}

size_t hle_psh_glsl(const HlePshKey *k, char *buf, size_t cap, unsigned *unsupported)
{
    Buf b = { buf, cap, 0, false, "(128.0 / 255.0)" };
    const char *interp = k->flat ? "flat " : (k->compare & HLE_PSH_TEX_AFFINE) ? "noperspective " : "";
    /* The combiners compute in signed fixed point, 1.0 = 0xFF: the bias
     * mappings' 0.5 is 0x80, its MSB set, while a halving of 1.0 is 0x7F,
     * clear; both are 0.5 in float. A MUX on r0.a's MSB tells them apart:
     * Halo 2's scope mask sets r0.a = 0.5 - t0.b (HALFBIAS_NEGATE) to pick
     * its black where t0.b is 0 (a dimmed view around the scope, black
     * lines) and a light blue elsewhere, and read as 0.5 it painted the
     * whole view light blue; PGR2's road halves t0.a (SHIFTRIGHT_1) and must
     * not pick at 1.0. Every shader takes 0x80 / 0xFF for the bias (the
     * HALFBIAS input mappings, the BIAS output ones): nxdk_pgraph_tests'
     * Combiner InputMappings (-0.4 through HALFBIAS_NORMAL, times 0.5, plus
     * 0.5: 63 on the console, 64 with a bias of 0.5), CombinerOps,
     * SignedCombinerOps and ShiftClamping exact with it, nothing worse; it
     * was only the MUX shaders' before. */
    unsigned unsup = 0;
    /* PSDotMapping HILO_1 (4) in a stage's field: dmap reads the 32-bit texel
     * as two 16-bit halves, hi A:R and lo G:B, over 65535 (as xemu). Read as
     * its R and G bytes mapped to -1..1, the depth buffer Halo 2's fog pass reads as
     * A8R8G8B8 (its top 16 bits in A:R) gave its fog-table lookup depth bits
     * 8-15, which wrap every 256 steps: stripes of fog and none across the
     * scenery, moving with the camera. */
    bool hilo1 = false;
    for (unsigned s = 0; s < 3; s++) {
        hilo1 |= ((k->dot_map >> (4 * s)) & 7) == 4;
    }
    const char *rect_code = k->vol & HLE_PSH_RECT4 ?
            /* vertex programs: each stage's own (pc.scr, its word; 0 none) */
            "  { uint r = floatBitsToUint(pc.scr[s]);\n"
            "    if (r != 0u)\n"
            "      uv = nvrectr(uv, r, (pc.flags & (256u << s)) != 0u ? 0.5 / float(pc.flags >> 24) : 0.5); }\n"
            : k->vol & HLE_PSH_RECT ?
            "  { uint r = floatBitsToUint(pc.fog.w);\n"
            "    if (r != 0u && s == ((r >> 28 & 1u) | (r >> 30 & 2u)))\n"
            "      uv = nvrect(uv, (pc.flags & (256u << s)) != 0u ? 0.5 / float(pc.flags >> 24) : 0.5); }\n"
            : "";
    /* Signed channels (signed_stage): converted, then filtered, as the
     * texture unit does. Blended as bytes and converted after, two texels
     * either side of 0 (0x01, 0xFF) gave 0x80, -1: Fable's pond, whose
     * ripple map a pass renders each frame around 0, came out streaked
     * with its pattern. A one-level image (a render target's) is gathered
     * and blended here; a mipmapped or point-sampled one converted as
     * sampled. Only in the shaders that have such a stage: the others'
     * text, and their cached SPIR-V, stay. */
    /* (room for the per-stage rectangle's code too: at 2600 the function was cut
     * short with it, Conker's signed stage of a program draw failing to build;
     * one a thread: the look-ahead's and the workers' shaders are made at the same
     * time, and a buffer shared by them emptied another's before it was used:
     * Panzer Dragoon Orta's signed stages built without the function, failed) */
    static _Thread_local char sfun[8192];
    sfun[0] = 0;
    for (unsigned s = 0; s < 4; s++) {
        if (signed_stage(k, s)) {
            snprintf(sfun, sizeof(sfun),
                "vec4 sgn4(vec4 c) {\n"
                "  vec4 v = floor(c * 255.0 + 0.5);\n"
                "  return max(mix(v, v - 256.0, greaterThanEqual(v, vec4(128.0))) / 127.5, -1.0);\n"
                "}\n"
                /* a signed channel (samps, sgn4: a whole over 127.5, -128 cut to -1) back
                 * to its byte's bits, over 255 */
                "float sbit(float t) {\n"
                "  float x = t * 127.5;\n"
                "  return float((x < -127.25 ? -128 : int(floor(x + 0.5))) & 255) / 255.0;\n"
                "}\n"
                /* The texels' bytes blended as the texture unit does: signed bytes, the
                 * weights cut to 1/256, each blend (a (256 - w) + b w + 128) >> 8
                 * (nxdk_pgraph_tests' Bump_map: the band where the bump stage's dv goes from
                 * +127 to -128, every edge of the console's picture, both signed
                 * rectangles) */
                "ivec4 sbi(vec4 c, bool fl) { ivec4 v = ivec4(floor(c * 255.0 + 0.5)); return fl ? v - 128 : v - 256 * (v >> 7); }\n"
                "float ibil(ivec4 c, ivec2 q) {\n"
                "  int t = (c.w * (256 - q.x) + c.z * q.x + 128) >> 8;\n"
                "  int b = (c.x * (256 - q.x) + c.y * q.x + 128) >> 8;\n"
                "  return max(float((t * (256 - q.y) + b * q.y + 128) >> 8) / 127.5, -1.0); }\n"
                /* (the same at level l of a target's mipmapped image: its four texels read at
                 * their centres through the stage's sampler, its addressing with them) */
                "vec4 sbilod(uint ti, uint si, vec2 uv, int l) {\n"
                "  vec2 sz = vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), l));\n"
                "  vec2 p = uv * sz - 0.5, i = floor(p);\n"
                "  ivec2 q = ivec2(floor((p - i) * 256.0 + 1.0 / 1024.0));\n"
                "  float lv = float(l);\n"
                "  vec4 t00 = textureLod(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), (i + vec2(0.5, 0.5)) / sz, lv);\n"
                "  vec4 t10 = textureLod(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), (i + vec2(1.5, 0.5)) / sz, lv);\n"
                "  vec4 t01 = textureLod(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), (i + vec2(0.5, 1.5)) / sz, lv);\n"
                "  vec4 t11 = textureLod(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), (i + vec2(1.5, 1.5)) / sz, lv);\n"
                "  mat4 c = transpose(mat4(t01, t11, t10, t00));\n"
                "  return vec4(ibil(sbi(c[0], false), q), ibil(sbi(c[1], false), q), ibil(sbi(c[2], false), q), ibil(sbi(c[3], false), q));\n"
                "}\n"
                "vec4 samps(uint s, vec2 uv, vec4 f, bvec4 m, bool pt, bool fl) {\n"
                "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0x1FFu;\n"
                "%s"
                "  if ((pc.flags & (1u << s)) != 0u)\n"
                "    uv /= vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)) /\n"
                "          ((pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
                "  uv = nvcte2i(pc.tex[s], uv, ti, dFdxCoarse(uv), dFdyCoarse(uv));\n"
                "  int nl = textureQueryLevels(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]));\n"
                "  if (pt || (fl && nl > 1))\n"
                "    return mix(f, fl ? clamp((f * 255.0 - 128.0) / 127.5, -1.0, 1.0) : sgn4(f), m);\n"
                /* (a target's image with levels, its texture's levels' targets (pb/pbdraw.c
                 * mips_of_targets): the sampler's own filter blends its bytes unsigned, and
                 * where a channel crosses 0 (0x7F to 0x80) that is its far end read signed.
                 * Fable's pond reads its ripple map so, all four channels signed: as the
                 * sampler's texel, fine bright lines along every crossing. Each level the
                 * sampler would take, its bytes blended signed, then the two levels by the
                 * sampler's level of detail) */
                "  if (nl > 1) {\n"
                "    float lx = textureQueryLod(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv).x;\n"
                "    int l0 = min(int(floor(lx)), nl - 1);\n"
                "    float lf = lx - float(l0);\n"
                "    vec4 v = sbilod(ti, si, uv, l0);\n"
                "    if (lf > 0.0 && l0 + 1 < nl) v = mix(v, sbilod(ti, si, uv, l0 + 1), lf);\n"
                "    return mix(f, v, m);\n"
                "  }\n"
                "  vec2 w = fract(uv * vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)) - 0.5);\n"
                /* (a weight on a whole 256th is that one: the interpolation here lands a hair
                 * under it, nxdk_pgraph_tests' Bump_env_lum's columns at 32, 96 and 224 256ths;
                 * 256 is the next texel's whole) */
                "  ivec2 q = ivec2(floor(w * 256.0 + 1.0 / 1024.0));\n"
                "  ivec4 r = sbi(textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 0), fl);\n"
                "  ivec4 g = sbi(textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 1), fl);\n"
                "  ivec4 b = sbi(textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 2), fl);\n"
                "  ivec4 a = sbi(textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 3), fl);\n"
                "  return mix(f, vec4(ibil(r, q), ibil(g, q), ibil(b, q), ibil(a, q)), m);\n"
                "}\n", rect_code);
            break;
        }
    }
    bool lumst = false, bumpst = false;
    for (unsigned s = 1; s < 4; s++) {
        lumst |= ((k->tex_modes >> (5 * s)) & 0x1F) == HLE_PST_BUMPENVMAP_LUM;
        bumpst |= ((k->tex_modes >> (5 * s)) & 0x1F) == HLE_PST_BUMPENVMAP;
    }
    if (bumpst) {
        /* a bump stage's matrix (without luminance): its halves and, in its bumpl
         * word, each value's rest in 256ths of its half's last place (vk/draw.c
         * bump_mat: the NV2A's 16 bits of mantissa) */
        const size_t n = strlen(sfun);
        snprintf(sfun + n, sizeof(sfun) - n,
            "mat2 bmat(uint a, uint c, uint r) {\n"
            "  vec4 h = vec4(unpackHalf2x16(a), unpackHalf2x16(c));\n"
            "  ivec4 e; frexp(h, e);\n"
            "  ivec4 q = ivec4(int(r << 24u) >> 24, int(r << 16u) >> 24, int(r << 8u) >> 24, int(r) >> 24);\n"
            "  h += vec4(q) / 256.0 * ldexp(vec4(1.0), max(e, ivec4(-13)) - 11);\n"
            "  return mat2(h.xy, h.zw);\n"
            "}\n");
    }
    if (lumst) {
        /* a bump-env luminance stage's luminance from an unsigned linear-filtered
         * texel: its red bytes blended as the texture unit does (weights cut to
         * 1/256, samps), not by the sampler's rounded weights (nxdk_pgraph_tests'
         * Bump_env_lum, the 5- and 4-bit maps' ramps from 123 to 132 across a
         * texel edge: a column a byte over where the weight's cut and rounded
         * 256ths part); point-sampled, mipmapped or minified (the stage's own
         * min filter: Pixel_shader BumpEnvMapLuminance's water map), as sampled */
        const size_t n = strlen(sfun);
        snprintf(sfun + n, sizeof(sfun) - n,
            "float lum8(uint s, vec2 uv, float f, bool pt) {\n"
            "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0x1FFu;\n"
            "%s"
            "  if ((pc.flags & (1u << s)) != 0u)\n"
            "    uv /= vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)) /\n"
            "          ((pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
            "  uv = nvcte2i(pc.tex[s], uv, ti, dFdxCoarse(uv), dFdyCoarse(uv));\n"
            "  vec2 tz = uv * vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0));\n"
            "  float rho = max(length(dFdx(tz)), length(dFdy(tz)));\n"
            "  if (pt || rho > 1.0 || textureQueryLevels(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)])) > 1) return f;\n"
            "  vec2 w = fract(tz - 0.5);\n"
            "  ivec2 q = ivec2(floor(w * 256.0 + 1.0 / 1024.0));\n"
            "  ivec4 c = ivec4(floor(textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 0) * 255.0 + 0.5));\n"
            "  int t = (c.w * (256 - q.x) + c.z * q.x + 128) >> 8, b = (c.x * (256 - q.x) + c.y * q.x + 128) >> 8;\n"
            "  return float((t * (256 - q.y) + b * q.y + 128) >> 8) / 255.0;\n"
            "}\n", rect_code);
    }
    if (k->bordc >> 4) {     /* (HLE_CONV2: a stage under a convolution filter) */
        const size_t n = strlen(sfun);
        snprintf(sfun + n, sizeof(sfun) - n, "%s",
            "vec4 nvconv(uint ti, uint si, vec2 uv, bool gauss) {\n"
            "  vec2 sz = vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0));\n"
            "  vec2 c = floor(uv * sz) + 0.5;\n"
            "#define NVCT(o) textureLod(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), (c + o) / sz, 0.0)\n"
            "  if (gauss) return 0.25 * (NVCT(vec2(-0.5, -0.5)) + NVCT(vec2(0.5, -0.5)) + NVCT(vec2(-0.5, 0.5)) + NVCT(vec2(0.5, 0.5)));\n"
            "  return 0.5 * NVCT(vec2(0.0)) + 0.125 * (NVCT(vec2(-1.0, -0.5)) + NVCT(vec2(1.0, -0.5)) + NVCT(vec2(-1.0, 0.5)) + NVCT(vec2(1.0, 0.5)));\n"
            "#undef NVCT\n"
            "}\n");
    }
    bool need16 = false;
    for (unsigned s = 1; s < 4; s++) {
        const unsigned in = s == 3 ? (k->input_tex >> 20) & 3 : s == 2 ? (k->input_tex >> 16) & 1 : 0;
        const unsigned m = (k->tex_modes >> (5 * s)) & 0x1F;
        need16 |= ((k->dot_map >> (4 * (s - 1))) & 7) == 4 && tex16(k, in) == 1;
        need16 |= (m == HLE_PST_BUMPENVMAP || m == HLE_PST_BUMPENVMAP_LUM) && tex16(k, in);
    }
    bool yuv = (k->input_tex & HLE_PSH_CSC) != 0;
    for (unsigned s = 0; s < 4; s++) {
        yuv |= yuv_stage(k, s);
    }
    if (yuv) {
        /* a YUY2 / UYVY stage's texel or SET_CONTROL0's colour-space conversion: its
         * (r, g, b) as (Y, Cb, Cr), BT.601 video range as the NV2A's integer steps:
         * the luma term (298 (Y - 16) - 96) / 256 cut, red's chroma term and green's
         * two cut at half a unit (doubled after), green one up, blue's whole
         * (nxdk_pgraph_tests' Texture_format YUY2: 2168 texels' colours fitted, every
         * pixel exact so; xemu's form rounded the sum, a unit or two off) */
        const size_t n = strlen(sfun);
        snprintf(sfun + n, sizeof(sfun) - n,
            "vec3 nvcsc(vec3 c) {\n"
            "  ivec3 b = ivec3(floor(clamp(c, 0.0, 1.0) * 255.0 + 0.5));\n"
            "  int y = (298 * (b.r - 16) - 96) >> 8, u = b.g - 128, v = b.b - 128;\n"
            "  return vec3(clamp(ivec3(y + 2 * ((409 * v + 128) >> 9),\n"
            "                          y + 1 - 2 * (((100 * u) >> 9) + ((208 * v) >> 9)),\n"
            "                          y + ((516 * u) >> 8)), 0, 255)) / 255.0;\n"
            "}\n");
    }
    if (need16) {
        /* a 16-bit texture's words (tex16): its four texels gathered, each word
         * whole, and blended as the texture unit does (weights cut to 1/256, as
         * samps); one texel or a mipmapped one as sampled. hl16: an R16B16's two
         * words for HILO_1; tb16: the bytes a bump stage reads, as the texel's
         * (r, g, b, a) */
        const size_t n = strlen(sfun);
        snprintf(sfun + n, sizeof(sfun) - n,
            "int ibil16(ivec4 c, ivec2 q) {\n"
            "  int t = (c.w * (256 - q.x) + c.z * q.x + 128) >> 8;\n"
            "  int b = (c.x * (256 - q.x) + c.y * q.x + 128) >> 8;\n"
            "  return (t * (256 - q.y) + b * q.y + 128) >> 8; }\n"
            "#define HLE_G16(c, k) ivec4(textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], "
            "uSmp[nonuniformEXT(si)]), uv, c) * k + 0.5)\n"
            "ivec2 w16(uint s, vec2 uv, vec4 f, bvec2 sg, bool pt, bool y) {\n"
            "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0x1FFu;\n"
            "%s"
            "  if ((pc.flags & (1u << s)) != 0u)\n"
            "    uv /= vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)) /\n"
            "          ((pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
            "  uv = nvcte2i(pc.tex[s], uv, ti, dFdxCoarse(uv), dFdyCoarse(uv));\n"
            "  if (pt || (pc.flags & (16u << s)) != 0u ||\n"
            "      textureQueryLevels(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)])) > 1)\n"
            "    return y ? ivec2(int(f.g * 255.0 + 0.5) * 256 + int(f.b * 255.0 + 0.5), 0)\n"
            "             : ivec2(f.ag * 65535.0 + 0.5);\n"
            /* (a weight on a whole 256th is that one, as samps: nxdk_pgraph_tests'
             * Bump_map R16B16, four pixels a column off) */
            "  ivec2 q = ivec2(floor(fract(uv * vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], "
            "uSmp[0]), 0)) - 0.5) * 256.0 + 1.0 / 1024.0));\n"
            "  ivec4 h = y ? HLE_G16(1, 255.0) * 256 + HLE_G16(2, 255.0) : HLE_G16(3, 65535.0);\n"
            "  ivec4 l = y ? ivec4(0) : HLE_G16(1, 65535.0);\n"
            "  if (sg.x) h -= (h >> 15) * 65536;\n"
            "  if (sg.y) l -= (l >> 15) * 65536;\n"
            "  return ivec2(ibil16(h, q), ibil16(l, q)) & 0xFFFF;\n"
            "}\n"
            "vec3 hl16(uint s, vec2 uv, vec4 f, bvec2 sg, bool pt, bool y) {\n"
            "  return vec3(vec2(w16(s, uv, f, sg, pt, y)) / 65535.0, 1.0);\n"
            "}\n"
            "vec4 tb16(uint s, vec2 uv, vec4 f, bvec2 sg, bool pt, bool y) {\n"
            "  ivec2 w = w16(s, uv, f, sg, pt, y);\n"
            "  return y ? vec4(1.0, float(w.x >> 8) / 255.0, float(w.x & 255) / 255.0, 1.0)\n"
            "           : vec4(w.x & 255, w.y >> 8, w.y & 255, w.x >> 8) / 255.0;\n"
            "}\n", rect_code);
    }
    const bool nvcol = (k->compare & HLE_PSH_NVCOL) != 0;
    /* (the perspective attributes in doubles: nvtiep, HLE_NVPW) */
    const bool nvp = nvcol && (k->compare & HLE_PSH_ZWORD);
    /* (the fixed formats' depth word in integers where it can be: nvzfast) */
    const bool nvz64 = nvp && g_hle_glsl_i64 && !(k->compare & HLE_PSH_ZFWORD);
    put(&b, "#version 450\n#extension GL_EXT_nonuniform_qualifier : require\n%s%s%s",
        nvz64 ? "#extension GL_ARB_gpu_shader_int64 : require\n#define HLE_NVZ64\n" : "",
        k->wbuf == 2 || nvcol ? HLE_WBUF_BARY_EXT_GLSL : "", hle_push_glsl());
    /* The depth word over the rasterizer's own depth, biased toward the eye (zcons:
     * vk/draw.c, the word's early test): the GPU tests a pixel before its stage
     * runs again (writing gl_FragDepth turns that off: 7% of the GPU in Grand
     * Theft Auto: Vice City's saved state) and drops only what would fail */
    if (k->zcons) {
        put(&b, "layout(depth_greater) out float gl_FragDepth;\n");
    }
    /* The fragment's position as the pixel stage reads it (every gl_FragCoord
     * below is this, replaced at the end). A draw into an
     * antialiased surface (vk/draw.c aarep) draws once per sample: the position
     * is then the pixel's centre, in pixels, and the texels of the other
     * samples are left alone (at the end of main: their derivatives stay). */
    put(&b, "vec4 nvFC;\nbool nvAAk = false;\nint nvlK = -1;\n");    /* With SET_SMOOTHING_CONTROL's bit 0 (the multisample switch) set, each
     * sample of an antialiased surface is covered by its own point, in 1/16
     * pixel from the pixel's corner (nvms: such a pass, vk/draw.c aarep):
     * CENTER_CORNER_2 the centre and the corner, SQUARE_OFFSET_4 the corner,
     * (1/2, 0), (0, 1/2) and the centre (nxdk_pgraph_tests' Antialiasing_tests
     * Composite* and Swath_width through their resolve: every edge pixel). The
     * values stay the centre's. */
    if ((k->compare & HLE_PSH_NVLINE) || nvcol) {
        put(&b, "bool nvms() { return (pc.flags & 4096u) != 0u && pc.mvp[1].x >= 1.0; }\n"
                "ivec2 nvsmp() { return pc.mvp[1].y >= 2.0 ? ivec2(pc.mvp[1].zw) * 8 : pc.mvp[1].z != 0.0 ? ivec2(0) : ivec2(8); }\n");
    }
    /* The NV2A's colour interpolation, measured on the console's pictures of
     * nxdk_pgraph_tests' Attrib_carryover (pgraph/tools/colourtri.py: 27240
     * pixels of a triangle with known corners and colours, three pictures):
     * the corners' colours as bytes, three whole weights that add up to 256
     * (the first and the last corner's 256 times their barycentric weight
     * rounded down, the middle corner's the rest) and one rounding,
     * (sum + 128) >> 8. The corners in the order the triangle is drawn in: a
     * strip's odd triangle i + 1, i, i + 2, a fan's and a polygon's their
     * first vertex first (Shade_model; pb/pbdraw.c draws them as lists so). */
    /* a colour as the byte the NV2A makes of it: the float's first 14 bits
     * of mantissa, times 255 in halves cut, then rounded (0.1 is 25, 0.5 is
     * 128, 0.7 is 178: the console's quads and lines of one colour;
     * nxdk_pgraph_tests' Depth_buffer, 2000 quads a picture whose corners'
     * greys are a few millionths apart: pgraph/tools/depthcol.py, every one) */
    put(&b, "float nvc8(float x) {\n"
            "  uint v = floatBitsToUint(x);\n"
            "  int e = int((v >> 23) & 0xffu);\n"
            "  if ((v >> 31) != 0u || e < 0x76) return 0.0;\n"
            "  if (e >= 0x7f) return 255.0;\n"
            "  uint fr = (((v >> 10) & 0x1fffu) | 0x2000u) << uint(e - 0x74);\n"
            "  return float(min((((fr * 0xffu) >> 23) + 1u) >> 1, 255u));\n"
            "}\n"
            "vec4 NVC8(vec4 v) { return vec4(nvc8(v.x), nvc8(v.y), nvc8(v.z), nvc8(v.w)); }\n");
    put(&b, "float nvfw16(float z) {\n"
            "  if (!(z > 0.0)) return 0.0;\n"
            "  if (z >= 511.9375) return 65535.0;\n"
            "  uint b = floatBitsToUint(z);\n"
            "  int e = int(b >> 23) - 120;\n"
            "  return float(e >= 1 ? (uint(e) << 12) | ((b >> 11) & 0xfffu) : 0u);\n"
            "}\n"
            "float nvfw24(float z) {\n"
            "  if (!(z > 0.0)) return 0.0;\n"
            "  return float(min(floatBitsToUint(z) >> 7, 0xfeffffu));\n"
            "}\n");
    if (k->compare & HLE_PSH_ZFWORD) {
        /* A float depth buffer's word for a depth, as the image holds it (the
         * word over 2^n, exact): the 24-bit format's is the float's first 24
         * bits (8 of exponent, 16 of mantissa), the 16-bit one's 4 of exponent
         * (2^-6 its 1) and 12 of mantissa, and 0 for a depth under 2^-6.
         * The words order as the depths do, so the image's own test is the
         * chip's, at the format's precision, and a depth of 1e-40 keeps its
         * word where the depth itself, scaled into the image, was 0
         * (nxdk_pgraph_tests' Depth_buffer, the float clears near 0). */
        put(&b, k->compare & HLE_PSH_ZF16 ?
                "float nvfword(float z) { return nvfw16(z) / 65536.0; }\n" :
                "float nvfword(float z) { return nvfw24(z) / 16777216.0; }\n");
    }
    /* a line's colours (nxdk_pgraph_tests' Attrib_carryover lines, pgraph/tools/
     * colour1d.py, colourloop.py): two whole weights that add up to 256, the
     * end's 256 times the share of the line where the pixel's centre projects
     * on it, rounded down (a whole number up to 128 one less), the start's the
     * rest; one rounding */
    if (hle_psh_line_ends(k)) {
        interp = "flat ";
        put(&b, "layout(location = 14) flat in vec4 vDiffuse1;\n"
                "layout(location = 15) flat in vec4 vSpecular1;\n"
                "layout(location = 10) flat in vec2 vLineIW;\n"
                "#define HLE_V0 nvcol1(vDiffuse, vDiffuse1)\n#define HLE_V1 nvcol1(vSpecular, vSpecular1)\n");
    } else
    if (nvcol) {
        /* the weights from the corners' positions on the rasterizer's 1/16 pixels
         * (vScr: unsnapped ones did worse on Shade_model's projected triangles),
         * in whole numbers where the corners' w is the same (the edge functions,
         * a long division: the float barycentrics were a weight step off on a
         * few pixels in a hundred); else in floats, with perspective.
         * The chip's weights are floats: an edge function times the reciprocal
         * of the triangle's area, the transform unit's kind (a table and two
         * steps, the products cut), so where that reciprocal comes out short a
         * weight that is a whole number lands a hair under it and counts one
         * less: except at the favoured corner (the one over 128; none: the
         * corner after the one under 64, the last if none), whose weight is
         * right. An area whose reciprocal is exact (a power of two:
         * nxdk_pgraph_tests' Alpha_func bars) leaves every whole weight alone;
         * Attrib_carryover's triangle, 15 * 2^20, drops them. */
        put(&b, "#define NVAFFINE %s\n#define NVBARYF %s\n",
            k->compare & HLE_PSH_TEX_AFFINE ? "true" : "false",
            k->compare & HLE_PSH_TEX_AFFINE ? "gl_BaryCoordNoPerspEXT" : "gl_BaryCoordEXT");
        interp = "pervertexEXT ";
        /* (a bump stage too: its own coordinate on an edge, the offset zero along it) */
#define NVTIE_M(sh) ((((k->tex_modes >> (sh)) & 0x1F) == HLE_PST_PROJECT2D || \
                      ((k->tex_modes >> (sh)) & 0x1F) == HLE_PST_BUMPENVMAP || \
                      ((k->tex_modes >> (sh)) & 0x1F) == HLE_PST_BUMPENVMAP_LUM || \
                      (((k->tex_modes >> (sh)) & 0x1F) == HLE_PST_PROJECT3D && ((k->shadow >> ((sh) / 5)) & 1))) ? "true" : "false")
        put(&b, "#define NVTIE_2D(s) ((s) == 0 ? %s : (s) == 1 ? %s : (s) == 2 ? %s : %s)\n",
            NVTIE_M(0), NVTIE_M(5), NVTIE_M(10), NVTIE_M(15));
        /* (and a volume stage's s and t: nxdk_pgraph_tests' Volume_texture, its quads'
         * second triangles a texel column under the console's on every edge) */
#define NVTIE_V(sh) (((k->tex_modes >> (sh)) & 0x1F) == HLE_PST_PROJECT3D && !((k->shadow >> ((sh) / 5)) & 1) ? "true" : "false")
        put(&b, "#define NVTIE_3D(s) ((s) == 0 ? %s : (s) == 1 ? %s : (s) == 2 ? %s : %s)\n",
            NVTIE_V(0), NVTIE_V(5), NVTIE_V(10), NVTIE_V(15));
#undef NVTIE_V
        /* With perspective the weights are the shares themselves, cut: each corner's
         * edge function over its w, over their sum, to the nearest float, times 256
         * and cut, the first and the last corner's (the middle one the rest); a share
         * that is a whole number as that float counts one less unless it is the
         * favoured corner's, whatever the area's reciprocal (the small quads' pixel
         * between two corners, shares 128, 0, 128). nxdk_pgraph_tests' Depth_buffer
         * _fixed_function, the big quad from 135.67 to 200 in w: pgraph/tools/
         * perspcol.py, 14198 of 14199 pixels on plain cuts (148.999997 is 149); its
         * top row, shares whole on every third pixel, drops them past the middle.
         * In doubles: the floats here land a hair to either side of a whole number.
         * Only where they would: what the weights take of a share is its 256ths,
         * whole or not, and the floats' share is within 3e-4 of a 256th of the
         * doubles' (a pixel inside: no sum cancels), so a share farther than 1/1024
         * of one from a whole 256th (or 0, or the only one: set so, the GPU's own
         * division is not exact on x / x) is the doubles' own. */
        put(&b, nvp ?
                "#define HLE_NVPW if (!flat_w && near && t > 0 && vScrV[0].z > 0.0 && vScrV[1].z > 0.0 && vScrV[2].z > 0.0) { "
                "vec3 bq = vec3(e) / vec3(vScrV[0].z, vScrV[1].z, vScrV[2].z), bf = bq / (bq.x + bq.y + bq.z), xf = bf * 256.0; "
                "int nz = int(e.x == 0) + int(e.y == 0) + int(e.z == 0); "
                "if (nz == 2) { bf = vec3(notEqual(e, ivec3(0))); xf = bf * 256.0; } "
                "bvec3 far = bvec3(uvec3(greaterThan(abs(xf - floor(xf + 0.5)), vec3(1.0 / 1024.0))) | uvec3(equal(e, ivec3(0)))); "
                "if (!(e.x >= 0 && e.y >= 0 && e.z >= 0 && (nz == 2 || all(far)))) { "
                "dvec3 bd = dvec3(e) / dvec3(vScrV[0].z, vScrV[1].z, vScrV[2].z); bd /= bd.x + bd.y + bd.z; "
                "bf = vec3(bd); xf = bf * 256.0; } "
                "vec3 rf = floor(xf); bool dr = true; "
                "int pf = bf.x > 0.5 ? 0 : bf.y > 0.5 ? 1 : bf.z > 0.5 ? 2 : bf.z <= 0.25 ? 0 : bf.x <= 0.25 ? 1 : 2; "
                "w0 = max(rf.x - (xf.x == rf.x && pf != 0 && dr ? 1.0 : 0.0), 0.0); "
                "w2 = max(rf.z - (xf.z == rf.z && pf != 2 && dr ? 1.0 : 0.0), 0.0); "
                /* (a smoothed polygon's pixel past an edge: as nvcol's own rule below) */
                "if (pc.mvp[2].z != 0.0 && (e.x < 0 || e.y < 0 || e.z < 0)) { w0 = trunc(xf.x); w2 = trunc(xf.z); mid0 = true; } }\n" :
                "#define HLE_NVPW\n");
        put(&b, "layout(location = 11) pervertexEXT in vec3 vScrV[3];\n"
                "%s"
                "int nvdiv(int e, int t, out bool whole) {\n"
                "  if (e >= t) { whole = true; return 256; }\n"
                "  int q = 0;\n"
                "  for (int i = 0; i < 8; i++) { e <<= 1; q <<= 1; if (e >= t) { e -= t; q |= 1; } }\n"
                "  whole = e == 0;\n"
                "  return q;\n"
                "}\n"
                /* Texel ties. The weights again: the two outer corners' are short
                 * where the area's reciprocal is (the favoured one aside), the middle
                 * one takes what they leave. A texture coordinate that falls exactly
                 * on a texel edge comes out under it where those corners pull it down
                 * (their coordinate over the middle one's): nxdk_pgraph_tests'
                 * checkerboard background, exact at every edge row and column: the
                 * console takes the texel before the edge in the quad's first
                 * triangle on the rows where v is the last corner's weight and that
                 * corner is not the favoured one, the exact texel everywhere else. */
                "layout(location = 22) pervertexEXT in vec4 vHPV[3];\n"
                "layout(location = 12) pervertexEXT in vec4 vTexP0[3];\n"
                "layout(location = 13) pervertexEXT in vec4 vTexP1[3];\n"
                "layout(location = 14) pervertexEXT in vec4 vTexP2[3];\n"
                "layout(location = 15) pervertexEXT in vec4 vTexP3[3];\n"
                "vec2 nvTie[4] = vec2[4](vec2(0.0), vec2(0.0), vec2(0.0), vec2(0.0));\n"
                "vec2 nvNice[4] = vec2[4](vec2(0.0), vec2(0.0), vec2(0.0), vec2(0.0));\n"
                /* (a stage of a 2D texture whose three corners sit on whole sixteenths
                 * of a texel: only there is a coordinate on an edge a tie for sure;
                 * generated coordinates land near edges without being on them) */
                "#define NVTIE_ST(s, P) if ((pc.flags & (16u << s)) != 0u && (pc.tex[s] & 0xFFFFu) != 0u && (NVTIE_2D(s) || NVTIE_3D(s))) { \\\n"
                "    vec2 z = NVTIE_3D(s) ? 16.0 * vec2(textureSize(sampler3D(uTex3D[nonuniformEXT(pc.tex[s] & 0xFFFFu)], uSmp[0]), 0).xy) : \\\n"
                "             (pc.flags & (1u << s)) != 0u ? vec2(16.0) : 16.0 * vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(pc.tex[s] & 0xFFFFu)], uSmp[0]), 0)); \\\n"
                "    vec2 w = fract(P[0].xy * z) + fract(P[1].xy * z) + fract(P[2].xy * z); \\\n"
                "    nvNice[s] = vec2(equal(w, vec2(0.0))); \\\n"
                /* (a q under 0 at every corner: the coordinate is the interpolation over q,
                 * so one that comes out over lands under the edge: nxdk_pgraph_tests'
                 * Texture_projective_2D UniformSigns, its q = -1 and -0.5 quads' first and
                 * last rows on the frame's texel edges as the positive ones) */
                "    float sq = P[0].w < 0.0 && P[1].w < 0.0 && P[2].w < 0.0 ? -1.0 : 1.0; \\\n"
                "    if (short_r) nvTie[s] = vec2(greaterThan(sq * (l0 * (P[0].xy - P[1].xy) + l2 * (P[2].xy - P[1].xy)), vec2(0.0))) * nvNice[s]; }\n"
                /* With perspective the interpolated coordinate here is a millionth off
                 * either way, enough to take the wrong texel beside an edge, and the
                 * console's is the exact one to a float: on an edge it takes the
                 * texel over it, and a last place short of the edge it is on it. So
                 * the coordinate itself, from the corners' own and their w, in
                 * doubles, then the nearest float: a point-filtered 2D stage takes
                 * that, set just inside its texel (nxdk_pgraph_tests' W_buffering:
                 * TriH's top row, s exactly 1/2 of 256, is texel 128, where the
                 * interpolation here gave 127; WallQuad's s at 119.999997 is 120). */
                "vec3 nvUV[4] = vec3[4](vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));\n"
                "float nvFogPix = 0.0; bool nvFogOk = false;\n"
                /* (a corner whose w is held at 2^64 or under 2^-32: the vertex stage scaled
                 * its clip-space point, vsh.c tail, and the GPU's perspective weights
                 * are not the corners' there, so every 2D stage takes its coordinate
                 * from the corners, not only the point-sampled ones) */
                "#define NVWX (any(lessThan(abs(vec3(vScrV[0].z, vScrV[1].z, vScrV[2].z)), vec3(uintBitsToFloat(0x2F800000u)))) || "
                "any(greaterThanEqual(abs(vec3(vScrV[0].z, vScrV[1].z, vScrV[2].z)), vec3(uintBitsToFloat(0x5F800000u)))))\n"
                "#define NVTIEP_NEED(s) (((pc.flags & (16u << s)) != 0u || NVWX) && (pc.flags & (256u << s)) == 0u && (pc.tex[s] & 0xFFFFu) != 0u && NVTIE_2D(s))\n"
                "#define NVSQ1(P, c) (double(P[0].c) * double(P[1].w) == double(P[1].c) * double(P[0].w) && "
                "double(P[0].c) * double(P[2].w) == double(P[2].c) * double(P[0].w))\n"
                "#define NVSQ(s, P) (NVTIEP_NEED(s) && !(P[0].w == 1.0 && P[1].w == 1.0 && P[2].w == 1.0) && (NVSQ1(P, x) || NVSQ1(P, y)))\n"
                "#define NVTIEP_ST(s, P) if (NVTIEP_NEED(s)) { \\\n"
                "    double qq = nw.x * double(P[0].w) + nw.y * double(P[1].w) + nw.z * double(P[2].w); \\\n"
                "    if (qq != 0.0lf) { \\\n"
                "      dvec2 st = (nw.x * dvec2(P[0].xy) + nw.y * dvec2(P[1].xy) + nw.z * dvec2(P[2].xy)); \\\n"
                /* (a projective q, one not 1 at every corner, and a coordinate whose
                 * ratio to it is the same at every corner, so that no interpolation
                 * moves it: s or t and q each to the nearest float, then times q's
                 * reciprocal cut to a float, the product cut too; a coordinate that is
                 * q itself comes out a hair under 1, the last texel, unless q is a power
                 * of two (nxdk_pgraph_tests' Texgen_normal_map, its faces flat: Identity,
                 * Double, Half exact so, Shift*Minus's q = t a hair under 1, 832 of 891
                 * faces, the other 59 wrap by no rule of q alone found; Texture_projective
                 * _2D ZeroCorner_Bitri, s = q: the edge texel, the border on the columns
                 * where q is 1/2, 1/4 ..). A ratio that varies keeps the exact divide:
                 * its ties go up on the console (VerticalCross: 0.484375 / 0.96875 is
                 * 0.5, texel 64 of 128).) */
                "      dvec3 cw = dvec3(P[0].w, P[1].w, P[2].w); \\\n"
                "      dvec3 cs = dvec3(P[0].x, P[1].x, P[2].x), ct = dvec3(P[0].y, P[1].y, P[2].y); \\\n"
                "      bvec2 sq = bvec2(cs.x * cw.y == cs.y * cw.x && cs.x * cw.z == cs.z * cw.x, \\\n"
                "                       ct.x * cw.y == ct.y * cw.x && ct.x * cw.z == ct.z * cw.x); \\\n"
                "      if (any(sq) && !(P[0].w == 1.0 && P[1].w == 1.0 && P[2].w == 1.0)) { ivec2 ee; double ws = nw.x + nw.y + nw.z; \\\n"
                "        dvec2 rq = dvec2(1.0lf / double(float(qq / ws))); \\\n"
                "        rq = ldexp(trunc(frexp(rq, ee) * 16777216.0lf) / 16777216.0lf, ee); \\\n"
                "        dvec2 sc = dvec2(vec2(st / ws)) * rq.x; sc = ldexp(trunc(frexp(sc, ee) * 16777216.0lf) / 16777216.0lf, ee); \\\n"
                "        st = mix(st / qq, sc, sq); } \\\n"
                "      else st /= qq; \\\n"
                "      dvec2 sz = dvec2(textureSize(sampler2D(uTex2D[nonuniformEXT(pc.tex[s] & 0xFFFFu)], uSmp[0]), 0)); \\\n"
                "      dvec2 tt = (pc.flags & (1u << s)) != 0u ? st : st * sz; \\\n"
                "      dvec2 tf = dvec2(vec2(tt)), r = floor(tf + 0.5lf), d = tf - r; \\\n"
                /* (on an edge: the texel over it, unless the corners' cut shares pull the
                 * coordinate under: the first and the last corner's share cut, the middle
                 * one's the rest, the favoured corner's and a share of 0 whole, so the
                 * coordinate moves by d0 (c1 - c0) + d2 (c1 - c2), d0, d2 >= 0; under where
                 * that is below 0 for sure (both parts at or under 0). nxdk_pgraph_tests'
                 * W_param ff_w_zero_inf bitri: u = v = 128 down the far corner's line, the
                 * console's texel (127, 128); Texture_perspective bitri's centre likewise;
                 * W_buffering TriH's top row, no pull, over) */
                "      dvec2 dz = dvec2(1.0lf); \\\n"
                "      vec3 aw = abs(vec3(vScrV[0].z, vScrV[1].z, vScrV[2].z)); \\\n"
                "      bvec3 wt = lessThan(aw, vec3(uintBitsToFloat(0x2F800000u))); \\\n"
                "      if (P[0].w == P[1].w && P[1].w == P[2].w && !NVWX) { dvec3 bb = nw / (nw.x + nw.y + nw.z); \\\n"
                "        int pf = bb.x > 0.5lf ? 0 : bb.y > 0.5lf ? 1 : bb.z > 0.5lf ? 2 : bb.z <= 0.25lf ? 0 : bb.x <= 0.25lf ? 1 : 2; \\\n"
                "        dvec2 a0 = bb.x != 0.0lf && pf != 0 ? dvec2(P[1].xy) - dvec2(P[0].xy) : dvec2(0.0lf); \\\n"
                "        dvec2 a2 = bb.z != 0.0lf && pf != 2 ? dvec2(P[1].xy) - dvec2(P[2].xy) : dvec2(0.0lf); \\\n"
                "        dz = mix(dz, dvec2(-1.0lf), bvec2(uvec2(lessThan(a0 + a2, dvec2(0.0lf))) & uvec2(greaterThanEqual(a0 * a2, dvec2(0.0lf))))); } \\\n"
                "      if ((pc.flags & (16u << s)) != 0u) tt = mix(tf, r + mix(sign(d), dz, equal(d, dvec2(0.0lf))) / 4096.0lf, lessThan(abs(d), dvec2(1.0lf / 4096.0lf))); else tt = tf; \\\n"
                "      if (all(lessThan(abs(tt), dvec2(1.0e6lf)))) nvUV[s] = vec3(vec2(tt / sz), 1.0); } }\n"
                "#define HLE_NVTIEP_FN void nvtiep() { \\\n"
                "  if (!(NVTIEP_NEED(0) || NVTIEP_NEED(1) || NVTIEP_NEED(2) || NVTIEP_NEED(3) || NVFOGP_NEED)) return; \\\n"
                "  if (!(NVTIEP_NEED(0) || NVTIEP_NEED(1) || NVTIEP_NEED(2) || NVTIEP_NEED(3)) && nvfogf()) return; \\\n"
                "  vec2 fa = vScrV[0].xy * 16.0, fm = vScrV[1].xy * 16.0, fd = vScrV[2].xy * 16.0; \\\n"
                "  vec3 wv = vec3(vScrV[0].z, vScrV[1].z, vScrV[2].z); \\\n"
                /* (the clip-space triangle, each corner's position times its w: a corner
                 * behind the eye is one like the rest) */
                "  if (any(isnan(wv)) || any(isinf(wv))) return; \\\n"
                "  bool scrok = max(max(abs(fa.x), abs(fa.y)), max(max(abs(fm.x), abs(fm.y)), max(abs(fd.x), abs(fd.y)))) < 4000000.0; \\\n"
                "  if (!scrok && !NVWX) return; \\\n"
                "  dvec3 ww = NVAFFINE ? dvec3(1.0lf) : dvec3(wv); \\\n"
                "  dvec3 p0 = dvec3(dvec2(floor(fa + 0.5)) * ww.x, ww.x), p1 = dvec3(dvec2(floor(fm + 0.5)) * ww.y, ww.y); \\\n"
                "  dvec3 p2 = dvec3(dvec2(floor(fd + 0.5)) * ww.z, ww.z), ph = dvec3(dvec2(floor(gl_FragCoord.xy * 16.0 + 0.5)), 1.0lf); \\\n"
                "  dvec3 nw = dvec3(dot(ph, cross(p1, p2)), dot(ph, cross(p2, p0)), dot(ph, cross(p0, p1))); \\\n"
                /* (a corner out at its ends whose screen position is past 2^22 or no
                 * number, the fixed-function divide's: the clip-space points the GPU
                 * took, each weight times its corner's scale (the points' common
                 * factor cancels), or times its scaled w without perspective (the
                 * screen's own weights), the pixel's centre on the same axes) */
                "  if (!scrok) { \\\n"
                "    dvec3 h0 = dvec3(vHPV[0].xyz), h1 = dvec3(vHPV[1].xyz), h2 = dvec3(vHPV[2].xyz); \\\n"
                "    dvec3 hq = dvec3(dvec2(gl_FragCoord.xy) * dvec2(pc.vp) + dvec2(-1.0lf, 1.0lf), 1.0lf); \\\n"
                "    dvec3 hs = NVAFFINE ? dvec3(h0.z, h1.z, h2.z) : dvec3(vHPV[0].w, vHPV[1].w, vHPV[2].w); \\\n"
                "    nw = dvec3(dot(hq, cross(h1, h2)), dot(hq, cross(h2, h0)), dot(hq, cross(h0, h1))) * hs; } \\\n"
                "  if (nw.x + nw.y + nw.z == 0.0lf) return; \\\n"
                "  NVTIEP_ST(0, vTexP0) \\\n"
                "  NVTIEP_ST(1, vTexP1) \\\n"
                "  NVTIEP_ST(2, vTexP2) \\\n"
                "  NVTIEP_ST(3, vTexP3) \\\n"
                "  if (NVFOGP_NEED) { double sn = nw.x + nw.y + nw.z; if (sn != 0.0lf) { \\\n"
                "    nvFogPix = float((nw.x * double(vFogPV[0]) + nw.y * double(vFogPV[1]) + nw.z * double(vFogPV[2])) / sn); \\\n"
                "    nvFogOk = true; } } \\\n"
                "}\n"
                "#define HLE_NVTIE_FN void nvtie() { \\\n"
                /* (a corner out at its ends: three clip-space points with no area
                 * between them, one the other's through the eye, draw nothing on the
                 * console; the clip's scale leaves a hair of one here: W_param's ff
                 * quad w -inf, corners 2^-64 (240, 160, 1) and -2^64 (240, 160, 1)) */
                /* (and a corner behind the eye at an ordinary w: W_param's w_neg_strip, a
                 * strip's corner at w 1 and the next at -0.1 over the same screen point,
                 * where the GPU's clipper filled a band from a corner at (0, 0, 0, 0)) */
                "  if (NVWX || any(lessThan(vec3(vScrV[0].z, vScrV[1].z, vScrV[2].z), vec3(0.0)))) { \\\n"
                "    dvec3 h0 = dvec3(vHPV[0].xyz), h1 = dvec3(vHPV[1].xyz), h2 = dvec3(vHPV[2].xyz); \\\n"
                "    if (abs(dot(h0, cross(h1, h2))) <= 1.0e-5lf * length(h0) * length(h1) * length(h2)) discard; } \\\n"
                "  ivec2 p = ivec2(floor(gl_FragCoord.xy * 16.0 + 0.5)); \\\n"
                "  vec2 fa = vScrV[0].xy * 16.0, fm = vScrV[1].xy * 16.0, fd = vScrV[2].xy * 16.0; \\\n"
                /* (with perspective: nvtiep, the coordinates themselves) */
                /* (and without perspective a projective q whose s / q or t / q is one value
                 * at every corner: nvtiep's divide, nxdk_pgraph_tests' Texture_projective_2D
                 * ZeroCorner) */
                "  if (!(vScrV[0].z == vScrV[1].z && vScrV[1].z == vScrV[2].z) || \\\n"
                "      NVSQ(0, vTexP0) || NVSQ(1, vTexP1) || NVSQ(2, vTexP2) || NVSQ(3, vTexP3)) { HLE_NVTIEP_CALL return; } \\\n"
                "  if (max(max(abs(fa.x), abs(fa.y)), max(max(abs(fm.x), abs(fm.y)), max(abs(fd.x), abs(fd.y)))) >= 16000.0) return; \\\n"
                "  ivec2 a = ivec2(floor(fa + 0.5)) - p, m = ivec2(floor(fm + 0.5)) - p, d = ivec2(floor(fd + 0.5)) - p; \\\n"
                "  ivec3 e = ivec3(m.x * d.y - m.y * d.x, d.x * a.y - d.y * a.x, a.x * m.y - a.y * m.x); \\\n"
                "  int t = e.x + e.y + e.z; \\\n"
                "  if (t < 0) { e = -e; t = -t; } \\\n"
                "  if (t <= 0 || e.x < 0 || e.y < 0 || e.z < 0) return; \\\n"
                "  bool short_r = nvrsign(t) < 0; \\\n"
                /* (the favoured corner: the one over a half; none: the first where the
                 * third's weight is a quarter or under, else the second where the first's
                 * is and the second's is over it, else the third (weights 1/4, 1/4, 1/2:
                 * the third; Lighting_spotlight's checker background, pixel (480, 240) in
                 * 24 pictures)) */
                "  int f = 2 * e.x > t ? 0 : 2 * e.y > t ? 1 : 2 * e.z > t ? 2 : 4 * e.z <= t ? 0 : 4 * e.x <= t && 4 * e.y > t ? 1 : 2; \\\n"
                "  float l0 = f != 0 ? float(e.x) : 0.0, l2 = f != 2 ? float(e.z) : 0.0; \\\n"
                "  NVTIE_ST(0, vTexP0) \\\n"
                "  NVTIE_ST(1, vTexP1) \\\n"
                "  NVTIE_ST(2, vTexP2) \\\n"
                "  NVTIE_ST(3, vTexP3) \\\n"
                "}\n"
                "#define HLE_NVTIE nvtie();\n"
                "#define HLE_NVTIE_AT(s) nvTie[s]\n#define HLE_NVNICE_AT(s) nvNice[s]\n#define HLE_NVUV_AT(s) nvUV[s]\n"
                /* A pass-through stage's coordinate without perspective: the corners' by
                 * the edge functions, in doubles (the host's interpolation lands either
                 * side of a whole 4096th, and the stage's 12 bits go up from it:
                 * nxdk_pgraph_tests' Pixel_shader Passthru, every pixel of its quad,
                 * pgraph/tools/ptfit.py); ex says it is the exact one */
                "vec4 nvptc(vec4 P[3], vec4 f, out bool ex) {\n"
                "  ex = false;\n"
                "  if (!(vScrV[0].z == vScrV[1].z && vScrV[1].z == vScrV[2].z)) return f;\n"
                "  vec2 fa = vScrV[0].xy * 16.0, fm = vScrV[1].xy * 16.0, fd = vScrV[2].xy * 16.0;\n"
                "  if (max(max(abs(fa.x), abs(fa.y)), max(max(abs(fm.x), abs(fm.y)), max(abs(fd.x), abs(fd.y)))) >= 16000.0) return f;\n"
                "  ivec2 p = ivec2(floor(gl_FragCoord.xy * 16.0 + 0.5));\n"
                "  ivec2 a = ivec2(floor(fa + 0.5)) - p, m = ivec2(floor(fm + 0.5)) - p, d = ivec2(floor(fd + 0.5)) - p;\n"
                "  ivec3 e = ivec3(m.x * d.y - m.y * d.x, d.x * a.y - d.y * a.x, a.x * m.y - a.y * m.x);\n"
                "  int t = e.x + e.y + e.z;\n"
                "  if (t == 0) return f;\n"
                "  ex = true;\n"
                "  return vec4((double(e.x) * dvec4(P[0]) + double(e.y) * dvec4(P[1]) + double(e.z) * dvec4(P[2])) / double(t));\n"
                "}\n"
                "#define HLE_NVPT(P, f, ex) nvptc(P, f, ex)\n"
                /* A smoothed polygon (vk/draw.c, ff.h HLE_TOPO_CONSERV): how many of the
                 * pixel's 8 points (the smoothed line's, ff.h nvl_cov) lie inside the
                 * triangle; one on an edge counts on a left or top edge. */
                "int nvp_cov() {\n"
                "  ivec2 p = ivec2(floor(gl_FragCoord.xy)) * 16;\n"
                "  vec2 fa = vScrV[0].xy * 16.0, fm = vScrV[1].xy * 16.0, fd = vScrV[2].xy * 16.0;\n"
                "  if (max(max(abs(fa.x), abs(fa.y)), max(max(abs(fm.x), abs(fm.y)), max(abs(fd.x), abs(fd.y)))) >= 16000.0) return 8;\n"
                "  ivec2 A = ivec2(floor(fa + 0.5)), M = ivec2(floor(fm + 0.5)), D = ivec2(floor(fd + 0.5));\n"
                "  int t = (M.x - A.x) * (D.y - A.y) - (M.y - A.y) * (D.x - A.x);\n"
                "  if (t == 0) return 0;\n"
                "  int sg = t < 0 ? -1 : 1, k = 0;\n"
                "  ivec3 ex = sg * ivec3(M.y - D.y, D.y - A.y, A.y - M.y), ey = sg * ivec3(D.x - M.x, A.x - D.x, M.x - A.x);\n"
                "  for (int j = 0; j < 8; j++) {\n"
                "    ivec2 s = p + ivec2(2 * j, 2 * ((3 * j) & 7));\n"
                "    ivec2 a = A - s, m = M - s, d = D - s;\n"
                "    ivec3 e = sg * ivec3(m.x * d.y - m.y * d.x, d.x * a.y - d.y * a.x, a.x * m.y - a.y * m.x);\n"
                "    bool ins = true;\n"
                "    for (int i = 0; i < 3; i++)\n"
                "      ins = ins && (e[i] > 0 || (e[i] == 0 && (ex[i] > 0 || (ex[i] == 0 && ey[i] > 0))));\n"
                "    if (ins) k++;\n"
                "  }\n"
                "  return k;\n"
                "}\n"
                /* An antialiased surface's sample under SET_SMOOTHING_CONTROL's bit 0
                 * (vk/draw.c aarep): whether its own point lies inside the triangle, the
                 * values staying the pixel's centre's. */
                /* (a corner 1000 pixels off or more: the edge functions' products past
                 * the integers', each cross product's two vectors scaled to their own
                 * largest component first, which keeps its sign, in floats. Accepting
                 * every pixel there let the conservative raster's extra pixels through:
                 * Vice City's people, slivers with a corner far off the screen, drew
                 * long streaks across it.) */
                "float nvs_x(vec2 p, vec2 q) {\n"
                "  p /= max(max(abs(p.x), abs(p.y)), 1.0e-30); q /= max(max(abs(q.x), abs(q.y)), 1.0e-30);\n"
                "  return p.x * q.y - p.y * q.x;\n"
                "}\n"
                "bool nvs_far(vec2 S, vec2 fa, vec2 fm, vec2 fd) {\n"
                "  vec2 A = floor(fa + 0.5), M = floor(fm + 0.5), D = floor(fd + 0.5);\n"
                "  if (!all(lessThan(abs(vec3(A.x, M.x, D.x)), vec3(3.0e38))) || !all(lessThan(abs(vec3(A.y, M.y, D.y)), vec3(3.0e38)))) return false;\n"
                "  float t = nvs_x(M - A, D - A);\n"
                "  if (t == 0.0) return false;\n"
                "  float sg = t < 0.0 ? -1.0 : 1.0;\n"
                "  vec3 ex = sg * vec3(M.y - D.y, D.y - A.y, A.y - M.y), ey = sg * vec3(D.x - M.x, A.x - D.x, M.x - A.x);\n"
                "  vec2 a = A - S, m = M - S, d = D - S;\n"
                "  vec3 e = sg * vec3(nvs_x(m, d), nvs_x(d, a), nvs_x(a, m));\n"
                "  bool ins = true;\n"
                "  for (int i = 0; i < 3; i++)\n"
                "    ins = ins && (e[i] > 0.0 || (e[i] == 0.0 && (ex[i] > 0.0 || (ex[i] == 0.0 && ey[i] > 0.0))));\n"
                "  return ins;\n"
                "}\n"
                "bool nvs_cov() {\n"
                "  ivec2 s = ivec2(floor(gl_FragCoord.xy)) * 16 + nvsmp();\n"
                "  vec2 fa = vScrV[0].xy * 16.0, fm = vScrV[1].xy * 16.0, fd = vScrV[2].xy * 16.0;\n"
                "  if (max(max(abs(fa.x), abs(fa.y)), max(max(abs(fm.x), abs(fm.y)), max(abs(fd.x), abs(fd.y)))) >= 16000.0) return nvs_far(vec2(s), fa, fm, fd);\n"
                "  ivec2 A = ivec2(floor(fa + 0.5)), M = ivec2(floor(fm + 0.5)), D = ivec2(floor(fd + 0.5));\n"
                "  int t = (M.x - A.x) * (D.y - A.y) - (M.y - A.y) * (D.x - A.x);\n"
                "  if (t == 0) return false;\n"
                "  int sg = t < 0 ? -1 : 1;\n"
                "  ivec3 ex = sg * ivec3(M.y - D.y, D.y - A.y, A.y - M.y), ey = sg * ivec3(D.x - M.x, A.x - D.x, M.x - A.x);\n"
                "  ivec2 a = A - s, m = M - s, d = D - s;\n"
                "  ivec3 e = sg * ivec3(m.x * d.y - m.y * d.x, d.x * a.y - d.y * a.x, a.x * m.y - a.y * m.x);\n"
                "  bool ins = true;\n"
                "  for (int i = 0; i < 3; i++)\n"
                "    ins = ins && (e[i] > 0 || (e[i] == 0 && (ex[i] > 0 || (ex[i] == 0 && ey[i] > 0))));\n"
                "  return ins;\n"
                "}\n"
                "#define HLE_NVPS if (pc.mvp[2].z == 2.0) { if (!nvs_cov()) discard; } else if (pc.mvp[2].z != 0.0) { nvlK = nvp_cov(); if (nvlK == 0) discard; }\n"
                "float nvW0, nvW2; bool nvMid0, nvWok = false;\n"
                "void nvweights() {\n"
                "  ivec2 p = ivec2(floor(gl_FragCoord.xy * 16.0 + 0.5));\n"
                "  vec2 fa = vScrV[0].xy * 16.0, fm = vScrV[1].xy * 16.0, fd = vScrV[2].xy * 16.0;\n"
                "  float w0, w2;\n"
                "  bool mid0 = false;\n"
                "  bool flat_w = NVAFFINE || (vScrV[0].z == vScrV[1].z && vScrV[1].z == vScrV[2].z);\n"
                "  bool near = max(max(abs(fa.x), abs(fa.y)), max(max(abs(fm.x), abs(fm.y)), max(abs(fd.x), abs(fd.y)))) < 16000.0;\n"
                "  ivec2 a = ivec2(floor(fa + 0.5)) - p, m = ivec2(floor(fm + 0.5)) - p, d = ivec2(floor(fd + 0.5)) - p;\n"
                "  ivec3 e = ivec3(m.x * d.y - m.y * d.x, d.x * a.y - d.y * a.x, a.x * m.y - a.y * m.x);\n"
                "  int t = e.x + e.y + e.z;\n"
                "  if (t < 0) { e = -e; t = -t; }\n"
                "  if (near && flat_w && t > 0 && e.x >= 0 && e.y >= 0 && e.z >= 0) {\n"
                "    bool h0, h2;\n"
                "    int q0 = nvdiv(e.x, t, h0), q2 = nvdiv(e.z, t, h2);\n"
                /* (none over a half: the corner after the smallest share's, and where the
                 * two smallest are both a quarter the third: nxdk_pgraph_tests'
                 * High_vertex_count, its 6 pixel quads' pixels at shares 1/4, 1/2, 1/4
                 * as 63, 130, 63 and at 1/4, 1/4, 1/2 as 63, 65, 128, the 1/12, 5/12,
                 * 1/2 one as 21, 108, 127: pgraph/tools/hvcfit2.py, every pixel) */
                "    int f = 2 * e.x > t ? 0 : 2 * e.y > t ? 1 : 2 * e.z > t ? 2 : 4 * e.z <= t && 4 * e.x != t ? 0 : 4 * e.x <= t && 4 * e.y != t ? 1 : 2;\n"
                /* (nvdivr, above, while the area is a float's whole number, under 2^24:
                 * past it the area and the edge values are rounded themselves, how
                 * was not measured, and the whole shares' drop is what is known:
                 * Texture_perspective's big quad, 65.0000045 of an area of 71621564,
                 * stays 65 where the product with the reciprocal of its float is under) */
                "    if (t < 16777216) {\n"
                "      w0 = float(clamp(f == 0 ? 256 - nvdivr(t - e.x, t, true) : nvdivr(e.x, t, false), 0, 256));\n"
                "      w2 = float(clamp(f == 2 ? 256 - nvdivr(t - e.z, t, true) : nvdivr(e.z, t, false), 0, 256));\n"
                "    } else {\n"
                "      bool drop = (h0 || h2) && nvrsign(t) < 0;\n"
                "      w0 = float(max(h0 && f != 0 && drop ? q0 - 1 : q0, 0));\n"
                "      w2 = float(max(h2 && f != 2 && drop ? q2 - 1 : q2, 0));\n"
                "    }\n"
                "  } else if (near && flat_w && t > 0 && pc.mvp[2].z != 0.0) {\n"
                /* a smoothed polygon's pixel whose centre lies outside the triangle
                 * (nvp_cov): the shares carried on past the edge, the outer corners'
                 * cut toward 0 and the middle one's the rest, and each channel kept
                 * within the three corners' own (pgraph/tools/polysmooth.py on
                 * nxdk_pgraph_tests' Smoothing_control: its two triangles' 172 such
                 * pixels, none off) */
                "    bool hx;\n"
                "    ivec2 n = abs(ivec2(e.x, e.z)) / t;\n"
                "    w0 = float(sign(e.x) * (n.x * 256 + nvdiv(abs(e.x) - n.x * t, t, hx)));\n"
                "    w2 = float(sign(e.z) * (n.y * 256 + nvdiv(abs(e.z) - n.y * t, t, hx)));\n"
                "    mid0 = true;\n"
                "  } else {\n"
                "    vec3 b = NVBARYF;\n"
                "    if (near && t > 0) {\n"
                "      b = vec3(e) / float(t);\n"
                "      if (!flat_w) { b /= vec3(vScrV[0].z, vScrV[1].z, vScrV[2].z); b /= b.x + b.y + b.z; }\n"
                "    }\n"
                "    vec3 x = b * 256.0, r = floor(x + 0.5);\n"
                "    bvec3 whole = lessThan(abs(x - r), vec3(1.0 / 1024.0));\n"
                "    int f = x.x > 128.0 ? 0 : x.y > 128.0 ? 1 : x.z > 128.0 ? 2 : x.z < 64.0 ? 0 : x.x < 64.0 ? 1 : 2;\n"
                /* (a whole share of 0 stays 0: W_param's rcc_w_zero_inf, the far corner's
                 * share 0 to a float, its weight -1 gave the others one more) */
                "    w0 = max(whole.x ? r.x - (f == 0 ? 0.0 : 1.0) : floor(x.x), 0.0);\n"
                "    w2 = max(whole.z ? r.z - (f == 2 ? 0.0 : 1.0) : floor(x.z), 0.0);\n"
                "    HLE_NVPW\n"
                "  }\n"
                "  nvW0 = w0; nvW2 = w2; nvMid0 = mid0; nvWok = true;\n"
                "}\n"
                "vec4 nvcol(vec4 c[3]) {\n"
                "  if (!nvWok) nvweights();\n"
                "  float w0 = nvW0, w2 = nvW2; bool mid0 = nvMid0;\n"
                "  float w1 = 256.0 - w0 - w2;\n"
                "  vec4 ca = NVC8(c[0]), cm = NVC8(c[1]), cd = NVC8(c[2]);\n"
                "  vec4 cs = floor((ca * w0 + cm * w1 + cd * w2 + 128.0) / 256.0) / 255.0;\n"
                "  return mid0 ? clamp(cs, min(min(ca, cm), cd) / 255.0, max(max(ca, cm), cd) / 255.0) : cs;\n"
                "}\n"
                "#define vDiffuse vDiffuseV[3]\n#define vSpecular vSpecularV[3]\n"
                "%s", k_nvrsign,
                /* two-sided lighting: a back face's colours are the program's oB0 / oB1,
                 * by the same weights */
                /* (flat shaded: the colours of the triangle's first vertex, the one
                 * the NV2A takes them from: pb/pbdraw.c puts it first, a flat quad's
                 * geometry stage gives all three its last's) */
                (k->vol & HLE_PSH_TWO_SIDE) ? k->flat ?
                "layout(location = 7) pervertexEXT in vec4 vBack0V[3];\n"
                "layout(location = 8) pervertexEXT in vec4 vBack1V[3];\n"
                "#define HLE_V0 (gl_FrontFacing ? vDiffuseV[0] : vBack0V[0])\n"
                "#define HLE_V1 (gl_FrontFacing ? vSpecularV[0] : vBack1V[0])\n" :
                "layout(location = 7) pervertexEXT in vec4 vBack0V[3];\n"
                "layout(location = 8) pervertexEXT in vec4 vBack1V[3];\n"
                "#define HLE_V0 (gl_FrontFacing ? nvcol(vDiffuseV) : nvcol(vBack0V))\n"
                "#define HLE_V1 (gl_FrontFacing ? nvcol(vSpecularV) : nvcol(vBack1V))\n" :
                k->flat ? "#define HLE_V0 vDiffuseV[0]\n#define HLE_V1 vSpecularV[0]\n" :
                "#define HLE_V0 nvcol(vDiffuseV)\n#define HLE_V1 nvcol(vSpecularV)\n");
        if ((k->compare & HLE_PSH_ZWORD) || (k->wbuf == 2 && g_hle_glsl_f64)) {
            /* The pixel's w from the corners' own, in doubles, to the nearest float: a
             * wall at w 17 is 17 on every pixel, where the interpolated one was a last
             * place under on some and stored 16 (nxdk_pgraph_tests' Depth_Clamp, a
             * floor clamped to 16 that hides it). */
            put(&b, "double nvwpixd(float fb) {\n"
                    "  vec2 fa = vScrV[0].xy * 16.0, fm = vScrV[1].xy * 16.0, fd = vScrV[2].xy * 16.0;\n"
                    "  vec3 wv = vec3(vScrV[0].z, vScrV[1].z, vScrV[2].z);\n"
                    "  if (any(isnan(wv)) || any(isinf(wv))) return fb;\n"
                    "  if (max(max(abs(fa.x), abs(fa.y)), max(max(abs(fm.x), abs(fm.y)), max(abs(fd.x), abs(fd.y)))) >= 4000000.0) return fb;\n"
                    "  dvec3 ww = dvec3(wv);\n"
                    "  dvec3 p0 = dvec3(dvec2(floor(fa + 0.5)) * ww.x, ww.x), p1 = dvec3(dvec2(floor(fm + 0.5)) * ww.y, ww.y);\n"
                    "  dvec3 p2 = dvec3(dvec2(floor(fd + 0.5)) * ww.z, ww.z), ph = dvec3(dvec2(floor(gl_FragCoord.xy * 16.0 + 0.5)), 1.0lf);\n"
                    "  dvec3 c0 = cross(p1, p2);\n"
                    "  double dm = dot(p0, c0), dn = dot(ph, c0 + cross(p2, p0) + cross(p0, p1));\n"
                    "  if (dm == 0.0lf || dn == 0.0lf) return fb;\n"
                    "  double w = dm / dn;\n"
                    "  return w > 0.0lf && w < 3.0e38lf ? w : double(fb);\n"
                    "}\n"
                    "float nvwpix(float fb) { return float(nvwpixd(fb)); }\n"
                    /* (an integer W format's w: the difference from the triangle's middle
                     * corner's (its second in the order drawn) cut toward it to 5 bits under
                     * that corner's last float place, 1/32 of a word near 2^24:
                     * nxdk_pgraph_tests' W_buffering LargeZ, w from 16777046 to 16777047 down
                     * the screen: 16777046 on the console's words, 16777047 on the second
                     * triangle's last rows from a fraction of 31/32 on, the first triangle's
                     * (whose middle corner is a top one) 16777046 to its bottom) */
                    "double nvwq(double w) {\n"
                    "  double wf = double(vScrV[1].z);\n"
                    "  if (!(wf > 0.0lf) || isinf(vScrV[1].z)) return w;\n"
                    "  int ee; frexp(wf, ee);\n"
                    "  double q = ldexp(1.0lf, ee - 1 - 23 - 5);\n"
                    "  return wf + trunc((w - wf) / q) * q;\n"
                    "}\n"
                    /* (cut toward zero to a float, as nvzdepth's nvc24: the word's sum with its
                     * offsets is the depth's, in floats of 24 bits) */
                    "double nvwc24(double x) {\n"
                    "  float f = float(abs(x));\n"
                    "  if (double(f) > abs(x)) f = uintBitsToFloat(floatBitsToUint(f) - 1u);\n"
                    "  return x < 0.0lf ? -double(f) : double(f);\n"
                    "}\n"
                    "#define HLE_WBUF_VW nvwpix(dot(gl_BaryCoordEXT, bWv))\n"
                    "#define HLE_WBUF_VWD nvwpixd(dot(gl_BaryCoordEXT, bWv))\n");
        }
        /* (nvtiep is in doubles: only where the device has them) */
        /* The fog's value in the table modes is only ever cut to its 4096ths
         * (and set against 1 and 2), so where the floats' value is farther from
         * a cut than their error (the shares e / w as nvcol's, the corners'
         * differences from the first: 1.6e-6 of those and 1.5e-7 of the value)
         * it decides as the doubles' (nvtiep) does; the doubles elsewhere. */
        put(&b, nvp ? "layout(location = 17) pervertexEXT in float vFogPV[3];\n"
                      "bool nvfogf() {\n"
                      "  vec2 fa = vScrV[0].xy * 16.0, fm = vScrV[1].xy * 16.0, fd = vScrV[2].xy * 16.0;\n"
                      "  vec3 wv = NVAFFINE ? vec3(1.0) : vec3(vScrV[0].z, vScrV[1].z, vScrV[2].z);\n"
                      "  if (!(wv.x > 0.0 && wv.y > 0.0 && wv.z > 0.0 && max(wv.x, max(wv.y, wv.z)) < 1.0e30)) return false;\n"
                      "  if (!(max(max(abs(fa.x), abs(fa.y)), max(max(abs(fm.x), abs(fm.y)), max(abs(fd.x), abs(fd.y)))) < 16000.0)) return false;\n"
                      "  ivec2 p = ivec2(floor(gl_FragCoord.xy * 16.0 + 0.5));\n"
                      "  ivec2 a = ivec2(floor(fa + 0.5)) - p, m = ivec2(floor(fm + 0.5)) - p, d = ivec2(floor(fd + 0.5)) - p;\n"
                      "  ivec3 e = ivec3(m.x * d.y - m.y * d.x, d.x * a.y - d.y * a.x, a.x * m.y - a.y * m.x);\n"
                      "  if (e.x + e.y + e.z < 0) e = -e;\n"
                      "  if (e.x < 0 || e.y < 0 || e.z < 0 || e.x + e.y + e.z == 0) return false;\n"
                      "  vec3 fv = vec3(vFogPV[0], vFogPV[1], vFogPV[2]);\n"
                      "  if (!all(lessThan(abs(fv), vec3(1.0e30)))) return false;\n"
                      "  float v;\n"
                      "  if (int(e.x == 0) + int(e.y == 0) + int(e.z == 0) == 2) v = e.x != 0 ? fv.x : e.y != 0 ? fv.y : fv.z;\n"
                      "  else {\n"
                      "    vec3 bq = vec3(e) / wv;\n"
                      "    float d1 = fv.y - fv.x, d2 = fv.z - fv.x;\n"
                      "    v = fv.x + (bq.y * d1 + bq.z * d2) / (bq.x + bq.y + bq.z);\n"
                      "    float x = (v - 1.0) * 4096.0, mg = 4096.0 * (1.6e-6 * (abs(d1) + abs(d2)) + 1.5e-7 * abs(v)) + 1.0e-4;\n"
                      "    if (!(abs(x - floor(x + 0.5)) > mg)) return false;\n"
                      "  }\n"
                      "  nvFogPix = v; nvFogOk = true;\n"
                      "  return true;\n"
                      "}\n"
                      "#define HLE_NVTIEP_CALL nvtiep();\n" :
                "#define HLE_NVTIEP_CALL\n#undef HLE_NVTIEP_FN\n#define HLE_NVTIEP_FN\n");
        if (nvp) {          /* (the table modes alone read nvFogPix: vFogE) */
            put(&b, k->fog >= 5 && k->fog <= 8 ?
                    "#define NVFOGP_NEED (!(vFogPV[0] == vFogPV[1] && vFogPV[1] == vFogPV[2]))\n" :
                    "#define NVFOGP_NEED false\n");
        }
        if (k->compare & HLE_PSH_ZWORD) {
            /* The depth word. The NV2A stores the depth cut to the buffer's
             * steps, the polygon offset added before the cut (its constant in
             * steps, its factor times the larger of the depth's two slopes):
             * nxdk_pgraph_tests' W_buffering ZBuf pictures, 325 down to 58
             * over 200 rows, every word the exact depth's whole part, plus 16
             * with the offset 16. gl_FragCoord.z cannot give that near the far
             * plane (a float's 24 bits are the word itself there: a step off
             * on half the pixels, and its derivatives no slope at all), so the
             * depth comes from the corners' own, in doubles: the corners'
             * 1/16-pixel positions and edge functions, the products exact on any
             * screen-sized polygon. Corners behind the eye keep the
             * plain cut. */
            /* The chip works on floats of 24 bits, cut: the depth is one before
             * the offsets go on, so past 2^23 it is whole already and the slope
             * term adds its own whole part (W_buffering FloorQuad: 7783 on every
             * word for 7783.75), and where depth and slope term add up to a
             * whole number the cut depth leaves the sum a hair under it (TriH:
             * 324 for 303.5865 + 21.4135). nvc24: a double cut to a float.
             * The depth itself is the favoured corner's (the one whose weight is
             * a half or over; none: the second corner when its weight is a
             * quarter or over, else the larger of the first's and the third's,
             * the first on a tie: the float dumps' quads near 0, where the
             * differences vanish and the word is the favoured corner's alone,
             * show it on 768 pixels, pgraph/tools/zflush.py) plus the other two's weights times their depths'
             * differences from it: each weight the edge function over the area cut
             * to a float, each product kept 5 bits under the largest term's last
             * place (Depth_buffer's 24-bit dumps: 370 of 372 rows where one weight
             * matters, pgraph/tools/zmodel3.py). A difference under the smallest
             * float is none, as a corner's depth under it is 0: the float dumps'
             * quads near 0 show the left corners' word on their left half and the
             * right ones' on the right, no gradient. */
            /* (a weight: the fixed formats' is the edge function over the area, a division
             * cut to a float; the float formats' is the edge function times the area's
             * reciprocal cut to a float, the product a float multiply: a depth that is a
             * whole word in exact arithmetic comes out one under on Depth_buffer's bottom
             * quad (weight 1/16 of an area 460800) and not on W_buffering's TriH
             * (0.2581 of 510720, 6e-8 over the word): pgraph's RESULTS.md, full123 - full124.
             * The edge value itself rounded to a float is untested past 2^24: no picture
             * moves with it kept whole) */
            put(&b, "#define NVZW(n, dt) %s\n", (k->compare & HLE_PSH_ZFWORD) ?
                "double(float(n) * float(nvc24(rdt)))" : "nvc24((n) * rdt)");
            /* (the doubles' reach: corners within 2^53 / 8 sixteenths of a pixel squared,
             * the edge functions' products and their differences exact. W_buffering's
             * FloorQuad has corners past the 2^22 it was before, toward the horizon:
             * its words from the corners there are the console's on 8 depth pictures
             * more than the rasterizer's depth was) */
            put(&b, "#define NVZNEAR 33000000.0\n");
            /* (the last fallback's depth, past that reach or with no depths: under
             * zcons the rasterizer's is biased, so the corners' interpolated across
             * the screen as it interpolates them) */
            put(&b, "#define NVZFCZ %s\n", k->zcons ?
                "(dot(gl_BaryCoordNoPerspEXT, vec3(vZsV[0], vZsV[1], vZsV[2])) * pc.zscale)" : "gl_FragCoord.z");
            put(&b, "layout(location = 16) pervertexEXT in float vZsV[3];\n"
                    "double nvc24(double x) {\n"
                    "  float f = float(abs(x));\n"
                    "  if (double(f) > abs(x)) f = uintBitsToFloat(floatBitsToUint(f) - 1u);\n"
                    "  return x < 0.0lf ? -double(f) : double(f);\n"
                    "}\n"
                    /* The depth clip range (SET_CLIP_MIN / MAX, in the buffer's units)
                     * and SET_ZMIN_MAX_CONTROL: a depth past the range is clamped to
                     * it (ZCLAMP_EN CLAMP) or the pixel is not drawn (CULL), and with
                     * CULL_NEAR_FAR_EN a primitive whose corners are all past the same
                     * end is not drawn at all (nxdk_pgraph_tests' ZMinMaxControl: the
                     * range 10 .. 100 under quads from 0 to 150). The GPU's own clip
                     * is the buffer's whole range. With CONTROL0's Z_PERSPECTIVE the
                     * pixel's test is on w (a quad at w 10 drawn at any z), the whole
                     * primitive's still on z. */
                    "bool nvzcull = false, nvzprim = false;\n"
                    /* The same word in integers (the fixed formats, no offsets, the
                     * range on z). The edge functions and the area in 64 bits; each
                     * share the float under n / area as the doubles make it (n times
                     * the area's reciprocal, each rounded: within 2^-52 of the
                     * quotient, so the float under the quotient itself wherever that
                     * is farther than that from a float, which a 64-bit remainder
                     * tells, else the doubles' own); each product the share's
                     * mantissa times the depths' difference on their common step,
                     * rounded to 53 bits as the doubles round it, its exponent's
                     * float as theirs, cut to the steps of 2^-28 of the largest term;
                     * the sum cut to 24 bits. Anything else (corners 2^15 apart in
                     * magnitude, the favoured corner's depth finer than those steps)
                     * the doubles (pgraph/tools/zfuzz.c: 49 million pixels, in and
                     * around their triangles, the same word both ways). */
                    "#ifdef HLE_NVZ64\n"
                    "int nvbl64(uint64_t x) { uint h = uint(x >> 32), l = uint(x); return h != 0u ? 33 + findMSB(h) : l != 0u ? 1 + findMSB(l) : 0; }\n"
                    "float nvzw64(int64_t n, int64_t dt) {\n"
                    "  if (n == 0l) return 0.0;\n"
                    "  int64_t an = abs(n);\n"
                    "  uint bw = floatBitsToUint(float(an) / float(dt));\n"
                    "  for (int i = 0; i < 8; i++) {\n"
                    "    uint m = (bw & 0x7fffffu) | 0x800000u;\n"
                    "    int k = 150 - int((bw >> 23) & 0xffu);\n"
                    "    if (k < 0) break;\n"
                    "    uint64_t nk = k >= 64 ? 0ul : uint64_t(an) << k;\n"
                    "    int64_t R = int64_t(nk - uint64_t(m) * uint64_t(dt));\n"
                    "    if (R < 0l) { bw--; continue; }\n"
                    "    if (R >= dt) { bw++; continue; }\n"
                    "    if (R == 0l) break;\n"
                    "    float md = float(m) * float(dt);\n"
                    "    if (float(R) / md < 4.5e-16 || float(dt - R) / md < 4.5e-16) break;\n"
                    "    return n < 0l ? -uintBitsToFloat(bw) : uintBitsToFloat(bw);\n"
                    "  }\n"
                    "  return float(nvc24(double(n) * (1.0lf / double(dt))));\n"
                    "}\n"
                    "uint64_t nvrn53(uint64_t x, out int s) {\n"
                    "  int L = nvbl64(x), sh = L > 53 ? L - 53 : 0;\n"
                    "  uint64_t q = x >> sh;\n"
                    "  if (sh > 0) {\n"
                    "    uint64_t rem = x & ((1ul << sh) - 1ul), hf = 1ul << (sh - 1);\n"
                    "    if (rem > hf || (rem == hf && (q & 1ul) != 0ul)) q++;\n"
                    "  }\n"
                    "  s = sh;\n"
                    "  return q;\n"
                    "}\n"
                    "int nvrn24e(uint64_t r, int t) {\n"
                    "  int L = nvbl64(r);\n"
                    "  if (L <= 24) return L - 1 + t;\n"
                    "  int sh = L - 24;\n"
                    "  uint64_t top = r >> sh, rem = r & ((1ul << sh) - 1ul), hf = 1ul << (sh - 1);\n"
                    "  bool up = rem > hf || (rem == hf && (top & 1ul) != 0ul);\n"
                    "  return L - 1 + t + (up && top == 0xfffffful ? 1 : 0);\n"
                    "}\n"
                    "bool nvzfast(float zm, out float res) {\n"
                    "  res = 0.0;\n"
                    /* (a constant offset of whole steps is the sum's: the testing passes' step, draw.c) */
                    "  float zo0 = uintBitsToFloat(pc.psc[18]);\n"
                    "  if (uintBitsToFloat(pc.psc[19]) != 0.0 || zo0 != floor(zo0) || abs(zo0) > 65536.0) return false;\n"
                    "  vec2 zr = vec2(pc.mvp[0].x, pc.mvp[0].y);\n"
                    "  int zrm = int(pc.mvp[0].z);\n"
                    "  bool ranged = zr.y > zr.x;\n"
                    "  if (ranged && (zrm & 8) != 0) return false;\n"
                    "  if (!(vScrV[0].z > 0.0 && vScrV[1].z > 0.0 && vScrV[2].z > 0.0)) return false;\n"
                    "  vec2 fa = vScrV[0].xy * 16.0, fm = vScrV[1].xy * 16.0, fd = vScrV[2].xy * 16.0;\n"
                    "  if (!(max(max(abs(fa.x), abs(fa.y)), max(max(abs(fm.x), abs(fm.y)), max(abs(fd.x), abs(fd.y)))) < 4000000.0)) return false;\n"
                    "  i64vec2 a = i64vec2(floor(fa + 0.5));\n"
                    "  i64vec2 u = i64vec2(floor(fm + 0.5)) - a, v = i64vec2(floor(fd + 0.5)) - a;\n"
                    "  i64vec2 q = i64vec2(floor(gl_FragCoord.xy * 16.0 + 0.5)) - a;\n"
                    "  int64_t det = u.x * v.y - u.y * v.x;\n"
                    "  if (det == 0l) return false;\n"
                    "  int64_t sg = det < 0l ? -1l : 1l, dt = det * sg;\n"
                    "  int64_t n1 = (q.x * v.y - q.y * v.x) * sg, n2 = (u.x * q.y - u.y * q.x) * sg, n0 = dt - n1 - n2;\n"
                    "  int f = 2l * n0 >= dt ? 0 : 2l * n1 >= dt ? 1 : 2l * n2 >= dt ? 2 : 4l * n1 >= dt ? 1 : n0 >= n2 ? 0 : 2;\n"
                    "  int ia = (f + 1) % 3, ib = (f + 2) % 3;\n"
                    "  float zc[3] = float[3](vZsV[0], vZsV[1], vZsV[2]);\n"
                    "  int64_t nn[3] = int64_t[3](n0, n1, n2);\n"
                    "  for (int i = 0; i < 3; i++)\n"
                    "    if (!(zc[i] == 0.0 || (zc[i] >= 1.0e-30 && zc[i] < 3.0e38))) return false;\n"
                    "  float zf = zc[f];\n"
                    "  uint zfb = floatBitsToUint(zf);\n"
                    "  int ezf = int((zfb >> 23) & 0xffu) - 150;\n"
                    "  uint zfm = zf == 0.0 ? 0u : (zfb & 0x7fffffu) | 0x800000u;\n"
                    "  uint64_t ra[2]; int ta[2], sa[2], ex[2];\n"
                    "  for (int j = 0; j < 2; j++) {\n"
                    "    int ci = j == 0 ? ia : ib;\n"
                    "    float w = nvzw64(nn[ci], dt);\n"
                    "    float zo = zc[ci];\n"
                    "    uint zob = floatBitsToUint(zo);\n"
                    "    int ezo = int((zob >> 23) & 0xffu) - 150;\n"
                    "    uint zom = zo == 0.0 ? 0u : (zob & 0x7fffffu) | 0x800000u;\n"
                    "    int g = zf == 0.0 ? ezo : zo == 0.0 ? ezf : min(ezo, ezf);\n"
                    "    if ((zo != 0.0 && ezo - g > 15) || (zf != 0.0 && ezf - g > 15)) return false;\n"
                    "    int64_t D = int64_t(uint64_t(zom) << (zo != 0.0 ? ezo - g : 0)) - int64_t(uint64_t(zfm) << (zf != 0.0 ? ezf - g : 0));\n"
                    "    sa[j] = (D < 0l) != (w < 0.0) ? -1 : 1;\n"
                    "    uint64_t aD = uint64_t(D < 0l ? -D : D);\n"
                    "    uint wb = floatBitsToUint(abs(w));\n"
                    "    uint wm = w == 0.0 ? 0u : (wb & 0x7fffffu) | 0x800000u;\n"
                    "    int sh;\n"
                    "    ra[j] = nvrn53(uint64_t(wm) * aD, sh);\n"
                    "    ta[j] = int((wb >> 23) & 0xffu) - 150 + g + sh;\n"
                    "    ex[j] = ra[j] != 0ul ? nvrn24e(ra[j], ta[j]) : -10000;\n"
                    "  }\n"
                    "  int E = max(zf != 0.0 ? ezf + 23 : -10000, max(ex[0], ex[1]));\n"
                    "  float zz = 0.0;\n"
                    "  if (E != -10000) {\n"
                    "    int ue = E - 28;\n"
                    "    if (zf != 0.0 && ezf < ue) return false;\n"
                    "    int64_t S = zf != 0.0 ? int64_t(uint64_t(zfm) << (ezf - ue)) : 0l;\n"
                    "    for (int j = 0; j < 2; j++) {\n"
                    "      int s = ta[j] - ue;\n"
                    "      uint64_t t = ra[j] == 0ul ? 0ul : s >= 0 ? ra[j] << s : (-s >= 64 ? 0ul : ra[j] >> (-s));\n"
                    "      S += int64_t(sa[j]) * int64_t(t);\n"
                    "    }\n"
                    "    int64_t aS = abs(S);\n"
                    "    int L = nvbl64(uint64_t(aS));\n"
                    "    if (L > 24) aS &= ~((1l << (L - 24)) - 1l);\n"
                    "    S = S < 0l ? -aS : aS;\n"
                    /* (the offset after the word's cut, the sum cut again: nvzdepth's nvc24s) */
                    "    if (zo0 != 0.0) {\n"
                    "      if (ue > 0 || ue < -40) return false;\n"
                    "      S += int64_t(zo0) * (1l << (-ue));\n"
                    "      aS = abs(S);\n"
                    "      L = nvbl64(uint64_t(aS));\n"
                    "      if (L > 24) aS &= ~((1l << (L - 24)) - 1l);\n"
                    "      S = S < 0l ? -aS : aS;\n"
                    "    }\n"
                    "    zz = ldexp(float(S), ue);\n"
                    "  } else {\n"
                    "    zz = zo0;\n"
                    "  }\n"
                    "  if (ranged) {\n"
                    "    nvzprim = (zrm & 1) != 0 && ((zc[0] < zr.x && zc[1] < zr.x && zc[2] < zr.x) || (zc[0] > zr.y && zc[1] > zr.y && zc[2] > zr.y));\n"
                    "    nvzcull = nvzprim || ((zrm & 2) == 0 && (zz < zr.x || zz > zr.y));\n"
                    "    zz = clamp(zz, zr.x, zr.y);\n"
                    "  }\n"
                    "  res = clamp(zz, 0.0, zm);\n"
                    "  return true;\n"
                    "}\n"
                    "#endif\n"
                    "float nvzdepth(float zm) {\n"
                    "#ifdef HLE_NVZ64\n"
                    "  { float nvr; if (nvzfast(zm, nvr)) return nvr; }\n"
                    "#endif\n"
                    "  float zo0 = uintBitsToFloat(pc.psc[18]), zo1 = uintBitsToFloat(pc.psc[19]);\n"
                    "  vec2 zr = vec2(pc.mvp[0].x, pc.mvp[0].y);\n"
                    "  int zrm = int(pc.mvp[0].z);\n"
                    "  bool ranged = zr.y > zr.x;\n"
                    "  vec2 fa = vScrV[0].xy * 16.0, fm = vScrV[1].xy * 16.0, fd = vScrV[2].xy * 16.0;\n"
                    "  bool near = max(max(abs(fa.x), abs(fa.y)), max(max(abs(fm.x), abs(fm.y)), max(abs(fd.x), abs(fd.y)))) < NVZNEAR;\n"
                    "  dvec2 a = dvec2(floor(fa + 0.5));\n"
                    "  dvec2 u = dvec2(floor(fm + 0.5)) - a, v = dvec2(floor(fd + 0.5)) - a;\n"
                    "  dvec2 q = dvec2(floor(gl_FragCoord.xy * 16.0 + 0.5)) - a;\n"
                    "  double det = u.x * v.y - u.y * v.x;\n"
                    "  vec3 zc = vec3(vZsV[0], vZsV[1], vZsV[2]);\n"
                    "  if (near && det != 0.0lf && vScrV[0].z > 0.0 && vScrV[1].z > 0.0 && vScrV[2].z > 0.0 &&\n"
                    "      all(lessThan(abs(zc), vec3(3.0e38)))) {\n"
                    "    const double fmin = 1.1754943508222875e-38lf;\n"
                    "    dvec3 zd = dvec3(zc);\n"
                    "    zd = mix(zd, dvec3(0.0lf), lessThan(abs(zd), dvec3(fmin)));\n"
                    "    double d1 = zd.y - zd.x, d2 = zd.z - zd.x, dt = abs(det), sg = det < 0.0lf ? -1.0lf : 1.0lf; double rdt = 1.0lf / dt;\n"
                    "    double n1 = (q.x * v.y - q.y * v.x) * sg, n2 = (u.x * q.y - u.y * q.x) * sg;\n"
                    "    dvec3 nn = dvec3(dt - n1 - n2, n1, n2);\n"
                    "    int f = 2.0lf * nn.x >= dt ? 0 : 2.0lf * nn.y >= dt ? 1 : 2.0lf * nn.z >= dt ? 2 :\n"
                    "            4.0lf * nn.y >= dt ? 1 : nn.x >= nn.z ? 0 : 2;\n"
                    "    int ia = (f + 1) % 3, ib = (f + 2) % 3;\n"
                    "    double zf = zd[f], da = zd[ia] - zf, db = zd[ib] - zf;\n"
                    "    if (abs(da) < fmin) da = 0.0lf;\n"
                    "    if (abs(db) < fmin) db = 0.0lf;\n"
                    "    double pa = NVZW(nn[ia], dt) * da, pb = NVZW(nn[ib], dt) * db;\n"
                    "    double mx = max(abs(zf), max(abs(pa), abs(pb))), zz = zf;\n"
                    "    if (mx > 0.0lf) {\n"
                    "      int ue = int((floatBitsToUint(float(mx)) >> 23) & 0xffu) - 155;\n"
                    "      double un = ldexp(1.0lf, ue), ru = ldexp(1.0lf, -ue);\n"
                    "      zz = zf + trunc(pa * ru) * un + trunc(pb * ru) * un;\n"
                    "    }\n"
                    "    double sl = zo1 != 0.0 ? max(abs(v.y * d1 - u.y * d2), abs(u.x * d2 - v.x * d1)) * 16.0lf * rdt : 0.0lf;\n"
                    "    zz = nvc24(zz);\n"
                    "    zz = nvc24(zz + double(zo0) + nvc24(double(zo1) * nvc24(sl)));\n"
                    "    if (ranged) {\n"
                    "      double r0 = double(zr.x), r1 = double(zr.y), dv = zz;\n"
                    "      if ((zrm & 8) != 0)\n"
                    "        dv = dt / (nn.x / double(vScrV[0].z) + nn.y / double(vScrV[1].z) + nn.z / double(vScrV[2].z));\n"
                    "      nvzprim = (zrm & 1) != 0 && (all(lessThan(zd, dvec3(r0))) || all(greaterThan(zd, dvec3(r1))));\n"
                    "      nvzcull = nvzprim || ((zrm & 2) == 0 && (dv < r0 || dv > r1));\n"
                    "      if ((zrm & 8) == 0) zz = clamp(zz, r0, r1);\n"
                    "    }\n"
                    "    return float(clamp(zz, 0.0lf, double(zm)));\n"
                    "  }\n"
                    /* A corner behind the eye: the depth from the corners as they are
                     * all the same, each one's screen position and depth times its w
                     * (the clip-space triangle), so it does not depend on how the GPU
                     * clips it: two passes over one polygon, one clipped at the near
                     * plane and one clamped, make the same word (nxdk_pgraph_tests'
                     * Depth_Clamp depth_equal: an EQUAL pass over a floor through the
                     * eye, drawn on all of it on the console). */
                    "  if (near && all(lessThan(abs(zc), vec3(3.0e38)))) {\n"
                    "    dvec3 wv = dvec3(vScrV[0].z, vScrV[1].z, vScrV[2].z);\n"
                    "    dvec3 p0 = dvec3(dvec2(floor(fa + 0.5)) * wv.x, wv.x), p1 = dvec3(dvec2(floor(fm + 0.5)) * wv.y, wv.y);\n"
                    "    dvec3 p2 = dvec3(dvec2(floor(fd + 0.5)) * wv.z, wv.z);\n"
                    "    dvec3 c0 = cross(p1, p2), c1 = cross(p2, p0), c2 = cross(p0, p1);\n"
                    "    double dm = dot(p0, c0);\n"
                    "    if (dm != 0.0lf && abs(dm) < 1.0e300lf) {\n"
                    "      dvec3 ph = dvec3(dvec2(floor(gl_FragCoord.xy * 16.0 + 0.5)), 1.0lf);\n"
                    "      dvec3 qz = dvec3(zc) * wv;\n"
                    "      dvec3 gz = (c0 * qz.x + c1 * qz.y + c2 * qz.z) / dm;\n"
                    "      double zz = nvc24(dot(ph, gz));\n"
                    "      zz = nvc24(zz + double(zo0) + nvc24(double(zo1) * nvc24(max(abs(gz.x), abs(gz.y)) * 16.0lf)));\n"
                    "      if (ranged) {\n"
                    "        double r0 = double(zr.x), r1 = double(zr.y), dv = zz;\n"
                    "        if ((zrm & 8) != 0) dv = dm / dot(ph, c0 + c1 + c2);\n"
                    "        nvzcull = (zrm & 2) == 0 && (dv < r0 || dv > r1);\n"
                    "        if ((zrm & 8) == 0) zz = clamp(zz, r0, r1);\n"
                    "      }\n"
                    "      return float(clamp(zz, 0.0lf, double(zm)));\n"
                    "    }\n"
                    "  }\n"
                    "  float zo = zo0 + zo1 * max(abs(dFdx(NVZFCZ)), abs(dFdy(NVZFCZ))) * zm;\n"
                    "  float zp = NVZFCZ * zm + zo;\n"
                    "  if (ranged && (zrm & 8) == 0) {\n"
                    "    nvzcull = (zrm & 2) == 0 && (zp < zr.x || zp > zr.y);\n"
                    "    zp = clamp(zp, zr.x, zr.y);\n"
                    "  }\n"
                    "  return clamp(zp, 0.0, zm);\n"
                    "}\n"
                    "float nvzword(float zm) { return floor(nvzdepth(zm)); }\n"
                    /* The clip range's cull alone (nothing stored): a pixel inside its
                     * triangle has a depth within the corners' (nvzdepth's weights are
                     * the shares cut toward 0, their products too, the sum cut to a
                     * float), w too, so with every corner inside the range and no
                     * offsets no pixel can be past it, and the doubles are only for the
                     * triangles that reach an end of it. Inside: each edge function's
                     * sign sure in floats, its error under a few last places of the
                     * products. */
                    "bool nvzmay() {\n"
                    "  vec2 zr = vec2(pc.mvp[0].x, pc.mvp[0].y);\n"
                    "  if (!(zr.y > zr.x)) return false;\n"
                    "  if (pc.mvp[2].z != 0.0 || uintBitsToFloat(pc.psc[18]) != 0.0 || uintBitsToFloat(pc.psc[19]) != 0.0) return true;\n"
                    "  vec3 zc = vec3(vZsV[0], vZsV[1], vZsV[2]), wv = vec3(vScrV[0].z, vScrV[1].z, vScrV[2].z);\n"
                    "  if (!all(lessThan(abs(zc), vec3(3.0e38))) || !all(greaterThan(wv, vec3(0.0)))) return true;\n"
                    "  float lo = min(min(zc.x, zc.y), zc.z), hi = max(max(zc.x, zc.y), zc.z);\n"
                    "  if ((int(pc.mvp[0].z) & 8) != 0) { lo = min(lo, min(min(wv.x, wv.y), wv.z)); hi = max(hi, max(max(wv.x, wv.y), wv.z)); }\n"
                    "  float mg = 1.0e-6 * max(max(abs(lo), abs(hi)), max(abs(zr.x), abs(zr.y))) + 1.0e-30;\n"
                    "  if (!(lo - mg > zr.x && hi + mg < zr.y)) return true;\n"
                    "  vec2 fa = vScrV[0].xy * 16.0, fm = vScrV[1].xy * 16.0, fd = vScrV[2].xy * 16.0;\n"
                    "  if (!(max(max(abs(fa.x), abs(fa.y)), max(max(abs(fm.x), abs(fm.y)), max(abs(fd.x), abs(fd.y)))) < 4000000.0)) return true;\n"
                    "  vec2 p = floor(gl_FragCoord.xy * 16.0 + 0.5);\n"
                    "  vec2 a = floor(fa + 0.5) - p, m = floor(fm + 0.5) - p, d = floor(fd + 0.5) - p;\n"
                    "  vec3 q1 = vec3(m.x * d.y, d.x * a.y, a.x * m.y), q2 = vec3(m.y * d.x, d.y * a.x, a.y * m.x);\n"
                    "  vec3 e = q1 - q2, eb = (abs(q1) + abs(q2) + abs(e)) * (1.0 / 4194304.0);\n"
                    "  return !(all(greaterThan(e, eb)) || all(lessThan(e, -eb)));\n"
                    "}\n");
        }
    } else
    /* two-sided lighting: a back face's v0 / v1 from the program's oB0 / oB1
     * (nxdk_pgraph_tests' Specular_back: black on the console, the front
     * colours here) */
    if (k->vol & HLE_PSH_TWO_SIDE) {
        put(&b, "layout(location = 7) %sin vec4 vBack0;\n"
                "layout(location = 8) %sin vec4 vBack1;\n"
                "#define HLE_V0 (gl_FrontFacing ? vDiffuse : vBack0)\n"
                "#define HLE_V1 (gl_FrontFacing ? vSpecular : vBack1)\n", interp, interp);
    } else {
        put(&b, "#define HLE_V0 vDiffuse\n#define HLE_V1 vSpecular\n");
    }
    /* a texture with its own border (pbdraw.c tex_border_fmt): the reported
     * size's coordinates 4 texels into the bordered image (xemu's) */
    if ((k->compare >> 20) & 0xFu) {
        put(&b, "#define HLE_BORDER2(s, uv) if (((%uu >> (s)) & 1u) != 0u) { "
                "uint bw = floatBitsToUint(pc.scr[s]); "
                "uv = (uv * vec2(uvec2(1u) << uvec2((bw >> 4) & 15u, (bw >> 8) & 15u)) + 4.0) / "
                "vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)); }\n"
                "#define HLE_BORDER3(s, uvw) if (((%uu >> (s)) & 1u) != 0u) { "
                "uint bw = floatBitsToUint(pc.scr[s]); "
                "uvw = (uvw * vec3(uvec3(1u) << uvec3((bw >> 4) & 15u, (bw >> 8) & 15u, (bw >> 12) & 15u)) + 4.0) / "
                "vec3(textureSize(sampler3D(uTex3D[nonuniformEXT(ti)], uSmp[0]), 0)); }\n",
            (k->compare >> 20) & 0xFu, (k->compare >> 20) & 0xFu);
    } else {
        put(&b, "#define HLE_BORDER2(s, uv)\n#define HLE_BORDER3(s, uvw)\n");
    }
    /* A border-mode stage's border colour, the raw ARGB whatever the format:
     * the view's mapping (alpha 1 for R5G6B5 and X8R8G8B8, (1, 1, 1, a) for A8,
     * R8B8's...) went over the sampler's border colour too, the console's is
     * the colour as set (nxdk_pgraph_tests' Texture_border_color). The stage's
     * trio of samplers (vk/draw.c sampler_index): with the standard
     * transparent-black and opaque-white borders s0 and s1 (every mapping
     * gives those alike, applied to borders or not); d = s1 - s0 is the
     * border's share on a channel the texture gives and 0 on a constant one,
     * omw its largest; the stage's own sampler at a point outside the white
     * texture (an identity view, slot 0) is the colour itself. Filtered,
     * mipmapped and anisotropic alike: the share is the same sum. */
    /* A convolution filter (SET_TEXTURE_FILTER MIN 7 / MAG 4, D3D's QUINCUNX and
     * GAUSSIANCUBIC: an antialiased back buffer's swap filter, the XDK's
     * antialiasing filters) at level 0 around the texel under the coordinate:
     * Gaussian (pc.tex bit 29) 1 2 1 / 2 4 2 / 1 2 1 over 16, as four bilinear
     * taps half a texel out; quincunx (a + b + 4c + d + e) / 8, its four
     * outer samples diagonal (the 2x layout's next row half a pixel along): a
     * texel either side, half a row up and down, bilinear. In the image's
     * texels, as xemu's (at a resolution scale the console's blur would undo
     * the scale). Both were bilinear here: the samples D3D's half-texel shift
     * puts the swap's coordinates on, alone, and every FSAA title's edges
     * jagged. */
    if (k->bordc >> 4) {
        put(&b, "vec4 nvconv(uint ti, uint si, vec2 uv, bool gauss);\n"     /* (defined in sfun) */
                "#define HLE_CONV2(s, uv) if (((%uu >> (s)) & 1u) != 0u) return nvconv(ti, si, uv, (pc.tex[s] & 0x20000000u) != 0u);\n",
            (unsigned)(k->bordc >> 4));
    } else {
        put(&b, "#define HLE_CONV2(s, uv)\n");
    }
    if (k->bordc & 15u) {
        put(&b, "vec4 nvbc(vec4 s0, vec4 s1, vec4 bc) {\n"
                "  vec4 d = s1 - s0;\n"
                "  float omw = max(max(d.x, d.y), max(d.z, d.w));\n"
                "  return mix(s0 * (1.0 - omw), s0, greaterThan(d, vec4(omw * 0.5))) + omw * bc;\n"
                "}\n"
                "#define HLE_BORDC2(s, uv) if (((%uu >> (s)) & 1u) != 0u) return nvbc("
                "NVT2C(si + 1u, uv), NVT2C(si + 2u, uv), "
                "textureLod(sampler2D(uTex2D[0], uSmp[nonuniformEXT(si)]), vec2(-4.0), 0.0));\n"
                "#define HLE_BORDC3(s, uvw) if (((%uu >> (s)) & 1u) != 0u) return nvbc("
                "texture(sampler3D(uTex3D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si + 1u)]), uvw), "
                "texture(sampler3D(uTex3D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si + 2u)]), uvw), "
                "textureLod(sampler3D(uTex3D[0], uSmp[nonuniformEXT(si)]), vec3(-4.0), 0.0));\n",
            (unsigned)k->bordc & 15u, (unsigned)k->bordc & 15u);
    } else {
        put(&b, "#define HLE_BORDC2(s, uv)\n#define HLE_BORDC3(s, uvw)\n");
    }
    /* SET_CONTROL0's texture perspective off: the
     * coordinates interpolated across the screen, as the console's
     * (nxdk_pgraph_tests' Texture_perspective_enable) */
    put(&b, k->compare & HLE_PSH_TEX_AFFINE ? "#define HLE_TQ noperspective\n" : "#define HLE_TQ\n");
    /* a stage with cylinder wrap (HLE_PSH_CYLWRAP; the corners' own coordinates, vTexP,
     * there with nvcol): the inputs under other names, main's copies wrapped */
    const bool cylw = (k->input_tex & HLE_PSH_CYLWRAP) && nvcol;
    put(&b, cylw ? "#define HLE_TC0 vTex0c\n#define HLE_TC1 vTex1c\n#define HLE_TC2 vTex2c\n"
                 : "#define HLE_TC0 vTex0\n#define HLE_TC1 vTex1\n#define HLE_TC2 vTex2\n");
    /* a one-pixel line: the pixels the NV2A lights (ff.h) */
    if (k->compare & HLE_PSH_NVLINE) {
        put(&b, "const bool nvl_thin = %s;\n" HLE_NVLINE_FS_GLSL
                /* (a smoothed line, vk/draw.c: the pixel's coverage in eighths) */
                "#define HLE_NVL if (pc.mvp[2].x != 0.0) { nvlK = nvl_cov(); if (nvlK == 0) discard; } "
                "else if (!nvl_lit()) discard;\n",
            k->compare & HLE_PSH_NVLINE_THIN ? "true" : "false");
        if (hle_psh_line_ends(k)) {
            put(&b, "%s", k_nvrsign);
            put(&b, "vec4 nvcol1(vec4 c0, vec4 c1) {\n"
                    "  ivec2 c = ivec2(floor(gl_FragCoord.xy * 16.0 + 0.5)), d = vLine.zw - vLine.xy, e = c - vLine.xy;\n"
                    "  int num = e.x * d.x + e.y * d.y, den = d.x * d.x + d.y * d.y, w = 0;\n"
                    /* a line of its own (not a polygon's side, which takes the polygon's
                     * weights) under the thin rule (SET_SMOOTHING_CONTROL's bit 0 clear):
                     * the share along the longer axis, half a pixel ahead of the pixel's
                     * centre (nxdk_pgraph_tests' 3D_primitive lines, pgraph/tools/
                     * linepersp.py: four lines, every pixel, and Smoothing_control's
                     * plain loop at 0xFFFF0000, w 1: 915 pixels a level or two off with
                     * the other); Attrib_carryover's, under the wide rule, and
                     * Shade_model's outlines keep the centre and the line's own
                     * direction, and so does a smoothed line: Lines-ls, its 45 degree
                     * line's pixels a level off either way with the share along x) */
                    "  bool pw = vLineIW.x > 0.0 && (pc.flags & 32768u) == 0u && nvl_thin && pc.mvp[2].x == 0.0;\n"
                    "  if (pw && abs(d.x) >= abs(d.y)) { num = (e.x + (d.x > 0 ? 8 : -8)) * (d.x > 0 ? 1 : -1); den = abs(d.x); }\n"
                    "  else if (pw) { num = (e.y + (d.y > 0 ? 8 : -8)) * (d.y > 0 ? 1 : -1); den = abs(d.y); }\n"
                    /* the ends' w apart: the share with perspective (as the console's
                     * 3D_primitive lines) */
                    "  if (vLineIW.x != vLineIW.y && den > 0) {\n"
                    "%s"
                    "    w = clamp(w, 0, 256);\n"
                    "  } else\n"
                    "  if (den > 0 && num >= den) w = 256;\n"
                    "  else if (den > 0 && num > 0) {\n"
                    "    int rem = num;\n"
                    "    for (int i = 0; i < 8; i++) { rem <<= 1; w <<= 1; if (rem >= den) { rem -= den; w |= 1; } }\n"
                    "    if (rem == 0 && w <= 128 && nvrsign(max(abs(d.x), abs(d.y))) < 0) w -= 1;\n"
                    "  }\n"
                    "  return floor((NVC8(c0) * float(256 - w) + NVC8(c1) * float(w) + 128.0) / 256.0) / 255.0;\n"
                    "}\n",
                /* (the second end's share as a triangle's: the exact one to the nearest
                 * float, times 256 and cut, a whole one a step less up to the middle;
                 * in doubles where the device has them) */
                g_hle_glsl_f64 ?
                "    double s = clamp(double(num) / double(den), 0.0lf, 1.0lf);\n"
                "    double sa = (1.0lf - s) / double(abs(vLineIW.x)), sb = s / double(abs(vLineIW.y));\n"
                "    float x = float(sb / (sa + sb)) * 256.0;\n"
                "    w = int(floor(x)) - (x == floor(x) && x <= 128.0 ? 1 : 0);\n" :
                "    float s = clamp(float(num) / float(den), 0.0, 1.0);\n"
                "    float x = 256.0 * s / abs(vLineIW.y) / mix(1.0 / abs(vLineIW.x), 1.0 / abs(vLineIW.y), s);\n"
                "    w = int(x > 128.0 ? floor(x) : ceil(x) - 1.0);\n");
        }
    } else {
        put(&b, "#define HLE_NVL\n");
    }
    if ((k->compare & HLE_PSH_NVLINE) && k->linez) {
        /* A line's depth word: its ends' depths by the share of the line where the
         * pixel's centre projects on it, in doubles, cut (nxdk_pgraph_tests'
         * W_buffering ZBuf24D_LineStrip: a level line at 16777210 stores 16777210
         * on the console, where gl_FragCoord.z, a float, has no fraction left there) */
        put(&b, "layout(location = 18) flat in vec2 vLineZ;\n"
                "double nvlzd(float zm) {\n"
                "  ivec2 c = ivec2(floor(gl_FragCoord.xy * 16.0 + 0.5)), d = vLine.zw - vLine.xy, e = c - vLine.xy;\n"
                "  double num = double(e.x * d.x + e.y * d.y), den = double(d.x * d.x + d.y * d.y);\n"
                "  double s = den > 0.0lf ? clamp(num / den, 0.0lf, 1.0lf) : 0.0lf;\n"
                "  double z = double(vLineZ.x) + s * (double(vLineZ.y) - double(vLineZ.x));\n"
                "  return clamp(z, 0.0lf, double(zm));\n"
                "}\n"
                "float nvlword(float zm) { return float(floor(nvlzd(zm))); }\n");
    }
    if (!nvcol) {
        put(&b, "#define HLE_NVPS\n#define HLE_NVTIE\n#define HLE_NVTIE_FN\n#define HLE_NVTIEP_FN\n#define HLE_NVTIE_AT(s) vec2(0.0)\n#define HLE_NVNICE_AT(s) vec2(0.0)\n#define HLE_NVUV_AT(s) vec3(0.0)\n"
                "#define HLE_NVPT(P, f, ex) (f)\n");
    }
    /* DOT_ZW's z over w: z times w's table reciprocal (nvrmant, the transform
     * unit's), not a division (nxdk_pgraph_tests' Pixel_shader DotZW: a depth word
     * one over on the console's in places) */
    put(&b, nvcol ? "float nvzwdiv(float z, float w) {\n"
                    "  uint x = floatBitsToUint(w), fx = x & 0x7fffffu, ex = (x >> 23) & 0xffu;\n"
                    "  if (ex == 0u || ex >= 253u) return z / w;\n"
                    "  double p = double(z) * double(uintBitsToFloat((x & 0x80000000u) | (fx == 0u ? (254u - ex) << 23 :\n"
                    "      ((253u - ex) << 23) | (nvrmant(fx + 0x800000u) & 0x7fffffu))));\n"
                    "  int pe; double pm = frexp(p, pe);\n"
                    "  return float(ldexp(trunc(pm * 16777216.0lf) / 16777216.0lf, pe));\n"
                    "}\n" :
                    "float nvzwdiv(float z, float w) { return z / w; }\n");
    /* a stippled polygon (psh.h g_hle_stipple): the pixel by the target's own
     * pixels (the resolution scale's left out) */
    if (k->stipple) {
        const uint32_t *w = g_hle_stipple[(k->stipple - 1) % HLE_PSH_STIPPLES];
        put(&b, "const uint nvStip[32] = uint[32](");
        for (unsigned i = 0; i < 32; i++) {
            put(&b, "0x%08xu%s", w[i], i < 31 ? ", " : ");\n");
        }
        put(&b, "#define HLE_NVSTIP { ivec2 sp = ivec2(floor(gl_FragCoord.xy / float(max((pc.flags >> 16) & 255u, 1u)))); "
                "if (((nvStip[31 - (sp.y & 31)] >> uint((31 - (sp.x & 31)) ^ 24)) & 1u) == 0u) discard; }\n");
    } else {
        put(&b, "#define HLE_NVSTIP\n");
    }
    put(&b, "layout(set = 0, binding = 0) uniform texture2D uTex2D[];\n"
            "layout(set = 0, binding = 1) uniform sampler uSmp[];\n"
            "layout(set = 0, binding = 2) uniform textureCube uTexCube[];\n"
            "layout(set = 0, binding = 3) uniform texture3D uTex3D[];\n"
            HLE_CTE_GLSL HLE_CTE3I_GLSL
            /* (sampx's reads: a clamp-to-edge stage's by its unclamped coordinate's steps) */
            "#define NVT2C(sx, c) ((pc.tex[s] & 0x1C000000u) != 0u ? "
            "textureGrad(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(sx)]), c, cgx, cgy) : "
            "texture(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(sx)]), c))\n"
            "layout(location = 0) %sin vec4 vDiffuse;\n"
            "layout(location = 1) %sin vec4 vSpecular;\n"
            "layout(location = 2) HLE_TQ in vec4 HLE_TC0;\n"
            "layout(location = 3) HLE_TQ in vec4 HLE_TC1;\n"
            "layout(location = 4) HLE_TQ in vec4 HLE_TC2;\n"
            "layout(location = 5) HLE_TQ in vec4 vTex3i;\n"
            "layout(location = 6) in float vFog;\n"
            "%s%s%s"
            "layout(location = 0) out vec4 oColor;\n"
            "HLE_NVTIEP_FN\nHLE_NVTIE_FN\n"
            "vec4 col(uint c) { return unpackUnorm4x8(c).zyxw; }\n"
            HLE_NVPROJ_GLSL
            "%s"
            /* A stage with anisotropy (SET_TEXTURE_CONTROL0 bits 4-5: 2, 4, 8; vk/draw.c
             * puts it in pc.tex bits 30-31): the NV2A's probes along the footprint's
             * longer axis, L texels (the 2x2 quad's derivatives), m the shorter. N =
             * L / max(m, 1) within 1 .. the level; the probes a step of L / N apart,
             * at +-(k + 1/2) steps, the outermost pair weighted by what N leaves past
             * the others (a box N steps long); under 2 steps the pair at +-(N - 1) / 2
             * steps, alike; the weights over their sum rounded to 1/256. Each probe by
             * the stage's own filter at the step's level.
             * (nxdk_pgraph_tests' Texture_anisotropy, the coordinates per pixel dumped
             * and fitted: pgraph/tools/aniso2.py, 96 - 98 % of the pixels of a
             * point-sampled checkerboard, the rest probes on a cell's edge.) */
            "vec4 sampan(uint ti, uint si, vec2 uv, vec2 gx, vec2 gy, uint an) {\n"
            "  vec2 sz = vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0));\n"
            "  float lx = length(gx * sz), ly = length(gy * sz);\n"
            "  vec2 mj = lx >= ly ? gx : gy;\n"
            "  float L = max(lx, ly), nf = clamp(L / max(min(lx, ly), 1.0), 1.0, float(1u << an));\n"
            "  float cnt = 2.0 * ceil(nf * 0.5), un = L / nf;\n"
            "  vec2 gq = vec2(un / sz.x, 0.0), gr = vec2(0.0, un / sz.y);\n"
            "  vec4 acc = vec4(0.0);\n"
            "  float ws = cnt > 2.0 ? nf : 2.0;\n"
            "  for (int k = 0; k < 4; k++) {\n"
            "    float kk = float(k);\n"
            "    if (2.0 * kk + 2.0 > cnt) break;\n"
            "    bool outer = 2.0 * kk + 2.0 == cnt;\n"
            "    float w = outer && cnt > 2.0 ? (nf - (cnt - 2.0)) * 0.5 : 1.0;\n"
            "    w = floor(w / ws * 256.0 + 0.5) / 256.0;\n"
            "    vec2 o = mj * ((outer && cnt == 2.0 ? (nf - 1.0) * 0.5 : kk + 0.5) / nf);\n"
            "    acc += w * (textureGrad(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv - o, gq, gr) +\n"
            "                textureGrad(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv + o, gq, gr));\n"
            "  }\n"
            "  return acc;\n"
            "}\n"
            /* Xbox linear (non-swizzled) textures use texel coordinates */
            "vec4 sampx(uint s, vec2 uv, bool bump) {\n"
            "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0x1FFu;\n"
            "%s"
            "  if ((pc.flags & (1u << s)) != 0u)\n"
            "    uv /= vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)) /\n"
            "          ((pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
            "  HLE_CONV2(s, uv);\n"
            /* (the interpolated coordinate's steps: a clamp-to-edge stage's filter and
             * level, ff.h HLE_CTE_GLSL; the exact one below is no use in a helper pixel) */
            "  vec2 cgx = dFdxCoarse(uv), cgy = dFdyCoarse(uv);\n"
            /* a primitive without perspective (1/w the
             * same over it): a coordinate within 1/65536 texel of a texel edge (or, where the
             * triangle's corners sit on whole sixteenths of a texel, its own
             * millionth: the floats here get coarser with the coordinate, and
             * between such corners an edge that near is the edge)
             * on it (the NV2A's exact edges where the interpolation here lands
             * a hair short: nxdk_pgraph_tests' checkerboard rows 75, 150 ...
             * took the texel before the console's; under perspective the
             * console's own ties go the other way, Shade_model) */
            "  if (!bump && HLE_NVUV_AT(s).z != 0.0) uv = HLE_NVUV_AT(s).xy;\n"
            "  vec2 agx = dFdxCoarse(uv), agy = dFdyCoarse(uv);\n"
            "  HLE_BORDER2(s, uv);\n"
            "  if (dFdx(gl_FragCoord.w) == 0.0 && dFdy(gl_FragCoord.w) == 0.0) {\n"
            "    vec2 qs = vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0));\n"
            "    vec2 qt = uv * qs, qr = floor(qt + 0.5);\n"
            "    bool qd = bump;\n"
            "    vec2 qe = (pc.flags & (16u << s)) != 0u ? qr - HLE_NVTIE_AT(s) / 4096.0 : qr;\n"
            "    uv = mix(uv, qe / qs, lessThan(abs(qt - qr), max(vec2(qd ? 1.0 / 4096.0 : 1.0 / 65536.0), abs(qt) / 1048576.0 * HLE_NVNICE_AT(s))));\n"
            "  }\n"
            /* (a clamp-to-edge stage's point held on the texture after the ties: an
             * edge's tie took v 0 to a hair under it, the border's texel; ff.h
             * HLE_CTE_GLSL; a texture with its own border, in its image's
             * coordinates by now, is held there on the whole image) */
            "  uv = nvcte2i(pc.tex[s], uv, ti, cgx, cgy);\n"
            "  HLE_BORDC2(s, uv);\n"
            "  if ((pc.tex[s] >> 30) != 0u) return sampan(ti, si, uv, agx, agy, pc.tex[s] >> 30);\n"
            "  return NVT2C(si, uv);\n"
            "}\n"
            "#define samp(s, uv) sampx(s, uv, false)\n"
            /* (a point-sampled volume's s and t on a texel edge: sampx's ties, without
             * perspective) */
            "vec4 samp3x(uint s, vec3 uvw, bool tie) {\n"
            "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0x1FFu;\n"
            "  if (tie && (pc.flags & (16u << s)) != 0u && dFdx(gl_FragCoord.w) == 0.0 && dFdy(gl_FragCoord.w) == 0.0) {\n"
            "    vec2 qs = vec2(textureSize(sampler3D(uTex3D[nonuniformEXT(ti)], uSmp[0]), 0).xy);\n"
            "    vec2 qt = uvw.xy * qs, qr = floor(qt + 0.5), qe = qr - HLE_NVTIE_AT(s) / 4096.0;\n"
            "    uvw.xy = mix(uvw.xy, qe / qs, lessThan(abs(qt - qr), max(vec2(1.0 / 65536.0), abs(qt) / 1048576.0 * HLE_NVNICE_AT(s))));\n"
            "  }\n"
            "  uvw = nvcte3i(pc.tex[s], uvw, ti, dFdxCoarse(uvw), dFdyCoarse(uvw));\n"
            "  HLE_BORDER3(s, uvw);\n"
            "  HLE_BORDC3(s, uvw);\n"
            "  return texture(sampler3D(uTex3D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), "
            "uvw);\n"
            "}\n"
            "#define samp3(s, uvw) samp3x(s, uvw, false)\n"
            /* a volume read by a 2D stage: s and t on a texel edge as sampx's
             * (without perspective, a hair short of the edge is on it:
             * nxdk_pgraph_tests' Texture_3D_as_2D, magnified, its texel rows and
             * columns a pixel early here; a volume stage's own ties are left) */
            "vec4 samp3t(uint s, vec2 uv) {\n"
            "  if (dFdx(gl_FragCoord.w) == 0.0 && dFdy(gl_FragCoord.w) == 0.0) {\n"
            "    vec2 qs = vec2(textureSize(sampler3D(uTex3D[nonuniformEXT(pc.tex[s] & 0xFFFFu)], uSmp[0]), 0).xy);\n"
            "    vec2 qt = uv * qs, qr = floor(qt + 0.5);\n"
            "    uv = mix(uv, qr / qs, lessThan(abs(qt - qr), max(vec2(1.0 / 65536.0), abs(qt) / 1048576.0 * HLE_NVNICE_AT(s))));\n"
            "  }\n"
            "  return samp3(s, vec3(uv, 0.0));\n"
            "}\n"
            /* a cube along no direction (a program that left the stage's
             * texture coordinates unwritten, (0, 0, 0)) is opaque black:
             * Driv3r's characters add an environment cube through stage 3
             * with oT3 unwritten, and the face a PC GPU picks for (0, 0, 0)
             * added a skin tone over Tanner's jeans and hair. Its alpha goes
             * to the frame's, which a later pass reads: 0 there hid him. */
            /* (the level: the face's coordinates' steps across the 2x2 quad, each
             * neighbour's direction taken onto this pixel's face, the longer of the
             * two in texels; the host GPU's own cube level blurred a grazing face the
             * console point-samples most of: nxdk_pgraph_tests' Texture_border
             * Cube_BorderTex_SZ_32x32, 4256 -> 533 pixels) */
            "vec4 sampc(uint s, vec3 dir) {\n"
            "  vec3 cgx = dFdxCoarse(dir), cgy = dFdyCoarse(dir);\n"
            "  if (dir == vec3(0.0)) return vec4(0.0, 0.0, 0.0, 1.0);\n"
            "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0x1FFu;\n"
            "  vec3 ad = abs(dir), ex = dir + cgx, ey = dir + cgy;\n"
            "  vec2 f0, fx, fy;\n"
            "  if (ad.x >= ad.y && ad.x >= ad.z) { f0 = dir.yz / dir.x; fx = ex.yz / ex.x; fy = ey.yz / ey.x; }\n"
            "  else if (ad.y >= ad.z) { f0 = dir.xz / dir.y; fx = ex.xz / ex.y; fy = ey.xz / ey.y; }\n"
            "  else { f0 = dir.xy / dir.z; fx = ex.xy / ex.z; fy = ey.xy / ey.z; }\n"
            "  float hs = float(textureSize(samplerCube(uTexCube[nonuniformEXT(ti)], uSmp[0]), 0).x) * 0.5;\n"
            "  float rho = max(length(fx - f0), length(fy - f0)) * hs;\n"
            /* (a point-sampled face without perspective: a face coordinate on a texel
             * edge, to a 4096th, goes to the side of the smaller ratio (minor over
             * major), where the host GPU's face arithmetic lands a hair either side:
             * nxdk_pgraph_tests' Texture_3D_as_2D Cubemap_as_2D, a texel column off
             * every 8 pixels, exact so) */
            "  if ((pc.flags & (16u << s)) != 0u && dFdx(gl_FragCoord.w) == 0.0 && dFdy(gl_FragCoord.w) == 0.0) {\n"
            "    vec2 q = (f0 + 1.0) * hs, qr = floor(q + 0.5);\n"
            "    vec2 f1 = mix(f0, (qr - 1.0 / 4096.0) / hs - 1.0, lessThan(abs(q - qr), vec2(1.0 / 4096.0)));\n"
            "    if (ad.x >= ad.y && ad.x >= ad.z) dir = vec3(dir.x, f1 * dir.x);\n"
            "    else if (ad.y >= ad.z) dir = vec3(f1.x * dir.y, dir.y, f1.y * dir.y);\n"
            "    else dir = vec3(f1 * dir.z, dir.z);\n"
            "  }\n"
            "  return textureLod(samplerCube(uTexCube[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), dir, "
            "log2(max(rho, 1.0e-6)));\n"
            "}\n"
            /* PSDotMapping of a texture color to a vector */
            "vec3 dmap(uint m, vec4 c) {\n"
            "  if (m == 0u) return c.rgb;\n"
            "%s"
            "  if (m == 3u) { vec3 v = floor(c.rgb * 255.0 + 0.5);\n"
            "    return mix(v, v - 256.0, greaterThanEqual(v, vec3(128.0))) / 127.0; }\n"
            "  if (m == 4u) { vec2 h = c.rg * 2.0 - 1.0; return vec3(h, 1.0); }\n"
            /* HILO_HEMISPHERE: the texel's two 16-bit halves (A R, G B) as signed
             * numbers over 32767, z completing the unit vector */
            "  if (m >= 5u) { uvec4 i = uvec4(c * 255.0 + 0.5);\n"
            "    ivec2 hs = ivec2(int(i.a << 8 | i.r), int(i.g << 8 | i.b));\n"
            "    hs = hs - ivec2(greaterThanEqual(hs, ivec2(32768))) * 65536;\n"
            "    vec2 h = clamp(vec2(hs) / 32767.0, -1.0, 1.0);\n"
            "    return vec3(h, sqrt(max(1.0 - dot(h, h), 0.0))); }\n"
            /* MINUS1_TO_1_D3D: (b - 128) / 127; _GL: (b + 0.5) / 127.5 below 128,
             * (b - 255.5) / 127.5 from it (xemu's, measured on a console) */
            "  { vec3 b = floor(c.rgb * 255.0 + 0.5);\n"
            "    if (m == 1u) return (b - 128.0) / 127.0;\n"
            "    return mix((b + 0.5) / 127.5, (b - 255.5) / 127.5, greaterThanEqual(b, vec3(128.0))); }\n"
            "}\n"
            /* bytes as two's complement over 128 (bump offsets) */
            "vec2 sgn8(vec2 c) {\n"
            "  vec2 v = floor(c * 255.0 + 0.5);\n"
            "  return mix(v, v - 256.0, greaterThanEqual(v, vec2(128.0))) / 128.0;\n"
            "}\n"
            "%s%s%s"
            "void main() {\n"
            "  nvFC = HLE_REAL_FC_;\n"
            "  if ((pc.flags & 4096u) != 0u && pc.mvp[1].x >= 1.0) {\n"
            "    ivec2 ms = ivec2(pc.mvp[1].xy), mp = ivec2(nvFC.xy);\n"
            "    nvAAk = mp - (mp / ms) * ms != ivec2(pc.mvp[1].zw);\n"
            "    nvFC.xy = vec2(mp / ms) + 0.5;\n"
            "  }\n"
            "  HLE_NVSTIP\n"
            "  HLE_NVL\n"
            "  HLE_NVPS\n"
            "  HLE_NVTIE\n"
            "  vec4 v0 = HLE_V0, v1 = HLE_V1;\n"
            "  v0 = NVC8(v0) / 255.0; v1 = NVC8(v1) / 255.0;\n"
            "  vec4 vTex3 = %s;\n", interp, interp, k->wbuf == 2 ? HLE_WBUF_IN_BARY_GLSL : k->wbuf ? HLE_WBUF_IN_GLSL : "",
        (k->fbfetch & ~HLE_FB_ZEQ) || (k->sblend && k->sblend != 7 && k->sblend != 8 && k->sblend != 11) ?
            "layout(input_attachment_index = 0, set = 2, binding = 0) "
            "uniform subpassInput uFb;\n" : "",
        k->fbfetch & HLE_FB_ZEQ ? HLE_ZEQ_DECL_GLSL : "",
        /* a linear texture kept to its own size (ff.h HLE_NVRECT_GLSL): the stage fog.w names */
        k->vol & (HLE_PSH_RECT | HLE_PSH_RECT4) ? HLE_NVRECT_GLSL : "",
        rect_code,
        /* HILO_1: the texel's two 16-bit halves, unsigned, and 1 (only where a
         * stage maps so: other shaders' text, and their cached SPIR-V, stay) */
        hilo1 ? "  if (m == 4u) { uvec4 i = uvec4(c * 255.0 + 0.5);\n"
                "    return vec3(float(i.a << 8 | i.r), float(i.g << 8 | i.b), 65535.0) / 65535.0; }\n" : "",
        sfun,
        /* a texture of another kind than its stage's mode fetches (HLE_PSH_TKIND), as the NV2A
         * reads it (xemu's tests on a console): a 2D one under a cube mode at the direction's
         * two minor coordinates over the major one, -1..1, as they are (cube2d; samp2: by
         * normalised coordinates whatever its layout); a cube under PROJECT2D along
         * (1, t, -s) (sampc) */
        (k->compare >> 16) & 0xFu ?
            "vec2 cube2d(vec3 d) {\n"
            "  vec3 a = abs(d);\n"
            "  if (a.x > a.y && a.x > a.z) return vec2(d.x > 0.0 ? -d.z : d.z, d.y) / a.x;\n"
            "  if (a.y > a.x && a.y > a.z) return vec2(d.x, d.y > 0.0 ? -d.z : d.z) / a.y;\n"
            "  return vec2(d.z > 0.0 ? d.x : -d.x, d.y) / a.z;\n"
            "}\n"
            "vec4 samp2(uint s, vec2 uv) {\n"
            "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0x1FFu;\n"
            "  return texture(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv);\n"
            "}\n" : "",
        /* the colour key's test (color_key), in the shaders that have one */
        (k->tex_modes >> 20) & 0xFFu ?
            "bool ckey(vec4 t, uint key, uint mask) {\n"
            "  uvec4 c = uvec4(clamp(t, 0.0, 1.0) * 255.0 + 0.5);\n"
            "  return (((c.a << 24) | (c.r << 16) | (c.g << 8) | c.b) & mask) == (key & mask);\n"
            "}\n"
            /* (color_key: the key on each texel a level-0 bilinear filter weighs, the
             * weights in 256ths as samps'; the stage's own filtered texel where none
             * is the key's) */
            "vec4 ckeyt(uint s, vec2 uv, vec4 t, uint key, uint mask, uint op) {\n"
            "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0x1FFu;\n"
            "  bool f = (pc.flags & (16u << s)) == 0u &&\n"
            "           textureQueryLevels(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)])) == 1;\n"
            "  vec4 hit = vec4(0.0), wt = vec4(0.0), R = vec4(0.0), G = vec4(0.0), B = vec4(0.0), A = vec4(0.0);\n"
            "  if (f) {\n"
            "    vec2 sz = vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0));\n"
            "    if ((pc.flags & (1u << s)) != 0u)\n"
            "      uv /= sz / ((pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
            "    uv = nvcte2(pc.tex[s], uv, sz, dFdxCoarse(uv), dFdyCoarse(uv));\n"
            "    vec2 q = floor(fract(uv * sz - 0.5) * 256.0 + 1.0 / 1024.0) / 256.0;\n"
            "    wt = vec4((1.0 - q.x) * q.y, q.x * q.y, q.x * (1.0 - q.y), (1.0 - q.x) * (1.0 - q.y));\n"
            "    R = textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 0);\n"
            "    G = textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 1);\n"
            "    B = textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 2);\n"
            "    A = textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv, 3);\n"
            "    for (int i = 0; i < 4; i++) hit[i] = ckey(vec4(R[i], G[i], B[i], A[i]), key, mask) ? 1.0 : 0.0;\n"
            "  }\n"
            "  if (!f || hit == vec4(0.0)) {\n"
            "    if (ckey(t, key, mask)) { if (op == 3u) discard; t = op == 1u ? vec4(t.rgb, 0.0) : vec4(0.0); }\n"
            "    return t;\n"
            "  }\n"
            "  if (op == 3u) { if (dot(hit, vec4(greaterThan(wt, vec4(0.0)))) > 0.0) discard; return t; }\n"
            "  A *= 1.0 - hit;\n"
            "  if (op == 2u) { R *= 1.0 - hit; G *= 1.0 - hit; B *= 1.0 - hit; }\n"
            "  return vec4(dot(R, wt), dot(G, wt), dot(B, wt), dot(A, wt));\n"
            "}\n" : "",
        k->point_sprite ? "vec4(gl_PointCoord, 1.0, 1.0)" : "vTex3i");
    if (cylw) {
        /* cylinder wrap: corners 1 and 2's wrapped coordinates moved by whole units
         * to within a half of corner 0's, then interpolated (nxdk_pgraph_tests'
         * TextureWrapMode CylinderWrapping: u stepping by 1 across a quad comes
         * out constant on the console) */
        put(&b, "  vec4 vTex0 = vTex0c, vTex1 = vTex1c, vTex2 = vTex2c;\n");
        for (unsigned s = 0; s < 4; s++) {
            const unsigned cm = (k->input_tex >> (22 + 2 * s)) & 3;
            if (!cm) {
                continue;
            }
            put(&b, "  { vec4 a = vTexP%u[0], b1 = vTexP%u[1], c1 = vTexP%u[2];\n", s, s, s);
            for (unsigned j = 0; j < 2; j++) {
                if ((cm >> j) & 1) {
                    put(&b, "    b1.%c += floor(a.%c - b1.%c + 0.5); c1.%c += floor(a.%c - c1.%c + 0.5);\n",
                        "xy"[j], "xy"[j], "xy"[j], "xy"[j], "xy"[j], "xy"[j]);
                }
            }
            put(&b, "    vec3 bw = %s;\n"
                    "    vTex%u = bw.x * a + bw.y * b1 + bw.z * c1; }\n",
                k->compare & HLE_PSH_TEX_AFFINE ? "gl_BaryCoordNoPerspEXT" : "gl_BaryCoordEXT", s);
        }
    }
    if (k->wbuf) {
        if (k->wbuf == 2 && nvcol) {
            put(&b, "#define HLE_WBUF_SCRV\n");     /* (ff.h: the corners from vScrV) */
        }
        put(&b, k->wbuf == 2 ? HLE_WBUF_CALC_BARY_GLSL : HLE_WBUF_CALC_GLSL);
    }
    /* the table modes' distance: signed, as D3D's fog modes (LTCG 5849
     * 0x5421d0: 0x2601 linear, 0x800 / 0x801 exp, never the _ABS ones);
     * Conker's menu room fogs by a program's oFog that is negative */
    put(&b, "#define vFogE %s\n", nvp ?
        "(vFogPV[0] == vFogPV[1] && vFogPV[1] == vFogPV[2] ? vFogPV[0] : (nvFogOk ? nvFogPix : vFog))" : "vFog");
    /* (the guard a whole number needs when the value is a float interpolation's alone: where the
     * doubles don't reach, the corners' equal value by the sampler's weights comes a last place
     * under: nxdk_pgraph_tests' Fog_param) */
    put(&b, "#define vFogO %s\n", nvp ? "0.0" : "0.01");
    const char *fd = k->fog_abs ? "abs(vFog)" : "vFog";
    switch (k->fog) {
    case 0:  put(&b, "  float fogf = 1.0;\n"); break;
    /* vertex fog: a NaN factor (D3D's linear scale / bias are -inf, NaN when
     * FOGSTART == FOGEND: Otogi's characters) is 1, unfogged, as the NV2A
     * shows them (its min / max keep the number) */
    case 1:  put(&b, "  float fogf = isnan(vFog) ? 1.0 : clamp(vFog, 0.0, 1.0);\n"); break;
    /* the table modes too: the NV2A's linear mode is the same hardware
     * (push-buffer mode decodes D3D's vertex fog as linear, p0 1 / p1 1), so
     * Otogi 2's NaN fog coordinates turned every surface the fog colour */
    case 2:  put(&b, "  float fogf = pc.fog.x + exp(-%s * pc.fog.z);\n", fd); break;
    case 3:  put(&b, "  float fd = %s * pc.fog.z;\n"
                     "  float fogf = pc.fog.x + exp(-fd * fd);\n", fd); break;
    /* 5 .. 8: the NV2A's table on the fog value the
     * vertex stage hands over (vsh.h HLE_VSH_FOG_NV; a pure function of it in
     * every mode: nxdk_pgraph_tests' Fog_param, 320 quads a picture). The
     * value's 12 bits of fraction f, nothing up to 1.
     * LINEAR: (255 f + 256) >> 12, all from 2.
     * The others: 64 steps of the fraction, at each the byte nearest to
     * 255 g, a straight line between two, (a (64 - w) + b w + 32) >> 6, with
     * g = 2^(16 (v - 1.5)) up to 1.5 and 1 above for EXP (all from 2),
     * 2^(-16 |v - 1.5|) for EXP_ABS and 2^(-32 (v - 1.5)^2) for EXP2
     * (nothing from 2): every quad of the suite but those at exactly 1. */
    case 5:  put(&b, "  float fogf = vFogE <= 1.0 ? 0.0 : vFogE >= 2.0 ? 1.0 :\n"
                     "      float((int(floor((vFogE - 1.0) * 4096.0 + vFogO)) * 255 + 256) >> 12) / 255.0;\n");
             break;
    case 6: case 7: case 8: {
        put(&b, "  const int fogk[65] = int[65](");
        for (int i = 0; i <= 64; i++) {
            const double d = (64.0 * i - 2048.0) / 256.0;   /* 16 (v - 1.5) */
            const double g = k->fog == 6 ? (d < 0.0 ? exp2(d) : 1.0) : k->fog == 7 ? exp2(-fabs(d)) :
                                                                        exp2(-d * d / 8.0);
            put(&b, "%d%s", (int)(g * 255.0 + 0.5), i < 64 ? ", " : ");\n");
        }
        put(&b, "  float fogf = vFogE <= 1.0 ? 0.0 : vFogE >= 2.0 ? %s;\n"
                "  if (vFogE > 1.0 && vFogE < 2.0) {\n"
                "    int ff = int(floor((vFogE - 1.0) * 4096.0 + vFogO)), fw = ff & 63;\n"
                "    fogf = float((fogk[ff >> 6] * (64 - fw) + fogk[(ff >> 6) + 1] * fw + 32) >> 6) / 255.0;\n"
                "  }\n", k->fog == 6 ? "1.0 : 0.0" : "0.0 : 0.0");
        break;
    }
    default: put(&b, "  float fogf = (pc.fog.y - %s) / (pc.fog.y - pc.fog.x);\n", fd); break;
    }
    if (k->fog >= 2 && k->fog <= 4) {
        /* A coordinate that is infinite, or a factor that is no number: the
         * NV2A gives 1 (unfogged) in the linear modes and plain EXP, 0 in
         * EXP_ABS and the EXP2 modes, whatever the parameters (xemu's tests
         * on a console, its pull requests 660, 2485, 2089). A program's RCP
         * of a zero w lands +infinity in oFog: by the formula alone that is
         * full fog in linear and EXP, and MechAssault's intro, Marvel vs.
         * Capcom 2's character select and Capcom vs. SNK 2 EO's sprites were
         * the fog's colour. */
        const bool one = k->fog == 4 || (k->fog == 2 && !k->fog_abs);
        put(&b, "  fogf = isinf(vFog) || isnan(fogf) ? %s : clamp(fogf, 0.0, 1.0);\n",
            one ? "1.0" : "0.0");
    }
    /* the vertex and linear modes: the factor truncated
     * to the byte, as the console's (nxdk_pgraph_tests' Fog_gen, Fog_param,
     * Fog_coord_vec4: a step more fog colour; the EXP modes' got further) */
    if (k->fog == 1 || k->fog == 4) {
        put(&b, "  fogf = floor(fogf * 255.0 + 1.0 / 1024.0) / 255.0;\n");
    }
    put(&b, "  vec4 fogr = vec4(col(pc.fog_color).rgb, fogf);\n"
            "  float dot1 = 0.0, dot2 = 0.0, dot3 = 0.0;\n%s",
        k->debug == 17 ? "  vec4 shdbg = vec4(0.0, 0.0, 0.0, 1.0);\n" : "");
    for (unsigned s = 0; s < 4; s++) {
        unsigned m = (k->tex_modes >> (5 * s)) & 0x1F;
        /* dot-product stages: the texture coordinates dotted with the
         * stage's input texture (PSInputTexture) through PSDotMapping */
        unsigned in = s == 3 ? (k->input_tex >> 20) & 3 : s == 2 ? (k->input_tex >> 16) & 1 : 0;
        unsigned dm = s ? (k->dot_map >> (4 * (s - 1))) & 7 : 0;
        bool dotmode = s && (m == HLE_PST_DOTPRODUCT || (m >= HLE_PST_DOT_ST &&
                                                         m <= HLE_PST_DOT_STR_CUBE) ||
                             m == HLE_PST_DOT_RFLCT_SPEC_CONST);
        char tn[192];
        if (dotmode) {
            /* (without perspective the corners' coordinate, nvptc: nxdk_pgraph_tests'
             * Pixel_shader DotZW, its rows of 1000s to 40000 interpolated by the host
             * a depth word off in places) */
            put(&b, "  bool dx%u = false;\n  dot%u = dot(HLE_NVPT(vTexP%u, vTex%u, dx%u).xyz, ", s, s, s, s, s);
            dot_vec(&b, k, in, dm);
        }
        if (s == 2 && m == HLE_PST_DOT_RFLCT_DIFF) {
            /* the diffuse lookup needs stage 3's dot, computed here first */
            unsigned in3 = (k->input_tex >> 20) & 3, dm3 = (k->dot_map >> 8) & 7;
            put(&b, "  dot3 = dot(vTex3.xyz, ");
            dot_vec(&b, k, in3, dm3);
        }
        if ((k->fbfetch >> s) & 1) {
            /* the target drawn into: this pixel as the earlier draws left it
             * (an 8-bit target holds blue: back to its colour) */
            put(&b, "  vec4 t%u = subpassLoad(uFb)%s;\n", s, k->out_bgra ? ".bgra" : "");
            alpha_kill(&b, k, s, m);
            color_key(&b, k, s, m);
            continue;
        }
        if (((k->shadow >> s) & 1) && (m == HLE_PST_PROJECT2D || m == HLE_PST_PROJECT3D)) {
            shadow_sample(&b, k, s, m == HLE_PST_PROJECT3D);
            alpha_kill(&b, k, s, m);
            color_key(&b, k, s, m);
            continue;
        }
        switch (m) {
        case HLE_PST_NONE:
            /* a stage with no texture reads opaque black, as an unbound D3D
             * texture samples: Voodoo Vince draws its cutscene letterbox
             * as t0 * v0 (black diffuse, SRCALPHA blending) with nothing on
             * stage 0; with t0.a 0 the bars blended to nothing */
            put(&b, "  vec4 t%u = vec4(0.0, 0.0, 0.0, 1.0);\n", s);
            break;
        case HLE_PST_PASSTHRU:
            /* the coordinate as 12 bits, up (one more than its 4096ths cut, a
             * whole number of them too), times 255 cut (nxdk_pgraph_tests'
             * Texgen_with_texture_matrix, the PassThrough_Disabled pictures' t
             * down the quad: pgraph/tools/passthru.py, 739 rows and none off; cut
             * from the coordinate itself 22 rows were a step under, 13.974 as 13
             * where the console has 14; Pixel_shader Passthru, t at k / 4096 as
             * k + 1: pgraph/tools/ptfit.py, every pixel of its quad; the host's
             * interpolation a hair under a whole 4096th taken as on it) */
            put(&b, "  bool px%u = false;\n"
                    "  vec4 t%u = clamp(HLE_NVPT(vTexP%u, vTex%u, px%u), 0.0, 1.0);\n"
                    "  t%u = floor((floor(t%u * 4096.0 + (px%u ? 0.0 : 1.0 / 1024.0)) + 1.0) * 255.0 / 4096.0) / 255.0;\n",
                s, s, s, s, s, s, s, s);
            break;
        case HLE_PST_CLIPPLANE:
            /* (without perspective the corners' coordinate, nvptc: the host's
             * interpolation of a zero lands either side; nxdk_pgraph_tests'
             * Pixel_shader ClipPlane, its quads' rows and columns of zeros on
             * pixel centres, pgraph/tools/cpfit.py) */
            put(&b, "  vec4 t%u = vec4(0.0);\n  bool cx%u = false;\n  vec4 cp%u = HLE_NVPT(vTexP%u, vTex%u, cx%u);\n",
                s, s, s, s, s, s);
            for (unsigned c = 0; c < 4; c++) {
                put(&b, "  if (cp%u.%c %s 0.0) discard;\n", s, "xyzw"[c],
                    ((k->compare >> (4 * s + c)) & 1) ? ">=" : "<");
            }
            break;
        case HLE_PST_PROJECT2D:
            if (HLE_PSH_TKIND(k, s)) {          /* a cube: along (1, t, -s) */
                put(&b, "  vec4 t%u;\n  { vec2 p = nvproj(vTex%u).xy;\n"
                        "    t%u = sampc(%uu, normalize(vec3(1.0, p.y, -p.x))); }\n", s, s, s, s);
                break;
            }
            put(&b, (k->vol >> s) & 1 ? "  vec4 t%u = samp3t(%uu, nvproj(vTex%u).xy);\n"
                                      : "  vec4 t%u = samp(%uu, nvproj(vTex%u).xy);\n", s, s, s);
            break;
        case HLE_PST_PROJECT3D:
            put(&b, "  vec4 t%u = samp3x(%uu, nvproj(vTex%u), true);\n", s, s, s);
            break;
        case HLE_PST_CUBEMAP:
            put(&b, HLE_PSH_TKIND(k, s) ? "  vec4 t%u = samp2(%uu, cube2d(vTex%u.xyz));\n"
                                        : "  vec4 t%u = sampc(%uu, vTex%u.xyz);\n", s, s, s);
            break;
        case HLE_PST_BUMPENVMAP:
        case HLE_PST_BUMPENVMAP_LUM: {
            /* offset texture: the input stage's (b, g) as signed bytes (du,
             * dv) through the bump matrix (column-major, as the NV2A takes
             * BUMPENVMAT 00, 01, 10, 11) move this stage's coordinates; LUM
             * scales the RGB by LSCALE * the input's r + LOFFSET */
            unsigned in = s == 3 ? (k->input_tex >> 20) & 3 : s == 2 ? (k->input_tex >> 16) & 1 : 0;
            if (!s) {
                unsup |= 1u << m;
                put(&b, "  vec4 t0 = samp(0u, nvproj(vTex0).xy);\n");
                break;
            }
            static const char *const mat[4][2] = {
                { "", "" }, { "pc.bump.x", "pc.bump.y" }, { "pc.bump.z", "pc.bump.w" },
                { "pc.bump3.x", "pc.bump3.y" } };
            const bool b16 = tex16(k, in) != 0;   /* its bytes from the words (tb16) */
            char mx[96];                        /* (a luminance stage's: its halves alone) */
            if (m == HLE_PST_BUMPENVMAP) {
                snprintf(mx, sizeof(mx), "bmat(%s, %s, pc.bumpl[%u])", mat[s][0], mat[s][1], s - 1);
            } else {
                snprintf(mx, sizeof(mx), "mat2(unpackHalf2x16(%s), unpackHalf2x16(%s))", mat[s][0], mat[s][1]);
            }
            put(&b, yuv_stage(k, s) ? "  vec4 t%u, ty%u;\n" : "  vec4 t%u;\n", s, s);
            if (b16) {
                char wa[160];
                put(&b, "  { vec4 tw = tb16(%s);\n"
                        "    vec2 d = %s * sgn8(tw.bg);\n", w16_args(k, in, wa), mx);
            } else {
                put(&b, "  { vec2 d = %s * %s;\n", mx, bump_in(k, in, tn));
            }
            if ((k->vol >> s) & 1) {            /* a volume: r passes through (Conker's
                                                 * electricity, animated along r) */
                put(&b, "    t%u = samp3(%uu, vec3(nvproj(vTex%u).xy + d, vTex%u.z));\n",
                    s, s, s, s);
            } else {
                put(&b, "    t%u = sampx(%uu, nvproj(vTex%u).xy + d, true);\n", s, s, s);
            }
            if (yuv_stage(k, s)) {
                put(&b, "    ty%u = t%u;\n", s, s);
            }
            if (m == HLE_PST_BUMPENVMAP_LUM) {
                /* (the scale and offset in 4096ths, signed 16 bits each (vk/draw.c fix12x2):
                 * nxdk_pgraph_tests' Bump_env_lum, 656 columns of four bump-map formats
                 * fitted against the console's colours, every one but a single 4-bit column
                 * so; as halves 0.7 was 0.70020 and a column in ten a step over) */
                put(&b, "    vec2 l = vec2(bitfieldExtract(int(pc.bumpl[%u]), 0, 16), bitfieldExtract(int(pc.bumpl[%u]), 16, 16)) / 4096.0;\n"
                        /* (cut to its byte, not rounded: the console's dark cells come out one under
                         * what rounding gives, nxdk_pgraph_tests' Bump_env_lum; the factor not held
                         * under 1: Pixel_shader BumpEnvMapLuminance's scale 2 and offset 0.75 cells
                         * brighter than the texel) */
                        /* (the luminance itself a byte, the filtered texel's: nxdk_pgraph_tests'
                         * Bump_env_lum, its ramps between the bump map's texels, 25 % fewer
                         * pixels off than the filter's own fraction) */
                        "    t%u.rgb = floor(t%u.rgb * max(l.x * (floor(%s * 255.0 + 0.5) / 255.0) + l.y, 0.0) * 255.0 + 1.0 / 1024.0) / 255.0;\n", s - 1, s - 1, s, s,
                    lum_in(k, in, b16, tn));
            }
            if (yuv_stage(k, s) || csc_stage(k, s)) {
                /* scaled by its luminance, then converted (nxdk_pgraph_tests' Bump_env_lum
                 * YUY2: the console's red checker at a luminance factor of 0.536,
                 * (136, 0, 0) as (Y, Cb, Cr), comes out (0, 255, 0)) */
                put(&b, "    t%u.rgb = nvcsc(t%u.rgb);\n", s, s);
            }
            put(&b, "  }\n");
            break;
        }
        case HLE_PST_DPNDNT_AR:
        case HLE_PST_DPNDNT_GB: {
            /* dependent read: the input stage's (a, r) or (g, b), colours in
             * 0..1, are this stage's 2D coordinates, normalised whatever the
             * texture's layout. Mercenaries' sky dome looks its fade up by
             * the scene depth's top bytes (the depth read as A8R8G8B8): at
             * its own coordinates (none written) the fade was 0 and the sky
             * black. */
            unsigned in = s == 3 ? (k->input_tex >> 20) & 3 : s == 2 ? (k->input_tex >> 16) & 1 : 0;
            if (!s) {
                unsup |= 1u << m;
                put(&b, "  vec4 t0 = samp(0u, nvproj(vTex0).xy);\n");
                break;
            }
            put(&b, "  vec4 t%u;\n"
                    "  { uint ti = pc.tex[%u] & 0xFFFFu, si = (pc.tex[%u] >> 16) & 0x1FFu;\n"
                    "    vec2 dc = %s.%s;\n"
                    "    t%u = texture(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), "
                    "nvcte2i(pc.tex[%u], dc, ti, "
                    "dFdxCoarse(dc), dFdyCoarse(dc))); }\n",
                s, s, s, tex_in(k, in, tn), m == HLE_PST_DPNDNT_AR ? "ar" : "gb", s, s);
            break;
        }
        case HLE_PST_DOT_STR_3D:                    /* stage 3: (dot1, dot2, dot3) */
            if (k->input_tex & HLE_PSH_CUBE3D) {
                /* a cube: its first face (+X) as a 2D texture at (dot1, dot2) over their
                 * coordinates' q (0: the dot's sign as an infinity), clamped to the edge
                 * texels, s from the face's far side (nxdk_pgraph_tests' Texture_cubemap
                 * DotSTR3D, coordinates with q 0: the console's picture holds face 0's
                 * texels (0, 0) and (0, 63), blue and green, split where dot2 changes
                 * sign) */
                put(&b, "  vec4 t%u;\n"
                        "  { uint ti = pc.tex[%u] & 0xFFFFu;\n"
                        "    vec2 n = vec2(textureSize(samplerCube(uTexCube[nonuniformEXT(ti)], uSmp[0]), 0));\n"
                        "    vec2 d = vec2(dot1, dot2), q = vec2(vTex1.w, vTex2.w);\n"
                        "    d = mix(d / q, sign(d) * 1.0e30, equal(q, vec2(0.0)));\n"
                        "    vec2 st = clamp(d, 0.5 / n, 1.0 - 0.5 / n);\n"
                        "    t%u = sampc(%uu, vec3(1.0, 1.0 - 2.0 * st.y, 2.0 * st.x - 1.0)); }\n",
                    s, s, s, s);
                break;
            }
            put(&b, HLE_PSH_TKIND(k, s) ? "  vec4 t%u = samp2(%uu, vec2(dot1, dot2));\n"
                                        : "  vec4 t%u = samp3(%uu, vec3(dot1, dot2, dot3));\n", s, s);
            break;
        case HLE_PST_DOTPRODUCT:
        case HLE_PST_DOT_ZW:                        /* (its dot is the depth's w, below) */
            put(&b, "  vec4 t%u = vec4(0.0);\n", s);
            break;
        case HLE_PST_DOT_ST:
            /* the previous stage's dot and this one's: (dot1, dot2) on stage
             * 2, (dot2, dot3) on stage 3 (texm3x2pad / texm3x2tex on stages 2
             * and 3). Read at (dot1, dot2) on stage 3, Splinter Cell Chaos
             * Theory's thermal vision (stage 2 DOTPRODUCT, stage 3 DOT_ST over
             * the heat image, a shimmer by a noise map) took one column of
             * the image at a height set by one dot: vertical stripes */
            put(&b, "  vec4 t%u = samp(%uu, vec2(dot%u, dot%u));\n", s, s, s - 1, s);
            break;
        case HLE_PST_DOT_STR_CUBE:                  /* stage 3: N */
        case HLE_PST_DOT_RFLCT_DIFF:                /* stage 2: N */
            put(&b, HLE_PSH_TKIND(k, s) ? "  vec4 t%u = samp2(%uu, cube2d(vec3(dot1, dot2, dot3)));\n"
                                        : "  vec4 t%u = sampc(%uu, vec3(dot1, dot2, dot3));\n", s, s);
            break;
        case HLE_PST_DOT_RFLCT_SPEC:                /* stage 3: N reflected about E */
        case HLE_PST_DOT_RFLCT_SPEC_CONST:          /* (E = the eye vector: q coordinates, or
                                                       SET_EYE_VECTOR's: pc.bump) */
            put(&b, "  vec4 t%u;\n  { vec3 n = vec3(dot1, dot2, dot3), "
                    "e = %s;\n", s, m == HLE_PST_DOT_RFLCT_SPEC_CONST ? "uintBitsToFloat(pc.bump.xyz)" :
                                                                       "vec3(vTex1.w, vTex2.w, vTex3.w)");
            put(&b, 
                    "    vec3 rv = 2.0 * n * dot(n, e) / max(dot(n, n), 1e-8) - e;\n"
                    "    t%u = %s; }\n",
                s, HLE_PSH_TKIND(k, s) ? (s == 3 ? "samp2(3u, cube2d(rv))" : "samp2(2u, cube2d(rv))")
                                          : (s == 3 ? "sampc(3u, rv)" : "sampc(2u, rv)"));

            break;
        case HLE_PST_BRDF:
            if (s >= 2) {
                /* the volume at (theta eye, theta light, phi light - phi eye): the
                 * two stages before it read as HILO, two 16-bit halves (theta the
                 * high word, phi the low one) over 65535, the third coordinate
                 * wrapped by the stage's own mode (NVIDIA's US 6664963, "texture3D
                 * (Phi e, Phi l, d phi)"; nxdk_pgraph_tests' Texture_BRDF: the
                 * console's wedge is the volume's texel (48, 61, 54..55), the light
                 * (0.1, 0.05, -1) at theta 0.9645 pi) */
                put(&b, "  vec4 t%u;\n"
                        "  { uvec4 ie = uvec4(t%u * 255.0 + 0.5), il = uvec4(t%u * 255.0 + 0.5);\n"
                        "    vec2 he = vec2(float(ie.a << 8 | ie.r), float(ie.g << 8 | ie.b)) / 65535.0;\n"
                        "    vec2 hl = vec2(float(il.a << 8 | il.r), float(il.g << 8 | il.b)) / 65535.0;\n"
                        "    t%u = samp3(%uu, vec3(he.x, hl.x, hl.y - he.y)); }\n",
                    s, s - 2, s - 1, s, s);
                break;
            }
            unsup |= 1u << m;
            put(&b, "  vec4 t%u = samp(%uu, nvproj(vTex%u).xy); /* mode %u */\n", s, s, s, m);
            break;
        default:                        /* volume, DOT_ZW, dependent AR/GB */
            unsup |= 1u << m;
            put(&b, "  vec4 t%u = samp(%uu, nvproj(vTex%u).xy); /* mode %u */\n",
                s, s, s, m);
            break;
        }
        if (m != HLE_PST_BUMPENVMAP && m != HLE_PST_BUMPENVMAP_LUM) {  /* (a bump stage's: its own code) */
            if (yuv_stage(k, s)) {
                put(&b, "  vec4 ty%u = t%u;\n  t%u.rgb = nvcsc(t%u.rgb);\n", s, s, s, s);
            } else if (csc_stage(k, s)) {
                put(&b, "  t%u.rgb = nvcsc(t%u.rgb);\n", s, s);
            }
        }
        if (signed_stage(k, s)) {
            /* the signed channels (samps, sgn4), the texel as sampled kept
             * for the stages that address by it (tr). Over 127.5, as the
             * console's (nxdk_pgraph_tests' Texture_signed_component: 0x7F comes
             * out 254, not 255) */
            const unsigned g = (HLE_PSH_TSIGN(k) >> (4 * s)) & 15;
            char mask[64];
            snprintf(mask, sizeof(mask), "bvec4(%s, %s, %s, %s)", g & 2 ? "true" : "false",
                     g & 4 ? "true" : "false", g & 8 ? "true" : "false", g & 1 ? "true" : "false");
            if ((k->input_tex >> s) & 1) {
                /* its bytes flipped at definition (hle_tex_sign_xor), filtered
                 * as signed by the sampler: centred on 0x80 */
                put(&b, "  vec4 tr%u = mix(t%u, mod(floor(t%u * 255.0 + 0.5) + 128.0, 256.0) / 255.0, %s);\n",
                    s, s, s, mask);
                if (m == HLE_PST_PROJECT2D && !((k->vol >> s) & 1)) {
                    /* a linear filter: the bytes gathered and blended as the texture unit does
                     * (weights cut to 1/256), not by the sampler's own weights */
                    put(&b, "  t%u = samps(%uu, nvproj(vTex%u).xy, t%u, %s, %s, true);\n", s, s, s, s,
                        mask, (HLE_PSH_TPOINT(k) >> s) & 1 ? "true" : "false");
                    /* (the signed channels' bits for the stages that read them, a bump-env
                     * luminance stage's red: the blend's own, not the sampler's weights on
                     * the flipped bytes, which round where the texture unit cuts:
                     * nxdk_pgraph_tests' Bump_env_lum signed rectangles, the luminance a
                     * byte lower in every column whose weight is half a 256th or more
                     * past a whole one) */
                    put(&b, "  tr%u = mix(tr%u, vec4(sbit(t%u.r), sbit(t%u.g), sbit(t%u.b), sbit(t%u.a)), %s);\n",
                        s, s, s, s, s, s, mask);
                } else {
                    put(&b, "  t%u = mix(t%u, clamp((t%u * 255.0 - 128.0) / 127.5, -1.0, 1.0), %s);\n",
                        s, s, s, mask);
                }
            } else {
                put(&b, "  vec4 tr%u = t%u;\n", s, s);
                if (m == HLE_PST_PROJECT2D && !((k->vol >> s) & 1)) {
                    /* a target's one-level image: gathered and filtered as signed */
                    put(&b, "  t%u = samps(%uu, nvproj(vTex%u).xy, t%u, %s, %s, false);\n", s, s, s, s,
                        mask, (HLE_PSH_TPOINT(k) >> s) & 1 ? "true" : "false");
                } else {
                    put(&b, "  t%u = mix(t%u, sgn4(t%u), %s);\n", s, s, s, mask);
                }
            }
        }
        alpha_kill(&b, k, s, m);
        color_key(&b, k, s, m);
    }
    put(&b, "  vec4 r0 = vec4(0.0, 0.0, 0.0, t0.a), r1 = vec4(0.0);\n"
            "  vec4 t0s = t0, t3s = t3;\n");      /* as sampled (debug views 15, 16) */
    unsigned n = k->count & 0xFF;
    for (unsigned s = 0; s < n && s < 8; s++) {
        stage(&b, k, s);
    }
    final_combiner(&b, k);
    put(&b, "  outc = clamp(outc, 0.0, 1.0);\n");
    /* the colour as the byte nearest to it. Left to the
     * target's conversion, a value just over a half step can land under it
     * (a driver has 0.6 of a step to play with): nxdk_pgraph_tests' Fog_param,
     * fog 15 / 255 over a diffuse of 128: 7.53 written as 7, the console's 8 */
    put(&b, "  outc = floor(outc * 255.0 + 0.5) / 255.0;\n");
    if ((k->compare & HLE_PSH_NVLINE) || nvcol) {
        /* a smoothed line's or polygon's alpha: the pixel's coverage, eighths cut to a byte
         * (ff.h nvl_cov; nxdk_pgraph_tests' Smoothing_control: 31, 63, 95 .. 223, 255
         * from a full alpha) */
        put(&b, "  if (nvlK >= 0) {\n"
                "    outc.a = floor(floor(outc.a * 255.0 + 0.5) * float(nvlK) / 8.0) / 255.0;\n"
                "  }\n");
    }
    if (k->alpha_test && k->alpha_func < 8) {
        /* the NV2A tests the alpha it would write, eight bits, against the reference's eight
         * (as xemu): compared as floats, an alpha within half a step of the reference fell
         * on the other side, and EQUAL / NOTEQUAL held for no computed alpha */
        put(&b, "  { float a = floor(outc.a * 255.0 + 0.5), r = floor(pc.alpha_ref * 255.0 + 0.5); "
                "if (!(%s)) discard; }\n", k_cmp[k->alpha_func]);
    }
    if (k->fbfetch & HLE_FB_ZEQ) {
        put(&b, "%s", HLE_ZEQ_TEST_GLSL(k->fbfetch));
    }
    static const char *const k_dbg[17] = {
        "outc", "t0", "v0", "r0", "vec4(vec3(fogf), 1.0)", "v1", "t3", "vec4(vec3(r0.a), 1.0)",
        "t2",
        /* 16-pixel bands: stage 1 (a shadow compare), its r / 65535 */
        ("((int(gl_FragCoord.y) & 16) == 0 ? t1 : "
         "vec4(vec3(clamp(vTex1.z / vTex1.w / 65535.0, 0.0, 1.0)), 1.0))"),
        "vec4(vec3(vSpecular.a), 1.0)", "vec4(vec3(t0.a), 1.0)", "vec4(vec3(t3.a), 1.0)",
        "vec4(fract(vTex3.xy / vTex3.w), 0.0, 1.0)", "vec4(clamp(abs(vTex3.xyz) * 0.25, 0.0, 1.0), 1.0)",
        "t3s", "t0s" };
    /* 17: shadow-buffer draws show (stored depth, r, result) / range, others as is */
    const char *dbg = k->debug < 17 ? k_dbg[k->debug] :
                      k->debug == 17 && (k->shadow & 15) ? "shdbg" :
                      k->debug == 18 ? "vec4(1.0, 0.0, 1.0, 1.0)" :
                      k->debug == 19 ? "t1" : k->debug == 20 ? "r1" :
                      /* stage 2's coordinates: |str| / 4, direction, NaN / Inf, and
                       * its cube sampled along a fixed direction */
                      k->debug == 21 ? "vec4(clamp(abs(vTex2.xyz) * 0.25, 0.0, 1.0), 1.0)" :
                      k->debug == 22 ? "vec4(normalize(vTex2.xyz) * 0.5 + 0.5, 1.0)" :
                      k->debug == 23 ? "vec4(any(isnan(vTex2)) ? 1.0 : 0.0, "
                                       "any(isinf(vTex2)) ? 1.0 : 0.0, 0.0, 1.0)" :
                      k->debug == 24 ? "sampc(2u, vec3(0.3, 0.5, 0.8))" : "outc";
    put(&b, "  oColor = %s;\n%s", dbg, k->out_bgra ? "  oColor = oColor.bgra;\n" : "");
    if (k->sblend >= 13) {
        /* a 16-bit target blended (vk/draw.c a7_blend: 13 X1R5G5B5_Z1R5G5B5, 14
         * _O1R5G5B5, 15 R5G6B5): the NV2A cuts the source to the target's bits
         * first, then blends the source and the target's value with the factors
         * as bytes, half a byte added before the cut (1/16 of a 5-bit step, 1/8 of
         * a 6-bit one: the blend done on the bits' bytes) (nxdk_pgraph_tests'
         * Surface_format X1R5G5B5: the radial quad over a cleared target, 8192
         * pixels, none off, pgraph/tools/blend16.py; Blend_surface's 16-bit SrcA
         * pictures over a target of their own, exact); the spare bit 0 or 1 */
        put(&b, "  { vec4 sv = clamp(oColor, 0.0, 1.0);\n"
                "    vec4 dv = subpassLoad(uFb);\n"
                "    vec3 nb = vec3(31.0, %s, 31.0);\n"
                "    vec3 s5 = %s, d5 = floor(dv.rgb * nb + 0.5);\n"
                "    sv.a = floor(sv.a * 255.0 + 0.5) / 255.0;\n"
                "#define NVBF(f) ((f) == 0u ? vec4(0.0) : (f) == 1u ? vec4(1.0) : (f) == 0x300u ? sv : "
                "(f) == 0x301u ? 1.0 - sv : (f) == 0x302u ? sv.aaaa : (f) == 0x303u ? 1.0 - sv.aaaa : "
                "(f) == 0x304u ? dv.aaaa : (f) == 0x305u ? 1.0 - dv.aaaa : (f) == 0x306u ? dv : "
                "(f) == 0x307u ? 1.0 - dv : vec4(vec3(min(sv.a, 1.0 - dv.a)), 1.0))\n"
                "    vec3 fs = NVBF(NVB_SF).rgb, fd = NVBF(NVB_DF).rgb;\n"
                "    vec3 r = NVB_OP == 0x800Au ? s5 * fs - d5 * fd : NVB_OP == 0x800Bu ? d5 * fd - s5 * fs :\n"
                "             NVB_OP == 0x8007u ? min(s5, d5) : NVB_OP == 0x8008u ? max(s5, d5) : s5 * fs + d5 * fd;\n"
                "    r = clamp(floor(r + 0.5 * (nb + 1.0) / 256.0), vec3(0.0), nb);\n"
                /* (the middle of the step's bytes: the step under a conversion through
                 * bytes or straight to the target's bits alike) */
                "    oColor = vec4((r + 0.5) * (256.0 / (nb + 1.0)) / 255.0, %s); }\n",
            k->sblend == 15 ? "63.0" : "31.0",
            /* the source's cut, dithered (SET_DITHER_ENABLE: a threshold of the
             * step under it, vk/draw.c dither_text) */
            k->dither ? "min(floor((floor(sv.rgb * 255.0 + 0.5) + nvbayer() * 256.0 / (nb + 1.0)) * (nb + 1.0) / 256.0), nb)"
                      : "floor(floor(sv.rgb * 255.0 + 0.5) * (nb + 1.0) / 256.0)",
            k->sblend == 14 ? "1.0" : "0.0");
    } else if (k->sblend >= 11) {
        /* an X1R5G5B5_O1R5G5B5 or X8R8G8B8_O8R8G8B8 target (vk/draw.c a7_blend):
         * its alpha 1 where the draw goes; blended (12) by the factors and equation
         * against the source's own alpha and the target's as it holds it */
        put(&b, "  { vec4 sv = clamp(oColor, 0.0, 1.0);\n");
        if (k->sblend == 12) {
            put(&b, "    vec4 dv = subpassLoad(uFb);\n"
                    "#define NVBF(f) ((f) == 0u ? vec4(0.0) : (f) == 1u ? vec4(1.0) : (f) == 0x300u ? sv : "
                    "(f) == 0x301u ? 1.0 - sv : (f) == 0x302u ? sv.aaaa : (f) == 0x303u ? 1.0 - sv.aaaa : "
                    "(f) == 0x304u ? dv.aaaa : (f) == 0x305u ? 1.0 - dv.aaaa : (f) == 0x306u ? dv : "
                    "(f) == 0x307u ? 1.0 - dv : vec4(vec3(min(sv.a, 1.0 - dv.a)), 1.0))\n"
                    "    vec4 fs = NVBF(NVB_SF), fd = NVBF(NVB_DF);\n"
                    "    vec4 r = NVB_OP == 0x800Au ? sv * fs - dv * fd : NVB_OP == 0x800Bu ? dv * fd - sv * fs :\n"
                    "             NVB_OP == 0x8007u ? min(sv, dv) : NVB_OP == 0x8008u ? max(sv, dv) : sv * fs + dv * fd;\n"
                    "    sv = floor(clamp(r, 0.0, 1.0) * 255.0 + 0.5) / 255.0;\n");
        }
        put(&b, "    oColor = vec4(sv.rgb, 1.0); }\n");
    } else if (k->sblend >= 7) {
        /* an X1A7R8G8B8 target (vk/draw.c a7_blend: 7 / 9 Z1A7, 8 / 10 O1A7): the
         * alpha byte as it stores it, the format's top bit over the alpha's seven
         * high bits; blended (9, 10: the factors and equation as NVB_SF, NVB_DF,
         * NVB_OP), the target's alpha its seven bits widened, the result in bytes */
        put(&b, "  { vec4 sv = clamp(oColor, 0.0, 1.0);\n");
        if (k->sblend >= 9) {
            put(&b, "    vec4 dv = subpassLoad(uFb);\n"
                    "    float a7 = mod(floor(dv.a * 255.0 + 0.5), 128.0);\n"
                    "    dv.a = (a7 * 2.0 + floor(a7 / 64.0)) / 255.0;\n"
                    "#define NVBF(f) ((f) == 0u ? vec4(0.0) : (f) == 1u ? vec4(1.0) : (f) == 0x300u ? sv : "
                    "(f) == 0x301u ? 1.0 - sv : (f) == 0x302u ? sv.aaaa : (f) == 0x303u ? 1.0 - sv.aaaa : "
                    "(f) == 0x304u ? dv.aaaa : (f) == 0x305u ? 1.0 - dv.aaaa : (f) == 0x306u ? dv : "
                    "(f) == 0x307u ? 1.0 - dv : vec4(vec3(min(sv.a, 1.0 - dv.a)), 1.0))\n"
                    "    vec4 fs = NVBF(NVB_SF), fd = NVBF(NVB_DF);\n"
                    "    vec4 r = NVB_OP == 0x800Au ? sv * fs - dv * fd : NVB_OP == 0x800Bu ? dv * fd - sv * fs :\n"
                    "             NVB_OP == 0x8007u ? min(sv, dv) : NVB_OP == 0x8008u ? max(sv, dv) : sv * fs + dv * fd;\n"
                    "    sv = floor(clamp(r, 0.0, 1.0) * 255.0 + 0.5) / 255.0;\n");
        }
        put(&b, "    oColor = vec4(sv.rgb, (%s + floor(floor(sv.a * 255.0 + 0.5) / 2.0)) / 255.0); }\n",
            k->sblend == 8 || k->sblend == 10 ? "128.0" : "0.0");
    } else if (k->sblend >= 3) {
        /* the NV2A's (draw.c signed_blend): the target's byte as it
         * is, the output's two's complement, from 0 to 255 */
        put(&b, "  { ivec4 sv = ivec4(floor(clamp(oColor, 0.0, 1.0) * 255.0 + 0.5));\n"
        "    ivec4 dv = ivec4(floor(clamp(subpassLoad(uFb), 0.0, 1.0) * 255.0 + 0.5));\n"
        "    sv -= 256 * (sv >> 7);\n"
        "    oColor = vec4(clamp(dv %s sv, ivec4(0), ivec4(255))) / 255.0; }\n", k->sblend == 3 ? "+" : "-");
    } else if (k->sblend) {
        put(&b, "  { ivec4 sv = ivec4(floor(clamp(%s, 0.0, 1.0) * 255.0 + 0.5));\n"
        "    ivec4 dv = ivec4(floor(clamp(subpassLoad(uFb), 0.0, 1.0) * 255.0 + 0.5));\n"
        "    sv -= 256 * (sv >> 7); dv -= 256 * (dv >> 7);\n"
        "    ivec4 r = clamp(dv %s sv, ivec4(-128), ivec4(127));\n"
        "    oColor = vec4(r & 255) / 255.0; }\n", "oColor", k->sblend == 1 ? "+" : "-");
    }
    /* the NV2A truncates the depth to the buffer's steps where the GPU here
     * rounds to the nearest (nxdk_pgraph_tests' W_buffering ZBuf pictures:
     * a step above the console's on half their rows), the polygon offset
     * added before the cut: its constant in steps, its factor times the
     * larger of the depth's slopes (psc[18], psc[19]; ZBuf24D_TriH: 325 down
     * to 58 over 200 rows, the offset 16 is 16 words on every one, the factor
     * 16 is 21.4 with the 1.338 a row) */
    if (k->wbuf && nvcol && (k->compare & HLE_PSH_ZWORD)) {
        /* (W-buffered: the pixel's w with its offsets, as it is before the cut) */
        put(&b, "  nvzdepth(1.0 / pc.zscale);\n"
                "  if (pc.mvp[0].y > pc.mvp[0].x && (int(pc.mvp[0].z) & 8) != 0)\n"
                "    nvzcull = nvzprim || ((int(pc.mvp[0].z) & 2) == 0 && (wCull < pc.mvp[0].x || wCull > pc.mvp[0].y));\n"
                /* (and clamped: w itself to the range, then stored as it is: a floor past
                 * the range's far end hides a wall just behind that end, nxdk_pgraph
                 * _tests' Depth_Clamp) */
                "  if (pc.mvp[0].y > pc.mvp[0].x && (int(pc.mvp[0].z) & 10) == 10) {\n"
                "    bool wi = pc.zscale == uintBitsToFloat(0x37800000u) || pc.zscale == uintBitsToFloat(0x33800000u);\n"
                "    wRaw = clamp(wCull, pc.mvp[0].x, pc.mvp[0].y);\n"
                "    if (wi) wRaw = floor(wRaw);\n"
                "#ifdef HLE_WBUF_VWD\n"
                "    if (wi) wRaw = float(floor(clamp(wRawD, double(pc.mvp[0].x), double(pc.mvp[0].y))));\n"
                "#endif\n"
                "    precise float wn2 = wRaw * pc.zscale + wRaw * (wi ? pc.zscale * pc.zscale : 0.0);\n"
                "    wDepth = clamp(wn2, 0.0, 1.0);\n"
                "  }\n"
                "  if (nvzcull) discard;\n");
    }
    const unsigned zws = hle_psh_dotzw_stage(k->tex_modes);
    if (zws) {
        /* DOT_ZW: the depth is the previous stage's dot over this one's, in
         * the buffer's units (nxdk_pgraph_tests' Pixel_shader DotZW: z from
         * 1000s over w near 1, the depth clip at 2^24), cut to its word in
         * the fixed formats, the word of it in the float ones */
        put(&b, "  { float zm = 1.0 / pc.zscale, zv = nvzwdiv(dot%u, dot%u);\n", zws - 1, zws);
        if (k->compare & HLE_PSH_ZFWORD) {
            put(&b, "    gl_FragDepth = nvfword(clamp(zv, 0.0, zm)); }\n");
        } else if (k->vol & HLE_PSH_ZTRUNC) {
            put(&b, "    float zw = floor(clamp(zv, 0.0, zm - 1.0));\n"
                    "    precise float zf = zw * pc.zscale + zw * (pc.zscale * pc.zscale);\n"
                    "    gl_FragDepth = zf; }\n");
        } else {
            put(&b, "    gl_FragDepth = clamp(zv * pc.zscale, 0.0, 1.0); }\n");
        }
        put(&b, "  if (nvAAk) discard;\n}\n");
    } else
    put(&b, "%s  if (nvAAk) discard;\n}\n", (k->compare & HLE_PSH_ZFWORD) && k->wbuf ? "  gl_FragDepth = nvfword(wRaw);\n" :
                     (k->compare & HLE_PSH_ZFWORD) && nvcol && (k->compare & HLE_PSH_ZWORD) ?
                     "  gl_FragDepth = nvfword(nvzdepth(1.0 / pc.zscale));\n"
                     "  if (nvzcull) discard;\n" :
                     (k->compare & HLE_PSH_ZFWORD) && (k->compare & HLE_PSH_NVLINE) && k->linez ?
                     "  if (pc.mvp[2].x != 0.0) gl_FragDepth = nvfword(clamp(gl_FragCoord.z / pc.zscale, 0.0, 1.0 / pc.zscale));\n"
                     "  else gl_FragDepth = nvfword(float(nvlzd(1.0 / pc.zscale)));\n" :
                     k->compare & HLE_PSH_ZFWORD ?
                     "  { float zm = 1.0 / pc.zscale;\n"
                     "    float zo = uintBitsToFloat(pc.psc[18]) + uintBitsToFloat(pc.psc[19]) *\n"
                     "        max(abs(dFdx(gl_FragCoord.z)), abs(dFdy(gl_FragCoord.z))) * zm;\n"
                     "    gl_FragDepth = nvfword(clamp(gl_FragCoord.z * zm + zo, 0.0, zm)); }\n" :
                     k->wbuf ? HLE_WBUF_STORE_GLSL :
                     nvcol && (k->compare & HLE_PSH_ZWORD) && (k->vol & HLE_PSH_ZTRUNC) ?
                     /* (the word over 2^n - 1 as HLE_ZNORM_GLSL makes it: a plain
                      * division came out as a product with a short reciprocal, a
                      * step over on every word past 2^23) */
                     "  { float zw = nvzword(1.0 / pc.zscale - 1.0);\n"
                     "    precise float zf = zw * pc.zscale + zw * (pc.zscale * pc.zscale);\n"
                     "    gl_FragDepth = zf; }\n"
                     "  if (nvzcull) discard;\n" :
                     (k->compare & HLE_PSH_NVLINE) && k->linez && (k->vol & HLE_PSH_ZTRUNC) ?
                     "  if (pc.mvp[2].x != 0.0) { float zm = 1.0 / pc.zscale - 1.0;\n"
                     "    gl_FragDepth = clamp(floor(gl_FragCoord.z * zm) / zm, 0.0, 1.0); } else\n"
                     "  { float zw = nvlword(1.0 / pc.zscale - 1.0);\n"
                     "    precise float zf = zw * pc.zscale + zw * (pc.zscale * pc.zscale);\n"
                     "    gl_FragDepth = zf; }\n" :
                     k->vol & HLE_PSH_ZTRUNC ?
                     "  { float zm = 1.0 / pc.zscale - 1.0;\n"
                     "    float zo = uintBitsToFloat(pc.psc[18]) + uintBitsToFloat(pc.psc[19]) *\n"
                     "        max(abs(dFdx(gl_FragCoord.z)), abs(dFdy(gl_FragCoord.z))) * zm;\n"
                     "    gl_FragDepth = clamp(floor(gl_FragCoord.z * zm + zo) / zm, 0.0, 1.0); }\n" :
                     /* a 16-bit float depth target has no word for a depth under
                      * 2^-6 (its exponent 1's), the console stores 0: nxdk_pgraph_tests'
                      * Depth_buffer, the float pictures' first two rows of quads */
                     k->compare & HLE_PSH_ZF16 ?
                     "  gl_FragDepth = gl_FragCoord.z < 0.015625 * pc.zscale ? 0.0 : gl_FragCoord.z;\n" :
                     /* (no depth to store: the clip range's cull alone) */
                     nvcol && (k->compare & HLE_PSH_ZWORD) ?
                     "  if (nvzmay()) nvzdepth(1.0 / pc.zscale);\n"
                     "  if (nvzcull) discard;\n" : "");
    if (unsupported) {
        *unsupported = unsup;
    }
    if (!b.overflow) {                  /* (nvFC, above) */
        for (char *f = buf; (f = strstr(f, "gl_FragCoord")) != NULL; ) {
            memcpy(f, "nvFC        ", 12);
        }
        char *f = strstr(buf, "HLE_REAL_FC_");
        if (f) {
            memcpy(f, "gl_FragCoord", 12);
        }
        if (g_hle_psh_fast) {
            b.len = fast_cut(buf, b.len);
        }
    }
    return b.overflow ? 0 : b.len;
}
