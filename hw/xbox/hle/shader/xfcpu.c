/*
 * xbased HLE - the NV2A's transform unit on the CPU (xfcpu.h): xflib.h's
 * arithmetic in C, line for line, and vsh.c's reading of a program.
 */
#include "hw/xbox/hle/shader/xfcpu.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t k_rcp_lut[64] = {
    0x7f, 0x7d, 0x7b, 0x79, 0x78, 0x76, 0x74, 0x73, 0x71, 0x6f, 0x6e, 0x6d, 0x6b, 0x6a, 0x68, 0x67,
    0x66, 0x65, 0x63, 0x62, 0x61, 0x60, 0x5f, 0x5e, 0x5d, 0x5c, 0x5b, 0x5a, 0x59, 0x58, 0x57, 0x56,
    0x55, 0x54, 0x53, 0x52, 0x52, 0x51, 0x50, 0x4f, 0x4e, 0x4e, 0x4d, 0x4c, 0x4c, 0x4b, 0x4a, 0x49,
    0x49, 0x48, 0x48, 0x47, 0x46, 0x46, 0x45, 0x45, 0x44, 0x43, 0x43, 0x42, 0x42, 0x41, 0x41, 0x40};
static const uint8_t k_rsq_lut[128] = {
    0x7f, 0x7f, 0x7e, 0x7d, 0x7c, 0x7b, 0x7a, 0x79, 0x78, 0x77, 0x77, 0x76, 0x75, 0x74, 0x74, 0x73,
    0x72, 0x71, 0x71, 0x70, 0x6f, 0x6f, 0x6e, 0x6d, 0x6d, 0x6c, 0x6c, 0x6b, 0x6a, 0x6a, 0x69, 0x69,
    0x68, 0x68, 0x67, 0x67, 0x66, 0x66, 0x65, 0x65, 0x64, 0x64, 0x63, 0x63, 0x62, 0x62, 0x61, 0x61,
    0x61, 0x60, 0x60, 0x5f, 0x5f, 0x5e, 0x5e, 0x5e, 0x5d, 0x5d, 0x5d, 0x5c, 0x5c, 0x5b, 0x5b, 0x5b,
    0x5a, 0x59, 0x59, 0x58, 0x57, 0x57, 0x56, 0x56, 0x55, 0x54, 0x54, 0x53, 0x53, 0x52, 0x52, 0x51,
    0x51, 0x50, 0x50, 0x4f, 0x4f, 0x4e, 0x4e, 0x4d, 0x4d, 0x4d, 0x4c, 0x4c, 0x4b, 0x4b, 0x4a, 0x4a,
    0x4a, 0x49, 0x49, 0x49, 0x48, 0x48, 0x48, 0x47, 0x47, 0x46, 0x46, 0x46, 0x46, 0x45, 0x45, 0x45,
    0x44, 0x44, 0x44, 0x43, 0x43, 0x43, 0x43, 0x42, 0x42, 0x42, 0x41, 0x41, 0x41, 0x41, 0x40, 0x40};

/* GLSL's umulExtended */
static void umul(uint32_t a, uint32_t b, uint32_t *h, uint32_t *l)
{
    const uint64_t p = (uint64_t)a * b;
    *h = (uint32_t)(p >> 32);
    *l = (uint32_t)p;
}

static int msb(uint32_t v)
{
    int n = -1;
    while (v) {
        n++;
        v >>= 1;
    }
    return n;
}

static int clampi(int x, int lo, int hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

/* (xflib.h xfm: without branches, as there) */
uint32_t hle_xf_mul(uint32_t a, uint32_t b)
{
    const uint32_t ea = (a >> 23) & 0xffu, eb = (b >> 23) & 0xffu;
    const uint32_t sg = (a ^ b) & 0x80000000u;
    uint32_t h, l;
    umul((a & 0x7fffffu) | 0x800000u, (b & 0x7fffffu) | 0x800000u, &h, &l);
    const bool nrm = (h & 0x8000u) == 0u;
    const uint32_t m = nrm ? ((h << 9) | (l >> 23)) : ((h << 8) | (l >> 24));
    const int er = (int)ea + (int)eb - (nrm ? 0x7f : 0x7e);
    uint32_t r = sg | ((uint32_t)clampi(er, 0, 0xfe) << 23) | (m & 0x7fffffu);
    r = er <= 0 ? sg : r;
    r = er >= 0xff ? (sg | 0x7f7fffffu) : r;
    const bool ia = ea == 0xffu, ib = eb == 0xffu;
    r = (ia || ib) ? (sg | 0x7f800000u) : r;
    r = ((ia && (a & 0x7fffffu)) || (ib && (b & 0x7fffffu))) ? 0x7fffffffu : r;
    return (ea == 0u || eb == 0u) ? 0u : r;
}

/* (xflib.h xfs) */
uint32_t hle_xf_sum(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    const uint32_t v[4] = {a, b, c, d};
    int ev[4], er = 0;
    uint32_t fv[4];
    bool nan = false, pinf = false, ninf = false;
    for (int i = 0; i < 4; i++) {
        ev[i] = (int)((v[i] >> 23) & 0xffu);
        fv[i] = v[i] & 0x7fffffu;
        const bool isf = ev[i] == 0xff, neg = (v[i] >> 31) != 0;
        nan = nan || (isf && fv[i]);
        pinf = pinf || (isf && !neg);
        ninf = ninf || (isf && neg);
        fv[i] |= ev[i] ? 0x800000u : 0u;
        er = er > ev[i] + 2 ? er : ev[i] + 2;
    }
    int res = 0;
    for (int i = 0; i < 4; i++) {
        const int sh = er - ev[i] - 7;
        uint32_t f = (fv[i] >> clampi(sh, 0, 31)) << clampi(-sh, 0, 31);
        f = sh >= 32 ? 0u : f;
        res += (v[i] >> 31) ? -(int)f : (int)f;
    }
    const uint32_t sg = res < 0 ? 0x80000000u : 0u;
    uint32_t r = (uint32_t)(res < 0 ? -res : res);
    const int n = 30 - msb(r);
    r = (r << ((uint32_t)n & 31u)) >> 7;
    er -= n;
    uint32_t o = sg | ((uint32_t)clampi(er, 0, 0xfe) << 23) | (r & 0x7fffffu);
    o = er <= 0 ? sg : o;
    o = er >= 0xff ? (sg | 0x7f7fffffu) : o;
    o = res == 0 ? 0u : o;
    o = ninf ? 0xff800000u : o;
    o = pinf ? 0x7f800000u : o;
    o = (pinf && ninf) || nan ? 0x7fffffffu : o;
    return o;
}

uint32_t hle_xf_rcp(uint32_t x, bool rcc)
{
    const uint32_t sx = x & 0x80000000u;
    uint32_t fx = x & 0x7fffffu;
    const int ex = (int)((x >> 23) & 0xffu);
    if (ex == 0xff && fx) {
        return 0x7fffffffu;
    }
    if (ex == 0 && !rcc) {
        return sx | 0x7f800000u;
    }
    int er = 0xfe - ex;
    uint32_t fr = 0;
    if (fx) {
        fx += 0x800000u;
        const uint32_t s0 = k_rcp_lut[(fx >> 17) & 0x3fu];
        uint32_t h, l;
        umul(0x80000000u - s0 * fx, s0, &h, &l);
        const uint32_t s1 = (h << 8) | (l >> 24);
        umul(s1, fx, &h, &l);
        const uint32_t bl = 0u - l, bh = 0x20u - h - (l ? 1u : 0u);
        uint32_t h0, w0, h1, l1;
        umul(bl, s1, &h0, &w0);
        umul(bh, s1, &h1, &l1);
        fr = (((l1 + h0) << 7) | (w0 >> 25)) - 0x800000u;
        er--;
    }
    if (rcc) {
        if (er < 0x3f) {
            er = 0x3f;
            fr = 0;
        }
        if (er >= 0xbf) {
            er = 0xbf;
            fr = 0;
        }
    } else if (er <= 0) {
        er = 0;
        fr = 0;
    }
    return sx | ((uint32_t)er << 23) | fr;
}

uint32_t hle_xf_rsq(uint32_t x)
{
    uint32_t fx = x & 0x7fffffu;
    const int ex = (int)((x >> 23) & 0xffu);
    if (ex == 0xff && fx) {
        return 0x7fffffffu;
    }
    if (ex == 0) {
        return 0x7f800000u;
    }
    if (ex == 0xff) {
        return 0;
    }
    int er;
    if (ex & 1) {
        er = 0x7e - (ex - 0x7f) / 2;
    } else {
        er = 0x7e - (ex - 0x80) / 2;
        fx |= 0x800000u;
    }
    const uint32_t s0 = k_rsq_lut[(fx >> 17) & 0x7fu];
    if (fx >= 0x800000u) {
        fx *= 2u;
    } else {
        fx += 0x800000u;
    }
    uint32_t h, l;
    umul(s0 * s0, fx, &h, &l);
    const uint32_t al = 0u - l, ah = 0x60u - h - (l ? 1u : 0u);
    umul(al, s0, &h, &l);
    const uint32_t s1 = ah * s0 + h;
    umul(s1 * s1, fx, &h, &l);
    const uint32_t bl = 0u - l, bh = 0x60000u - h - (l ? 1u : 0u);
    uint32_t h0, w0, h1, l1;
    umul(bl, s1, &h0, &w0);
    umul(bh, s1, &h1, &l1);
    const uint32_t w1 = l1 + h0, w2 = h1 + (w1 < l1 ? 1u : 0u);
    return ((uint32_t)er << 23) | (((w2 << 25) | (w1 >> 7)) - 0x800000u);
}

/* ---- a program run ---- */

static uint32_t fb(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static float bf(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

typedef struct {
    const float *v;
    float *c;
    bool *cwrote;
    float *out;                 /* 13 x 4; oPos is R12 */
    float r[12][4];
    int a0;
} Xf;

static void read_in(const Xf *x, const Nv2aVshInput *in, float d[4])
{
    const float *s;
    static const float k_zero[4];
    switch (in->type) {
    case NV2ART_TEMPORARY:
        s = in->index == 12 ? x->out : in->index < 12 ? x->r[in->index] : k_zero;
        break;
    case NV2ART_INPUT:
        s = x->v + 4 * (in->index & 15);
        break;
    case NV2ART_CONTEXT: {
        int i = (int)in->index;
        if (in->is_relative) {
            i += x->a0;
            i = i < 0 ? 0 : i > 191 ? 191 : i;
        } else {
            i %= 192;
        }
        s = x->c + 4 * i;
        break;
    }
    default:
        s = k_zero;
        break;
    }
    for (int k = 0; k < 4; k++) {
        const uint32_t u = fb(s[in->swizzle[k] & 3]);
        d[k] = bf(in->is_negated ? u ^ 0x80000000u : u);
    }
}

static void vec(float d[4], uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
    d[0] = bf(x);
    d[1] = bf(y);
    d[2] = bf(z);
    d[3] = bf(w);
}

/* GLSL's min / max / lessThan as the GPU's (y < x ? y : x) */
static float gmin(float a, float b) { return b < a ? b : a; }
static float gmax(float a, float b) { return a < b ? b : a; }

static void op(const Xf *x, const Nv2aVshOperation *o, float d[4])
{
    float a[4], b[4], c[4];
    read_in(x, &o->inputs[0], a);
    read_in(x, &o->inputs[1], b);
    read_in(x, &o->inputs[2], c);
    uint32_t s;
    switch (o->opcode) {
    case NV2AOP_MOV:
    case NV2AOP_ARL:
        memcpy(d, a, 16);
        break;
    case NV2AOP_MUL:
        vec(d, hle_xf_mul(fb(a[0]), fb(b[0])), hle_xf_mul(fb(a[1]), fb(b[1])), hle_xf_mul(fb(a[2]), fb(b[2])),
            hle_xf_mul(fb(a[3]), fb(b[3])));
        break;
    case NV2AOP_ADD:
        vec(d, hle_xf_sum(fb(a[0]), fb(b[0]), 0, 0), hle_xf_sum(fb(a[1]), fb(b[1]), 0, 0),
            hle_xf_sum(fb(a[2]), fb(b[2]), 0, 0), hle_xf_sum(fb(a[3]), fb(b[3]), 0, 0));
        break;
    case NV2AOP_MAD:
        vec(d, hle_xf_sum(hle_xf_mul(fb(a[0]), fb(b[0])), fb(c[0]), 0, 0),
            hle_xf_sum(hle_xf_mul(fb(a[1]), fb(b[1])), fb(c[1]), 0, 0),
            hle_xf_sum(hle_xf_mul(fb(a[2]), fb(b[2])), fb(c[2]), 0, 0),
            hle_xf_sum(hle_xf_mul(fb(a[3]), fb(b[3])), fb(c[3]), 0, 0));
        break;
    case NV2AOP_DP3:
        s = hle_xf_sum(hle_xf_mul(fb(a[0]), fb(b[0])), hle_xf_mul(fb(a[1]), fb(b[1])),
                       hle_xf_mul(fb(a[2]), fb(b[2])), 0);
        vec(d, s, s, s, s);
        break;
    case NV2AOP_DP4:
        s = hle_xf_sum(hle_xf_mul(fb(a[0]), fb(b[0])), hle_xf_mul(fb(a[1]), fb(b[1])),
                       hle_xf_mul(fb(a[2]), fb(b[2])), hle_xf_mul(fb(a[3]), fb(b[3])));
        vec(d, s, s, s, s);
        break;
    case NV2AOP_DPH:
        s = hle_xf_sum(hle_xf_mul(fb(a[0]), fb(b[0])), hle_xf_mul(fb(a[1]), fb(b[1])),
                       hle_xf_mul(fb(a[2]), fb(b[2])), hle_xf_mul(0x3f800000u, fb(b[3])));
        vec(d, s, s, s, s);
        break;
    case NV2AOP_DST:
        vec(d, 0x3f800000u, hle_xf_mul(fb(a[1]), fb(b[1])), fb(a[2]), fb(b[3]));
        break;
    case NV2AOP_MIN:
        for (int k = 0; k < 4; k++) {
            d[k] = gmin(a[k], b[k]);
        }
        break;
    case NV2AOP_MAX:
        for (int k = 0; k < 4; k++) {
            d[k] = gmax(a[k], b[k]);
        }
        break;
    case NV2AOP_SLT:
        for (int k = 0; k < 4; k++) {
            d[k] = a[k] < b[k] ? 1.0f : 0.0f;
        }
        break;
    case NV2AOP_SGE:
        for (int k = 0; k < 4; k++) {
            d[k] = a[k] >= b[k] ? 1.0f : 0.0f;
        }
        break;
    case NV2AOP_RCP:
        s = hle_xf_rcp(fb(a[0]), false);
        vec(d, s, s, s, s);
        break;
    case NV2AOP_RCC:
        s = hle_xf_rcp(fb(a[0]), true);
        vec(d, s, s, s, s);
        break;
    case NV2AOP_RSQ:
        s = hle_xf_rsq(fb(a[0]));
        vec(d, s, s, s, s);
        break;
    case NV2AOP_EXP: {                  /* (vsh.c k_lib exp4) */
        const float f = floorf(a[0]);
        d[0] = exp2f(f);
        d[1] = a[0] - f;
        d[2] = exp2f(a[0]);
        d[3] = 1.0f;
        break;
    }
    case NV2AOP_LOG: {                  /* (k_lib log4) */
        const float m = fabsf(a[0]);
        if (m == 0.0f) {
            d[0] = d[2] = -INFINITY;
            d[1] = d[3] = 1.0f;
        } else {
            const float e = floorf(log2f(m));
            d[0] = e;
            d[1] = m / exp2f(e);
            d[2] = log2f(m);
            d[3] = 1.0f;
        }
        break;
    }
    case NV2AOP_LIT: {                  /* (k_lib lit4) */
        const float px = gmax(a[0], 0.0f), py = gmax(a[1], 0.0f);
        const float w = a[3] < -127.9961f ? -127.9961f : a[3] > 127.9961f ? 127.9961f : a[3];
        d[0] = 1.0f;
        d[1] = px;
        d[2] = px > 0.0f ? exp2f(w * log2f(py)) : 0.0f;
        d[3] = 1.0f;
        break;
    }
    default:
        d[0] = d[1] = d[2] = d[3] = 0.0f;
        break;
    }
}

static void masked(float *dst, const float s[4], unsigned m)
{
    if (m & 8) dst[0] = s[0];
    if (m & 4) dst[1] = s[1];
    if (m & 2) dst[2] = s[2];
    if (m & 1) dst[3] = s[3];
}

/* (vsh.c out_write) */
static void write_out(Xf *x, const Nv2aVshOutput *o, const float s[4])
{
    const unsigned m = (unsigned)o->writemask;
    if (!m) {
        return;
    }
    switch (o->type) {
    case NV2ART_TEMPORARY:
        if (o->index == 12) {
            masked(x->out, s, m);
        } else {
            masked(x->r[o->index % 12], s, m);
        }
        break;
    case NV2ART_OUTPUT:
        if ((o->index & 15) == 5) {
            x->out[4 * 5] = s[0];       /* oFog: the first component, whatever the mask */
        } else if ((o->index & 15) < 13) {
            masked(x->out + 4 * (o->index & 15), s, m);
        }
        break;
    case NV2ART_CONTEXT:
        if (o->index < 192) {
            masked(x->c + 4 * o->index, s, m);
            if (x->cwrote) {
                x->cwrote[o->index] = true;
            }
        }
        break;
    case NV2ART_ADDRESS:
        x->a0 = (int)floorf(s[0] + 1.0f / 1024.0f);
        break;
    default:
        break;
    }
}

void hle_xf_run(const Nv2aVshProgram *prog, const float *v, float *c, bool *cwrote, float *out)
{
    Xf x;
    memset(&x, 0, sizeof(x));
    x.v = v;
    x.c = c;
    x.cwrote = cwrote;
    x.out = out;
    const Nv2aVshStep *st = prog->steps;
    for (unsigned n = 0; st && n < 136; n++, st++) {     /* (HLE_VSH_MAX_SLOTS) */
        const bool mac = st->mac.opcode != NV2AOP_NOP, ilu = st->ilu.opcode != NV2AOP_NOP;
        float m[4], i[4];
        if (mac) {
            op(&x, &st->mac, m);
        }
        if (ilu) {
            op(&x, &st->ilu, i);
        }
        for (int k = 0; mac && k < 2; k++) {
            write_out(&x, &st->mac.outputs[k], m);
        }
        for (int k = 0; ilu && k < 2; k++) {
            write_out(&x, &st->ilu.outputs[k], i);
        }
        if (st->is_final) {
            break;
        }
    }
}

/* ---- hle_xf_fog_slice ----
 * A component's bit among a program's temporaries (r0 - r11: 48), outputs
 * (o0 - o12, r12 being o0: 52) and A0, as read_in and write_out reach them */
typedef struct { uint64_t t, o; bool a; } Live;

static unsigned comps(unsigned wm)          /* a write mask's components, x as bit 0 */
{
    return ((wm >> 3) & 1) | ((wm >> 1) & 2) | ((wm << 1) & 4) | ((wm << 3) & 8);
}

static void live_reads(const Nv2aVshOperation *op, Live *l)
{
    for (unsigned i = 0; i < 3; i++) {
        const Nv2aVshInput *in = &op->inputs[i];
        unsigned c = 0;
        for (unsigned k = 0; k < 4; k++) {
            c |= 1u << (in->swizzle[k] & 3);
        }
        if (in->type == NV2ART_TEMPORARY && in->index == 12) {
            l->o |= c;
        } else if (in->type == NV2ART_TEMPORARY && in->index < 12) {
            l->t |= (uint64_t)c << (4 * in->index);
        } else if (in->type == NV2ART_CONTEXT && in->is_relative) {
            l->a = true;
        }
    }
}

static bool live_writes(const Nv2aVshOperation *op, Live *l)
{
    for (unsigned k = 0; k < 2; k++) {
        const Nv2aVshOutput *o = &op->outputs[k];
        const unsigned c = comps((unsigned)o->writemask);
        if (!c) {
            continue;
        }
        if (o->type == NV2ART_TEMPORARY) {
            if (o->index == 12) {
                l->o |= c;
            } else {
                l->t |= (uint64_t)c << (4 * (o->index % 12));
            }
        } else if (o->type == NV2ART_OUTPUT) {
            const unsigned r = o->index & 15;
            if (r == 5) {
                l->o |= 1ull << 20;             /* (oFog's first component, whatever the mask) */
            } else if (r < 13) {
                l->o |= (uint64_t)c << (4 * r);
            }
        } else if (o->type == NV2ART_ADDRESS) {
            l->a = true;
        } else if (o->type == NV2ART_CONTEXT) {
            return false;
        }
    }
    return true;
}

bool hle_xf_fog_slice(const Nv2aVshProgram *prog, uint32_t ns, Nv2aVshProgram *slice)
{
    slice->steps = NULL;
    if (!prog->steps || !ns) {
        return false;
    }
    uint8_t keep[136];                          /* (HLE_VSH_MAX_SLOTS) */
    uint32_t last = 0;
    while (last + 1 < ns && last + 1 < 136 && !prog->steps[last].is_final) {
        last++;
    }
    Live live = { .o = 1ull << 20 };
    unsigned nk = 0;
    for (int s = (int)last; s >= 0; s--) {
        const Nv2aVshStep *st = &prog->steps[s];
        Live wm = { 0 }, wi = { 0 };
        if (!live_writes(&st->mac, &wm) || !live_writes(&st->ilu, &wi)) {
            return false;
        }
        const bool nm = st->mac.opcode != NV2AOP_NOP && ((wm.t & live.t) || (wm.o & live.o) || (wm.a && live.a));
        const bool ni = st->ilu.opcode != NV2AOP_NOP && ((wi.t & live.t) || (wi.o & live.o) || (wi.a && live.a));
        keep[s] = (uint8_t)(nm | ni << 1);
        nk += keep[s] != 0;
        /* (both operations read before either writes: the writes leave first) */
        if (nm) {
            live.t &= ~wm.t;
            live.o &= ~wm.o;
            live.a = live.a && !wm.a;
        }
        if (ni) {
            live.t &= ~wi.t;
            live.o &= ~wi.o;
            live.a = live.a && !wi.a;
        }
        if (nm) {
            live_reads(&st->mac, &live);
        }
        if (ni) {
            live_reads(&st->ilu, &live);
        }
    }
    if (!nk) {
        return true;
    }
    Nv2aVshStep *out = malloc(sizeof(*out) * nk);
    if (!out) {
        return false;
    }
    unsigned n = 0;
    for (uint32_t s = 0; s <= last; s++) {
        if (!keep[s]) {
            continue;
        }
        out[n] = prog->steps[s];
        if (!(keep[s] & 1)) {
            out[n].mac.opcode = NV2AOP_NOP;
        }
        if (!(keep[s] & 2)) {
            out[n].ilu.opcode = NV2AOP_NOP;
        }
        out[n].is_final = false;
        n++;
    }
    out[n - 1].is_final = true;
    slice->steps = out;
    return true;
}
