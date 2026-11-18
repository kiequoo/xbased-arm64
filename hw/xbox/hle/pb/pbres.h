/*
 * xbased HLE GPU — push-buffer mode's resources (pbres.c): texels into the
 * upload arena for the back end, render targets' rows the CPU wrote,
 * readbacks landing in guest memory, and the primitive conversions.
 */
#ifndef HW_XBOX_HLE_PB_PBRES_H
#define HW_XBOX_HLE_PB_PBRES_H

#include "hw/xbox/hle/front/cmd.h"

/* Arena space for the next submission; HLE_ARENA_FULL if none. */
uint32_t hle_draw_arena_alloc(uint32_t len, uint8_t **ptr);
/* The same, 16 bytes aligned instead of 256 (a draw's indices). */
uint32_t hle_draw_arena_alloc16(uint32_t len, uint8_t **ptr);
/* A draw's arena space from its first block to its command (on), then (off)
 * that space tagged with the submission the draw went out in */
void hle_draw_arena_hold(bool on);
/* The texels of header words h5 (P8: palette pal, npal entries) defined as
 * object id's image (HLE_OP_TEX); id, or 0 if they can't be. *bytes: the
 * span of guest memory they came from. pitch: a linear format's rows as the
 * stage's register has them (0: the header's, a multiple of 64). */
uint32_t hle_draw_tex_define(uint32_t id, const uint32_t h5[5], uint32_t pitch, const uint32_t *pal,
                             uint32_t npal, uint32_t *bytes);
/* The span of guest memory header words h5's texels take (0: not a texture;
 * pitch as hle_draw_tex_define's) */
uint32_t hle_draw_tex_bytes(const uint32_t h5[5], uint32_t pitch);
/* The video overlay's surface (header words h5, YUY2 / UYVY) as B8G8R8A8 in
 * the arena: its offset, HLE_ARENA_FULL = none. */
uint32_t hle_draw_overlay_upload(const uint32_t h5[5], uint32_t *w, uint32_t *h);
/* NV2A / Xbox D3D primitive types (1 points .. 10 polygon): the Vulkan
 * topology they are drawn with, the index count after conversion, and the
 * converted indices (in NULL: 0..n-1; rebased by -base). */
/* SET_BEGIN_END's primitives (1 points .. 10 polygon), and 11 / 12: a quad
 * list's / strip's sides as lines (SET_FRONT_POLYGON_MODE GL_LINE: the NV2A
 * draws a quad's outline, not the triangles' diagonals) */
#define HLE_PT_QUADLIST_EDGES  11u
#define HLE_PT_QUADSTRIP_EDGES 12u
/* 13 .. 16: a triangle list, strip or fan whose flat shade takes each
 * triangle's last vertex (SET_FLAT_SHADE_OP VERTEX_LAST), and a polygon,
 * whose flat shade takes its first vertex whatever the op: as triangle
 * lists with that vertex first */
#define HLE_PT_TRILIST_LAST    13u
#define HLE_PT_TRISTRIP_LAST   14u
#define HLE_PT_TRIFAN_LAST     15u
#define HLE_PT_POLYGON_HUB     16u
/* 17, 18: the quads of a quad list / strip, flat shaded: four vertices each
 * (lines with adjacency: a geometry stage draws the two triangles with the
 * colours of the last vertex of the quad, the one the NV2A takes whatever the op) */
#define HLE_PT_QUADLIST_FLAT   17u
#define HLE_PT_QUADSTRIP_FLAT  18u
/* 19 .. 21: the sides of a flat-shaded quad list, quad strip or polygon
 * (GL_LINE mode), a triangle each: the vertex whose colours the primitive
 * takes, then the side (HleCmdDraw.quads 2: the geometry stage draws that side) */
#define HLE_PT_QUADLIST_FLATEDGES  19u
#define HLE_PT_QUADSTRIP_FLATEDGES 20u
#define HLE_PT_POLYGON_FLATEDGES   21u
/* 23: a polygon's sides as lines (GL_LINE mode), in the order the NV2A draws them:
 * 1 - 2, 0 - 1, 2 - 3 .. n-1 - 0 (the sides of its triangles 0, i, i + 1, each
 * 1 - 2, 2 - 0, 0 - 1 without the diagonals; the last drawn is on top where two
 * overlap at a corner: nxdk_pgraph_tests' Line_width). A quad list's go the same
 * way (b c, a b, c d, d a) and a strip's quad d a, a b, b c, c d (its triangles
 * q, q + 1, q + 2 and q + 2, q + 1, q + 3). */
#define HLE_PT_POLYGON_EDGES       23u
/* 22: a triangle or quad strip as a triangle list, an odd triangle's first
 * two vertices swapped (the winding): the order the NV2A weights its
 * colours in (psh.c nvcol; Vulkan's own strips give an odd triangle's
 * corners as i, i + 2, i + 1) */
#define HLE_PT_TRISTRIP_LIST       22u
uint32_t hle_draw_prim_topology(uint32_t pt);
uint32_t hle_draw_prim_count(uint32_t pt, uint32_t n);
void hle_draw_prim_indices(uint32_t pt, const uint16_t *in, uint32_t n, uint32_t base,
                           uint16_t *out);

/* Texels the CPU wrote at pa (linear rows of pitch bytes) into object id's
 * image: rects of source pixels, each to its pts[i] (HLE_OP_UPLOAD). */
void hle_res_upload(uint32_t id, uint32_t pa, uint32_t pitch, uint32_t n,
                    const HleRect *rects, const int32_t (*pts)[2]);
/* A direct readback (HLE_RB_DIRECT) when the GPU can't write guest RAM (not
 * imported: the native core's): hle_res_rb_fb reserves len bytes of arena
 * for the back end's copy (0: none needed, or no room: the readback is
 * skipped and guest memory stays), before the command (a full arena kicks
 * one); hle_res_rb_land, between the command's begin and commit, has the
 * copy land at rb->pa once the GPU reaches value. */
uint32_t hle_res_rb_fb(uint32_t len);
void hle_res_rb_land(HleCmdReadback *rb, uint32_t fb, uint32_t len, uint64_t value);
/* len bytes of the landing buffer (the arena without one) for a packed
 * readback (HLE_RB_PACKED): its fb (offset + 1), 0 if there's no room;
 * reserved before the command's value is taken (hle_fe_next_value). */
uint32_t hle_res_land_alloc(uint32_t len);
/* Whether the GPU writes guest RAM in place (zero-copy): a direct readback
 * needs no hle_res_rb_fb then. */
bool hle_res_ram_direct(void);
/* The depth word's byte for each channel (HleCmdZRead.map) of a 32-bit
 * linear colour format read over a D24S8 buffer; false for other formats. */
bool hle_res_zread_map(uint32_t xfmt, uint8_t map[4]);

#endif
