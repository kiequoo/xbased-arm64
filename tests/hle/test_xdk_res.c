#include "hw/xbox/hle/tex/xdk_res.h"
#include "hle_test.h"

static void test_texture_full_chain(void)
{
    HleResDesc d = { .kind = HLE_RK_TEXTURE, .fmt = HLE_XFMT_A8R8G8B8,
                     .w = 256, .h = 256, .d = 1, .levels = 0 };
    HleResInfo ri;
    HT_CHECK(hle_res_info(&d, &ri));
    HT_EQ_INT(ri.levels, 9);
    HT_EQ_U32(ri.bytes, 349524);
    HT_EQ_U32(ri.common, 0x01040001);
    HT_EQ_U32(ri.format, 0x08890629);
    HT_EQ_U32(ri.size, 0);
    HT_EQ_INT(ri.hdr_bytes, 20);
    HT_CHECK(ri.contiguous);
    uint32_t pitch = 0;
    HT_EQ_U32(hle_res_subresource(&d, 0, 2, &pitch), 327680);
    HT_EQ_U32(pitch, 256);
    HleResDesc back;
    HT_CHECK(hle_res_from_header(ri.common, ri.format, ri.size, &back));
    HT_EQ_INT(back.kind, HLE_RK_TEXTURE);
    HT_EQ_U32(back.w, 256);
    HT_EQ_U32(back.h, 256);
    HT_EQ_U32(back.levels, 9);
}

static void test_dxt1_nonsquare(void)
{
    HleResDesc d = { .kind = HLE_RK_TEXTURE, .fmt = HLE_XFMT_DXT1,
                     .w = 64, .h = 32, .d = 1, .levels = 0 };
    HleResInfo ri;
    HT_CHECK(hle_res_info(&d, &ri));
    HT_EQ_INT(ri.levels, 7);
    HT_EQ_U32(ri.bytes, 1384);
    HT_EQ_U32(ri.format, 0x05670C29);
}

static void test_linear_surface(void)
{
    HleResDesc d = { .kind = HLE_RK_SURFACE, .fmt = HLE_XFMT_LIN_X8R8G8B8,
                     .w = 640, .h = 480, .d = 1 };
    HleResInfo ri;
    HT_CHECK(hle_res_info(&d, &ri));
    HT_EQ_U32(ri.pitch, 2560);
    HT_EQ_U32(ri.bytes, 1228800);
    HT_EQ_U32(ri.size, 0x271DF27F);
    HT_EQ_U32(ri.format, 0x00011E29);   /* PGR2's native back buffer (P2 Task 11 dump) */
    HT_EQ_U32(ri.common, 0x01050001);
    HT_EQ_INT(ri.hdr_bytes, 24);
    HleResDesc back;
    HT_CHECK(hle_res_from_header(ri.common, ri.format, ri.size, &back));
    HT_EQ_INT(back.kind, HLE_RK_SURFACE);
    HT_EQ_U32(back.w, 640);
    HT_EQ_U32(back.h, 480);
    HT_EQ_U32(back.pitch, 2560);
}

/* Linear pitch above the XDK minimum (XGSetTextureHeader's Pitch, tiled
 * frame buffers) must survive a header round trip: Lock and level headers
 * decode it from Size. */
static void test_linear_custom_pitch(void)
{
    HleResDesc d = { .kind = HLE_RK_TEXTURE, .fmt = HLE_XFMT_LIN_A8R8G8B8,
                     .w = 640, .h = 480, .d = 1, .pitch = 3072 };
    HleResInfo ri, ri2;
    HT_CHECK(hle_res_info(&d, &ri));
    HT_EQ_U32(ri.pitch, 3072);
    HT_EQ_U32(ri.bytes, 3072 * 480);
    HT_EQ_U32(ri.size, 0x2F1DF27F);             /* 639 | 479 << 12 | 47 << 24 */
    HT_EQ_U32(ri.format, 0x00011229);
    HleResDesc back;
    HT_CHECK(hle_res_from_header(ri.common, ri.format, ri.size, &back));
    HT_EQ_U32(back.pitch, 3072);
    uint32_t pitch = 0;
    HT_EQ_U32(hle_res_subresource(&back, 0, 0, &pitch), 0);
    HT_EQ_U32(pitch, 3072);
    HT_CHECK(hle_res_info(&back, &ri2));
    HT_EQ_U32(ri2.size, ri.size);
    HT_EQ_U32(ri2.bytes, ri.bytes);
    /* Header pitch 4096 on a 640-wide LIN_A8R8G8B8 decoded as 2560 before. */
    HT_CHECK(hle_res_from_header(0x01040001, 0x00011221,
                                 639 | (479u << 12) | (63u << 24), &back));
    HT_EQ_U32(back.pitch, 4096);
    hle_res_subresource(&back, 0, 0, &pitch);
    HT_EQ_U32(pitch, 4096);
    /* A header pitch below the row (2496 < 640 * 4) is rejected. */
    HT_CHECK(!hle_res_from_header(0x01040001, 0x00011221,
                                  639 | (479u << 12) | (38u << 24), &back));
    d.pitch = 3000;                             /* not a multiple of 64 */
    HT_CHECK(!hle_res_info(&d, &ri));
    d.pitch = 2496;                             /* below the minimum */
    HT_CHECK(!hle_res_info(&d, &ri));
    d.pitch = 16448;                            /* past Size[31:24] */
    HT_CHECK(!hle_res_info(&d, &ri));
    d.pitch = 16384;
    HT_CHECK(hle_res_info(&d, &ri));
    HT_EQ_U32(ri.size >> HLE_SIZE_PITCH_SHIFT, 255);
    /* Swizzled formats have no Size pitch: the field is ignored. */
    HleResDesc s = { .kind = HLE_RK_TEXTURE, .fmt = HLE_XFMT_A8R8G8B8,
                     .w = 256, .h = 256, .d = 1, .levels = 1, .pitch = 4096 };
    HT_CHECK(hle_res_info(&s, &ri));
    HT_EQ_U32(ri.pitch, 1024);
    HT_EQ_U32(ri.size, 0);
}

static void test_cube_and_volume(void)
{
    HleResDesc c = { .kind = HLE_RK_CUBE, .fmt = HLE_XFMT_A8R8G8B8,
                     .w = 16, .h = 16, .d = 1, .levels = 1 };
    HleResInfo ri;
    HT_CHECK(hle_res_info(&c, &ri));
    HT_EQ_U32(ri.bytes, 6144);
    HT_EQ_U32(ri.format, 0x0441062D);
    uint32_t pitch = 0;
    HT_EQ_U32(hle_res_subresource(&c, 3, 0, &pitch), 3072);
    HleResDesc v = { .kind = HLE_RK_VOLUME, .fmt = HLE_XFMT_L8,
                     .w = 16, .h = 16, .d = 4, .levels = 0 };
    HT_CHECK(hle_res_info(&v, &ri));
    HT_EQ_INT(ri.levels, 5);
    HT_EQ_U32(ri.bytes, 1173);
    HT_EQ_U32(ri.format, 0x24450039);
}

/* A volume's bytes are counted whole. The largest the header's fields can say (4096 x 4096 x
 * 512 at 32 bits a texel, 32 GiB) is no texture, nor any other past the memory a console has:
 * counted in 32 bits these came to nothing or little, and passed as textures of that size. */
static void test_volume_too_large(void)
{
    HleResInfo ri;
    HleResDesc d;
    const uint32_t fmt = HLE_FMT_DMA_A | HLE_FMT_BORDER_COLOR | (3u << HLE_FMT_DIM_SHIFT) |
                         ((uint32_t)HLE_XFMT_A8R8G8B8 << HLE_FMT_FORMAT_SHIFT) |
                         (1u << HLE_FMT_MIPMAP_SHIFT) | (12u << HLE_FMT_USIZE_SHIFT) |
                         (12u << HLE_FMT_VSIZE_SHIFT) | (9u << HLE_FMT_PSIZE_SHIFT);
    HT_CHECK(hle_res_from_header(HLE_COMMON_TYPE_TEXTURE, fmt, 0, &d));
    HT_EQ_INT(d.kind, HLE_RK_VOLUME);
    HT_EQ_U32(d.w, 4096);
    HT_EQ_U32(d.h, 4096);
    HT_EQ_U32(d.d, 512);
    HT_CHECK(!hle_res_info(&d, &ri));
    static const uint32_t dims[][3] = {
        { 4096, 4096, 16 }, { 2048, 2048, 64 }, { 4096, 2048, 512 }, { 1024, 1024, 256 },
        { 512, 512, 256 },                      /* 256 MiB: no wrap, more than memory */
    };
    for (unsigned i = 0; i < sizeof(dims) / sizeof(dims[0]); i++) {
        for (uint32_t levels = 0; levels < 2; levels++) {
            HleResDesc v = { .kind = HLE_RK_VOLUME, .fmt = HLE_XFMT_A8R8G8B8, .w = dims[i][0],
                             .h = dims[i][1], .d = dims[i][2], .levels = levels };
            HT_CHECK(!hle_res_info(&v, &ri));
            uint32_t pitch = 1;
            HT_EQ_U32(hle_res_subresource(&v, 0, 0, &pitch), 0);
            HT_EQ_U32(pitch, 0);
        }
    }
    /* the largest ones memory holds still are */
    HleResDesc ok = { .kind = HLE_RK_VOLUME, .fmt = HLE_XFMT_A8R8G8B8, .w = 256, .h = 256,
                      .d = 256, .levels = 1 };
    HT_CHECK(hle_res_info(&ok, &ri));
    HT_EQ_U32(ri.bytes, 64u << 20);
    HleResDesc l8 = { .kind = HLE_RK_VOLUME, .fmt = HLE_XFMT_L8, .w = 512, .h = 512, .d = 512,
                      .levels = 1 };
    HT_CHECK(hle_res_info(&l8, &ri));
    HT_EQ_U32(ri.bytes, 128u << 20);
    /* a cube's six faces count */
    HleResDesc cube = { .kind = HLE_RK_CUBE, .fmt = HLE_XFMT_A8R8G8B8, .w = 4096, .h = 4096,
                        .d = 1, .levels = 1 };
    HT_CHECK(!hle_res_info(&cube, &ri));        /* 6 x 64 MiB */
    cube.w = cube.h = 2048;
    HT_CHECK(hle_res_info(&cube, &ri));
    HT_EQ_U32(ri.bytes, 6u * (16u << 20));
}

/* Stranger's Wrath's lighting cubes: XGSetCubeTextureHeader(8, 1, 0,
 * X8R8G8B8, ...) with BORDER_COLOR then cleared (0x1b2317), 0x1800 bytes
 * apart, each face filled as 16x16 swizzled with the image at (4, 4). */
static void test_border_texture(void)
{
    HleResDesc back;
    HT_CHECK(hle_res_from_header(0x000c0001, 0x03310725, 0, &back));
    HT_EQ_INT(back.kind, HLE_RK_CUBE);
    HT_CHECK(back.border);
    HT_EQ_U32(back.w, 8);
    HleResInfo ri;
    HT_CHECK(hle_res_info(&back, &ri));
    HT_EQ_U32(ri.bytes, 0x1800);
    HT_EQ_U32(ri.format & 0xFFFFFFFCu, 0x03310724);    /* border kept; DMA A */
    uint32_t pitch = 0, sw, sh, ofs;
    HT_EQ_U32(hle_res_subresource(&back, 2, 0, &pitch), 0x800);
    HT_EQ_U32(pitch, 64);
    hle_res_level(&back, 0, &sw, &sh, &ofs);
    HT_EQ_U32(sw, 16);
    HT_EQ_U32(sh, 16);
    HT_EQ_U32(ofs, 4);
    /* larger than 8: twice the size; the chain halves it */
    HleResDesc t = { .kind = HLE_RK_TEXTURE, .fmt = HLE_XFMT_A8R8G8B8,
                     .w = 64, .h = 32, .d = 1, .levels = 2, .border = true };
    HT_CHECK(hle_res_info(&t, &ri));
    HT_EQ_U32(ri.bytes, 128 * 64 * 4 + 64 * 32 * 4);
    hle_res_level(&t, 1, &sw, &sh, &ofs);
    HT_EQ_U32(sw, 64);
    HT_EQ_U32(sh, 32);
    HT_EQ_U32(ofs, 2);
    /* D3D's own headers (BORDER_COLOR set) and DXT stay unbordered */
    HT_CHECK(hle_res_from_header(0x000c0001, 0x03310729, 0, &back));
    HT_CHECK(!back.border);
    HT_CHECK(hle_res_from_header(0x00040001, 0x05670C21, 0, &back));
    HT_CHECK(!back.border);
}

static void test_buffers_and_rejects(void)
{
    HleResDesc vb = { .kind = HLE_RK_VB, .length = 1000 };
    HleResInfo ri;
    HT_CHECK(hle_res_info(&vb, &ri));
    HT_EQ_U32(ri.common, 0x01000001);
    HT_EQ_U32(ri.bytes, 1000);
    HT_EQ_INT(ri.hdr_bytes, 12);
    HleResDesc ib = { .kind = HLE_RK_IB, .length = 64 };
    HT_CHECK(hle_res_info(&ib, &ri));
    HT_CHECK(!ri.contiguous);
    HleResDesc pal = { .kind = HLE_RK_PALETTE, .length = 64 };
    HT_CHECK(hle_res_info(&pal, &ri));
    HT_EQ_U32(ri.common, 0x81030001);          /* size code 2 in Common[31:30] */
    HT_EQ_U32(ri.bytes, 256);
    HleResDesc npot = { .kind = HLE_RK_TEXTURE, .fmt = HLE_XFMT_A8R8G8B8,
                        .w = 100, .h = 64, .d = 1 };
    HT_CHECK(!hle_res_info(&npot, &ri));       /* swizzled needs pow2 */
    HleResDesc unk = { .kind = HLE_RK_TEXTURE, .fmt = 0x7E, .w = 4, .h = 4, .d = 1 };
    HT_CHECK(!hle_res_info(&unk, &ri));
}

static void test_format_helpers(void)
{
    HT_EQ_U32(hle_res_data_va(0x00123000), 0x80123000);
    HT_EQ_U32(hle_res_data_pa(0x80123000), 0x00123000);
}

/* R6G5B5 (L6V5U5) into A8R8G8B8 */
static void test_r6g5b5(void)
{
    static const uint16_t in[] = { 0x0000, 0xFFFF, 0xFC00, 0x03E0, 0x001F, 0x8210, 0x0400, 0x0020, 0x0001 };
    uint32_t out[9];
    /* unsigned: each field widened by its top bits, alpha 0xFF */
    hle_tex_expand_r6g5b5(in, 9, 0, out);
    HT_EQ_U32(out[0], 0xFF000000u);
    HT_EQ_U32(out[1], 0xFFFFFFFFu);
    HT_EQ_U32(out[2], 0xFFFF0000u);
    HT_EQ_U32(out[3], 0xFF00FF00u);
    HT_EQ_U32(out[4], 0xFF0000FFu);
    HT_EQ_U32(out[5], 0xFF828484u);     /* 32 of 63, 16 of 31, 16 of 31 */
    HT_EQ_U32(out[6], 0xFF040000u);     /* 1 of 63 */
    HT_EQ_U32(out[7], 0xFF000800u);     /* 1 of 31 */
    HT_EQ_U32(out[8], 0xFF000008u);
    /* the sign word of a bump map (V and U signed), as the A8R8G8B8 it becomes; alpha: none */
    unsigned eff = 99;
    const uint32_t sx = hle_tex_sign_xor(0x27, 4 | 8, &eff);
    HT_EQ_U32(sx, 0x00008080u);
    HT_EQ_U32(eff, 4 | 8);
    HT_EQ_U32(hle_tex_sign_xor(0x37, 2, &eff), 0x00800000u);
    HT_EQ_U32(hle_tex_sign_xor(0x27, 1, &eff), 0);
    HT_EQ_U32(eff, 0);
    HT_EQ_U32(hle_tex_sign_xor(0x27, 0, &eff), 0);
    /* signed V and U: two's-complement fields over 15, centred on 0x80; L stays unsigned */
    static const uint16_t sv[] = {
        0x0000,                 /* 0, 0 */
        0x01EF,                 /* +15, +15 */
        0x0231,                 /* -15, -15 */
        0x0210,                 /* -16, -16: no further than -1 */
        0x03FF,                 /* -1, -1 */
        0x0021,                 /* +1, +1 */
        0xFC00,                 /* L alone */
    };
    hle_tex_expand_r6g5b5(sv, 7, sx, out);
    HT_EQ_U32(out[0], 0xFF008080u);
    HT_EQ_U32(out[1], 0xFF00FFFFu);
    HT_EQ_U32(out[2], 0xFF000101u);
    HT_EQ_U32(out[3], 0xFF000000u);
    HT_EQ_U32(out[4], 0xFF007878u);     /* -1 of 15: 8 below the centre */
    HT_EQ_U32(out[5], 0xFF008888u);
    HT_EQ_U32(out[6], 0xFFFF8080u);
    /* a signed L: over 31 */
    static const uint16_t sl[] = { 0x7C00, 0x8400, 0x8000 };       /* +31, -31, -32 */
    hle_tex_expand_r6g5b5(sl, 3, 0x00800000u, out);
    HT_EQ_U32(out[0], 0xFFFF0000u);
    HT_EQ_U32(out[1], 0xFF010000u);
    HT_EQ_U32(out[2], 0xFF000000u);
}

int main(void)
{
    HT_RUN(test_texture_full_chain);
    HT_RUN(test_dxt1_nonsquare);
    HT_RUN(test_linear_surface);
    HT_RUN(test_linear_custom_pitch);
    HT_RUN(test_cube_and_volume);
    HT_RUN(test_volume_too_large);
    HT_RUN(test_border_texture);
    HT_RUN(test_buffers_and_rejects);
    HT_RUN(test_format_helpers);
    HT_RUN(test_r6g5b5);
    HT_DONE();
}
