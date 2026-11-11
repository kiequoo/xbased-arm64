/*
 * hakuX HLE GPU — Kelvin's immediate vertex data (vimm.h).
 *
 * What each method writes is the hardware's as xemu's pgraph has it (which
 * follows the nxdk pgraph tests): the forms with fewer components than four
 * set the rest (z 0, w 1), a 3F or a 4F form sets only its words' own, shorts
 * are taken as numbers but for the normal's, which are unit range.
 */
#include "hw/xbox/hle/pb/vimm.h"

#include <string.h>

enum {
    K_F,        /* a float a word: that component */
    K_F3,       /* ... of three: w = 1 */
    K_F2,       /* ... of two: z = 0, w = 1 */
    K_F1,       /* one float: y = z = 0, w = 1 */
    K_ALL,      /* one float into the four (the fog coordinate) */
    K_UB,       /* four bytes, unit range */
    K_S2,       /* two shorts: z = 0, w = 1 */
    K_S4,       /* two shorts a word, two words */
    K_NS,       /* ... unit range (a normal) */
};

/* The position's and the named attributes' methods: the first word's number,
 * how many words, the attribute, the form. */
static const struct {
    uint16_t m;
    uint8_t words, attr, kind;
} k_named[] = {
    {0x1500, 3, 0, K_F3},   /* SET_VERTEX3F */
    {0x1518, 4, 0, K_F},    /* SET_VERTEX4F */
    {0x1530, 3, 2, K_F},    /* SET_NORMAL3F */
    {0x1540, 2, 2, K_NS},   /* SET_NORMAL3S */
    {0x1550, 4, 3, K_F},    /* SET_DIFFUSE_COLOR4F */
    {0x1560, 3, 3, K_F3},   /* SET_DIFFUSE_COLOR3F */
    {0x156C, 1, 3, K_UB},   /* SET_DIFFUSE_COLOR4UB */
    {0x1570, 4, 4, K_F},    /* SET_SPECULAR_COLOR4F */
    {0x1580, 3, 4, K_F3},   /* SET_SPECULAR_COLOR3F */
    {0x158C, 1, 4, K_UB},   /* SET_SPECULAR_COLOR4UB */
    {0x1590, 2, 9, K_F2},   /* SET_TEXCOORD0_2F */
    {0x1598, 1, 9, K_S2},   /* SET_TEXCOORD0_2S */
    {0x15A0, 4, 9, K_F},    /* SET_TEXCOORD0_4F */
    {0x15B0, 2, 9, K_S4},   /* SET_TEXCOORD0_4S */
    {0x15B8, 2, 10, K_F2},  /* SET_TEXCOORD1_... */
    {0x15C0, 1, 10, K_S2},
    {0x15C8, 4, 10, K_F},
    {0x15D8, 2, 10, K_S4},
    {0x15E0, 2, 11, K_F2},  /* SET_TEXCOORD2_... */
    {0x15E8, 1, 11, K_S2},
    {0x15F0, 4, 11, K_F},
    {0x1600, 2, 11, K_S4},
    {0x1608, 2, 12, K_F2},  /* SET_TEXCOORD3_... */
    {0x1610, 1, 12, K_S2},
    {0x1620, 4, 12, K_F},
    {0x1630, 2, 12, K_S4},
    {0x1698, 1, 5, K_ALL},  /* SET_FOG_COORD */
    {0x169C, 1, 1, K_F1},   /* SET_WEIGHT1F */
    {0x16A0, 2, 1, K_F2},   /* SET_WEIGHT2F */
    {0x16B0, 3, 1, K_F3},   /* SET_WEIGHT3F */
    {0x16C0, 4, 1, K_F},    /* SET_WEIGHT4F */
};

static float unit_short(uint32_t s)
{
    const float f = (float)(int16_t)s / 32767.0f;
    return f < -1.0f ? -1.0f : f;
}

/* Word `word` (of `words`) of a method of form kind into x; true if it was the last. */
static bool put(unsigned kind, unsigned word, unsigned words, uint32_t v, float x[4])
{
    float f;
    memcpy(&f, &v, 4);
    switch (kind) {
    case K_F:
        x[word] = f;
        break;
    case K_F3:
        x[word] = f;
        x[3] = 1.0f;
        break;
    case K_F2:
        x[word] = f;
        x[2] = 0.0f;
        x[3] = 1.0f;
        break;
    case K_F1:
        x[0] = f;
        x[1] = x[2] = 0.0f;
        x[3] = 1.0f;
        break;
    case K_ALL:
        x[0] = x[1] = x[2] = x[3] = f;
        break;
    case K_UB:
        for (unsigned c = 0; c < 4; c++) {
            x[c] = (float)((v >> (8 * c)) & 0xFF) / 255.0f;
        }
        break;
    case K_S2:
        x[0] = (float)(int16_t)(v & 0xFFFF);
        x[1] = (float)(int16_t)(v >> 16);
        x[2] = 0.0f;
        x[3] = 1.0f;
        break;
    case K_S4:
        x[2 * word] = (float)(int16_t)(v & 0xFFFF);
        x[2 * word + 1] = (float)(int16_t)(v >> 16);
        break;
    default:    /* K_NS */
        x[2 * word] = unit_short(v & 0xFFFF);
        x[2 * word + 1] = unit_short(v >> 16);
        break;
    }
    return word + 1 == words;
}

bool hle_vimm_span(uint32_t base, uint32_t stride, uint32_t lo, uint32_t hi, uint32_t end,
                    uint64_t ram, uint32_t *from, uint32_t *len)
{
    if (stride > HLE_VIMM_STRIDE_MAX || hi < lo) {
        return false;
    }
    const uint64_t first = (uint64_t)base + (uint64_t)lo * stride;
    const uint64_t bytes = stride ? (uint64_t)(hi - lo) * stride + (end ? end : 1u) : end;
    if (first > ram || bytes > ram - first) {
        return false;
    }
    *from = (uint32_t)first;
    *len = (uint32_t)bytes;
    return true;
}

int hle_vimm_method(uint32_t m, uint32_t v, float cur[HLE_VIMM_ATTRS][4], bool *vertex)
{
    unsigned attr, kind, word, words;
    *vertex = false;
    if (m & 3) {
        return -1;
    }
    if (m >= 0x1880 && m < 0x1B00) {            /* SET_VERTEX_DATA*: the attribute in the number */
        if (m < 0x1900) {                       /* 2F_M: two words an attribute */
            attr = (m - 0x1880) / 8, word = ((m - 0x1880) / 4) & 1, words = 2, kind = K_F2;
        } else if (m < 0x1940) {                /* 2S */
            attr = (m - 0x1900) / 4, word = 0, words = 1, kind = K_S2;
        } else if (m < 0x1980) {                /* 4UB */
            attr = (m - 0x1940) / 4, word = 0, words = 1, kind = K_UB;
        } else if (m < 0x1A00) {                /* 4S_M: two words */
            attr = (m - 0x1980) / 8, word = ((m - 0x1980) / 4) & 1, words = 2, kind = K_S4;
        } else {                                /* 4F_M: four words */
            attr = (m - 0x1A00) / 16, word = ((m - 0x1A00) / 4) & 3, words = 4, kind = K_F;
        }
    } else {
        if (m < 0x1500 || m >= 0x16D0) {
            return -1;
        }
        unsigned i = 0;
        const unsigned n = sizeof(k_named) / sizeof(k_named[0]);
        while (i < n && !(m >= k_named[i].m && m < k_named[i].m + 4u * k_named[i].words)) {
            i++;
        }
        if (i == n) {
            return -1;                          /* a number between two of them */
        }
        attr = k_named[i].attr, kind = k_named[i].kind, words = k_named[i].words;
        word = (m - k_named[i].m) / 4;
    }
    const bool last = put(kind, word, words, v, cur[attr]);
    *vertex = last && attr == 0;
    return (int)attr;
}

uint32_t hle_vimm_element_bytes(uint32_t fmt)
{
    const uint32_t type = fmt & 0xF, n = (fmt >> 4) & 0xF;
    switch (type) {
    case 0: case 4: return n <= 4 ? n : 0;                  /* bytes: D3D's colour order, OpenGL's */
    case 1: case 5: return n <= 4 ? 2 * n : 0;              /* shorts: normalised, as they are */
    case 2: return n <= 4 ? 4 * n : 0;                      /* floats */
    case 6: return n ? 4 : 0;                               /* 11:11:10 in a word */
    default: return 0;
    }
}

bool hle_vimm_element(uint32_t fmt, const uint8_t *p, float out[4])
{
    const uint32_t type = fmt & 0xF, n = (fmt >> 4) & 0xF;
    if (!hle_vimm_element_bytes(fmt)) {
        return false;
    }
    float v[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    switch (type) {
    case 0:     /* a D3D colour: bytes B, G, R, A, the attribute R, G, B, A */
        for (uint32_t i = 0; i < n; i++) {
            v[i < 3 ? 2 - i : 3] = (float)p[i] / 255.0f;
        }
        break;
    case 4:
        for (uint32_t i = 0; i < n; i++) {
            v[i] = (float)p[i] / 255.0f;
        }
        break;
    case 1:     /* -32768 is -1 too */
    case 5:
        for (uint32_t i = 0; i < n; i++) {
            int16_t x;
            memcpy(&x, p + 2 * i, 2);
            v[i] = type == 5 ? (float)x : x <= -32767 ? -1.0f : (float)x / 32767.0f;
        }
        break;
    case 2:
        memcpy(v, p, 4 * n);
        break;
    default: {  /* 6: x and y in 11 bits, z in 10, two's complement, over their positive range */
        uint32_t w;
        memcpy(&w, p, 4);
        const int32_t x = (int32_t)(w << 21) >> 21, y = (int32_t)(w << 10) >> 21, z = (int32_t)w >> 22;
        v[0] = x <= -1023 ? -1.0f : (float)x / 1023.0f;
        v[1] = y <= -1023 ? -1.0f : (float)y / 1023.0f;
        v[2] = z <= -511 ? -1.0f : (float)z / 511.0f;
        break;
    }
    }
    memcpy(out, v, sizeof(v));
    return true;
}
