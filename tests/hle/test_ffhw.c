#include "hw/xbox/hle/shader/ffhw.h"
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

int main(void)
{
    HT_RUN(test_unlit);
    HT_RUN(test_lit_skinned);
    HT_RUN(test_not_a_key);
    HT_RUN(test_light_curves);
    HT_DONE();
}
