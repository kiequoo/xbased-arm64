/*
 * xbased HLE GPU — the NV2A fixed-function vertex stage as GLSL (pure). See
 * ffhw.h. The transform engine's fixed function, as NV_vertex_program's
 * documentation and the hardware tests describe it (LLE's generator read as
 * reference only):
 * - skinning blends the position and normal over model-view matrices 0..n-1
 *   (and their inverses), the last weight generated (1 - the others) in the
 *   "G" modes; without skinning the composite matrix includes model-view;
 * - lighting in eye space, the registers D3D's products: light colours
 *   premultiplied by the material where its source is the material, the
 *   scene ambient (with the emission) and material emission registers
 *   reused for the vertex-colour sources;
 * - the composite matrix includes the viewport scale: x, y, z / w plus the
 *   viewport offset is the screen-space position a program leaves.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw/xbox/hle/shader/ffhw.h"

typedef struct Buf {
    char *p;
    size_t cap, len;
    bool overflow;
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

/* A colour source of SET_COLOR_MATERIAL: the material's (the register
 * already holds it: 1), the diffuse or the specular register. */
static const char *src_rgb(unsigned s)
{
    return s == 1 ? "v3.rgb" : s == 2 ? "v4.rgb" : "vec3(1.0)";
}

static void skinning(Buf *b, const HleFfhwKey *k)
{
    static const struct { uint8_t n, mix; } k_skin[7] = {
        { 0, 0 }, { 2, 1 }, { 2, 0 }, { 3, 1 }, { 3, 0 }, { 4, 1 }, { 4, 0 },
    };
    unsigned n = k->skin < 7 ? k_skin[k->skin].n : 0, mix = k->skin < 7 ? k_skin[k->skin].mix : 0;
    if (!n) {
        put(b, "    tPos = XF(pos, %d);\n"
               "    tN = XF(n4, %d).xyz;\n", HLE_FFHW_C_MODELVIEW, HLE_FFHW_C_INVMODELVIEW);
        return;
    }
    put(b, "    tPos = vec4(0.0);\n"
           "    tN = vec3(0.0);\n"
           "    float wn = 1.0, wi;\n");
    for (unsigned i = 0; i < n; i++) {
        if (mix && i == n - 1) {
            put(b, "    wi = wn;\n");
        } else {
            put(b, "    wi = v1.%c;\n", "xyzw"[i]);
            if (mix) {
                put(b, "    wn -= wi;\n");
            }
        }
        put(b, "    tPos += XF(pos, %u) * wi;\n"
               "    tN += XF(n4, %u).xyz * wi;\n", HLE_FFHW_C_MODELVIEW + 4 * i,
            HLE_FFHW_C_INVMODELVIEW + 4 * i);
    }
}

static void texgen(Buf *b, const HleFfhwKey *k)
{
    /* the direction a reflection is taken about: from the eye to the vertex,
     * or straight ahead for every vertex (SET_TEXGEN_VIEW_MODEL's infinite
     * viewer: xemu's tests on a console, its pull request 3087) */
    const char *view = k->texgen_inf ? "vec3(0.0, 0.0, 1.0)" : "normalize(tPos.xyz)";
    for (unsigned s = 0; s < 4; s++) {
        put(b, "    oT%u = v%u;\n", s, 9 + s);
        for (unsigned j = 0; j < 4; j++) {
            const unsigned g = k->texgen[s][j], plane = HLE_FFHW_C_TEXGEN + 4 * s + j;
            const char c = "xyzw"[j];
            if (g == HLE_FFHW_TG_EYE) {
                put(b, "    oT%u.%c = dot(c[%u], tPos);\n", s, c, plane);
            } else if (g == HLE_FFHW_TG_OBJECT) {
                put(b, "    oT%u.%c = dot(c[%u], pos);\n", s, c, plane);
            } else if (g == HLE_FFHW_TG_SPHERE && j < 2) {
                put(b, "    { vec3 r = reflect(%s, tN);\n"
                       "      oT%u.%c = r.%c / (2.0 * length(r + vec3(0.0, 0.0, 1.0))) + 0.5; }\n",
                    view, s, c, c);
            } else if (g == HLE_FFHW_TG_REFLECTION && j < 3) {
                put(b, "    oT%u.%c = reflect(%s, tN).%c;\n", s, c, view, c);
            } else if (g == HLE_FFHW_TG_NORMAL && j < 3) {
                put(b, "    oT%u.%c = tN.%c;\n", s, c, c);
            }
        }
        if (k->texmat & (1u << s)) {
            put(b, "    oT%u = XF(oT%u, %u);\n", s, s, HLE_FFHW_C_TEXMAT + 4 * s);
        }
    }
}

/* One side's colours: the front's in oD0 / oD1 from the front material and
 * lights; with two-sided lighting (SET_LIGHT_TWO_SIDE_ENABLE) the back's in
 * oB0 / oB1, the normal turned round, from the back registers (the back
 * scene ambient + emission, alpha, emission, specular numbers, the lights'
 * back colours and SET_COLOR_MATERIAL's back sources): nxdk_pgraph_tests'
 * Specular_back, the back faces lit in the back lights' colours. */
static void light_loop(Buf *b, const HleFfhwKey *k, bool back);

/* One side's colours (lighting 1); loop: the lights as one loop (a stand-in's, light_loop) */
static void light_side(Buf *b, const HleFfhwKey *k, bool back, bool loop)
{
    const uint8_t *src = back ? k->bsrc : k->src;
    const char *N = back ? "(-tN)" : "tN", *d0 = back ? "oB0" : "oD0", *d1 = back ? "oB1" : "oD1";
    const unsigned amb = back ? HLE_FFHW_C_BACK_AMBIENT : HLE_FFHW_C_AMBIENT;
    const unsigned emi = back ? HLE_FFHW_C_BACK_EMISSION : HLE_FFHW_C_EMISSION;
    const unsigned spc = back ? HLE_FFHW_C_BACK_SPEC : HLE_FFHW_C_SPEC;
    const char *alpha = src[2] == 1 ? "v3.a" : src[2] == 2 ? "v4.a" : "c[%u].w";
    char a[32];
    snprintf(a, sizeof(a), alpha, amb);
    /* the ambient source scaled by the emission register, plus the
     * emission source (the scene ambient register for the material: D3D
     * folds ambient * material ambient + emissive into it) */
    /* ... as the chip makes it (xemu's tests on a console): with the
     * emission or the ambient from the material, the scene ambient register
     * plus the other one's vertex colour scaled by the emission register;
     * with both from vertex colours, the emission's colour plus the
     * ambient's scaled by it. (The scene ambient scaled by the emission
     * register was added to the emission source whatever they were:
     * nxdk_pgraph_tests' Material_color_source, every case with an emission
     * off the console's.) */
    if (src[0] == 0 || src[1] == 0) {
        put(b, "    %s = vec4(c[%u].rgb, %s);\n", d0, amb, a);
        const unsigned add = src[0] != 0 ? src[0] : src[1];
        if (add) {
            put(b, "    %s.rgb += %s * c[%u].rgb;\n", d0, src_rgb(add), emi);
        }
    } else {
        put(b, "    %s = vec4(%s + %s * c[%u].rgb, %s);\n", d0, src_rgb(src[0]), src_rgb(src[1]), emi, a);
    }
    put(b, "    %s = vec4(0.0, 0.0, 0.0, v4.a);\n", d1);
    if (loop) {
        light_loop(b, k, back);
        return;
    }
    for (unsigned i = 0; i < 8; i++) {
        const unsigned t = k->light[i], L = HLE_FFHW_C_LIGHTS + HLE_FFHW_LIGHT_VEC4 * i;
        if (!t) {
            continue;
        }
        /* its colours: the front's, or the back's (SET_BACK_LIGHT_*_COLOR) */
        const unsigned lc = back ? HLE_FFHW_C_BACK_LIGHTS + 3 * i : L + HLE_FFHW_L_AMBIENT;
        put(b, "    { /* light %u */\n"
               "      float att = 1.0, nl, nh;\n"
               "      bool lit = true;\n", i);
        if (t == 1) {
            put(b, "      vec3 Ld = normalize(c[%u].xyz);\n"
                   "      nl = max(dot(%s, Ld), 0.0);\n", L + HLE_FFHW_L_DIRECTION, N);
            if (k->local_eye) {
                put(b, "      nh = max(dot(%s, normalize(Ld + eyeV)), 0.0);\n", N);
            } else {
                put(b, "      nh = max(dot(%s, c[%u].xyz), 0.0);\n", N, L + HLE_FFHW_L_HALF);
            }
        } else {
            put(b, "      vec3 VP = c[%u].xyz - tPos.xyz / tPos.w;\n"
                   "      float d = length(VP);\n"
                   "      lit = d <= c[%u].x;\n"
                   "      VP = normalize(VP);\n"
                   "      vec3 at = c[%u].xyz;\n"
                   "      att = 1.0 / (at.x + at.y * d + at.z * d * d);\n"
                   "      nl = max(dot(%s, VP), 0.0);\n"
                   "      nh = max(dot(%s, normalize(VP + eyeV)), 0.0);\n",
                L + HLE_FFHW_L_POSITION, L + HLE_FFHW_L_RANGE, L + HLE_FFHW_L_ATTENUATION, N, N);
            if (t == 3) {
                /* The spot: its direction comes scaled by 1 / (cos(theta/2)
                 * - cos(phi/2)) with w = -cos(phi/2) times that, so s below
                 * is 0 at the outer cone and 1 at the inner one. Between
                 * them the NV2A takes the falloff curve as a ratio of two
                 * lines, (s + L) / (M s + N), by SET_LIGHT_SPOT_FALLOFF's
                 * three numbers (the same form as the specular power's;
                 * xemu's pull request 3078, from its tests on a console). A
                 * falloff of 1 is (0, 0, 1): the straight line this was for
                 * every falloff. */
                put(b, "      vec4 sd = c[%u];\n"
                       "      float s = dot(sd.xyz, VP) + sd.w;\n"
                       "      vec3 fo = c[%u].xyz;\n"
                       "      float dn = fo.y * min(s, 1.0) + fo.z;\n"
                       "      if (s <= 0.0) att = 0.0;\n"
                       "      else if (dn == 0.0) att *= min(s, 1.0) + fo.x > 0.0 ? " HLE_SPOT_DIV0 " : 0.0;\n"
                       "      else if (s < 1.0) {\n"
                       "        att *= s <= -fo.x ? 0.0 :\n"
                       "               dn > 0.0 ? clamp((s + fo.x) / dn, 0.0, 1.0) : 0.0;\n"
                       "      }\n",
                    L + HLE_FFHW_L_SPOT, L + HLE_FFHW_L_FALLOFF);
            }
        }
        /* The specular factor: the NV2A has no power function. It takes
         * n.h through a ratio of two lines, (x + L) / (M x + N), by
         * SET_SPECULAR_PARAMS' first three numbers, which D3D fits to the
         * material's power (0 below x = -L, 1 at x = 1); xemu's pull request
         * 3076, from its tests on a console. The power was worked back from
         * the numbers and raised here: the same at a power of 1, a wider
         * foot to the highlight above it, and nothing for numbers that
         * weren't D3D's. */
        put(b, "      if (lit) {\n"
               "        vec3 sc = c[%u].xyz;\n"
               "        float dn = sc.y * nh + sc.z;\n"
               "        float pf = nl <= 0.0 || nh <= -sc.x ? 0.0 : dn == 0.0 ? 10000.0 :\n"
               "                   dn > 0.0 ? clamp((nh + sc.x) / dn, 0.0, 10000.0) : 0.0;\n"
               "        %s.rgb += %s * c[%u].rgb * att;\n"
               "        %s.rgb += %s * c[%u].rgb * (att * nl);\n"
               "        %s.rgb += %s * c[%u].rgb * (att * pf);\n"
               "      }\n"
               "    }\n",
            spc, d0, src_rgb(src[1]), lc + 0, d0, src_rgb(src[2]), lc + 1, d1, src_rgb(src[3]), lc + 2);
    }
}

/* light_side's lights as one loop over them, for a stand-in (vsh.h
 * HLE_VSH_XF_FAST): each light written out was ~3.4 ms of the driver's build
 * (Hunter: The Reckoning - Redeemer's seven, 42 of a stand-in's 51 ms); the
 * loop's body is built once, the lights' kinds 2 bits each in a word it reads.
 * The same arithmetic, light by light, as light_side's. */
static void light_loop(Buf *b, const HleFfhwKey *k, bool back)
{
    const uint8_t *src = back ? k->bsrc : k->src;
    const char *N = back ? "(-tN)" : "tN", *d0 = back ? "oB0" : "oD0", *d1 = back ? "oB1" : "oD1";
    const unsigned spc = back ? HLE_FFHW_C_BACK_SPEC : HLE_FFHW_C_SPEC;
    char lc[48];
    if (back) {
        snprintf(lc, sizeof(lc), "%u + 3 * li", (unsigned)HLE_FFHW_C_BACK_LIGHTS);
    } else {
        snprintf(lc, sizeof(lc), "L + %u", (unsigned)HLE_FFHW_L_AMBIENT);
    }
    uint32_t kinds = 0;
    for (unsigned i = 0; i < 8; i++) {
        kinds |= (uint32_t)(k->light[i] & 3u) << (2 * i);
    }
    put(b, "    [[dont_unroll]] for (int li = 0; li < 8; li++) {\n"
           "      uint t = (0x%xu >> uint(2 * li)) & 3u;\n"
           "      if (t == 0u) continue;\n"
           "      int L = %u + %u * li, lc = %s;\n"
           "      float att = 1.0, nl, nh;\n"
           "      bool lit = true;\n"
           "      if (t == 1u) {\n"
           "        vec3 Ld = normalize(c[L + %u].xyz);\n"
           "        nl = max(dot(%s, Ld), 0.0);\n",
        kinds, HLE_FFHW_C_LIGHTS, HLE_FFHW_LIGHT_VEC4, lc, HLE_FFHW_L_DIRECTION, N);
    if (k->local_eye) {
        put(b, "        nh = max(dot(%s, normalize(Ld + eyeV)), 0.0);\n", N);
    } else {
        put(b, "        nh = max(dot(%s, c[L + %u].xyz), 0.0);\n", N, HLE_FFHW_L_HALF);
    }
    put(b, "      } else {\n"
           "        vec3 VP = c[L + %u].xyz - tPos.xyz / tPos.w;\n"
           "        float d = length(VP);\n"
           "        lit = d <= c[L + %u].x;\n"
           "        VP = normalize(VP);\n"
           "        vec3 at = c[L + %u].xyz;\n"
           "        att = 1.0 / (at.x + at.y * d + at.z * d * d);\n"
           "        nl = max(dot(%s, VP), 0.0);\n"
           "        nh = max(dot(%s, normalize(VP + eyeV)), 0.0);\n"
           "        if (t == 3u) {\n"
           "          vec4 sd = c[L + %u];\n"
           "          float s = dot(sd.xyz, VP) + sd.w;\n"
           "          if (s <= 0.0) att = 0.0;\n"
           "          else if (s < 1.0) {\n"
           "            vec3 fo = c[L + %u].xyz;\n"
           "            float dn = fo.y * s + fo.z;\n"
           "            att *= s <= -fo.x ? 0.0 : dn == 0.0 ? 1.0 :\n"
           "                   dn > 0.0 ? clamp((s + fo.x) / dn, 0.0, 1.0) : 0.0;\n"
           "          }\n"
           "        }\n"
           "      }\n",
        HLE_FFHW_L_POSITION, HLE_FFHW_L_RANGE, HLE_FFHW_L_ATTENUATION, N, N, HLE_FFHW_L_SPOT,
        HLE_FFHW_L_FALLOFF);
    put(b, "      if (lit) {\n"
           "        vec3 sc = c[%u].xyz;\n"
           "        float dn = sc.y * nh + sc.z;\n"
           "        float pf = nl <= 0.0 || nh <= -sc.x ? 0.0 : dn == 0.0 ? 10000.0 :\n"
           "                   dn > 0.0 ? clamp((nh + sc.x) / dn, 0.0, 10000.0) : 0.0;\n"
           "        %s.rgb += %s * c[lc].rgb * att;\n"
           "        %s.rgb += %s * c[lc + 1].rgb * (att * nl);\n"
           "        %s.rgb += %s * c[lc + 2].rgb * (att * pf);\n"
           "      }\n"
           "    }\n",
        spc, d0, src_rgb(src[1]), d0, src_rgb(src[2]), d1, src_rgb(src[3]));
}

/* One side's colours by the lighting unit's arithmetic (ltlib.h), the
 * operations in the order envytools has the NV10's unit do them
 * (pgraph_celsius_lt_full), with the NV2A's material sources and its back
 * side: the base, then per light the ambient, the diffuse and the specular
 * terms added one by one; a light's vector and distance come from the
 * transform unit (xflib.h: the difference of the points, its length squared,
 * the table reciprocal root; xf off, the stand-in's: plain floats, vsh.h
 * HLE_VSH_XF_FAST). HleFfhwKey.lighting 2 goes with HLE_VSH_XF. */
static void light_side_lt(Buf *b, const HleFfhwKey *k, bool back, bool xf)
{
    const uint8_t *ksrc = back ? k->bsrc : k->src;
    const uint8_t src[4] = { ksrc[0] % 3u, ksrc[1] % 3u, ksrc[2] % 3u, ksrc[3] % 3u };  /* (3: the material) */
    const char *d0 = back ? "oB0" : "oD0", *d1 = back ? "oB1" : "oD1";
    const unsigned amb = back ? HLE_FFHW_C_BACK_AMBIENT : HLE_FFHW_C_AMBIENT;
    const unsigned emi = back ? HLE_FFHW_C_BACK_EMISSION : HLE_FFHW_C_EMISSION;
    const unsigned spc = back ? HLE_FFHW_C_BACK_SPEC : HLE_FFHW_C_SPEC;
    const unsigned spc2 = back ? HLE_FFHW_C_BACK_SPEC2 : HLE_FFHW_C_SPEC2;
    static const char *const k_src[3] = { "", "ltC0", "ltC1" };
    put(b, "    { uvec3 n = %s, e0, e1 = uvec3(0u), tm;\n", back ? "ltN ^ 0x80000000u" : "ltN");
    if (src[0] == 0 || src[1] == 0) {
        put(b, "      e0 = lt_v(c[%u].rgb);\n", amb);
        const unsigned add = src[0] != 0 ? src[0] : src[1];
        if (add) {
            put(b, "      e0 = lt_vadd(e0, lt_vmul(%s, lt_v(c[%u].rgb)));\n", k_src[add], emi);
        }
    } else {
        put(b, "      e0 = lt_vadd(%s, lt_vmul(%s, lt_v(c[%u].rgb)));\n", k_src[src[0]], k_src[src[1]], emi);
    }
    for (unsigned i = 0; i < 8; i++) {
        const unsigned t = k->light[i], L = HLE_FFHW_C_LIGHTS + HLE_FFHW_LIGHT_VEC4 * i;
        if (!t) {
            continue;
        }
        const unsigned lc = back ? HLE_FFHW_C_BACK_LIGHTS + 3 * i : L + HLE_FFHW_L_AMBIENT;
        put(b, "      { /* light %u */\n"
               "        uint ca = 0x3f800000u, cd, cs; bool zero = false; uvec3 lv;\n", i);
        if (t == 1) {
            put(b, "        lv = lt_v(c[%u].xyz);\n", L + HLE_FFHW_L_DIRECTION);
        } else {
            /* the vector to the light over its length, the attenuation's
             * three terms through the unit's reciprocal, the range */
            put(b, xf ? "        vec4 VP4 = xadd(vec4(c[%u].xyz, 0.0), -ePos3);\n"
                        "        float d2 = xdp3(VP4, VP4).x;\n"
                        "        vec4 rs = xrsq4(d2);\n"
                        "        vec3 VP = xmul(VP4, rs).xyz;\n"
                        "        float d = xmul(vec4(d2), rs).x;\n"
                      : "        vec4 VP4 = vec4(c[%u].xyz, 0.0) - ePos3;\n"
                        "        float d2 = dot(VP4.xyz, VP4.xyz);\n"
                        "        vec4 rs = vec4(inversesqrt(d2));\n"
                        "        vec3 VP = (VP4 * rs).xyz;\n"
                        "        float d = d2 * rs.x;\n", L + HLE_FFHW_L_POSITION);
            put(b, "        lv = lt_v(VP);\n"
                   "        ca = lt_rcp(lt_dp(uvec3(0x3f800000u, lt_s(d), lt_s(d2)), lt_v(c[%u].xyz)));\n"
                   "        if ((lts_add(lt_s(-d), lt_s(c[%u].x)) >> 31) != 0u) zero = true;\n",
                L + HLE_FFHW_L_ATTENUATION, L + HLE_FFHW_L_RANGE);
            if (t == 3) {
                put(b, "        { vec4 sd = c[%u];\n"
                       "          float s = dot(sd.xyz, VP) + sd.w, sf = 1.0;\n"
                       "          vec3 fo = c[%u].xyz;\n"
                       "          float dn = fo.y * min(s, 1.0) + fo.z;\n"
                       "          if (s <= 0.0) sf = 0.0;\n"
                       "          else if (dn == 0.0) sf = min(s, 1.0) + fo.x > 0.0 ? " HLE_SPOT_DIV0 " : 0.0;\n"
                       "          else if (s < 1.0) {\n"
                       "            sf = s <= -fo.x ? 0.0 :\n"
                       "                 dn > 0.0 ? clamp(uintBitsToFloat(lt_mul(lts_add(lt_s(s), lt_s(fo.x)), lt_rcp(lts_add(lt_mul(lt_s(fo.y), lt_s(s)), lt_s(fo.z))))), 0.0, 1.0) : 0.0;\n"
                       "          }\n"
                       "          ca = lt_mul(ca, lt_s(sf)); }\n",
                    L + HLE_FFHW_L_SPOT, L + HLE_FFHW_L_FALLOFF);
            }
            put(b, "        if (zero) ca = 0u;\n");
        }
        put(b, "        cd = lt_dp(n, lv);\n"
               "        if ((cd >> 31) != 0u) zero = true;\n"
               "        if (zero) cd = 0u;\n"
               "        cd = lt_mul(ca, cd);\n");
        if (t == 1 && !k->local_eye) {
            /* the infinite light and viewer: n.h through (x + A) / (B x + C) */
            put(b, "        { uint s = lt_dp(n, lt_v(c[%u].xyz)), tt = lts_add(s, lt_s(c[%u].x));\n"
                   "          if ((tt >> 31) != 0u) zero = true;\n"
                   "          cs = lt_mul(tt, lt_rcp(lts_add(lt_mul(s, lt_s(c[%u].y)), lt_s(c[%u].z)))); }\n",
                L + HLE_FFHW_L_HALF, spc, spc, spc);
        } else {
            /* else the half vector left unnormalized: its length squared hd,
             * (s s + D hd) / (E s s + F hd) */
            put(b, "        { uvec3 hi = lt_vadd(ltEv, lv);\n"
                   "          uint hd = lt_dp(hi, hi), s = lt_dp(n, hi);\n"
                   "          if ((s >> 31) != 0u) zero = true;\n"
                   "          uint ss = lt_mul(s, s), tt = lts_add(ss, lt_mul(hd, lt_s(c[%u].x)));\n"
                   "          if ((tt >> 31) != 0u) zero = true;\n"
                   "          cs = lt_mul(tt, lt_rcp(lts_add(lts_add(lt_mul(ss, lt_s(c[%u].y)), 0u),\n"
                   "                                         lt_mul(hd, lt_s(c[%u].z))))); }\n",
                spc2, spc2, spc2);
        }
        put(b, "        if (zero) cs = 0u;\n"
               "        cs = lt_mul(ca, cs);\n");
        static const char *const k_term[3] = { "ca", "cd", "cs" };
        for (unsigned j = 0; j < 3; j++) {
            /* (the specular apart only with SPECULAR_ENABLE: without it the
             * separate bit does nothing: Lighting_control 0x1 NoSpec) */
            const char *dst = j == 2 && k->separate_spec && k->specular ? "e1" : "e0";
            put(b, "        tm = lt_vmul(uvec3(%s), lt_v(c[%u].rgb));\n", k_term[j], lc + j);
            if (src[1 + j]) {
                put(b, "        tm = lt_vmul(tm, %s);\n", k_src[src[1 + j]]);
            }
            put(b, "        %s = lt_vadd(%s, tm);\n", dst, dst);
        }
        put(b, "      }\n");
    }
    /* the alpha: the diffuse source's, through the unit's way out */
    if (src[2] == 0) {
        put(b, "      %s = vec4(lt_vf2b(e0), lt_f2b(lt_s(c[%u].w)));\n", d0, amb);
    } else {
        put(b, "      %s = vec4(lt_vf2b(e0), lt_f2b(floatBitsToUint(%s)));\n", d0, src[2] == 1 ? "v3.a" : "v4.a");
    }
    put(b, "      %s = vec4(lt_vf2b(e1), v4.a);\n    }\n", d1);
}

/* The lighting unit's inputs (lighting 2): the eye-space point over its w;
 * a local viewer's direction, the eye minus it, times the reciprocal root of
 * its length squared, else the direction register as it stands. xf: by the
 * transform unit's arithmetic, else in plain floats (vsh.h HLE_VSH_XF_FAST) */
static void lt_inputs(Buf *b, const HleFfhwKey *k, bool xf)
{
    put(b, xf ? "    vec4 ePos3 = vec4(xmul(tPos, xrcp4(tPos.w)).xyz, 0.0);\n"
              : "    vec4 ePos3 = vec4(tPos.xyz / tPos.w, 0.0);\n");
    put(b, "    uvec3 ltN = lt_v(tN), ltC0 = lt_v(v3.rgb), ltC1 = lt_v(v4.rgb);\n");
    if (k->local_eye) {
        put(b, xf ? "    vec4 uev = xadd(vec4(c[%u].xyz, 0.0), -ePos3);\n"
                    "    uvec3 ltEv = lt_v(xmul(uev, xrsq4(xdp3(uev, uev).x)).xyz);\n"
                  : "    vec4 uev = vec4(c[%u].xyz, 0.0) - ePos3;\n"
                    "    uvec3 ltEv = lt_v(uev.xyz * inversesqrt(dot(uev.xyz, uev.xyz)));\n",
            HLE_FFHW_C_EYE);
    } else {
        put(b, "    uvec3 ltEv = lt_v(c[%u].xyz);\n", HLE_FFHW_C_EYEDIR);
    }
}

/* the direction to the eye (lighting 1): the eye position for a local
 * viewer, else along -z (D3D's eye space looks down +z) */
static void eye_dir(Buf *b, const HleFfhwKey *k)
{
    if (k->local_eye) {
        put(b, "    vec3 eyeV = normalize(c[%u].xyz / c[%u].w - tPos.xyz / tPos.w);\n",
            HLE_FFHW_C_EYE, HLE_FFHW_C_EYE);
    } else {
        put(b, "    vec3 eyeV = vec3(0.0, 0.0, -1.0);\n");
    }
}

static void lighting(Buf *b, const HleFfhwKey *k, bool xf, bool loop)
{
    if (!k->lighting) {
        put(b, "    oD0 = v3;\n    oD1 = v4;\n");
        return;
    }
    if (k->lighting == 2) {
        /* (the viewer at infinity: the direction register as it stands,
         * zero where nothing wrote it: the half vector is then the light's) */
        lt_inputs(b, k, xf);
        light_side_lt(b, k, false, xf);
        if (k->two_side) {
            light_side_lt(b, k, true, xf);
        }
        return;
    }
    eye_dir(b, k);
    light_side(b, k, false, loop);
    if (k->two_side) {
        light_side(b, k, true, loop);
    }
}

/* the colours after lighting: the back ones, SEPARATE_SPECULAR,
 * SPECULAR_ENABLE and ALPHA_FROM_MATERIAL_SPECULAR */
static void colours(Buf *b, const HleFfhwKey *k)
{
    /* the back colours (two-sided lighting's back faces, psh.c HLE_PSH_TWO_SIDE):
     * unlit, none (nxdk_pgraph_tests' Specular_back: back faces black with
     * lighting off, whatever colours the vertices carry); lit two-sided,
     * the back's own (light_side); lit one-sided, the front ones */
    if (!k->lighting || !k->two_side) {
        put(b, k->lighting ? "    oB0 = oD0;\n    oB1 = oD1;\n" :
                             "    oB0 = vec4(0.0, 0.0, 0.0, 1.0);\n    oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n");
    }
    /* SEPARATE_SPECULAR off: the lights' specular in the diffuse colour,
     * SPECULAR_ENABLE or not (nxdk_pgraph_tests' Lighting_control NoSpec
     * pictures: the highlights on the console with it off) */
    if (!k->separate_spec && k->lighting == 1) {
        put(b, k->two_side ? "    oD0.rgb += oD1.rgb;\n    oB0.rgb += oB1.rgb;\n" : "    oD0.rgb += oD1.rgb;\n");
    }
    if (!k->specular) {
        put(b, "    oD1 = vec4(0.0, 0.0, 0.0, 1.0);\n    oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n");
    } else {
        if (!k->separate_spec) {
            put(b, k->lighting && k->two_side ? "    oD1 = v4;\n    oB1 = v4;\n" : "    oD1 = v4;\n");
        }
        if (!k->alpha_mat_spec) {
            put(b, "    oD1.a = 1.0;\n    oB1.a = 1.0;\n");
        }
    }
}

size_t hle_ffhw_glsl(const uint32_t *tokens, unsigned nslots, const HleVshKey *vk, char *buf,
                     size_t cap)
{
    if (!hle_ffhw_is(tokens, nslots)) {
        return 0;
    }
    HleFfhwKey k;
    memcpy(&k, tokens, sizeof(k));
    char body[49152];
    Buf b = { body, sizeof(body), 0, false };
    /* (HLE_VSH_XF: the matrix products by the transform
     * unit's own arithmetic, xflib.h: each row one four-input sum of cut
     * products) */
    const bool xf = hle_vsh_xf_exact(vk);
    if (!xf && k.lighting == 2) {
        /* (the stand-in, vsh.h HLE_VSH_XF_FAST: the lights in plain floats too,
         * lighting 1; the lighting unit's integer arithmetic took the driver
         * up to 1.2 s a shader, Dead or Alive 3's eight lights) */
        k.lighting = 1;
    }
    put(&b, xf ? "#define XF(v, b) vec4(xdp4((v), c[(b)]).x, xdp4((v), c[(b) + 1]).x, "
                 "xdp4((v), c[(b) + 2]).x, xdp4((v), c[(b) + 3]).x)\n"
               : "#define XF(v, b) vec4(dot((v), c[(b)]), dot((v), c[(b) + 1]), "
                 "dot((v), c[(b) + 2]), dot((v), c[(b) + 3]))\n");
    put(&b, "  {\n"
            "    vec4 pos = v0, n4 = vec4(v2.xyz, 0.0), tPos;\n"
            "    vec3 tN;\n");
    skinning(&b, &k);
    if (k.normalize) {
        /* (the unit's: times the table reciprocal root of its length squared) */
        put(&b, xf ? "    tN = xmul(vec4(tN, 0.0), xrsq4(xdp3(vec4(tN, 0.0), vec4(tN, 0.0)).x)).xyz;\n"
                   : "    tN = normalize(tN);\n");
    }
    texgen(&b, &k);
    lighting(&b, &k, xf, (vk->spec_off & HLE_VSH_XF_FAST) != 0);
    colours(&b, &k);
    if (k.fog) {
        /* the fog coordinate by SET_FOG_GEN_MODE, from the eye-space point:
         * the eye position over its w. A model view whose last row isn't
         * (0, 0, 0, 1) (a planar projected shadow's: Hunter: The Reckoning -
         * Redeemer's under its characters, w ~18) leaves it homogeneous; the
         * screen position cancels the scale, the fog distance didn't, and the
         * shadows were the fog's colour (yellow) at 18 times their distance.
         * w 1 (every other draw): the same as before. */
        if (xf && k.fog - 1 >= 1 && k.fog - 1 <= 3) {
            /* (HLE_VSH_XF: the coordinate by the transform unit: the eye-space
             * point times the reciprocal of its w, all four of it, into the
             * plane's four-input sum; the radial distance its length squared
             * times the table reciprocal root of that) */
            put(&b, "    vec4 fE4 = xmul(tPos, xrcp4(tPos.w));\n");
            if (k.fog - 1 == 1) {
                put(&b, "    { float fd2 = xdp3(fE4, fE4).x;\n"
                        "      oFog = vec4(xmul(vec4(fd2), xrsq4(fd2)).x); }\n");
            } else {
                put(&b, "    oFog = vec4(%sxdp4(fE4, c[%u]).x%s);\n", k.fog - 1 == 3 ? "abs(" : "",
                    HLE_FFHW_C_FOGPLANE, k.fog - 1 == 3 ? ")" : "");
            }
        } else if (k.fog - 1 == 1 || k.fog - 1 == 2 || k.fog - 1 == 3) {
            put(&b, "    vec3 fE = tPos.w != 0.0 && tPos.w != 1.0 ? tPos.xyz / tPos.w : tPos.xyz;\n");
        }
        switch (xf && k.fog - 1 >= 1 && k.fog - 1 <= 3 ? -1 : k.fog - 1) {
        case -1: break;
        case 0: put(&b, "    oFog = vec4(clamp(v4.a, 0.0, 1.0));\n"); break;
        case 1: put(&b, "    oFog = vec4(length(fE));\n"); break;
        case 2: put(&b, "    oFog = vec4(dot(c[%u].xyz, fE) + c[%u].w);\n",
                    HLE_FFHW_C_FOGPLANE, HLE_FFHW_C_FOGPLANE); break;
        case 3: put(&b, "    oFog = vec4(abs(dot(c[%u].xyz, fE) + c[%u].w));\n",
                    HLE_FFHW_C_FOGPLANE, HLE_FFHW_C_FOGPLANE); break;
        default: put(&b, "    oFog = vec4(v5.x);\n"); break;
        }
    }
    if (k.points && xf) {
        /* (the transform unit's own reciprocal root, a hair under, and its own
         * adds and products after it: its 36 / sqrt(4) + 4 is 21.999998, 21 pixels
         * on the console where 22 would be 22, and (1 / sqrt(4) + 0.5) 14 + 16 is
         * 29.999998, 29 pixels across, where the GPU's floats round it to 30, 31
         * across: nxdk_pgraph_tests' Point_params) */
        const unsigned p = HLE_FFHW_C_POINT;
        put(&b, "    { float de = length(tPos.xyz);\n"
                "      float pmin = min(c[%u].w, 63.875), pmax = min(c[%u].w + pmin, 63.875);\n"
                "      vec4 pr = xrsq4(xdp3(vec4(c[%u].xyz, 0.0), vec4(1.0, de, de * de, 0.0)).x);\n"
                "      oPts.x = clamp(xadd(xmul(xadd(pr, vec4(c[%u].z)), vec4(c[%u].w)), vec4(c[%u].w)).x, pmin, pmax); }\n",
            p + 1, p, p, p + 1, p, p + 1);
    } else if (k.points) {
        const unsigned p = HLE_FFHW_C_POINT;
        put(&b, "    { float de = length(tPos.xyz);\n"
                "      float pmin = min(c[%u].w, 63.875), pmax = min(c[%u].w + pmin, 63.875);\n"
                "      oPts.x = clamp((inversesqrt(c[%u].x + c[%u].y * de + c[%u].z * de * de) + "
                "c[%u].z) * c[%u].w + c[%u].w, pmin, pmax); }\n",
            p + 1, p, p, p, p, p + 1, p, p + 1);
    } else {
        put(&b, "    oPts.x = max(c[%u].z, 1.0);\n", HLE_FFHW_C_PARAMS);
    }
    /* screen space: the composite (viewport scale included) over w, plus the
     * viewport offset; w kept for the clip space (as RCC, its reciprocal's
     * magnitude clamped) */
    put(&b, "    vec4 sp = XF(%s, %u);\n"
            "    float aw = clamp(abs(sp.w), 5.421011e-20, 1.8446744e19);\n"
            "    float sw = sp.w < 0.0 ? -aw : aw;\n",
        k.skin ? "tPos" : "pos", HLE_FFHW_C_COMPOSITE);
    /* (HLE_VSH_XF: times the unit's reciprocal, RCC, the offset by its adder) */
    put(&b, xf ? "    oPos = vec4(xadd(xmul(vec4(sp.xyz, 0.0), xrcc4(sp.w)), vec4(c[%u].xyz, 0.0)).xyz, sw);\n"
               : "    oPos = vec4(sp.xyz / sw + c[%u].xyz, sw);\n", HLE_FFHW_C_VPOFF);
    /* (and the clip-space point itself, the screen position times w with no
     * divide: the rasterizer's where w is out at its ends, vsh.c tail; the
     * console's coverage there, nxdk_pgraph_tests' W_param ff_w_zero_inf,
     * pgraph/tools/homog3.py) */
    put(&b, "    ffHP = vec4(sp.xyz + c[%u].xyz * sw, sw);\n"
            "    ffZ = sp.w == 0.0;\n", HLE_FFHW_C_VPOFF);
    put(&b, "  }\n"
            "#undef XF\n");
    if (b.overflow) {
        return 0;
    }
    return hle_vsh_glsl_wrap(vk, k.lighting == 2, body, k.fog != 0, buf, cap);
}

static bool idch(char ch)
{
    return ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
}

/* s[0..n) into b with whole identifiers renamed by map (from, to pairs), and a
 * back specular passed through ("oB1 = v4;") the program's own back one */
static void put_ids(Buf *b, const char *s, size_t n, const char *const (*map)[2], unsigned nmap)
{
    for (size_t i = 0; i < n && !b->overflow; ) {
        if (i + 9 <= n && !strncmp(s + i, "oB1 = v4;", 9)) {
            put(b, "oB1 = pB1;");
            i += 9;
            continue;
        }
        if (idch(s[i]) && (i == 0 || !idch(s[i - 1]))) {
            size_t j = i;
            while (j < n && idch(s[j])) {
                j++;
            }
            const char *to = NULL;
            for (unsigned m = 0; m < nmap && !to; m++) {
                if (strlen(map[m][0]) == j - i && !strncmp(s + i, map[m][0], j - i)) {
                    to = map[m][1];
                }
            }
            if (to) {
                put(b, "%s", to);
            } else {
                put(b, "%.*s", (int)(j - i), s + i);
            }
            i = j;
            continue;
        }
        put(b, "%c", s[i]);
        i++;
    }
}

size_t hle_ffhw_lt_glsl(const uint32_t *tokens, bool xf, bool loop, char *buf, size_t cap)
{
    HleFfhwKey k;
    memcpy(&k, tokens, sizeof(k));
    if (k.magic != HLE_FFHW_LT_MAGIC || k.lighting != 2) {
        return 0;
    }
    char *t = malloc(3 * 16384);
    if (!t) {
        return 0;
    }
    /* the front side (and the unit's inputs), the back side, the colours after */
    Buf f = { t, 16384, 0, false }, bk = { t + 16384, 16384, 0, false }, tl = { t + 32768, 16384, 0, false };
    put(&f, xf ? "#define XF(v, b) vec4(xdp4((v), c[(b)]).x, xdp4((v), c[(b) + 1]).x, "
                 "xdp4((v), c[(b) + 2]).x, xdp4((v), c[(b) + 3]).x)\n"
               : "#define XF(v, b) vec4(dot((v), c[(b)]), dot((v), c[(b) + 1]), "
                 "dot((v), c[(b) + 2]), dot((v), c[(b) + 3]))\n");
    put(&f, "  {\n"
            "    vec4 pD0 = oD0, pD1 = oD1, pB0 = oB0, pB1 = oB1;\n"
            "    vec4 pos = v0, n4 = vec4(v2.xyz, 0.0), tPos;\n"
            "    vec3 tN;\n");
    skinning(&f, &k);
    if (k.normalize) {
        put(&f, xf ? "    tN = xmul(vec4(tN, 0.0), xrsq4(xdp3(vec4(tN, 0.0), vec4(tN, 0.0)).x)).xyz;\n"
                   : "    tN = normalize(tN);\n");
    }
    if (xf) {
        lt_inputs(&f, &k, xf);
        light_side_lt(&f, &k, false, xf);
        if (k.two_side) {
            put(&bk, "    uvec3 ltC0b = lt_v(pB0.rgb), ltC1b = lt_v(pB1.rgb);\n");
            light_side_lt(&bk, &k, true, xf);
        }
    } else {
        /* (the stand-in: plain floats, as the fixed function's lighting 1) */
        k.lighting = 1;
        eye_dir(&f, &k);
        light_side(&f, &k, false, loop);
        if (k.two_side) {
            light_side(&bk, &k, true, loop);
        }
    }
    colours(&tl, &k);
    put(&tl, "  }\n#undef XF\n");
    /* (the constants: cf[] beside the program's own c[]. The vertex colours the
     * unit takes and passes on are the program's outputs, each side its own:
     * oD0 / oD1 for v3 / v4, oB0 / oB1 on the back, nxdk_pgraph_tests'
     * Specular_back ControlFlags_VS without SEPARATE_SPECULAR, whose back faces
     * show the program's back specular) */
    static const char *const front[][2] = { { "c", "cf" }, { "v3", "pD0" }, { "v4", "pD1" } };
    static const char *const back[][2] = { { "c", "cf" }, { "v3", "pB0" }, { "v4", "pB1" },
                                           { "ltC0", "ltC0b" }, { "ltC1", "ltC1b" } };
    Buf o = { buf, cap, 0, false };
    const bool ok = !f.overflow && !bk.overflow && !tl.overflow;
    if (ok) {
        put_ids(&o, f.p, f.len, front, 3);
        put_ids(&o, bk.p, bk.len, back, 5);
        put_ids(&o, tl.p, tl.len, front, 3);
    }
    free(t);
    return ok && !o.overflow ? o.len : 0;
}
