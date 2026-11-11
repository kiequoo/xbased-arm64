/*
 * hakuX HLE GPU — the NV2A fixed-function vertex stage as GLSL (pure). See
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
static void light_side(Buf *b, const HleFfhwKey *k, bool back)
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
                       "      if (s <= 0.0) att = 0.0;\n"
                       "      else if (s < 1.0) {\n"
                       "        vec3 fo = c[%u].xyz;\n"
                       "        float dn = fo.y * s + fo.z;\n"
                       "        att *= s <= -fo.x ? 0.0 : dn == 0.0 ? 1.0 :\n"
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

static void lighting(Buf *b, const HleFfhwKey *k)
{
    if (!k->lighting) {
        put(b, "    oD0 = v3;\n    oD1 = v4;\n");
        return;
    }
    /* the direction to the eye: the eye position for a local viewer, else
     * along -z (D3D's eye space looks down +z) */
    if (k->local_eye) {
        put(b, "    vec3 eyeV = normalize(c[%u].xyz / c[%u].w - tPos.xyz / tPos.w);\n",
            HLE_FFHW_C_EYE, HLE_FFHW_C_EYE);
    } else {
        put(b, "    vec3 eyeV = vec3(0.0, 0.0, -1.0);\n");
    }
    light_side(b, k, false);
    if (k->two_side) {
        light_side(b, k, true);
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
    char body[24576];
    Buf b = { body, sizeof(body), 0, false };
    put(&b, "#define XF(v, b) vec4(dot((v), c[(b)]), dot((v), c[(b) + 1]), "
            "dot((v), c[(b) + 2]), dot((v), c[(b) + 3]))\n"
            "  {\n"
            "    vec4 pos = v0, n4 = vec4(v2.xyz, 0.0), tPos;\n"
            "    vec3 tN;\n");
    skinning(&b, &k);
    if (k.normalize) {
        put(&b, "    tN = normalize(tN);\n");
    }
    texgen(&b, &k);
    lighting(&b, &k);
    /* the back colours (two-sided lighting's back faces, psh.c HLE_PSH_TWO_SIDE):
     * unlit, none (nxdk_pgraph_tests' Specular_back: back faces black with
     * lighting off, whatever colours the vertices carry); lit two-sided,
     * the back's own (light_side); lit one-sided, the front ones */
    if (!k.lighting || !k.two_side) {
        put(&b, k.lighting ? "    oB0 = oD0;\n    oB1 = oD1;\n" :
                             "    oB0 = vec4(0.0, 0.0, 0.0, 1.0);\n    oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n");
    }
    /* SEPARATE_SPECULAR off: the lights' specular in the diffuse colour,
     * SPECULAR_ENABLE or not (nxdk_pgraph_tests' Lighting_control NoSpec
     * pictures: the highlights on the console with it off) */
    if (!k.separate_spec && k.lighting) {
        put(&b, k.two_side ? "    oD0.rgb += oD1.rgb;\n    oB0.rgb += oB1.rgb;\n" : "    oD0.rgb += oD1.rgb;\n");
    }
    if (!k.specular) {
        put(&b, "    oD1 = vec4(0.0, 0.0, 0.0, 1.0);\n    oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n");
    } else {
        if (!k.separate_spec) {
            put(&b, k.lighting && k.two_side ? "    oD1 = v4;\n    oB1 = v4;\n" : "    oD1 = v4;\n");
        }
        if (!k.alpha_mat_spec) {
            put(&b, "    oD1.a = 1.0;\n    oB1.a = 1.0;\n");
        }
    }
    if (k.fog) {
        /* the fog coordinate by SET_FOG_GEN_MODE, from the eye-space point:
         * the eye position over its w. A model view whose last row isn't
         * (0, 0, 0, 1) (a planar projected shadow's: Hunter: The Reckoning -
         * Redeemer's under its characters, w ~18) leaves it homogeneous; the
         * screen position cancels the scale, the fog distance didn't, and the
         * shadows were the fog's colour (yellow) at 18 times their distance.
         * w 1 (every other draw): the same as before. */
        if (k.fog - 1 == 1 || k.fog - 1 == 2 || k.fog - 1 == 3) {
            put(&b, "    vec3 fE = tPos.w != 0.0 && tPos.w != 1.0 ? tPos.xyz / tPos.w : tPos.xyz;\n");
        }
        switch (k.fog - 1) {
        case 0: put(&b, "    oFog = vec4(clamp(v4.a, 0.0, 1.0));\n"); break;
        case 1: put(&b, "    oFog = vec4(length(fE));\n"); break;
        case 2: put(&b, "    oFog = vec4(dot(c[%u].xyz, fE) + c[%u].w);\n",
                    HLE_FFHW_C_FOGPLANE, HLE_FFHW_C_FOGPLANE); break;
        case 3: put(&b, "    oFog = vec4(abs(dot(c[%u].xyz, fE) + c[%u].w));\n",
                    HLE_FFHW_C_FOGPLANE, HLE_FFHW_C_FOGPLANE); break;
        default: put(&b, "    oFog = vec4(v5.x);\n"); break;
        }
    }
    if (k.points) {
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
            "    float sw = sp.w < 0.0 ? -aw : aw;\n"
            "    oPos = vec4(sp.xyz / sw + c[%u].xyz, sw);\n"
            "  }\n"
            "#undef XF\n",
        k.skin ? "tPos" : "pos", HLE_FFHW_C_COMPOSITE, HLE_FFHW_C_VPOFF);
    if (b.overflow) {
        return 0;
    }
    return hle_vsh_glsl_wrap(vk, body, k.fog != 0, buf, cap);
}
