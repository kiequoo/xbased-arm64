/*
 * hakuX HLE GPU — Kelvin's DMA contexts (kvdma.h).
 */
#include "hw/xbox/hle/pb/kvdma.h"

#include <string.h>

#define NV097_SET_CONTEXT_DMA_A         0x0184u
#define NV097_SET_CONTEXT_DMA_B         0x0188u
#define NV097_SET_CONTEXT_DMA_COLOR     0x0194u
#define NV097_SET_CONTEXT_DMA_ZETA      0x0198u
#define NV097_SET_CONTEXT_DMA_VERTEX_A  0x019Cu
#define NV097_SET_CONTEXT_DMA_VERTEX_B  0x01A0u
#define NV097_SET_SURFACE_COLOR_OFFSET  0x0210u
#define NV097_SET_SURFACE_ZETA_OFFSET   0x0214u
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720u  /* 16 of them */
#define NV097_SET_TEXTURE_OFFSET        0x1B00u     /* stage s at + 64 s */
#define NV097_SET_TEXTURE_FORMAT        0x1B04u

#define HAVE_COLOR 1u
#define HAVE_ZETA  2u
#define HAVE_TEX(s) (1u << (4 + (s)))
#define HAVE_VTX(a) (1u << (8 + (a)))

int hle_kvdma_context(uint32_t m)
{
    switch (m) {
    case NV097_SET_CONTEXT_DMA_A:        return HLE_KVDMA_A;
    case NV097_SET_CONTEXT_DMA_B:        return HLE_KVDMA_B;
    case NV097_SET_CONTEXT_DMA_COLOR:    return HLE_KVDMA_COLOR;
    case NV097_SET_CONTEXT_DMA_ZETA:     return HLE_KVDMA_ZETA;
    case NV097_SET_CONTEXT_DMA_VERTEX_A: return HLE_KVDMA_VTX_A;
    case NV097_SET_CONTEXT_DMA_VERTEX_B: return HLE_KVDMA_VTX_B;
    default:                             return -1;
    }
}

static bool tex_method(uint32_t m, uint32_t which, unsigned *s)
{
    if (m < 0x1B00 || m >= 0x1C00 || (m & 0x3C) != (which & 0x3C)) {
        return false;
    }
    *s = (m - 0x1B00) >> 6;
    return true;
}

bool hle_kvdma_acts(uint32_t m)
{
    unsigned s;
    return hle_kvdma_context(m) >= 0 || m == NV097_SET_SURFACE_COLOR_OFFSET ||
           m == NV097_SET_SURFACE_ZETA_OFFSET ||
           (m >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET && m < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 64) ||
           tex_method(m, NV097_SET_TEXTURE_OFFSET, &s) || tex_method(m, NV097_SET_TEXTURE_FORMAT, &s);
}

/* An array's offset as an address: bit 31 (its context) stays where the
 * draw path masks it off. */
static uint32_t vtx_addr(const HleKvDma *d, uint32_t v)
{
    const uint32_t base = d->base[v >> 31 ? HLE_KVDMA_VTX_B : HLE_KVDMA_VTX_A];
    return ((v + base) & 0x7FFFFFFFu) | (v & 0x80000000u);
}

static uint32_t tex_addr(const HleKvDma *d, unsigned s)
{
    return d->tex[s] + d->base[d->tex_ctx[s]];
}

static unsigned put(HleKvDmaOut *out, unsigned n, uint32_t m, uint32_t v)
{
    out[n].m = m;
    out[n].v = v;
    return n + 1;
}

unsigned hle_kvdma_method(HleKvDma *d, uint32_t m, uint32_t v, uint32_t base, HleKvDmaOut *out)
{
    unsigned n = 0, s;
    const int c = hle_kvdma_context(m);
    if (c >= 0) {
        n = put(out, n, m, v);
        if (d->base[c] == base) {
            return n;
        }
        d->base[c] = base;
        switch (c) {
        case HLE_KVDMA_COLOR:
            if (d->have & HAVE_COLOR) {
                n = put(out, n, NV097_SET_SURFACE_COLOR_OFFSET, d->color + base);
            }
            break;
        case HLE_KVDMA_ZETA:
            if (d->have & HAVE_ZETA) {
                n = put(out, n, NV097_SET_SURFACE_ZETA_OFFSET, d->zeta + base);
            }
            break;
        case HLE_KVDMA_A:
        case HLE_KVDMA_B:
            for (s = 0; s < 4; s++) {
                if ((d->have & HAVE_TEX(s)) && d->tex_ctx[s] == c) {
                    n = put(out, n, NV097_SET_TEXTURE_OFFSET + 64 * s, tex_addr(d, s));
                }
            }
            break;
        default:
            for (unsigned a = 0; a < 16; a++) {
                if ((d->have & HAVE_VTX(a)) && (d->vtx[a] >> 31) == (unsigned)(c == HLE_KVDMA_VTX_B)) {
                    n = put(out, n, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * a, vtx_addr(d, d->vtx[a]));
                }
            }
            break;
        }
        return n;
    }
    if (m == NV097_SET_SURFACE_COLOR_OFFSET) {
        d->color = v;
        d->have |= HAVE_COLOR;
        return put(out, n, m, v + d->base[HLE_KVDMA_COLOR]);
    }
    if (m == NV097_SET_SURFACE_ZETA_OFFSET) {
        d->zeta = v;
        d->have |= HAVE_ZETA;
        return put(out, n, m, v + d->base[HLE_KVDMA_ZETA]);
    }
    if (m >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET && m < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 64) {
        const unsigned a = (m - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) >> 2;
        d->vtx[a] = v;
        d->have |= HAVE_VTX(a);
        return put(out, n, m, vtx_addr(d, v));
    }
    if (tex_method(m, NV097_SET_TEXTURE_OFFSET, &s)) {
        d->tex[s] = v;
        d->have |= HAVE_TEX(s);
        return put(out, n, m, tex_addr(d, s));
    }
    if (tex_method(m, NV097_SET_TEXTURE_FORMAT, &s)) {
        /* the stage's context: B for 2, A for the rest (1, and a format not
         * set up yet) */
        const uint8_t ctx = (v & 3) == 2 ? HLE_KVDMA_B : HLE_KVDMA_A;
        if (ctx != d->tex_ctx[s]) {
            const uint32_t old = tex_addr(d, s);
            d->tex_ctx[s] = ctx;
            if ((d->have & HAVE_TEX(s)) && tex_addr(d, s) != old) {
                n = put(out, n, NV097_SET_TEXTURE_OFFSET + 64 * s, tex_addr(d, s));
            }
        }
    }
    return put(out, n, m, v);
}

void hle_kvdma_restore(HleKvDma *d, const uint32_t base[HLE_KVDMA_N], uint32_t (*reg)(uint32_t m))
{
    memset(d, 0, sizeof(*d));
    memcpy(d->base, base, sizeof(d->base));
    d->color = reg(NV097_SET_SURFACE_COLOR_OFFSET) - base[HLE_KVDMA_COLOR];
    d->zeta = reg(NV097_SET_SURFACE_ZETA_OFFSET) - base[HLE_KVDMA_ZETA];
    d->have = HAVE_COLOR | HAVE_ZETA;
    for (unsigned s = 0; s < 4; s++) {
        d->tex_ctx[s] = (reg(NV097_SET_TEXTURE_FORMAT + 64 * s) & 3) == 2 ? HLE_KVDMA_B : HLE_KVDMA_A;
        d->tex[s] = reg(NV097_SET_TEXTURE_OFFSET + 64 * s) - base[d->tex_ctx[s]];
        d->have |= HAVE_TEX(s);
    }
    for (unsigned a = 0; a < 16; a++) {
        const uint32_t v = reg(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * a);
        const uint32_t b = base[v >> 31 ? HLE_KVDMA_VTX_B : HLE_KVDMA_VTX_A];
        d->vtx[a] = ((v - b) & 0x7FFFFFFFu) | (v & 0x80000000u);
        d->have |= HAVE_VTX(a);
    }
}
