/*
 * hakuX HLE GPU — Kelvin's DMA contexts (hw/xbox/hle/pb/kvdma.c).
 */
#include "hle_test.h"

#include <string.h>

#include "hw/xbox/hle/pb/kvdma.h"

#define CTX_A      0x0184u
#define CTX_B      0x0188u
#define CTX_COLOR  0x0194u
#define CTX_ZETA   0x0198u
#define CTX_VTX_A  0x019Cu
#define CTX_VTX_B  0x01A0u
#define COLOR_OFF  0x0210u
#define ZETA_OFF   0x0214u
#define VTX_OFF(a) (0x1720u + 4 * (a))
#define TEX_OFF(s) (0x1B00u + 64 * (s))
#define TEX_FMT(s) (0x1B04u + 64 * (s))

static HleKvDmaOut g_o[HLE_KVDMA_OUT];

static unsigned run(HleKvDma *d, uint32_t m, uint32_t v, uint32_t base)
{
    memset(g_o, 0xEE, sizeof(g_o));
    return hle_kvdma_method(d, m, v, base, g_o);
}

/* The methods it takes: the six contexts and the offsets, nothing else. */
static void test_acts(void)
{
    unsigned n = 0;
    for (uint32_t m = 0; m < 0x2000; m += 4) {
        n += hle_kvdma_acts(m);
    }
    HT_EQ_U32(n, 6 + 2 + 16 + 8);
    HT_CHECK(hle_kvdma_acts(CTX_A) && hle_kvdma_acts(CTX_VTX_B) && hle_kvdma_acts(COLOR_OFF));
    HT_CHECK(hle_kvdma_acts(VTX_OFF(15)) && hle_kvdma_acts(TEX_OFF(3)) && hle_kvdma_acts(TEX_FMT(3)));
    HT_CHECK(!hle_kvdma_acts(0x0180) && !hle_kvdma_acts(0x0190) && !hle_kvdma_acts(0x01A4));
    HT_CHECK(!hle_kvdma_acts(0x020C) && !hle_kvdma_acts(0x1760) && !hle_kvdma_acts(0x1B08));
    HT_EQ_INT(hle_kvdma_context(CTX_ZETA), HLE_KVDMA_ZETA);
    HT_EQ_INT(hle_kvdma_context(COLOR_OFF), -1);
}

/* D3D's objects start at 0: every method goes through as it came, one for one. */
static void test_base_zero(void)
{
    HleKvDma d = { 0 };
    static const uint32_t k_m[] = { CTX_A, CTX_B, CTX_COLOR, CTX_ZETA, CTX_VTX_A, CTX_VTX_B,
                                    COLOR_OFF, ZETA_OFF, VTX_OFF(0), VTX_OFF(7), TEX_OFF(0),
                                    TEX_FMT(0), TEX_OFF(2), TEX_FMT(2), CTX_A, CTX_COLOR };
    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < sizeof(k_m) / sizeof(k_m[0]); i++) {
            const uint32_t v = 0x03A00000u + 0x1234 * i + (k_m[i] == VTX_OFF(7) ? 0x80000000u : 0) +
                               (k_m[i] == TEX_FMT(2) ? 2 : 1);
            HT_EQ_U32(run(&d, k_m[i], v, 0), 1);
            HT_EQ_U32(g_o[0].m, k_m[i]);
            HT_EQ_U32(g_o[0].v, v);
        }
    }
}

/* pbkit's frame: the colour object's base is the frame, the offset 0. Named
 * again with the next frame's base, the offset is sent again as that
 * address; with the same base, nothing more. */
static void test_color_follows_object(void)
{
    HleKvDma d = { 0 };
    HT_EQ_U32(run(&d, CTX_COLOR, 9, 0x03028000u), 1);          /* no offset pushed yet */
    HT_EQ_U32(g_o[0].m, CTX_COLOR);
    HT_EQ_U32(g_o[0].v, 9);
    HT_EQ_U32(run(&d, COLOR_OFF, 0, 0), 1);
    HT_EQ_U32(g_o[0].m, COLOR_OFF);
    HT_EQ_U32(g_o[0].v, 0x03028000u);
    HT_EQ_U32(run(&d, CTX_COLOR, 9, 0x034D8000u), 2);
    HT_EQ_U32(g_o[0].m, CTX_COLOR);
    HT_EQ_U32(g_o[1].m, COLOR_OFF);
    HT_EQ_U32(g_o[1].v, 0x034D8000u);
    HT_EQ_U32(run(&d, CTX_COLOR, 9, 0x034D8000u), 1);
    HT_EQ_U32(run(&d, COLOR_OFF, 0x1000, 0), 1);
    HT_EQ_U32(g_o[0].v, 0x034D9000u);
    /* the depth buffer's context is its own */
    HT_EQ_U32(run(&d, ZETA_OFF, 0x40, 0), 1);
    HT_EQ_U32(g_o[0].v, 0x40);
    HT_EQ_U32(run(&d, CTX_ZETA, 10, 0x02B78000u), 2);
    HT_EQ_U32(g_o[1].m, ZETA_OFF);
    HT_EQ_U32(g_o[1].v, 0x02B78040u);
    HT_EQ_U32(run(&d, COLOR_OFF, 0, 0), 1);
    HT_EQ_U32(g_o[0].v, 0x034D8000u);
}

/* A stage's texture lives in the context its format names; the format may
 * come after the offset, and change the context under it. */
static void test_texture_contexts(void)
{
    HleKvDma d = { 0 };
    run(&d, CTX_A, 3, 0);
    run(&d, CTX_B, 4, 0x01000000u);
    HT_EQ_U32(run(&d, TEX_OFF(1), 0x2000, 0), 1);
    HT_EQ_U32(g_o[0].v, 0x2000);                               /* A until a format says B */
    HT_EQ_U32(run(&d, TEX_FMT(1), 0x0001062Au, 0), 2);         /* context B: the offset first */
    HT_EQ_U32(g_o[0].m, TEX_OFF(1));
    HT_EQ_U32(g_o[0].v, 0x01002000u);
    HT_EQ_U32(g_o[1].m, TEX_FMT(1));
    HT_EQ_U32(g_o[1].v, 0x0001062Au);
    HT_EQ_U32(run(&d, TEX_FMT(1), 0x0001082Au, 0), 1);         /* B still */
    HT_EQ_U32(run(&d, TEX_OFF(1), 0x3000, 0), 1);
    HT_EQ_U32(g_o[0].v, 0x01003000u);
    HT_EQ_U32(run(&d, TEX_OFF(0), 0x5000, 0), 1);              /* another stage, in A */
    HT_EQ_U32(g_o[0].v, 0x5000);
    /* B moves: only the stages in it follow */
    HT_EQ_U32(run(&d, CTX_B, 4, 0x02000000u), 2);
    HT_EQ_U32(g_o[1].m, TEX_OFF(1));
    HT_EQ_U32(g_o[1].v, 0x02003000u);
    HT_EQ_U32(run(&d, CTX_A, 3, 0x00100000u), 2);
    HT_EQ_U32(g_o[1].m, TEX_OFF(0));
    HT_EQ_U32(g_o[1].v, 0x00105000u);
    HT_EQ_U32(run(&d, TEX_FMT(1), 0x00010829u, 0), 2);         /* back to A */
    HT_EQ_U32(g_o[0].v, 0x00103000u);
}

/* A vertex array's bit 31 names context B, and stays in the address. */
static void test_vertex_contexts(void)
{
    HleKvDma d = { 0 };
    run(&d, CTX_VTX_B, 5, 0x00400000u);
    HT_EQ_U32(run(&d, VTX_OFF(0), 0x00012340u, 0), 1);
    HT_EQ_U32(g_o[0].v, 0x00012340u);
    HT_EQ_U32(run(&d, VTX_OFF(3), 0x80012340u, 0), 1);
    HT_EQ_U32(g_o[0].v, 0x80412340u);
    HT_EQ_U32(run(&d, VTX_OFF(9), 0x00000010u, 0), 1);
    HT_EQ_U32(run(&d, CTX_VTX_A, 3, 0x00800000u), 3);          /* arrays 0 and 9, not 3 */
    HT_EQ_U32(g_o[1].m, VTX_OFF(0));
    HT_EQ_U32(g_o[1].v, 0x00812340u);
    HT_EQ_U32(g_o[2].m, VTX_OFF(9));
    HT_EQ_U32(g_o[2].v, 0x00800010u);
    HT_EQ_U32(run(&d, CTX_VTX_B, 5, 0), 2);
    HT_EQ_U32(g_o[1].m, VTX_OFF(3));
    HT_EQ_U32(g_o[1].v, 0x80012340u);
    /* all 16 in one context: the most one method becomes */
    HleKvDma e = { 0 };
    for (unsigned a = 0; a < 16; a++) {
        run(&e, VTX_OFF(a), 0x100 * a, 0);
    }
    HT_EQ_U32(run(&e, CTX_VTX_A, 3, 0x00200000u), HLE_KVDMA_OUT);
    HT_EQ_U32(g_o[16].m, VTX_OFF(15));
    HT_EQ_U32(g_o[16].v, 0x00200F00u);
}

/* After a load the shadow holds addresses: the offsets come back from them,
 * and a context that moves then sends the right ones. */
static uint32_t g_reg[0x2000 / 4];

static uint32_t reg(uint32_t m)
{
    return g_reg[m >> 2];
}

static void test_restore(void)
{
    HleKvDma d = { 0 };
    memset(g_reg, 0, sizeof(g_reg));
    static const struct { uint32_t m, v, base; } k_seq[] = {
        { CTX_COLOR, 9, 0x03028000u }, { CTX_ZETA, 10, 0x02B78000u }, { CTX_B, 4, 0x01000000u },
        { CTX_VTX_B, 5, 0x00400000u }, { COLOR_OFF, 0x80, 0 }, { ZETA_OFF, 0, 0 },
        { TEX_OFF(2), 0x7000, 0 }, { TEX_FMT(2), 0x0001062Au, 0 }, { VTX_OFF(4), 0x80000100u, 0 },
        { VTX_OFF(5), 0x00000200u, 0 },
    };
    for (unsigned i = 0; i < sizeof(k_seq) / sizeof(k_seq[0]); i++) {
        const unsigned n = run(&d, k_seq[i].m, k_seq[i].v, k_seq[i].base);
        for (unsigned k = 0; k < n; k++) {
            g_reg[g_o[k].m >> 2] = g_o[k].v;
        }
    }
    HleKvDma r;
    memset(&r, 0x5A, sizeof(r));
    hle_kvdma_restore(&r, d.base, reg);
    HT_EQ_U32(r.color, 0x80);
    HT_EQ_U32(r.zeta, 0);
    HT_EQ_U32(r.tex[2], 0x7000);
    HT_EQ_INT(r.tex_ctx[2], HLE_KVDMA_B);
    HT_EQ_U32(r.vtx[4], 0x80000100u);
    HT_EQ_U32(r.vtx[5], 0x00000200u);
    HT_EQ_U32(run(&r, CTX_COLOR, 9, 0x034D8000u), 2);
    HT_EQ_U32(g_o[1].v, 0x034D8080u);
    HT_EQ_U32(run(&r, CTX_VTX_B, 5, 0x00500000u), 2);
    HT_EQ_U32(g_o[1].m, VTX_OFF(4));
    HT_EQ_U32(g_o[1].v, 0x80500100u);
}

int main(void)
{
    HT_RUN(test_acts);
    HT_RUN(test_base_zero);
    HT_RUN(test_color_follows_object);
    HT_RUN(test_texture_contexts);
    HT_RUN(test_vertex_contexts);
    HT_RUN(test_restore);
    HT_DONE();
}
