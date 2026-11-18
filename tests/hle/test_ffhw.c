#include "hw/xbox/hle/shader/ffhw.h"
#include "hw/xbox/hle/shader/vsh.h"
#include "hle_test.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char buf[65536];

static void dump(const char *name)
{
    const char *dir = getenv("HLE_TEST_DUMP");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.vert", dir, name);
        FILE *f = fopen(path, "wb");
        if (f) {
            fputs(buf, f);
            fclose(f);
        }
    }
}

static size_t gen(const HleFfhwKey *k, uint32_t in_mask)
{
    uint32_t tok[HLE_FFHW_SLOTS * 4];
    memcpy(tok, k, sizeof(*k));
    HleVshKey vk = { .in_mask = in_mask };
    return hle_ffhw_glsl(tok, HLE_FFHW_SLOTS, &vk, buf, sizeof(buf));
}

static void test_unlit(void)
{
    HleFfhwKey k = { .magic = HLE_FFHW_MAGIC, .specular = 1, .separate_spec = 1,
                     .alpha_mat_spec = 1 };
    size_t n = gen(&k, 1u << 0 | 1u << 3 | 1u << 9);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("ffhw_unlit");
    /* object space through the composite, over w, plus the viewport offset */
    HT_CHECK(strstr(buf, "vec4 sp = XF(pos, 0);") != NULL);
    HT_CHECK(strstr(buf, "oPos = vec4(sp.xyz / sw + c[68].xyz, sw);") != NULL);
    HT_CHECK(strstr(buf, "oD0 = v3;") != NULL);
    HT_CHECK(strstr(buf, "oT0 = v9;") != NULL);
    HT_CHECK(strstr(buf, "vec4 v1 = vcur[1];") != NULL);   /* the program head's */
    HT_CHECK(strstr(buf, "vFog = 1.0;") != NULL);          /* no fog written */
    HT_CHECK(strstr(buf, "gl_Position") != NULL);
}

static void test_lit_skinned(void)
{
    HleFfhwKey k = { .magic = HLE_FFHW_MAGIC, .skin = 3, .lighting = 1, .normalize = 1,
                     .specular = 1, .fog = 3, .src = { 0, 1, 1, 0 },
                     .light = { 1, 2, 3 } };
    k.texgen[1][0] = HLE_FFHW_TG_SPHERE;
    k.texgen[1][1] = HLE_FFHW_TG_SPHERE;
    k.texgen[2][2] = HLE_FFHW_TG_REFLECTION;
    k.texmat = 1u << 1;
    size_t n = gen(&k, 0x1E1F);
    HT_CHECK(n > 0);
    dump("ffhw_lit");
    /* 3G: two weights given, the third generated */
    HT_CHECK(strstr(buf, "wi = v1.x;\n    wn -= wi;") != NULL);
    HT_CHECK(strstr(buf, "wi = v1.y;\n    wn -= wi;") != NULL);
    HT_CHECK(strstr(buf, "wi = wn;\n    tPos += XF(pos, 12) * wi;") != NULL);
    HT_CHECK(strstr(buf, "tN += XF(n4, 28).xyz * wi;") != NULL);
    HT_CHECK(strstr(buf, "tN = normalize(tN);") != NULL);
    HT_CHECK(strstr(buf, "vec4 sp = XF(tPos, 0);") != NULL);   /* skinned: eye space */
    /* ambient from the diffuse register, scaled by the emission register */
    HT_CHECK(strstr(buf, "oD0 = vec4(c[71].rgb, v3.a);\n    oD0.rgb += v3.rgb * c[72].rgb;") != NULL);
    HT_CHECK(strstr(buf, "/* light 0 */") && strstr(buf, "/* light 1 */") &&
             strstr(buf, "/* light 2 */") && !strstr(buf, "/* light 3 */"));
    HT_CHECK(strstr(buf, "nh = max(dot(tN, c[80].xyz), 0.0);") != NULL);  /* infinite: half */
    HT_CHECK(strstr(buf, "lit = d <= c[89].x;") != NULL);               /* local: range */
    HT_CHECK(strstr(buf, "vec4 sd = c[103];") != NULL);                 /* spot */
    HT_CHECK(strstr(buf, "oD0.rgb += v3.rgb * c[77].rgb * (att * nl);") != NULL);
    HT_CHECK(strstr(buf, "oD0.rgb += oD1.rgb;") != NULL);   /* not separate */
    HT_CHECK(strstr(buf, "oT1.x = r.x / (2.0 * length(r + vec3(0.0, 0.0, 1.0))) + 0.5;"));
    HT_CHECK(strstr(buf, "oT2.z = reflect(normalize(tPos.xyz), tN).z;") != NULL);
    HT_CHECK(strstr(buf, "oT1 = XF(oT1, 40);") != NULL);
    /* the fog distance from the eye position over its w (a homogeneous model view) */
    HT_CHECK(strstr(buf, "oFog = vec4(dot(c[69].xyz, fE) + c[69].w);") != NULL);
    HT_CHECK(strstr(buf, "vFog = oFog.x;") != NULL);
}

static void test_not_a_key(void)
{
    uint32_t tok[12] = { 0 };
    HleVshKey vk = { 0 };
    HT_CHECK(hle_ffhw_glsl(tok, 3, &vk, buf, sizeof(buf)) == 0);
    HT_CHECK(!hle_ffhw_is(tok, 3));
    tok[0] = HLE_FFHW_MAGIC;
    HT_CHECK(!hle_ffhw_is(tok, 2));
    HT_CHECK(hle_ffhw_is(tok, 3));
}

/* The specular factor and the spot's falloff by the NV2A's ratio of two lines; the texgen
 * view model */
static void test_light_curves(void)
{
    HleFfhwKey k;
    memset(&k, 0, sizeof(k));
    k.magic = HLE_FFHW_MAGIC;
    k.lighting = 1;
    k.specular = 1;
    k.light[0] = 3;                                 /* a spot */
    k.light[1] = 1;                                 /* an infinite one */
    k.texgen[0][0] = k.texgen[0][1] = HLE_FFHW_TG_SPHERE;
    k.texgen[1][0] = k.texgen[1][1] = k.texgen[1][2] = HLE_FFHW_TG_REFLECTION;
    uint32_t tok[12];
    memcpy(tok, &k, sizeof(k));
    HleVshKey vk = { .in_mask = 0x5 };
    HT_CHECK(hle_ffhw_glsl(tok, 3, &vk, buf, sizeof(buf)) > 0);
    dump("ffhw_curves");
    /* (n.h + L) / (M n.h + N) by SET_SPECULAR_PARAMS, nothing below -L or without n.l */
    HT_CHECK(strstr(buf, "vec3 sc = c[156].xyz;") != NULL);
    HT_CHECK(strstr(buf, "float dn = sc.y * nh + sc.z;") != NULL);
    HT_CHECK(strstr(buf, "float pf = nl <= 0.0 || nh <= -sc.x ? 0.0 : dn == 0.0 ? 10000.0 :") != NULL);
    HT_CHECK(strstr(buf, "dn > 0.0 ? clamp((nh + sc.x) / dn, 0.0, 10000.0) : 0.0;") != NULL);
    HT_CHECK(strstr(buf, "pow(") == NULL);
    /* the spot: (s + L) / (M s + N) by its falloff between the cones (light 0: falloff at
     * c[76 + 6], direction at c[76 + 7]) */
    HT_CHECK(strstr(buf, "vec4 sd = c[83];") != NULL);
    HT_CHECK(strstr(buf, "float s = dot(sd.xyz, VP) + sd.w;") != NULL);
    HT_CHECK(strstr(buf, "if (s <= 0.0) att = 0.0;") != NULL);
    HT_CHECK(strstr(buf, "vec3 fo = c[82].xyz;") != NULL);
    HT_CHECK(strstr(buf, "dn > 0.0 ? clamp((s + fo.x) / dn, 0.0, 1.0) : 0.0;") != NULL);
    /* reflections about the direction from the eye to the vertex (the local viewer) */
    HT_CHECK(strstr(buf, "vec3 r = reflect(normalize(tPos.xyz), tN);") != NULL);
    HT_CHECK(strstr(buf, "oT1.x = reflect(normalize(tPos.xyz), tN).x;") != NULL);
    /* ... or straight ahead (the infinite viewer) */
    k.texgen_inf = 1;
    memcpy(tok, &k, sizeof(k));
    HT_CHECK(hle_ffhw_glsl(tok, 3, &vk, buf, sizeof(buf)) > 0);
    dump("ffhw_infinite");
    HT_CHECK(strstr(buf, "vec3 r = reflect(vec3(0.0, 0.0, 1.0), tN);") != NULL);
    HT_CHECK(strstr(buf, "oT1.z = reflect(vec3(0.0, 0.0, 1.0), tN).z;") != NULL);
    HT_CHECK(strstr(buf, "reflect(normalize(tPos.xyz)") == NULL);
}

/* HLE_VSH_XF: every product by the transform unit's arithmetic; its stand-in
 * (HLE_VSH_XF_FAST): plain floats, the unit's library left out */
static void test_xf_fast(void)
{
    HleFfhwKey k = { .magic = HLE_FFHW_MAGIC, .skin = 3, .lighting = 2, .normalize = 1,
                     .local_eye = 1, .src = { 0, 1, 1, 0 }, .light = { 1 } };
    k.texmat = 1u << 1;
    uint32_t tok[HLE_FFHW_SLOTS * 4];
    memcpy(tok, &k, sizeof(k));
    HleVshKey vk = { .in_mask = 0x1E1F, .spec_off = HLE_VSH_XF };
    HT_CHECK(hle_ffhw_glsl(tok, HLE_FFHW_SLOTS, &vk, buf, sizeof(buf)) > 0);
    dump("ffhw_xf");
    HT_CHECK(strstr(buf, "#define XF(v, b) vec4(xdp4(") != NULL);
    HT_CHECK(strstr(buf, "tN = xmul(vec4(tN, 0.0), xrsq4(") != NULL);
    HT_CHECK(strstr(buf, "vec4 ePos3 = vec4(xmul(tPos, xrcp4(tPos.w)).xyz, 0.0);") != NULL);
    HT_CHECK(strstr(buf, "oPos = vec4(xadd(xmul(vec4(sp.xyz, 0.0), xrcc4(sp.w))") != NULL);
    HT_CHECK(strstr(buf, "uint xfm(uint a, uint b)") != NULL);
    HT_CHECK(strstr(buf, "layout(location = 12) out vec4 vTexP0;") != NULL);
    vk.spec_off = HLE_VSH_XF | HLE_VSH_XF_FAST;
    HT_CHECK(hle_ffhw_glsl(tok, HLE_FFHW_SLOTS, &vk, buf, sizeof(buf)) > 0);
    dump("ffhw_xf_fast");
    HT_CHECK(strstr(buf, "#define XF(v, b) vec4(dot(") != NULL);
    HT_CHECK(strstr(buf, "tN = normalize(tN);") != NULL);
    HT_CHECK(strstr(buf, "vec3 eyeV = normalize(c[") != NULL);    /* lighting 1's */
    HT_CHECK(strstr(buf, "oPos = vec4(sp.xyz / sw + c[") != NULL);
    HT_CHECK(strstr(buf, "lt_v(") == NULL);                       /* no lighting unit */
    HT_CHECK(strstr(buf, "xmul(") == NULL && strstr(buf, "xdp4(") == NULL);
    HT_CHECK(strstr(buf, "uint xfm(uint a, uint b)") == NULL);
    /* what the pixel stage takes from the exact form, written all the same */
    HT_CHECK(strstr(buf, "layout(location = 12) out vec4 vTexP0;") != NULL);
    HT_CHECK(strstr(buf, "vZs = oPos.z;") != NULL);
    /* the lights one loop, not each written out (ffhw.c light_loop: its body built once) */
    HT_CHECK(strstr(buf, "#extension GL_EXT_control_flow_attributes : enable") != NULL);
    HT_CHECK(strstr(buf, "[[dont_unroll]] for (int li = 0; li < 8; li++) {") != NULL);
    HT_CHECK(strstr(buf, "uint t = (0x1u >> uint(2 * li)) & 3u;") != NULL);
    HT_CHECK(strstr(buf, "/* light 0 */") == NULL);
    /* the colours' no-number rule left to the exact form */
    HT_CHECK(strstr(buf, "NVNAN1(") == NULL);
    /* points' rules: in a shader that may draw them, not in one that draws none */
    HT_CHECK(strstr(buf, "ptS = floor(ptS * 8.0) / 8.0;") != NULL);
    vk.spec_off |= HLE_VSH_NO_POINTS;
    HT_CHECK(hle_ffhw_glsl(tok, HLE_FFHW_SLOTS, &vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "ptS = floor(ptS * 8.0) / 8.0;") == NULL);
    HT_CHECK(strstr(buf, "bool ptCull = false;") != NULL);
    HT_CHECK(strstr(buf, "if (ptCull) gl_Position") != NULL);
    /* the fog value by the lighting unit's arithmetic in the exact form, plain floats in the stand-in */
    vk.fog_table = HLE_VSH_FOG_NV;
    HT_CHECK(hle_ffhw_glsl(tok, HLE_FFHW_SLOTS, &vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "float fv = pc.fog.x + pc.fog.y * fd;") != NULL);
    HT_CHECK(strstr(buf, "lt_add3(") == NULL);
    vk.spec_off = HLE_VSH_XF;
    HT_CHECK(hle_ffhw_glsl(tok, HLE_FFHW_SLOTS, &vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "uint fv = lt_add3(lt_s(pc.fog.x), lt_mul(lt_s(pc.fog.y), lt_s(fd)), 0u);") != NULL);
}

/* A transform-unit operation (xflib.h) the stand-in's text still calls: it
 * has no library, so the shader would not build and its draw would wait for
 * the exact form (Dead or Alive 3's and Fable's local lights did) */
static const char *xf_call(const char *t)
{
    static const char *const k_ops[] = { "xadd(", "xmul(", "xdp3(", "xdp4(", "xdph(", "xdst(",
                                         "xrcp4(", "xrcc4(", "xrsq4(" };
    for (unsigned i = 0; i < sizeof(k_ops) / sizeof(k_ops[0]); i++) {
        const char *q = strstr(t, k_ops[i]);
        if (q) {
            return q;
        }
    }
    return NULL;
}

/* Every lighting, fog, skinning, texgen and point mode, fixed function and a
 * program under SET_LIGHTING_ENABLE: the stand-in calls nothing of the
 * transform unit, nor of the lighting unit (ltlib.h: its integer arithmetic
 * is the other slow build) */
static void test_xf_fast_sweep(void)
{
    static const uint8_t k_lights[][8] = { { 1 }, { 2 }, { 3 }, { 1, 2, 3 }, { 3, 3, 2, 1, 2, 3, 1, 2 } };
    unsigned n = 0, bad = 0;
    for (unsigned li = 0; li < sizeof(k_lights) / sizeof(k_lights[0]); li++) {
        for (unsigned v = 0; v < 64; v++) {
            HleFfhwKey k;
            memset(&k, 0, sizeof(k));
            k.magic = HLE_FFHW_MAGIC;
            k.lighting = 2;
            memcpy(k.light, k_lights[li], 8);
            k.two_side = v & 1;
            k.local_eye = (v >> 1) & 1;
            k.normalize = (v >> 2) & 1;
            k.skin = (uint8_t)((v >> 3) & 1 ? 3 : 0);
            k.fog = (uint8_t)((v >> 4) % 4 + 1);
            k.points = (v >> 5) & 1;
            k.specular = 1;
            k.src[1] = 1;
            k.texgen[0][0] = HLE_FFHW_TG_SPHERE;
            k.texgen[0][1] = HLE_FFHW_TG_REFLECTION;
            k.texgen[1][0] = HLE_FFHW_TG_EYE;
            k.texgen[1][2] = HLE_FFHW_TG_NORMAL;
            k.texmat = 3;
            uint32_t tok[HLE_FFHW_SLOTS * 4];
            memcpy(tok, &k, sizeof(k));
            HleVshKey vk = { .in_mask = 0x1E1F, .spec_off = HLE_VSH_XF | HLE_VSH_XF_FAST };
            size_t len = hle_ffhw_glsl(tok, HLE_FFHW_SLOTS, &vk, buf, sizeof(buf));
            const char *q = len ? xf_call(buf) : NULL;
            q = q ? q : len ? strstr(buf, "lt_") : NULL;
            n++;
            char nm[48];
            snprintf(nm, sizeof(nm), "sweep_ff_%u_%u", li, v);
            dump(nm);
            if (!len || q) {
                if (bad++ < 3) {
                    printf("  fixed function, lights %u, variant %u: %s\n", li, v,
                           !len ? "no text" : q);
                }
            }
            /* the same key after a program's final slot (vsh.c, ffhw.c hle_ffhw_lt_glsl) */
            static const uint32_t k_prog[] = { 0x00000000, 0x0087601B, 0xC400286C, 0x3070E801 };
            uint32_t pt[4 + HLE_FFHW_SLOTS * 4];
            memcpy(pt, k_prog, sizeof(k_prog));
            k.magic = HLE_FFHW_LT_MAGIC;
            memcpy(pt + 4, &k, sizeof(k));
            len = hle_vsh_glsl(pt, 1 + HLE_FFHW_SLOTS, &vk, buf, sizeof(buf));
            q = len ? xf_call(buf) : NULL;
            q = q ? q : len ? strstr(buf, "lt_") : NULL;
            n++;
            snprintf(nm, sizeof(nm), "sweep_prog_%u_%u", li, v);
            dump(nm);
            if (!len || q) {
                if (bad++ < 3) {
                    printf("  program, lights %u, variant %u: %s\n", li, v, !len ? "no text" : q);
                }
            }
        }
    }
    printf("  %u stand-ins, %u calling the unit or empty\n", n, bad);
    HT_CHECK(bad == 0);
}

int main(void)
{
    HT_RUN(test_unlit);
    HT_RUN(test_lit_skinned);
    HT_RUN(test_not_a_key);
    HT_RUN(test_light_curves);
    HT_RUN(test_xf_fast);
    HT_RUN(test_xf_fast_sweep);
    HT_DONE();
}
