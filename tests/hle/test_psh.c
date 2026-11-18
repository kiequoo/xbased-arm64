#include "hw/xbox/hle/shader/psh.h"
#include "hw/xbox/hle/shader/ff.h"
#include "hle_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char buf[65536];

static void dump(const char *name)
{
    const char *dir = getenv("HLE_TEST_DUMP");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.frag", dir, name);
        FILE *f = fopen(path, "wb");
        if (f) {
            fputs(buf, f);
            fclose(f);
        }
    }
}

/* register-combiner encodings (XDK PS_* macros) */
#define IN4(a, b, c, d) (((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))
#define R_ZERO 0x0
#define R_C0   0x1
#define R_FOG  0x3
#define R_V0   0x4
#define R_T0   0x8
#define R_T1   0x9
#define R_R0   0xC
#define R_R1   0xD
#define CH_A   0x10
#define INVERT 0x20
#define EXPAND 0x40

/* tex t0; mul r0, t0, v0 */
static void test_modulate(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_V0, R_ZERO, R_ZERO);
    rs[HLE_PS_ALPHA_IN] = IN4(R_T0 | CH_A, R_V0 | CH_A, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_ALPHA_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 1;                            /* t0 PROJECT2D */
    rs[HLE_PS_RGB_IN + 5] = 0xDEADBEEF;                 /* past the count: ignored */
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_EQ_U32(k.rgb_in[5], 0);
    unsigned un = 99;
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), &un);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_modulate");
    HT_EQ_INT(un, 0);
    HT_CHECK(strstr(buf, "vec4 t0 = samp(0u, nvproj(vTex0).xy);") != NULL);
    HT_CHECK(strstr(buf, "vec3 nvproj(vec4 t)") != NULL);
    HT_CHECK(strstr(buf, "vec3 A = max(t0.rgb, 0.0);") != NULL);
    HT_CHECK(strstr(buf, "float Aa = max(t0.a, 0.0);") != NULL);
    HT_CHECK(strstr(buf, "vec3 ab = A * B") != NULL);
    HT_CHECK(strstr(buf, "r0.rgb = ab;") != NULL);
    HT_CHECK(strstr(buf, "r0.a = aab;") != NULL);
    /* no final combiner: D3D's own (fog over r0, alpha r0.a) */
    HT_CHECK(strstr(buf, "outc = vec4(mix(fogr.rgb, clamp(r0.rgb, 0.0, 1.0), fogf), "
                         "clamp(r0.a, 0.0, 1.0));") != NULL);
    HT_CHECK(strstr(buf, "r0 = vec4(0.0, 0.0, 0.0, t0.a)") != NULL);
    k.specular = 1;                                     /* SPECULARENABLE: v1 + r0 */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "clamp(v1.rgb + r0.rgb, 0.0, 1.0), fogf)") != NULL);
    /* no descriptor indexing: every array sized, nothing nonuniform */
    HT_CHECK(hle_glsl_fixed_slots(buf, sizeof(buf)) == strlen(buf));
    HT_CHECK(strstr(buf, "nonuniform") == NULL && strstr(buf, "[]") == NULL);
    HT_CHECK(strstr(buf, "uniform textureCube uTexCube[4];") != NULL);
    HT_CHECK(strstr(buf, "uniform texture3D uTex3D[4];") != NULL);
    HT_CHECK(strstr(buf, "uTex2D[(ti)]") != NULL);
    dump("psh_fixed");
}

/* fog through the final combiner: xfc fog.a, r0, fog.rgb, zero, zero, zero, r0.a;
 * alpha test GREATEREQUAL; linear fog */
static void test_final_fog(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_ZERO | INVERT, R_ZERO, R_ZERO);   /* t0 * 1 */
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 1;
    rs[HLE_PS_FINAL_ABCD] = IN4(R_FOG | CH_A, R_R0, R_FOG, R_ZERO);
    rs[HLE_PS_FINAL_EFG] = IN4(R_ZERO, R_ZERO, R_R0 | CH_A, 0x80);
    HlePshKey k;
    hle_psh_key(rs, &k);
    k.fog = 4;
    k.alpha_test = 1;
    k.alpha_func = 6;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    dump("psh_fog");
    HT_CHECK(strstr(buf, "vec3 B = max(r0.rgb, 0.0)") == NULL ||
             strstr(buf, "(1.0 - clamp(vec4(0.0).rgb, 0.0, 1.0))") != NULL);
    HT_CHECK(strstr(buf, "vec3 A = clamp(max(fogr.aaa, 0.0), 0.0, 1.0);") != NULL);
    HT_CHECK(strstr(buf, "outc = vec4(A * B + (1.0 - A) * C + D, G);") != NULL);
    HT_CHECK(strstr(buf, "float G = clamp(max(r0.a, 0.0), 0.0, 1.0);") != NULL);
    HT_CHECK(strstr(buf, "v1r0 = clamp(v1r0, 0.0, 1.0);") != NULL);
    HT_CHECK(strstr(buf, "(pc.fog.y - vFog) / (pc.fog.y - pc.fog.x)") != NULL);
    HT_CHECK(strstr(buf, "a >= r") != NULL);
    /* ... of the alpha in eight bits against the reference's */
    HT_CHECK(strstr(buf, "float a = floor(outc.a * 255.0 + 0.5), r = floor(pc.alpha_ref * 255.0 + 0.5);") != NULL);
    k.fog_abs = 1;                      /* an _ABS fog mode the title pushed */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "(pc.fog.y - abs(vFog)) / (pc.fog.y - pc.fog.x)") != NULL);
    k.fog_abs = 0;
    k.tex_modes = 2;                    /* PROJECT3D: (s, t, r) / q */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "vec4 t0 = samp3x(0u, nvproj(vTex0), true);") != NULL);
    /* (its s and t on a texel edge by the corners' ties, point-sampled only) */
    HT_CHECK(strstr(buf, "if (tie && (pc.flags & (16u << s)) != 0u") != NULL);
    k.tex_modes = 1;
    k.vol = 1;                          /* PROJECT2D on a volume: its first slice (texel edges as 2D's) */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "vec4 t0 = samp3t(0u, nvproj(vTex0).xy);") != NULL);
    HT_CHECK(strstr(buf, "return samp3(s, vec3(uv, 0.0));") != NULL);
    k.vol = 0;
}

/* two stages: dot3 with expanded normals and MSB mux, unique C0, clip plane,
 * an unsupported (cube) mode, blue-to-alpha and output scaling */
static void test_dot_mux(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_TEXMODES] = 1 | (3u << 5) | (5u << 10);   /* t0 2D, t1 cube, t2 clip */
    rs[HLE_PS_COMPARE] = 0x5u << 8;                      /* t2: s >= 0, r >= 0 */
    rs[HLE_PS_RGB_IN] = IN4(R_T0 | EXPAND, R_V0 | EXPAND, R_C0, R_T1);
    rs[HLE_PS_RGB_OUT] = (R_R1 << 4) | R_R0 | (0x2000) | (0x80000) | (2u << 15);
    rs[HLE_PS_RGB_IN + 1] = IN4(R_R0, R_C0, R_R1, R_C0);
    rs[HLE_PS_RGB_OUT + 1] = (R_R0 << 8) | 0x4000;      /* r0 = mux */
    rs[HLE_PS_COUNT] = 2 | 0x100 | 0x1000;
    HlePshKey k;
    hle_psh_key(rs, &k);
    unsigned un = 0;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_dot");
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "vec4 t1 = sampc(1u, vTex1.xyz);") != NULL);
    HT_CHECK(strstr(buf, "vec3 ab = vec3(dot(A, B))") != NULL);
    HT_CHECK(strstr(buf, "(2.0 * max(t0.rgb, 0.0) - 1.0)") != NULL);
    HT_CHECK(strstr(buf, "r1.a = ab.b;") != NULL);
    HT_CHECK(strstr(buf, "ab = clamp((ab * 2.0), -1.0, 1.0);") != NULL);
    HT_CHECK(strstr(buf, "bool mx = r0.a > 0.5;") != NULL);
    HT_CHECK(strstr(buf, "vec3 ms = mx ? cd : ab;") != NULL);
    HT_CHECK(strstr(buf, "col(pc.psc[1])") != NULL);    /* stage 1's own C0 */
    HT_CHECK(strstr(buf, "if (cp2.x >= 0.0) discard;") != NULL);
    HT_CHECK(strstr(buf, "if (cp2.y < 0.0) discard;") != NULL);
}

/* Halo 2's scope mask: r0.a = 0.5 - t0.b (HALFBIAS_NEGATE), then a MUX on
 * its MSB picks black (CD) where t0.b is 0: the bias's 0.5 is 0x80 / 0xFF
 * in shaders that MUX on the MSB, 0.5 in the others */
static void test_mux_bias(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_TEXMODES] = 1;
    rs[HLE_PS_ALPHA_IN + 1] = IN4(R_C0 | CH_A, R_ZERO | INVERT, R_T0 | 0xA0, R_ZERO | INVERT);
    rs[HLE_PS_ALPHA_OUT + 1] = R_R0 << 8;               /* r0.a = C0.a + (0.5 - t0.b) */
    rs[HLE_PS_RGB_IN + 2] = IN4(R_C0, R_ZERO | INVERT, R_R0, R_ZERO | INVERT);
    rs[HLE_PS_RGB_OUT + 2] = (R_T1 << 8) | 0x4000;      /* t1 = mux(C0, r0) */
    rs[HLE_PS_COUNT] = 3 | 0x100;
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    dump("psh_mux_bias");
    HT_CHECK(strstr(buf, "float Ca = (-max(t0.b, 0.0) + (128.0 / 255.0));") != NULL);
    HT_CHECK(strstr(buf, "bool mx = r0.a > 0.5;") != NULL);
    HT_CHECK(strstr(buf, "vec3 ms = mx ? cd : ab;") != NULL);
    /* a MUX in the alpha portion only; the output BIAS takes the same 0x80 */
    rs[HLE_PS_RGB_OUT + 2] = R_T1 << 8;
    rs[HLE_PS_ALPHA_IN + 2] = IN4(R_C0 | CH_A, R_ZERO | INVERT, R_R0 | CH_A, R_ZERO | INVERT);
    rs[HLE_PS_ALPHA_OUT + 2] = (R_T1 << 8) | 0x4000;
    rs[HLE_PS_RGB_IN + 1] = IN4(R_T0 | 0x80, R_ZERO | INVERT, R_ZERO, R_ZERO);  /* HALFBIAS_NORMAL */
    rs[HLE_PS_RGB_OUT + 1] = (R_R1 << 4) | (3u << 15);  /* SHIFTLEFT_1_BIAS */
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "ams = mx ? acd : aab;") != NULL);
    HT_CHECK(strstr(buf, "vec3 A = (max(t0.rgb, 0.0) - (128.0 / 255.0));") != NULL);
    HT_CHECK(strstr(buf, "ab = clamp(((ab - (128.0 / 255.0)) * 2.0), -1.0, 1.0);") != NULL);
    rs[HLE_PS_RGB_IN + 1] = 0;
    rs[HLE_PS_RGB_OUT + 1] = 0;
    rs[HLE_PS_ALPHA_IN + 2] = 0;
    rs[HLE_PS_ALPHA_OUT + 2] = 0;
    rs[HLE_PS_RGB_OUT + 2] = (R_T1 << 8) | 0x4000;
    rs[HLE_PS_COUNT] = 3;                               /* MUX on the LSB */
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "float Ca = (-max(t0.b, 0.0) + (128.0 / 255.0));") != NULL);
    rs[HLE_PS_COUNT] = 3 | 0x100;
    rs[HLE_PS_RGB_OUT + 2] = R_T1 << 8;                 /* no MUX: sum */
    rs[HLE_PS_ALPHA_OUT + 1] = (R_R0 << 8) | (1u << 15);   /* BIAS */
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "float Ca = (-max(t0.b, 0.0) + (128.0 / 255.0));") != NULL);
    HT_CHECK(strstr(buf, "ams = clamp((ams - (128.0 / 255.0)), -1.0, 1.0);") != NULL);
}

/* PGR2's car paint: t0 normal map, t1/t2 DOTPRODUCT, t3 DOT_RFLCT_SPEC */
static void test_reflect(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_TEXMODES] = 0x64621;
    rs[HLE_PS_DOTMAP] = 0x111;                          /* MINUS1_TO_1_D3D */
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_V0, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    unsigned un = 99;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_reflect");
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "dot1 = dot(HLE_NVPT(vTexP1, vTex1, dx1).xyz, dmap(1u, t0));") != NULL);
    HT_CHECK(strstr(buf, "dot2 = dot(HLE_NVPT(vTexP2, vTex2, dx2).xyz, dmap(1u, t0));") != NULL);
    HT_CHECK(strstr(buf, "dot3 = dot(HLE_NVPT(vTexP3, vTex3, dx3).xyz, dmap(1u, t0));") != NULL);
    HT_CHECK(strstr(buf, "e = vec3(vTex1.w, vTex2.w, vTex3.w)") != NULL);
    HT_CHECK(strstr(buf, "t3 = sampc(3u,") != NULL);
}

/* NGB's shadow receivers: t0 a D24 shadow buffer (PROJECT3D, SHADOWFUNC
 * GREATEREQUAL). The coordinates go through NV2A's projection, so a
 * receiver with oT0 = 0 compares r = 0 (lit) instead of NaN weights. */
static void test_shadow_project(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_C0, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 2;                            /* t0 PROJECT3D */
    HlePshKey k;
    hle_psh_key(rs, &k);
    k.shadow = 1 | (6u << 4);
    k.shadow_fmt = 1;
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), NULL);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_shadow");
    HT_CHECK(strstr(buf, "vec3 pq = nvproj(vTex0);") != NULL);
    HT_CHECK(strstr(buf, "float m = 16777215.0, r = clamp(pq.z, 0.0, m);") != NULL);
    HT_CHECK(strstr(buf, "greaterThanEqual(d, vec4(r))") != NULL);
    HT_CHECK(strstr(buf, "/ vTex0.w") == NULL);
}

/* Otogi's title ripple (modes 0x004c1): t0 the bump map, t1 BUMPENVMAP of
 * the copied scene, t2 2D; then t2 BUMPENVMAP_LUM reading t1 (PSInputTexture) */
static void test_bumpenv(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T1, R_ZERO | INVERT, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 0x004c1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    unsigned un = 99;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_bumpenv");
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "uvec4 bump;") != NULL);
    HT_CHECK(strstr(buf, "vec2 d = bmat(pc.bump.x, pc.bump.y, pc.bumpl[0]) * sgn8(t0.bg);") != NULL);
    HT_CHECK(strstr(buf, "mat2 bmat(uint a, uint c, uint r) {") != NULL);
    HT_CHECK(strstr(buf, "t1 = sampx(1u, nvproj(vTex1).xy + d, true);") != NULL);
    HT_CHECK(strstr(buf, "vec4 t2 = samp(2u, nvproj(vTex2).xy);") != NULL);
    k.vol = 2;                          /* t1 a volume: r passes through */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    HT_CHECK(strstr(buf, "t1 = samp3(1u, vec3(nvproj(vTex1).xy + d, vTex1.z));") != NULL);
    rs[HLE_PS_TEXMODES] = 1 | (1u << 5) | (7u << 10);
    rs[HLE_PS_INPUTTEX] = 1u << 16;                     /* t2 reads t1 */
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "sgn8(t1.bg);") != NULL);
    /* (an unsigned 2D input: its red blended with the weights cut, lum8) */
    HT_CHECK(strstr(buf, "float lum8(uint s, vec2 uv, float f, bool pt) {") != NULL);
    HT_CHECK(strstr(buf, "t2.rgb = floor(t2.rgb * max(l.x * (floor(lum8(1u, nvproj(vTex1).xy, t1.r, false) * 255.0 + 0.5) / 255.0) + l.y, 0.0) * 255.0") != NULL);
    /* a signed stage (its bytes flipped at definition) beside a program's own per-stage
     * rectangles: the signed sampling's function whole (its buffer had cut it short) */
    rs[HLE_PS_TEXMODES] = HLE_PST_PROJECT2D | HLE_PST_PROJECT2D << 5;
    rs[HLE_PS_INPUTTEX] = 0;
    hle_psh_key(rs, &k);
    k.dot_map |= 15u << (16 + 4);       /* t1: every channel signed */
    k.input_tex |= 1u << 1;
    k.vol |= HLE_PSH_RECT4;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_signed_rect4");
    HT_CHECK(strstr(buf, "t1 = samps(1u, nvproj(vTex1).xy, t1,") != NULL);
    HT_CHECK(strstr(buf, "vec4 samps(uint s, vec2 uv, vec4 f, bvec4 m, bool pt, bool fl) {") != NULL);
    HT_CHECK(strstr(buf, "  return mix(f, vec4(ibil(r, q), ibil(g, q), ibil(b, q), ibil(a, q)), m);\n}\n") != NULL);
    HT_CHECK(strstr(buf, "uv = nvrectr(uv, r,") != NULL);
}

/* A fog coordinate that is infinite, a factor that is no number: by the fog mode */
static void test_fog_exceptional(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_V0, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    static const struct {
        uint8_t fog, abs;
        const char *formula, *rule;
    } c[] = {
        /* EXP: unfogged; EXP_ABS: fogged (pc.fog.x: FOG_PARAMS p0 - 1.5) */
        { 2, 0, "float fogf = pc.fog.x + exp(-vFog * pc.fog.z);", "fogf = isinf(vFog) || isnan(fogf) ? 1.0 :" },
        { 2, 1, "float fogf = pc.fog.x + exp(-abs(vFog) * pc.fog.z);", "fogf = isinf(vFog) || isnan(fogf) ? 0.0 :" },
        /* EXP2, EXP2_ABS: fogged */
        { 3, 0, "float fogf = pc.fog.x + exp(-fd * fd);", "fogf = isinf(vFog) || isnan(fogf) ? 0.0 :" },
        { 3, 1, "float fogf = pc.fog.x + exp(-fd * fd);", "fogf = isinf(vFog) || isnan(fogf) ? 0.0 :" },
        /* LINEAR, LINEAR_ABS: unfogged */
        { 4, 0, "float fogf = (pc.fog.y - vFog) / (pc.fog.y - pc.fog.x);",
          "fogf = isinf(vFog) || isnan(fogf) ? 1.0 :" },
        { 4, 1, "float fogf = (pc.fog.y - abs(vFog)) / (pc.fog.y - pc.fog.x);",
          "fogf = isinf(vFog) || isnan(fogf) ? 1.0 :" },
    };
    for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        k.fog = c[i].fog;
        k.fog_abs = c[i].abs;
        HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
        if (i == 1) {
            dump("psh_fog_exc");
        }
        HT_CHECK(strstr(buf, c[i].formula) != NULL);
        HT_CHECK(strstr(buf, c[i].rule) != NULL);
        HT_CHECK(strstr(buf, "clamp(fogf, 0.0, 1.0);") != NULL);
    }
    /* a vertex factor (no table mode): as it was, a NaN unfogged */
    k.fog = 1;
    k.fog_abs = 0;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "float fogf = isnan(vFog) ? 1.0 : clamp(vFog, 0.0, 1.0);") != NULL);
    HT_CHECK(strstr(buf, "isinf") == NULL);
}

/* The colour key: a stage's fetched texel equal to SET_COLOR_KEY_COLOR, by the stage's mode */
static void test_color_key(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_V0, R_ZERO, R_ZERO);
    rs[HLE_PS_ALPHA_IN] = IN4(R_T0 | CH_A, R_V0 | CH_A, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_ALPHA_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    /* t0 PROJECT2D, t1 CUBEMAP, t2 PASSTHRU, t3 PROJECT2D */
    rs[HLE_PS_TEXMODES] = 1 | 3 << 5 | 4 << 10 | 1 << 15;
    HlePshKey k;
    hle_psh_key(rs, &k);
    unsigned un = 99;
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), &un);
    HT_CHECK(n > 0);
    HT_CHECK(strstr(buf, "ckey") == NULL);              /* no key: the text as it was */
    /* stage 0 discards on magenta, alpha compared; stage 1 loses its alpha on a key whose alpha
     * is not compared; stage 2's mode fetches nothing: no test; stage 3 goes to (0, 0, 0, 0) */
    k.tex_modes |= HLE_PSH_CKEY_BITS(0, 3, false) | HLE_PSH_CKEY_BITS(1, 1, true) |
                   HLE_PSH_CKEY_BITS(2, 3, false) | HLE_PSH_CKEY_BITS(3, 2, false);
    k.ckey[0] = 0xFFFF00FFu;
    k.ckey[1] = 0x00102030u;
    k.ckey[2] = 0xFF000000u;
    k.ckey[3] = 0x00000000u;
    HT_EQ_U32(HLE_PSH_CKEY(&k, 0), 3);
    HT_EQ_U32(HLE_PSH_CKEY(&k, 1), 1);
    HT_EQ_U32(HLE_PSH_CKEY_NOALPHA(&k, 1), 1);
    HT_EQ_U32(HLE_PSH_CKEY_NOALPHA(&k, 0), 0);
    HT_EQ_U32((k.tex_modes >> 5) & 0x1F, 3);            /* the modes untouched */
    n = hle_psh_glsl(&k, buf, sizeof(buf), &un);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_color_key");
    HT_EQ_INT(un, 0);
    HT_CHECK(strstr(buf, "bool ckey(vec4 t, uint key, uint mask)") != NULL);
    HT_CHECK(strstr(buf, "uvec4 c = uvec4(clamp(t, 0.0, 1.0) * 255.0 + 0.5);") != NULL);
    /* (the 2D stages through ckeyt: the key on each texel the filter weighs) */
    HT_CHECK(strstr(buf, "vec4 ckeyt(uint s, vec2 uv, vec4 t, uint key, uint mask, uint op)") != NULL);
    HT_CHECK(strstr(buf, "t0 = ckeyt(0u, nvproj(vTex0).xy, t0, 0xffff00ffu, 0xffffffffu, 3u);") != NULL);
    HT_CHECK(strstr(buf, "if (ckey(t1, 0x00102030u, 0x00ffffffu)) { t1.a = 0.0; }") != NULL);
    HT_CHECK(strstr(buf, "ckey(t2,") == NULL);
    HT_CHECK(strstr(buf, "t3 = ckeyt(3u, nvproj(vTex3).xy, t3, 0x00000000u, 0xffffffffu, 2u);") != NULL);
    /* the test follows the fetch and comes before the combiners read the texel */
    const char *fetch = strstr(buf, "vec4 t0 = samp(0u, nvproj(vTex0).xy);");
    const char *test = strstr(buf, "t0 = ckeyt(0u,");
    const char *comb = strstr(buf, "/* combiner stage 0 */");
    HT_CHECK(fetch && test && comb && fetch < test && test < comb);
    /* a stage under a convolution filter (bordc bits 4-7): sampx's taps */
    k.tex_modes &= ~(0xFFu << 20);
    k.bordc = 0x10;
    n = hle_psh_glsl(&k, buf, sizeof(buf), &un);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_conv");
    HT_CHECK(strstr(buf, "vec4 nvconv(uint ti, uint si, vec2 uv, bool gauss)") != NULL);
    HT_CHECK(strstr(buf, "#define HLE_CONV2(s, uv) if (((1u >> (s)) & 1u) != 0u)") != NULL);
    HT_CHECK(strstr(buf, "vec4 nvbc(") == NULL);       /* no border trio */
}

/* A texture of another kind than its stage's mode fetches: a 2D one under the cube modes and
 * DOT_STR_3D, a cube under PROJECT2D */
static void test_texture_kind(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_V0, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    /* t0 PROJECT2D, t1 CUBEMAP */
    rs[HLE_PS_TEXMODES] = HLE_PST_PROJECT2D | HLE_PST_CUBEMAP << 5;
    HlePshKey k;
    hle_psh_key(rs, &k);
    unsigned un = 99;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    HT_CHECK(strstr(buf, "cube2d") == NULL && strstr(buf, "samp2(") == NULL);  /* as it was */
    HT_CHECK(strstr(buf, "vec4 t0 = samp(0u, nvproj(vTex0).xy);") != NULL);
    HT_CHECK(strstr(buf, "vec4 t1 = sampc(1u, vTex1.xyz);") != NULL);
    k.compare |= HLE_PSH_TKIND_BIT(0) | HLE_PSH_TKIND_BIT(1);
    HT_EQ_U32(HLE_PSH_TKIND(&k, 0), 1);
    HT_EQ_U32(HLE_PSH_TKIND(&k, 2), 0);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_kind_cube");
    HT_EQ_INT(un, 0);
    /* a cube under PROJECT2D: along (1, t, -s) */
    HT_CHECK(strstr(buf, "t0 = sampc(0u, normalize(vec3(1.0, p.y, -p.x)));") != NULL);
    /* a 2D texture under CUBEMAP: the minor coordinates over the major one */
    HT_CHECK(strstr(buf, "vec2 cube2d(vec3 d)") != NULL);
    HT_CHECK(strstr(buf, "vec4 t1 = samp2(1u, cube2d(vTex1.xyz));") != NULL);

    /* the dot-product modes: t1 DOTPRODUCT, t2 DOT_RFLCT_DIFF, t3 DOT_RFLCT_SPEC, 2D textures */
    rs[HLE_PS_TEXMODES] = HLE_PST_PROJECT2D | HLE_PST_DOTPRODUCT << 5 | HLE_PST_DOT_RFLCT_DIFF << 10 |
                          HLE_PST_DOT_RFLCT_SPEC << 15;
    hle_psh_key(rs, &k);
    k.compare |= HLE_PSH_TKIND_BIT(2) | HLE_PSH_TKIND_BIT(3);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_kind_reflect");
    HT_CHECK(strstr(buf, "vec4 t2 = samp2(2u, cube2d(vec3(dot1, dot2, dot3)));") != NULL);
    HT_CHECK(strstr(buf, "t3 = samp2(3u, cube2d(rv));") != NULL);
    HT_CHECK(strstr(buf, "sampc(2u") == NULL && strstr(buf, "sampc(3u") == NULL);

    /* t3 DOT_STR_3D and DOT_STR_CUBE with a 2D texture */
    rs[HLE_PS_TEXMODES] = HLE_PST_PROJECT2D | HLE_PST_DOTPRODUCT << 5 | HLE_PST_DOTPRODUCT << 10 |
                          HLE_PST_DOT_STR_3D << 15;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    HT_CHECK(strstr(buf, "vec4 t3 = samp3(3u, vec3(dot1, dot2, dot3));") != NULL);
    k.compare |= HLE_PSH_TKIND_BIT(3);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_kind_str3d");
    HT_CHECK(strstr(buf, "vec4 t3 = samp2(3u, vec2(dot1, dot2));") != NULL);
    rs[HLE_PS_TEXMODES] = HLE_PST_PROJECT2D | HLE_PST_DOTPRODUCT << 5 | HLE_PST_DOTPRODUCT << 10 |
                          HLE_PST_DOT_STR_CUBE << 15;
    hle_psh_key(rs, &k);
    k.compare |= HLE_PSH_TKIND_BIT(3);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_kind_strcube");
    HT_CHECK(strstr(buf, "vec4 t3 = samp2(3u, cube2d(vec3(dot1, dot2, dot3)));") != NULL);
    /* the comparison bits of a clip-plane stage are not the kind's: the kind above them */
    HT_EQ_U32(k.compare & 0xFFFF, 0);
}

static void test_overflow(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_COUNT] = 8;
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, 1024, NULL) == 0);
}

/* the exact pixel stage (colour weights, depth word): with 64-bit integers the
 * fixed formats' word goes through nvzfast first, the float ones' never */
static char big[262144];

static void dump_big(const char *name)
{
    const char *dir = getenv("HLE_TEST_DUMP");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.frag", dir, name);
        FILE *f = fopen(path, "wb");
        if (f) {
            fputs(big, f);
            fclose(f);
        }
    }
}

static void test_exact_depth(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_V0, R_ZERO, R_ZERO);
    rs[HLE_PS_ALPHA_IN] = IN4(R_T0 | CH_A, R_V0 | CH_A, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_ALPHA_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    k.compare |= HLE_PSH_NVCOL | HLE_PSH_ZWORD;
    g_hle_glsl_f64 = true;
    for (int i64 = 0; i64 < 2; i64++) {
        g_hle_glsl_i64 = i64 != 0;
        static const char *nm[2][4] = {
            { "psh_exact_ztrunc", "psh_exact_cull", "psh_exact_zfword", "psh_exact_fog" },
            { "psh_exact64_ztrunc", "psh_exact64_cull", "psh_exact64_zfword", "psh_exact64_fog" } };
        for (int v = 0; v < 4; v++) {
            HlePshKey kv = k;
            if (v == 0) kv.vol |= HLE_PSH_ZTRUNC;
            if (v == 2) kv.compare |= HLE_PSH_ZFWORD;
            if (v == 3) { kv.vol |= HLE_PSH_ZTRUNC | HLE_PSH_TWO_SIDE; kv.fog = 5; }
            HT_CHECK(hle_psh_glsl(&kv, big, sizeof(big), NULL) > 0);
            dump_big(nm[i64][v]);
            HT_CHECK((strstr(big, "#define HLE_NVZ64") != NULL) == (i64 && v != 2));
            HT_CHECK((strstr(big, "GL_ARB_gpu_shader_int64") != NULL) == (i64 && v != 2));
            HT_CHECK(strstr(big, "nvweights()") != NULL);
            HT_CHECK(strstr(big, "nvzdepth(") != NULL);
        }
    }
    g_hle_glsl_f64 = g_hle_glsl_i64 = false;
}

int main(void)
{
    HT_RUN(test_modulate);
    HT_RUN(test_final_fog);
    HT_RUN(test_dot_mux);
    HT_RUN(test_mux_bias);
    HT_RUN(test_reflect);
    HT_RUN(test_shadow_project);
    HT_RUN(test_bumpenv);
    HT_RUN(test_fog_exceptional);
    HT_RUN(test_color_key);
    HT_RUN(test_texture_kind);
    HT_RUN(test_overflow);
    HT_RUN(test_exact_depth);
    HT_DONE();
}
