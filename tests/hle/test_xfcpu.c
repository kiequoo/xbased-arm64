/* The transform unit on the CPU (hw/xbox/hle/shader/xfcpu.c) against the
 * console: the results nxdk_low_level_nv2a_tests' vsh_test measured (its
 * ADD, MUL, MAD, DST, DP3 / DPH / DP4 rounding probes and the projection
 * epilogue of Rugby 2005, xemu-project/xemu#2388). */
#include "hw/xbox/hle/shader/xfcpu.h"
#include "hle_test.h"

#include <stdlib.h>

static const struct {
    const char *op, *label;
    uint32_t a[4], b[4], c[4];
    uint32_t want;
} k_cases[] = {
    {"ADD", "+0.75 ulp", {0x3F800000, 0x00000000, 0x00000000, 0x00000000}, {0x33C00000, 0x00000000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x3F800000},
    {"ADD", "-0.75 ulp", {0xBF800000, 0x00000000, 0x00000000, 0x00000000}, {0xB3C00000, 0x00000000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0xBF800000},
    {"ADD", "1 - 2^-28", {0x3F800000, 0x00000000, 0x00000000, 0x00000000}, {0xB1800000, 0x00000000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x3F7FFFFF},
    {"ADD", "1 - (2^-24 + 2^-29)", {0x3F800000, 0x00000000, 0x00000000, 0x00000000}, {0xB3840000, 0x00000000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x3F7FFFFF},
    {"MUL", "+0.75 ulp", {0x3F800800, 0x00000000, 0x00000000, 0x00000000}, {0x3F800C00, 0x00000000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x3F801400},
    {"MUL", "-0.75 ulp", {0xBF800800, 0x00000000, 0x00000000, 0x00000000}, {0x3F800C00, 0x00000000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0xBF801400},
    {"DST", "+0.75 ulp", {0x3F800800, 0x00000000, 0x00000000, 0x00000000}, {0x3F800C00, 0x00000000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x3F801400},
    {"DST", "-0.75 ulp", {0xBF800800, 0x00000000, 0x00000000, 0x00000000}, {0x3F800C00, 0x00000000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0xBF801400},
    {"MAD", "a * b - RN(a * b), +0.75 ulp", {0x3F800800, 0x00000000, 0x00000000, 0x00000000}, {0x3F800C00, 0x00000000, 0x00000000, 0x00000000}, {0xBF801401, 0x00000000, 0x00000000, 0x00000000}, 0xB4000000},
    {"MAD", "a * 1 + b, +0.75 ulp", {0x3F800000, 0x00000000, 0x00000000, 0x00000000}, {0x3F800000, 0x00000000, 0x00000000, 0x00000000}, {0x33C00000, 0x00000000, 0x00000000, 0x00000000}, 0x3F800000},
    {"MAD", "both inexact, -", {0xBF800800, 0x00000000, 0x00000000, 0x00000000}, {0x3F800C00, 0x00000000, 0x00000000, 0x00000000}, {0xB3800000, 0x00000000, 0x00000000, 0x00000000}, 0xBF801400},
    {"MAD", "1 * 1 - 2^-28", {0x3F800000, 0x00000000, 0x00000000, 0x00000000}, {0x3F800000, 0x00000000, 0x00000000, 0x00000000}, {0xB1800000, 0x00000000, 0x00000000, 0x00000000}, 0x3F7FFFFF},
    {"MAD", "1 * 1 - (2^-24 + 2^-29)", {0x3F800000, 0x00000000, 0x00000000, 0x00000000}, {0x3F800000, 0x00000000, 0x00000000, 0x00000000}, {0xB3840000, 0x00000000, 0x00000000, 0x00000000}, 0x3F7FFFFF},
    {"DP3", "(a, RN(a * b)) . (b, -1), +0.75 ulp", {0x3F800800, 0x3F801401, 0x00000000, 0x00000000}, {0x3F800C00, 0xBF800000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0xB4000000},
    {"DPH", "(a, RN(a * b)) . (b, -1), +0.75 ulp", {0x3F800800, 0x3F801401, 0x00000000, 0x00000000}, {0x3F800C00, 0xBF800000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0xB4000000},
    {"DP4", "(a, RN(a * b)) . (b, -1), +0.75 ulp", {0x3F800800, 0x3F801401, 0x00000000, 0x00000000}, {0x3F800C00, 0xBF800000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0xB4000000},
    {"DP3", "(a, b, c) . (1, 1, -1), +0.75 ulp", {0x3F800000, 0x33C00000, 0x3F800001, 0x00000000}, {0x3F800000, 0x3F800000, 0xBF800000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0xB3000000},
    {"DPH", "(a, b, c) . (1, 1, -1), +0.75 ulp", {0x3F800000, 0x33C00000, 0x3F800001, 0x00000000}, {0x3F800000, 0x3F800000, 0xBF800000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0xB3000000},
    {"DP4", "(a, b, c) . (1, 1, -1), +0.75 ulp", {0x3F800000, 0x33C00000, 0x3F800001, 0x00000000}, {0x3F800000, 0x3F800000, 0xBF800000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0xB3000000},
    {"DP3", "(a, b) . (1, 1), +0.75 ulp", {0x3F800000, 0x33C00000, 0x00000000, 0x00000000}, {0x3F800000, 0x3F800000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x3F800000},
    {"DPH", "(a, b) . (1, 1), +0.75 ulp", {0x3F800000, 0x33C00000, 0x00000000, 0x00000000}, {0x3F800000, 0x3F800000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x3F800000},
    {"DP4", "(a, b) . (1, 1), +0.75 ulp", {0x3F800000, 0x33C00000, 0x00000000, 0x00000000}, {0x3F800000, 0x3F800000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x3F800000},
    {"DP3", "(a, b) . (1, 1), -0.75 ulp", {0xBF800000, 0xB3C00000, 0x00000000, 0x00000000}, {0x3F800000, 0x3F800000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0xBF800000},
    {"DPH", "(a, b) . (1, 1), -0.75 ulp", {0xBF800000, 0xB3C00000, 0x00000000, 0x00000000}, {0x3F800000, 0x3F800000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0xBF800000},
    {"DP4", "(a, b) . (1, 1), -0.75 ulp", {0xBF800000, 0xB3C00000, 0x00000000, 0x00000000}, {0x3F800000, 0x3F800000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0xBF800000},
    {"DP3", "(a, b) . (1, 1) + w(-c), +0.75 ulp", {0x3F800000, 0x33C00000, 0x00000000, 0x00000000}, {0x3F800000, 0x3F800000, 0x00000000, 0xBF800001}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x3F800000},
    {"DPH", "(a, b) . (1, 1) + w(-c), +0.75 ulp", {0x3F800000, 0x33C00000, 0x00000000, 0x00000000}, {0x3F800000, 0x3F800000, 0x00000000, 0xBF800001}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0xB3000000},
    {"DP4", "(a, b) . (1, 1) + w(-c), +0.75 ulp", {0x3F800000, 0x33C00000, 0x00000000, 0x00000000}, {0x3F800000, 0x3F800000, 0x00000000, 0xBF800001}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x3F800000},
    {"DP3", "(a, RN(a * b), 0, NaN) . (b, -1, 0, NaN), Rugby", {0xBF80309E, 0x42203CC6, 0x00000000, 0x7FC00000}, {0xC2200000, 0xBF800000, 0x00000000, 0x7FC00000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0xB6800000},
    {"DPH", "(a, RN(a * b), 0, NaN) . (b, -1, 0, NaN), Rugby", {0xBF80309E, 0x42203CC6, 0x00000000, 0x7FC00000}, {0xC2200000, 0xBF800000, 0x00000000, 0x7FC00000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x7FFFFFFF},
    {"DP4", "(a, RN(a * b), 0, NaN) . (b, -1, 0, NaN), Rugby", {0xBF80309E, 0x42203CC6, 0x00000000, 0x7FC00000}, {0xC2200000, 0xBF800000, 0x00000000, 0x7FC00000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x7FFFFFFF},
    {"DP3", "1 + small - 1, gap 2^28", {0x3F800000, 0x31800000, 0xBF800000, 0x00000000}, {0x3F800000, 0x3F800000, 0x3F800000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x31800000},
    {"DPH", "1 + small - 1, gap 2^28", {0x3F800000, 0x31800000, 0xBF800000, 0x00000000}, {0x3F800000, 0x3F800000, 0x3F800000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x31800000},
    {"DP4", "1 + small - 1, gap 2^28", {0x3F800000, 0x31800000, 0xBF800000, 0x00000000}, {0x3F800000, 0x3F800000, 0x3F800000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x31800000},
    {"DP3", "big - big + small, gap 2^64", {0x4F800000, 0xCF800000, 0x2F800000, 0x00000000}, {0x3F800000, 0x3F800000, 0x3F800000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x00000000},
    {"DPH", "big - big + small, gap 2^64", {0x4F800000, 0xCF800000, 0x2F800000, 0x00000000}, {0x3F800000, 0x3F800000, 0x3F800000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x00000000},
    {"DP4", "big - big + small, gap 2^64", {0x4F800000, 0xCF800000, 0x2F800000, 0x00000000}, {0x3F800000, 0x3F800000, 0x3F800000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x00000000},
    {"DP3", "(1, -(2^-24 + 2^-29)) . (1, 1)", {0x3F800000, 0xB3840000, 0x00000000, 0x00000000}, {0x3F800000, 0x3F800000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x3F7FFFFF},
    {"DPH", "(1, -(2^-24 + 2^-29)) . (1, 1)", {0x3F800000, 0xB3840000, 0x00000000, 0x00000000}, {0x3F800000, 0x3F800000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x3F7FFFFF},
    {"DP4", "(1, -(2^-24 + 2^-29)) . (1, 1)", {0x3F800000, 0xB3840000, 0x00000000, 0x00000000}, {0x3F800000, 0x3F800000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000}, 0x3F7FFFFF},
};

static uint32_t dot(int n, const uint32_t *a, const uint32_t *b, bool h)
{
    uint32_t p[4] = {0, 0, 0, 0};
    for (int i = 0; i < n; i++) {
        p[i] = hle_xf_mul(a[i], b[i]);
    }
    if (h) {
        p[3] = hle_xf_mul(0x3f800000u, b[3]);
    }
    return hle_xf_sum(p[0], p[1], p[2], p[3]);
}

static void test_measured(void)
{
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        const char *op = k_cases[i].op;
        const uint32_t *a = k_cases[i].a, *b = k_cases[i].b, *c = k_cases[i].c;
        uint32_t got = 0;
        if (!strcmp(op, "ADD")) {
            got = hle_xf_sum(a[0], b[0], 0, 0);
        } else if (!strcmp(op, "MUL") || !strcmp(op, "DST")) {
            got = hle_xf_mul(a[0], b[0]);
        } else if (!strcmp(op, "MAD")) {
            got = hle_xf_sum(hle_xf_mul(a[0], b[0]), c[0], 0, 0);
        } else if (!strcmp(op, "DP3")) {
            got = dot(3, a, b, false);
        } else if (!strcmp(op, "DPH")) {
            got = dot(3, a, b, true);
        } else if (!strcmp(op, "DP4")) {
            got = dot(4, a, b, false);
        }
        if (got != k_cases[i].want) {
            fprintf(stderr, "%s %s: 0x%08x, the console 0x%08x\n", op, k_cases[i].label, got, k_cases[i].want);
            ht_failures++;
        }
    }
}

static uint32_t u(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    return x;
}

static void test_rugby(void)
{
    const uint32_t v1[4] = {u(100.0f), u(50.0f), u(-40.0f), u(1.0f)};
    const uint32_t c[4][4] = {{u(1.0f), 0, 0, 0}, {0, u(1.0f), 0, 0}, {0, 0, 0xBF80309Eu, 0xC2203CC6u},
                              {0, 0, u(-1.0f), 0}};
    uint32_t r12[4];
    for (int i = 0; i < 4; i++) {
        r12[i] = dot(4, v1, c[i], false);
    }
    HT_EQ_U32(r12[0], u(100.0f));
    HT_EQ_U32(r12[1], u(50.0f));
    HT_EQ_U32(r12[2], 0xB6800000u);
    HT_EQ_U32(r12[3], u(40.0f));
    const uint32_t c58[3] = {u(320.0f), u(-240.0f), u(16777215.0f)}, c59[3] = {u(319.53125f), u(239.53125f), 0};
    for (int i = 0; i < 3; i++) {
        r12[i] = hle_xf_mul(r12[i], c58[i]);
    }
    HT_EQ_U32(r12[0], u(32000.0f));
    HT_EQ_U32(r12[1], u(-12000.0f));
    HT_EQ_U32(r12[2], 0xC27FFFFFu);
    const uint32_t r1 = hle_xf_rcp(r12[3], true);
    HT_EQ_U32(r1, 0x3CCCCCCCu);
    for (int i = 0; i < 3; i++) {
        r12[i] = hle_xf_sum(hle_xf_mul(r12[i], r1), c59[i], 0, 0);
    }
    HT_EQ_U32(r12[0], 0x448BF0FFu);
    HT_EQ_U32(r12[1], 0xC271DFF8u);
    HT_EQ_U32(r12[2], 0xBFCCCCCBu);
}


/* hle_xf_fog_slice against the whole program: random programs over every
 * operation, register kind, mask, swizzle and A0's relative reads, the oFog.x
 * each leaves from random values, bit for bit */
static uint32_t g_rng = 0x12345678u;

static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void rnd_input(Nv2aVshInput *in, bool rel_ok)
{
    const uint32_t k = rnd() % 8;
    in->type = k < 4 ? NV2ART_TEMPORARY : k < 6 ? NV2ART_INPUT : NV2ART_CONTEXT;
    in->index = in->type == NV2ART_TEMPORARY ? rnd() % 13 : in->type == NV2ART_INPUT ? rnd() % 16 : rnd() % 192;
    in->is_relative = in->type == NV2ART_CONTEXT && rel_ok && rnd() % 3 == 0;
    in->is_negated = rnd() % 4 == 0;
    for (int i = 0; i < 4; i++) {
        in->swizzle[i] = (uint8_t)(rnd() % 4);
    }
}

static void rnd_output(Nv2aVshOutput *o, bool addr_ok)
{
    const uint32_t k = rnd() % 10;
    o->type = k < 5 ? NV2ART_TEMPORARY : k < 9 ? NV2ART_OUTPUT : addr_ok ? NV2ART_ADDRESS : NV2ART_NONE;
    o->index = o->type == NV2ART_TEMPORARY ? rnd() % 13 : o->type == NV2ART_OUTPUT ? (rnd() % 3 ? 5u : rnd() % 13) : 0;
    o->writemask = (Nv2aVshWritemask)(rnd() % 16);
}

static float rnd_f(void)
{
    const uint32_t k = rnd() % 16;
    float f = (float)((int32_t)(rnd() % 2001) - 1000) / (float)(1 + rnd() % 64);
    return k == 0 ? 0.0f : k == 1 ? -f * 1e6f : f;
}

static void test_fog_slice(void)
{
    static const Nv2aVshOpcode k_mac[] = { NV2AOP_MOV, NV2AOP_MUL, NV2AOP_ADD, NV2AOP_MAD, NV2AOP_DP3, NV2AOP_DPH,
                                           NV2AOP_DP4, NV2AOP_DST, NV2AOP_MIN, NV2AOP_MAX, NV2AOP_SLT, NV2AOP_SGE,
                                           NV2AOP_ARL };
    static const Nv2aVshOpcode k_ilu[] = { NV2AOP_MOV, NV2AOP_RCP, NV2AOP_RCC, NV2AOP_RSQ, NV2AOP_EXP, NV2AOP_LOG,
                                           NV2AOP_LIT };
    unsigned programs = 0, cut = 0, kept = 0, total = 0, empty = 0;
    for (int t = 0; t < 4000; t++) {
        Nv2aVshStep st[24];
        memset(st, 0, sizeof(st));
        const uint32_t ns = 1 + rnd() % 24;
        for (uint32_t i = 0; i < ns; i++) {
            if (rnd() % 5) {
                st[i].mac.opcode = k_mac[rnd() % (sizeof(k_mac) / sizeof(k_mac[0]))];
                const unsigned nin = st[i].mac.opcode == NV2AOP_MAD ? 3 :
                                     st[i].mac.opcode == NV2AOP_MOV || st[i].mac.opcode == NV2AOP_ARL ? 1 : 2;
                for (unsigned k = 0; k < nin; k++) {
                    rnd_input(&st[i].mac.inputs[k], true);
                }
                if (st[i].mac.opcode == NV2AOP_ARL) {
                    st[i].mac.outputs[0].type = NV2ART_ADDRESS;
                    st[i].mac.outputs[0].writemask = NV2AWM_X;
                } else {
                    rnd_output(&st[i].mac.outputs[0], false);
                    if (rnd() % 3 == 0) {
                        rnd_output(&st[i].mac.outputs[1], false);
                    }
                }
            }
            if (rnd() % 3 == 0) {
                st[i].ilu.opcode = k_ilu[rnd() % (sizeof(k_ilu) / sizeof(k_ilu[0]))];
                rnd_input(&st[i].ilu.inputs[0], true);
                rnd_output(&st[i].ilu.outputs[0], false);
            }
        }
        st[ns - 1].is_final = true;
        const Nv2aVshProgram whole = { st };
        Nv2aVshProgram part;
        if (!hle_xf_fog_slice(&whole, ns, &part)) {
            HT_CHECK(0);                        /* (none of these writes a constant) */
            continue;
        }
        programs++;
        unsigned n = 0;
        for (const Nv2aVshStep *q = part.steps; q; q++) {
            n++;
            if (q->is_final) {
                break;
            }
        }
        cut += n < ns;
        kept += n;
        total += ns;
        empty += !part.steps;
        for (int r = 0; r < 4; r++) {
            float v[16 * 4], c0[192 * 4], c1[192 * 4], o0[13 * 4], o1[13 * 4];
            for (int i = 0; i < 64; i++) {
                v[i] = rnd_f();
            }
            for (int i = 0; i < 192 * 4; i++) {
                c0[i] = c1[i] = rnd_f();
            }
            for (int i = 0; i < 52; i++) {
                o0[i] = o1[i] = rnd_f();
            }
            hle_xf_run(&whole, v, c0, NULL, o0);
            if (part.steps) {
                hle_xf_run(&part, v, c1, NULL, o1);
            }
            uint32_t a, b;
            memcpy(&a, &o0[20], 4);
            memcpy(&b, &o1[20], 4);
            if (a != b) {
                fprintf(stderr, "program %d (%u steps, %u kept), values %d: oFog.x %08x, the slice's %08x\n",
                        t, ns, n, r, a, b);
            }
            HT_EQ_U32(b, a);
        }
        nv2a_vsh_program_destroy(&part);
    }
    /* (a constant written: left whole) */
    Nv2aVshStep w[2];
    memset(w, 0, sizeof(w));
    w[0].mac.opcode = NV2AOP_MOV;
    w[0].mac.inputs[0].type = NV2ART_INPUT;
    w[0].mac.outputs[0].type = NV2ART_CONTEXT;
    w[0].mac.outputs[0].index = 7;
    w[0].mac.outputs[0].writemask = NV2AWM_XYZW;
    w[1].mac.opcode = NV2AOP_MOV;
    w[1].mac.inputs[0].type = NV2ART_CONTEXT;
    w[1].mac.inputs[0].index = 7;
    w[1].mac.outputs[0].type = NV2ART_OUTPUT;
    w[1].mac.outputs[0].index = 5;
    w[1].mac.outputs[0].writemask = NV2AWM_X;
    w[1].is_final = true;
    const Nv2aVshProgram wp = { w };
    Nv2aVshProgram wpart;
    HT_CHECK(!hle_xf_fog_slice(&wp, 2, &wpart));
    HT_CHECK(wpart.steps == NULL);
    HT_CHECK(cut > programs / 2);
    printf("  fog slices: %u programs, %u cut, %u of %u steps kept, %u with none\n", programs, cut, kept, total,
           empty);
}

int main(void)
{
    HT_RUN(test_measured);
    HT_RUN(test_rugby);
    HT_RUN(test_fog_slice);
    HT_DONE();
}
