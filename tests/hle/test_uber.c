#include "hw/xbox/hle/shader/uber.h"
#include "hw/xbox/hle/shader/ff.h"
#include "hw/xbox/hle/shader/ffhw.h"
#include "hle_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char buf[131072];

static void dump(const char *name)
{
    const char *dir = getenv("HLE_TEST_DUMP");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        FILE *f = fopen(path, "wb");
        if (f) {
            fputs(buf, f);
            fclose(f);
        }
    }
}

/* test_vsh.c's program (the nv2a_vsh_cpu disassembler tests' instructions) */
static const uint32_t k_prog[][4] = {
    { 0x00000000, 0x0420061B, 0x083613FC, 0x5011F818 }, /* MOV oD0, v3 + RCP R1.w, R1.w */
    { 0x00000000, 0x00EC801B, 0x0836186C, 0x20702800 }, /* DP4 oPos.z, v0, c[100] */
    { 0x00000000, 0x0047401A, 0xC434186C, 0x2070E800 }, /* MUL oPos.xyz, R12.xyz, c[58].xyz */
    { 0x00000000, 0x0400001B, 0x083613FC, 0x2070F82C }, /* RCP oFog, v0.w */
    { 0x00000000, 0x01A00000, 0x0436106C, 0x20700FF8 }, /* ARL A0, R0.x */
    { 0x00000000, 0x006F20BF, 0x9C001456, 0x7C000002 }, /* ADD R0.xy, c[A0+121].zw, -c[A0+121].xy */
    { 0x00000000, 0x0240081B, 0x1436186C, 0x2F20F824 }, /* MUL R2, R1, c[0] + MOV oD1, v4 */
    { 0x00000000, 0x0087601B, 0xC400286C, 0x3070E801 }, /* MAD oPos.xyz, R12, R1.x, c[59] (final) */
};
#define NSLOTS (sizeof(k_prog) / sizeof(k_prog[0]))

static uint32_t cfg[HLE_UBER_WORDS];

static void test_text(void)
{
    size_t n = hle_uber_vs_glsl(buf, sizeof(buf), 2);
    HT_CHECK(n > 0 && n == strlen(buf));
    HT_CHECK(strstr(buf, "layout(set = 2, binding = 0, std140) uniform UberCfg { uvec4 u[") != NULL);
    HT_CHECK(strstr(buf, "int(ix % 192u)") != NULL && strstr(buf, "vFog = fogw ? oFog.x") != NULL);
    dump("uber.vert");
    static const unsigned k_depth[2] = { 0, 3 };
    static const char *const k_names[2][2] = { { "uber0b.frag", "uber0.frag" }, { "uber3b.frag", "uber3.frag" } };
    for (unsigned d = 0; d < 2; d++) {
        for (unsigned f = 0; f < 2; f++) {
            n = hle_uber_fs_glsl(buf, sizeof(buf), 3, k_depth[d], f != 0);
            HT_CHECK(n > 0 && n == strlen(buf));
            HT_CHECK((strstr(buf, "gl_FragDepth") != NULL) == (k_depth[d] != 0));
            HT_CHECK(strstr(buf, f ? "#define UB_FULL 1" : "#define UB_FULL 0") != NULL);
            HT_CHECK(strstr(buf, "/ 127.0") != NULL);       /* psh.c's signed bytes */
            dump(k_names[d][f]);
        }
    }
    HT_EQ_INT(hle_uber_fs_glsl(buf, 64, 3, 0, true), 0);
}

static void test_ff(void)
{
    static uint32_t tok[4 * HLE_FFHW_SLOTS];
    HleFfhwKey k;
    memset(&k, 0, sizeof(k));
    k.magic = HLE_FFHW_MAGIC;
    k.skin = 3;
    k.light[2] = 3;
    k.points = 1;
    memcpy(tok, &k, sizeof(k));
    memset(cfg, 0, sizeof(cfg));
    HT_CHECK(hle_uber_ff(tok, HLE_FFHW_SLOTS, cfg));
    /* the bytes as kb() reads them: byte i of word 64 + i / 4 */
    HT_EQ_U32(cfg[HLE_UBER_STEPS_AT], HLE_FFHW_MAGIC);
    HT_EQ_U32(cfg[HLE_UBER_STEPS_AT + 1] & 0xFF, 3);                  /* skin: byte 4 */
    HT_EQ_U32((cfg[HLE_UBER_STEPS_AT + 4] >> 16) & 0xFF, 3);          /* light[2]: byte 18 */
    HT_EQ_U32((cfg[HLE_UBER_STEPS_AT + 10] >> 8) & 0xFF, 1);          /* points: byte 41 */
    HleVshKey vk = { .in_mask = 1 };
    HT_CHECK(hle_uber_ff_ok(&vk, tok, HLE_FFHW_SLOTS));
    HT_CHECK(!hle_uber_ff_ok(&vk, &k_prog[0][0], NSLOTS));
    HT_CHECK(!hle_uber_ff(&k_prog[0][0], NSLOTS, cfg));
    /* its text: ffhw.c's arithmetic from the key's bytes (D3D's spot, pow for the specular) */
    const size_t n = hle_uber_ff_vs_glsl(buf, sizeof(buf), 2);
    HT_CHECK(n > 0);
    HT_CHECK(strstr(buf, "uint kb(uint i)") != NULL);
    HT_CHECK(strstr(buf, "rho <= cphi") != NULL);
    HT_CHECK(strstr(buf, "pow(nh, c[CPAR].x)") != NULL);
    dump("uber_ff.vert");
    HT_EQ_INT(hle_uber_ff_vs_glsl(buf, 64, 2), 0);
}

static void test_prog(void)
{
    memset(cfg, 0xAB, sizeof(cfg));
    unsigned n = hle_uber_prog(&k_prog[0][0], NSLOTS, cfg);
    HT_EQ_INT(n, NSLOTS);
    const uint32_t *w = cfg + HLE_UBER_STEPS_AT;
    /* MOV oD0, v3 + RCP R1.w, R1.w */
    HT_EQ_U32(w[0] & 31, 1);                    /* MOV */
    HT_EQ_U32((w[0] >> 8) & 31, 14);            /* RCP */
    HT_EQ_U32(w[1] & 7, 2);                     /* v */
    HT_EQ_U32((w[1] >> 3) & 255, 3);            /* v3 */
    HT_EQ_U32((w[1] >> 11) & 255, 0xE4);        /* .xyzw */
    HT_EQ_U32(w[4] & 7, 3);                     /* an output */
    HT_EQ_U32((w[4] >> 3) & 255, 3);            /* oD0 */
    HT_EQ_U32((w[4] >> 11) & 15, 15);           /* .xyzw */
    HT_EQ_U32(w[9] & 7, 1);                     /* the ILU's: R1.w */
    HT_EQ_U32((w[9] >> 3) & 255, 1);
    HT_EQ_U32((w[9] >> 11) & 15, 1);
    /* ADD R0.xy, c[A0+121].zw, -c[A0+121].xy: relative, negated */
    const uint32_t *a = w + 5 * HLE_UBER_STEP_WORDS;
    HT_EQ_U32(a[0] & 31, 3);
    HT_EQ_U32(a[1] & 7, 4);
    HT_EQ_U32((a[1] >> 3) & 255, 121);
    HT_CHECK((a[1] >> 20) & 1);
    HT_CHECK(!((a[1] >> 19) & 1));
    HT_CHECK((a[2] >> 19) & 1);
    /* the words past the steps' are left alone */
    HT_EQ_U32(w[NSLOTS * HLE_UBER_STEP_WORDS], 0xABABABABu);
    /* no final slot: nothing */
    HT_EQ_INT(hle_uber_prog(&k_prog[0][0], NSLOTS - 1, cfg), 0);
}

static void test_keys(void)
{
    HleVshKey vk = { .in_mask = 0x19, .int_mask = 0x10, .cmp_mask = 0x8, .flat = 1, .spec_off = 1, .wbuf = 0,
                     .zfree = 1, .fog_table = 1, .w1_mask = 1 };
    HlePshKey pk;
    memset(&pk, 0, sizeof(pk));
    pk.rgb_in[0] = 0x08040000;
    pk.rgb_out[7] = 0xC0;
    pk.final_abcd = 0x11223344;
    pk.final_efg = 0x55667788;
    pk.count = 0x1101;
    pk.tex_modes = 1 | HLE_PSH_CKEY_BITS(0, 3, 1);
    pk.ckey[2] = 0xFF00FF00;
    pk.alpha_test = 1;
    pk.alpha_func = 4;
    pk.fog = 4;
    pk.fog_abs = 1;
    pk.out_bgra = 1;
    pk.specular = 1;
    pk.alpha_kill = 2;
    pk.vol = HLE_PSH_RECT | 1;
    hle_uber_keys(&vk, &pk, 7, cfg);
    HT_EQ_U32(cfg[0], 0x19);
    HT_EQ_U32(cfg[1], 0x10 | 0x8 << 16);
    HT_EQ_U32(cfg[2], 1);
    HT_EQ_U32(cfg[3], 0);
    HT_EQ_U32(cfg[4], HLE_UBER_V_FLAT | HLE_UBER_V_SPECOFF | HLE_UBER_V_ZFREE | 1u << 8);
    HT_EQ_U32(cfg[5], 7);
    HT_EQ_U32(cfg[8], 0x08040000);
    HT_EQ_U32(cfg[23], 0xC0);
    HT_EQ_U32(cfg[40], 0x11223344);
    HT_EQ_U32(cfg[41], 0x55667788);
    HT_EQ_U32(cfg[42], 0x1101);
    HT_EQ_U32(cfg[43], pk.tex_modes);
    HT_EQ_U32(cfg[49], 0xFF00FF00);
    HT_EQ_U32(cfg[51], 1 | 4 << 1 | 4 << 4 | 1 << 8 | 1 << 9 | 1 << 10 | 2 << 12 | (HLE_PSH_RECT | 1u) << 20);
    HT_EQ_U32(cfg[52], 0);
    HT_EQ_U32(cfg[53], 0);
    pk.dither = 2;
    hle_uber_keys(&vk, &pk, 7, cfg);
    HT_EQ_U32(cfg[53], 2);
    pk.dither = 0;
    HT_EQ_INT(hle_uber_fs_depth(&pk), 0);
    pk.wbuf = 1;
    HT_EQ_INT(hle_uber_fs_depth(&pk), 3);
    pk.wbuf = 0;
    pk.vol |= HLE_PSH_ZCUT;
    HT_EQ_INT(hle_uber_fs_depth(&pk), 1);
    HT_CHECK(hle_uber_ps_ok(&pk));
}

static void test_gates(void)
{
    HleVshKey vk = { .in_mask = 1 };
    HT_CHECK(hle_uber_vs_ok(&vk, &k_prog[0][0], NSLOTS));
    HT_CHECK(!hle_uber_vs_ok(&vk, &k_prog[0][0], NSLOTS - 1));      /* no final slot */
    HT_CHECK(!hle_uber_vs_ok(&vk, NULL, 0));
    static uint32_t ff[4 * HLE_FFHW_SLOTS];
    ff[0] = HLE_FFHW_MAGIC;
    HT_CHECK(!hle_uber_vs_ok(&vk, ff, HLE_FFHW_SLOTS));             /* the fixed function's key */
    vk.zclamp = 1;
    g_hle_glsl_zclamp = true;
    HT_CHECK(!hle_uber_vs_ok(&vk, &k_prog[0][0], NSLOTS));
    g_hle_glsl_zclamp = false;
    HT_CHECK(hle_uber_vs_ok(&vk, &k_prog[0][0], NSLOTS));

    HlePshKey pk;
    memset(&pk, 0, sizeof(pk));
    pk.tex_modes = HLE_PST_PROJECT2D | HLE_PST_CUBEMAP << 5 | HLE_PST_CLIPPLANE << 10 | HLE_PST_PASSTHRU << 15;
    pk.count = 2;
    pk.vol = 1;
    pk.compare = 0xF00;
    pk.fog = 4;
    HT_CHECK(hle_uber_ps_ok(&pk));
    HlePshKey b = pk;
    b.tex_modes |= HLE_PST_BUMPENVMAP << 5;     /* (stage 1: a bump stage of theirs) */
    HT_CHECK(hle_uber_ps_ok(&b));
    b = pk;
    b.tex_modes = (pk.tex_modes & ~0x1Fu) | HLE_PST_BUMPENVMAP;     /* stage 0: not */
    HT_CHECK(!hle_uber_ps_ok(&b));
    b = pk;
    b.tex_modes = (pk.tex_modes & ~(0x1Fu << 10)) | (uint32_t)HLE_PST_DOT_ST << 10;  /* stage 2: theirs */
    HT_CHECK(hle_uber_ps_ok(&b) && hle_uber_fs_full(&b));
    b = pk;
    b.tex_modes = (pk.tex_modes & ~(0x1Fu << 5)) | (uint32_t)HLE_PST_DOT_ST << 5;    /* stage 1: not */
    HT_CHECK(!hle_uber_ps_ok(&b));
    HT_CHECK(!hle_uber_fs_full(&pk));
    b = pk;
    b.dot_map |= 1u << 16;                      /* a signed channel */
    HT_CHECK(hle_uber_ps_ok(&b));
    b = pk;
    b.shadow = 0x41;                            /* a shadow buffer on stage 0 */
    b.compare |= HLE_PSH_TKIND_BIT(2);
    HT_CHECK(hle_uber_ps_ok(&b));
    b.compare |= 1u << 20;                      /* past the base's bits */
    HT_CHECK(!hle_uber_ps_ok(&b));
    b = pk;
    b.vol |= HLE_PSH_RECT;
    HT_CHECK(hle_uber_ps_ok(&b));
    b.sblend = 1;
    HT_CHECK(!hle_uber_ps_ok(&b));
    b = pk;
    b.fbfetch = 1;
    HT_CHECK(!hle_uber_ps_ok(&b));
    b = pk;
    b.wbuf = 1;
    HT_CHECK(hle_uber_ps_ok(&b));
    b.wbuf = 2;
    HT_CHECK(!hle_uber_ps_ok(&b));
    b = pk;
    b.fog = 5;
    HT_CHECK(!hle_uber_ps_ok(&b));
    b = pk;
    b.point_sprite = 1;
    HT_CHECK(!hle_uber_ps_ok(&b));
}

int main(void)
{
    HT_RUN(test_text);
    HT_RUN(test_prog);
    HT_RUN(test_keys);
    HT_RUN(test_gates);
    HT_RUN(test_ff);
    return ht_failures ? 1 : 0;
}
