/*
 * hakuX HLE GPU — Kelvin's immediate vertex data (hw/xbox/hle/pb/vimm.c):
 * every method number, the ones that are none between two that are, and what
 * each form writes.
 */
#include "hle_test.h"

#include <stdbool.h>

#include "hw/xbox/hle/pb/vimm.h"

static uint32_t bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static uint32_t fl(float f)
{
    return bits(f);
}

/* the attributes between two guards: nothing may be written outside them */
static struct {
    uint32_t before[8];
    float cur[HLE_VIMM_ATTRS][4];
    uint32_t after[8];
} g;

static void reset(void)
{
    memset(&g, 0xA5, sizeof(g));
    for (unsigned a = 0; a < HLE_VIMM_ATTRS; a++) {
        for (unsigned c = 0; c < 4; c++) {
            g.cur[a][c] = 9.0f;
        }
    }
}

static bool guards_whole(void)
{
    for (unsigned i = 0; i < 8; i++) {
        if (g.before[i] != 0xA5A5A5A5u || g.after[i] != 0xA5A5A5A5u) {
            return false;
        }
    }
    return true;
}

static int put(uint32_t m, uint32_t v, bool *vertex)
{
    return hle_vimm_method(m, v, g.cur, vertex);
}

#define IS(a, x, y, z, w)                                                                          \
    do {                                                                                           \
        HT_EQ_U32(bits(g.cur[a][0]), bits(x));                                                     \
        HT_EQ_U32(bits(g.cur[a][1]), bits(y));                                                     \
        HT_EQ_U32(bits(g.cur[a][2]), bits(z));                                                     \
        HT_EQ_U32(bits(g.cur[a][3]), bits(w));                                                     \
    } while (0)

/* Every number a push buffer can name: one attribute at most changes, a number that is no
 * method of these changes nothing, and nothing outside the attributes is written. */
static void test_every_number(void)
{
    unsigned methods = 0;
    for (uint32_t m = 0; m < 0x2000; m += 4) {
        float was[HLE_VIMM_ATTRS][4];
        bool vertex = true;
        reset();
        memcpy(was, g.cur, sizeof(was));
        const int a = put(m, 0x41414141u, &vertex);
        HT_CHECK(a >= -1 && a < HLE_VIMM_ATTRS);
        HT_CHECK(guards_whole());
        for (int i = 0; i < HLE_VIMM_ATTRS; i++) {
            if (i != a) {
                HT_CHECK(!memcmp(was[i], g.cur[i], 16));
            }
        }
        HT_CHECK(a == 0 || !vertex);
        methods += a >= 0;
    }
    /* 3 + 4 the position's, 68 the named attributes', 160 SET_VERTEX_DATA*'s words */
    HT_EQ_U32(methods, 3 + 4 + 68 + 160);
}

static void test_between(void)
{
    static const uint32_t none[] = {
        0x14FC, 0x150C, 0x1510, 0x1514,         /* between SET_VERTEX3F's words and SET_VERTEX4F */
        0x1528, 0x152C, 0x153C, 0x1548, 0x154C, 0x1614, 0x1618, 0x161C,
        0x1638, 0x1694, 0x16A8, 0x16AC, 0x16BC, 0x16D0, 0x187C, 0x1B00,
        0x1501, 0x1882,                         /* not a method's number at all */
    };
    for (unsigned i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
        bool vertex = true;
        reset();
        HT_EQ_INT(put(none[i], 0x3F800000u, &vertex), -1);
        HT_CHECK(!vertex);
        for (unsigned a = 0; a < HLE_VIMM_ATTRS; a++) {
            IS(a, 9.0f, 9.0f, 9.0f, 9.0f);
        }
    }
}

static void test_position(void)
{
    bool vertex;
    reset();
    HT_EQ_INT(put(0x1500, fl(1.0f), &vertex), 0);
    HT_CHECK(!vertex);
    HT_EQ_INT(put(0x1504, fl(2.0f), &vertex), 0);
    HT_CHECK(!vertex);
    HT_EQ_INT(put(0x1508, fl(3.0f), &vertex), 0);
    HT_CHECK(vertex);                           /* SET_VERTEX3F's third word */
    IS(0, 1.0f, 2.0f, 3.0f, 1.0f);
    reset();
    for (unsigned c = 0; c < 4; c++) {
        HT_EQ_INT(put(0x1518 + 4 * c, fl(5.0f + (float)c), &vertex), 0);
        HT_CHECK(vertex == (c == 3));           /* SET_VERTEX4F's fourth */
    }
    IS(0, 5.0f, 6.0f, 7.0f, 8.0f);
}

static void test_named(void)
{
    bool vertex;
    reset();
    /* the normal: three floats (w as it was), or shorts in the unit range */
    for (unsigned c = 0; c < 3; c++) {
        HT_EQ_INT(put(0x1530 + 4 * c, fl(0.25f * (float)(c + 1)), &vertex), 2);
        HT_CHECK(!vertex);
    }
    IS(2, 0.25f, 0.5f, 0.75f, 9.0f);
    HT_EQ_INT(put(0x1540, 0x80007FFFu, &vertex), 2);
    HT_EQ_INT(put(0x1544, 0x00004000u, &vertex), 2);
    IS(2, 1.0f, -1.0f, 16384.0f / 32767.0f, 0.0f);
    /* the colours: four floats, three (alpha 1), or four bytes */
    reset();
    for (unsigned c = 0; c < 4; c++) {
        HT_EQ_INT(put(0x1550 + 4 * c, fl(0.125f * (float)(c + 1)), &vertex), 3);
        HT_EQ_INT(put(0x1570 + 4 * c, fl(0.0625f * (float)(c + 1)), &vertex), 4);
    }
    IS(3, 0.125f, 0.25f, 0.375f, 0.5f);
    IS(4, 0.0625f, 0.125f, 0.1875f, 0.25f);
    for (unsigned c = 0; c < 3; c++) {
        HT_EQ_INT(put(0x1560 + 4 * c, fl(0.5f), &vertex), 3);
        HT_EQ_INT(put(0x1580 + 4 * c, fl(0.75f), &vertex), 4);
    }
    IS(3, 0.5f, 0.5f, 0.5f, 1.0f);
    IS(4, 0.75f, 0.75f, 0.75f, 1.0f);
    HT_EQ_INT(put(0x156C, 0x80FF0040u, &vertex), 3);
    IS(3, 64.0f / 255.0f, 0.0f, 1.0f, 128.0f / 255.0f);
    HT_EQ_INT(put(0x158C, 0xFF000000u, &vertex), 4);
    IS(4, 0.0f, 0.0f, 0.0f, 1.0f);
    /* a stage's texture coordinates: two floats or shorts (z 0, w 1), four floats, four shorts */
    static const uint16_t tex[4] = {0x1590, 0x15B8, 0x15E0, 0x1608};
    static const uint16_t tex4f[4] = {0x15A0, 0x15C8, 0x15F0, 0x1620};
    static const uint16_t tex4s[4] = {0x15B0, 0x15D8, 0x1600, 0x1630};
    for (unsigned s = 0; s < 4; s++) {
        const int a = 9 + (int)s;
        reset();
        HT_EQ_INT(put(tex[s], fl(0.5f), &vertex), a);
        HT_EQ_INT(put(tex[s] + 4u, fl(0.25f), &vertex), a);
        IS(a, 0.5f, 0.25f, 0.0f, 1.0f);
        HT_EQ_INT(put(tex[s] + 8u, 0xFFFE0003u, &vertex), a);       /* ..._2S */
        IS(a, 3.0f, -2.0f, 0.0f, 1.0f);
        for (unsigned c = 0; c < 4; c++) {
            HT_EQ_INT(put(tex4f[s] + 4u * c, fl(1.0f + (float)c), &vertex), a);
        }
        IS(a, 1.0f, 2.0f, 3.0f, 4.0f);
        HT_EQ_INT(put(tex4s[s], 0x00020001u, &vertex), a);
        HT_EQ_INT(put(tex4s[s] + 4u, 0xFFFF0003u, &vertex), a);
        IS(a, 1.0f, 2.0f, 3.0f, -1.0f);
        HT_CHECK(!vertex);
    }
    /* the fog coordinate in the four; the weights by how many */
    reset();
    HT_EQ_INT(put(0x1698, fl(0.375f), &vertex), 5);
    IS(5, 0.375f, 0.375f, 0.375f, 0.375f);
    HT_EQ_INT(put(0x169C, fl(0.5f), &vertex), 1);
    IS(1, 0.5f, 0.0f, 0.0f, 1.0f);
    reset();
    HT_EQ_INT(put(0x16A0, fl(0.25f), &vertex), 1);
    HT_EQ_INT(put(0x16A4, fl(0.75f), &vertex), 1);
    IS(1, 0.25f, 0.75f, 0.0f, 1.0f);
    reset();
    for (unsigned c = 0; c < 3; c++) {
        HT_EQ_INT(put(0x16B0 + 4 * c, fl(0.125f), &vertex), 1);
    }
    IS(1, 0.125f, 0.125f, 0.125f, 1.0f);
    for (unsigned c = 0; c < 4; c++) {
        HT_EQ_INT(put(0x16C0 + 4 * c, fl(2.0f + (float)c), &vertex), 1);
    }
    IS(1, 2.0f, 3.0f, 4.0f, 5.0f);
    HT_CHECK(!vertex);
}

static void test_by_index(void)
{
    bool vertex;
    for (unsigned a = 0; a < HLE_VIMM_ATTRS; a++) {
        reset();
        HT_EQ_INT(put(0x1880 + 8 * a, fl(1.5f), &vertex), (int)a);          /* 2F_M */
        HT_CHECK(!vertex);
        HT_EQ_INT(put(0x1884 + 8 * a, fl(2.5f), &vertex), (int)a);
        HT_CHECK(vertex == (a == 0));
        IS(a, 1.5f, 2.5f, 0.0f, 1.0f);
        HT_EQ_INT(put(0x1900 + 4 * a, 0x7FFF8000u, &vertex), (int)a);       /* 2S */
        HT_CHECK(vertex == (a == 0));
        IS(a, -32768.0f, 32767.0f, 0.0f, 1.0f);
        HT_EQ_INT(put(0x1940 + 4 * a, 0x000000FFu, &vertex), (int)a);       /* 4UB */
        HT_CHECK(vertex == (a == 0));
        IS(a, 1.0f, 0.0f, 0.0f, 0.0f);
        HT_EQ_INT(put(0x1980 + 8 * a, 0x00020001u, &vertex), (int)a);       /* 4S_M */
        HT_CHECK(!vertex);
        HT_EQ_INT(put(0x1984 + 8 * a, 0x00040003u, &vertex), (int)a);
        HT_CHECK(vertex == (a == 0));
        IS(a, 1.0f, 2.0f, 3.0f, 4.0f);
        for (unsigned c = 0; c < 4; c++) {                                  /* 4F_M */
            HT_EQ_INT(put(0x1A00 + 16 * a + 4 * c, fl(10.0f + (float)c), &vertex), (int)a);
            HT_CHECK(vertex == (a == 0 && c == 3));
        }
        IS(a, 10.0f, 11.0f, 12.0f, 13.0f);
        HT_CHECK(guards_whole());
    }
}

/* A vertex array's bytes under a draw's indices, counted whole. */
static void test_span(void)
{
    const uint64_t ram = 64u << 20;
    uint32_t from = 1, len = 1;
    /* D3D's: a stream of 36-byte vertices, indices 100 .. 131, the last attribute ending at 32 */
    HT_CHECK(hle_vimm_span(0x00400000, 36, 100, 131, 32, ram, &from, &len));
    HT_EQ_U32(from, 0x00400000 + 100 * 36);
    HT_EQ_U32(len, 31 * 36 + 32);
    /* no stride: one vertex for every index */
    HT_CHECK(hle_vimm_span(0x00400000, 0, 5, 900, 16, ram, &from, &len));
    HT_EQ_U32(from, 0x00400000);
    HT_EQ_U32(len, 16);
    /* up to the last byte of memory, not past it */
    HT_CHECK(hle_vimm_span((uint32_t)ram - 64, 16, 0, 3, 16, ram, &from, &len));
    HT_EQ_U32(from + len, (uint32_t)ram);
    HT_CHECK(!hle_vimm_span((uint32_t)ram - 64, 16, 0, 4, 16, ram, &from, &len));
    HT_CHECK(!hle_vimm_span((uint32_t)ram, 16, 0, 0, 16, ram, &from, &len));
    HT_CHECK(!hle_vimm_span(0x10000000, 16, 0, 0, 16, ram, &from, &len));
    /* an index times a stride past 4 GiB: in 32 bits these were spans inside memory */
    HT_CHECK(!hle_vimm_span(0x00400000, 16, 0x10000000, 0x10000010, 16, ram, &from, &len));
    HT_CHECK(!hle_vimm_span(0, 256, 0x01000000, 0x01000001, 16, ram, &from, &len));
    HT_CHECK(!hle_vimm_span(0x00400000, 2048, 0x00200000, 0x00200004, 16, ram, &from, &len));
    /* a stride of the register's 24 bits: its vertices' span wrapped as well */
    HT_CHECK(!hle_vimm_span(0x00400000, 0x00FFFFFF, 0, 0x101, 16, ram, &from, &len));
    HT_CHECK(!hle_vimm_span(0x00400000, 0x00010000, 0, 0xFFFE, 16, ram, &from, &len));
    HT_CHECK(!hle_vimm_span(0x00400000, HLE_VIMM_STRIDE_MAX + 1, 0, 0, 16, ram, &from, &len));
    HT_CHECK(hle_vimm_span(0x00400000, HLE_VIMM_STRIDE_MAX, 0, 1, 16, ram, &from, &len));
    HT_EQ_U32(len, HLE_VIMM_STRIDE_MAX + 16);
    HT_CHECK(!hle_vimm_span(0x00400000, 16, 9, 8, 16, ram, &from, &len));     /* no indices */
}

/* An array's element as the attribute's current value (what a draw's last vertex leaves) */
static void test_element(void)
{
    float v[4] = { 9, 9, 9, 9 };
    /* floats: the components the array has, the rest 0, 0, 0, 1 */
    const float f3[3] = { 1.5f, -2.0f, 0.25f };
    HT_EQ_U32(hle_vimm_element_bytes(0x32), 12);
    HT_CHECK(hle_vimm_element(0x32, (const uint8_t *)f3, v));
    HT_CHECK(v[0] == 1.5f && v[1] == -2.0f && v[2] == 0.25f && v[3] == 1.0f);
    const float f1[1] = { 7.0f };
    HT_CHECK(hle_vimm_element(0x12, (const uint8_t *)f1, v));
    HT_CHECK(v[0] == 7.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 1.0f);
    const float f4[4] = { 1, 2, 3, 4 };
    HT_CHECK(hle_vimm_element(0x42, (const uint8_t *)f4, v));
    HT_CHECK(v[0] == 1 && v[1] == 2 && v[2] == 3 && v[3] == 4);
    /* a D3D colour: bytes B, G, R, A */
    const uint8_t bgra[4] = { 0x33, 0x66, 0xFF, 0x00 };
    HT_EQ_U32(hle_vimm_element_bytes(0x40), 4);
    HT_CHECK(hle_vimm_element(0x40, bgra, v));
    HT_CHECK(v[0] == 1.0f && v[1] == 0x66 / 255.0f && v[2] == 0x33 / 255.0f && v[3] == 0.0f);
    /* OpenGL's byte order: as they come; three of them, alpha 1 */
    HT_CHECK(hle_vimm_element(0x34, bgra, v));
    HT_CHECK(v[0] == 0x33 / 255.0f && v[1] == 0x66 / 255.0f && v[2] == 1.0f && v[3] == 1.0f);
    /* normalised shorts: over 32767, -32768 no further than -1 */
    const int16_t s2[2] = { 32767, -32768 };
    HT_EQ_U32(hle_vimm_element_bytes(0x21), 4);
    HT_CHECK(hle_vimm_element(0x21, (const uint8_t *)s2, v));
    HT_CHECK(v[0] == 1.0f && v[1] == -1.0f && v[2] == 0.0f && v[3] == 1.0f);
    const int16_t s1[1] = { -16384 };
    HT_CHECK(hle_vimm_element(0x11, (const uint8_t *)s1, v));
    HT_CHECK(v[0] == -16384.0f / 32767.0f);
    /* shorts as they are */
    const int16_t k2[2] = { -300, 4096 };
    HT_CHECK(hle_vimm_element(0x25, (const uint8_t *)k2, v));
    HT_CHECK(v[0] == -300.0f && v[1] == 4096.0f && v[2] == 0.0f && v[3] == 1.0f);
    /* 11:11:10 */
    const uint32_t cmp = (1023u) | (uint32_t)(-1023 & 0x7FF) << 11 | (uint32_t)(-512 & 0x3FF) << 22;
    HT_EQ_U32(hle_vimm_element_bytes(0x16), 4);
    HT_CHECK(hle_vimm_element(0x16, (const uint8_t *)&cmp, v));
    HT_CHECK(v[0] == 1.0f && v[1] == -1.0f && v[2] == -1.0f && v[3] == 1.0f);
    const uint32_t cmp2 = (uint32_t)(-1 & 0x7FF) | 511u << 22;
    HT_CHECK(hle_vimm_element(0x16, (const uint8_t *)&cmp2, v));
    HT_CHECK(v[0] == -1.0f / 1023.0f && v[1] == 0.0f && v[2] == 1.0f);
    /* no array, an unknown type, a count past four: none, the value untouched */
    v[0] = v[1] = v[2] = v[3] = 5.0f;
    HT_CHECK(!hle_vimm_element(0x02, (const uint8_t *)f4, v));
    HT_CHECK(!hle_vimm_element(0x43, (const uint8_t *)f4, v));
    HT_CHECK(!hle_vimm_element(0x52, (const uint8_t *)f4, v));
    HT_CHECK(v[0] == 5.0f && v[3] == 5.0f);
    for (uint32_t fmt = 0; fmt < 256; fmt++) {
        HT_CHECK(hle_vimm_element_bytes(fmt) <= 16);
    }
}

int main(void)
{
    HT_RUN(test_element);
    test_span();
    test_every_number();
    test_between();
    test_position();
    test_named();
    test_by_index();
    if (ht_failures) {
        fprintf(stderr, "%d failures\n", ht_failures);
        return 1;
    }
    printf("test_vimm: ok\n");
    return 0;
}
