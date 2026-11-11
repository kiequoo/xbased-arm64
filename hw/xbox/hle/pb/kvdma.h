/*
 * hakuX HLE GPU — Kelvin's DMA contexts: the offsets its methods carry are
 * offsets inside the DMA objects its context methods name.
 *
 * D3D's objects all start at address 0, so an offset is the address. Code
 * that programs the chip itself need not keep to that: pbkit points the
 * colour and depth contexts at the frame it draws (it rewrites the object's
 * base in instance memory, names the object again, and pushes offset 0). The
 * register shadows downstream hold addresses, so the methods are rewritten
 * here on their way to them: an offset becomes its object's base plus the
 * offset, and a context named again with another base sends the offsets
 * that live in it once more.
 */
#ifndef HW_XBOX_HLE_PB_KVDMA_H
#define HW_XBOX_HLE_PB_KVDMA_H

#include <stdbool.h>
#include <stdint.h>

enum {
    HLE_KVDMA_A, HLE_KVDMA_B,           /* textures (a stage's format, bits 1:0: 1 A, 2 B) */
    HLE_KVDMA_COLOR, HLE_KVDMA_ZETA,
    HLE_KVDMA_VTX_A, HLE_KVDMA_VTX_B,   /* vertex arrays (an offset's bit 31: B) */
    HLE_KVDMA_N,
};

/* The most methods one method becomes (a vertex context and its 16 arrays). */
#define HLE_KVDMA_OUT 17

typedef struct HleKvDma {
    uint32_t base[HLE_KVDMA_N];
    uint32_t color, zeta;               /* offsets as pushed */
    uint32_t tex[4];
    uint32_t vtx[16];
    uint8_t tex_ctx[4];                 /* HLE_KVDMA_A or _B */
    uint32_t have;                      /* offsets pushed: bit 0 colour, 1 zeta, 4+s texture, 8+a array */
} HleKvDma;

typedef struct HleKvDmaOut {
    uint32_t m, v;
} HleKvDmaOut;

/* The context a Kelvin method names (SET_CONTEXT_DMA_A .. _VERTEX_B), or -1. */
int hle_kvdma_context(uint32_t m);

/* Whether hle_kvdma_method rewrites m (a context of those, or an offset). */
bool hle_kvdma_acts(uint32_t m);

/* Kelvin method m with value v, of those hle_kvdma_acts names. base: for a
 * context method, the base of the object v names (0: none, or at 0). Fills
 * out with the methods to run in its place, in order, and returns how many
 * (1 to HLE_KVDMA_OUT): the method itself, an offset as an address, and
 * with a context whose base changed the offsets inside it again. */
unsigned hle_kvdma_method(HleKvDma *d, uint32_t m, uint32_t v, uint32_t base, HleKvDmaOut *out);

/* After the shadows were restored (a savestate): the contexts' bases, and
 * the offsets from the addresses the shadow holds. reg: the shadow's value
 * of a method. */
void hle_kvdma_restore(HleKvDma *d, const uint32_t base[HLE_KVDMA_N], uint32_t (*reg)(uint32_t m));

#endif
