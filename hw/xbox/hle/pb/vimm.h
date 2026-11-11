/*
 * hakuX HLE GPU — Kelvin's immediate vertex data: the methods that set a
 * vertex attribute's current value.
 *
 * The NV2A keeps a current value for each of its 16 vertex attributes (0
 * position, 1 weights, 2 normal, 3 diffuse, 4 specular, 5 fog coordinate,
 * 9-12 texture coordinates). An attribute without an array takes it at a
 * draw; between SET_BEGIN_END's, the position's last word emits a vertex made
 * of all of them. Three families of methods write them:
 *   SET_VERTEX3F / SET_VERTEX4F         the position
 *   SET_NORMAL3F .. SET_WEIGHT4F        the fixed-function attributes by name
 *   SET_VERTEX_DATA2F_M .. 4F_M         any attribute by its index
 * A method between two of them is none (the numbers between SET_VERTEX3F's
 * three words and SET_VERTEX4F, between a texture coordinate's forms...).
 */
#ifndef HW_XBOX_HLE_PB_VIMM_H
#define HW_XBOX_HLE_PB_VIMM_H

#include <stdbool.h>
#include <stdint.h>

#define HLE_VIMM_ATTRS 16

/* The word v of method m into the attributes' current values. Returns the
 * attribute it changed, or -1 when m is not one of these methods (cur
 * untouched). *vertex: the word was the position's last. */
int hle_vimm_method(uint32_t m, uint32_t v, float cur[HLE_VIMM_ATTRS][4], bool *vertex);

/* A draw leaves each attribute it took from an array at the value of its last
 * vertex: that is the attribute's current value from then on, as if the
 * immediate methods had set it (an attribute without an array in a later
 * draw takes it). The bytes of one element of an array of format fmt (the
 * low byte of SET_VERTEX_DATA_ARRAY_FORMAT: type 3:0, count 7:4) as that
 * value: components the array lacks are 0, 0, 0, 1. False for a format that
 * is none (cur untouched). hle_vimm_element_bytes: the element's size, 0
 * for none (at most 16). */
bool hle_vimm_element(uint32_t fmt, const uint8_t *p, float out[4]);
uint32_t hle_vimm_element_bytes(uint32_t fmt);

/* The most a vertex array's stride can be: what every host draws with (Vulkan's least
 * maxVertexInputBindingStride). D3D's are a vertex's bytes, under 256. */
#define HLE_VIMM_STRIDE_MAX 2048u

/* The bytes of a vertex array that a draw's indices lo .. hi reach: from vertex lo of the
 * array at base (stride bytes a vertex; 0: one vertex for all of them) to the end of vertex
 * hi's attributes (end: their bytes from a vertex's start). False when the stride is more than
 * a host draws with or the bytes are not memory the console has (ram bytes). Counted in 64
 * bits: a stride is 24 bits of a register and an index 32, and their product in 32 was the
 * span of some other place and size. */
bool hle_vimm_span(uint32_t base, uint32_t stride, uint32_t lo, uint32_t hi, uint32_t end,
                    uint64_t ram, uint32_t *from, uint32_t *len);

#endif
