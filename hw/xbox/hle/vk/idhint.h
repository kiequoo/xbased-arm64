/*
 * hakuX HLE GPU — an object's slot by its id, remembered: a small table
 * with two entries for the ids that share their low bits, so an id of any
 * size has one (the front end's ids only grow: a table indexed by the id
 * itself served the first 8192, and every lookup after them walked all the
 * slots, several times a draw, for the rest of the session).
 *
 * An entry says where the id's object was last found (the owner checks or
 * forgets it), or that it was looked for and not found since the last
 * object was made. Two ids that share their low bits each keep theirs (two
 * live objects used in turn would otherwise put each other out at every
 * lookup); a third puts out the one written before the other.
 */
#ifndef HLE_VK_IDHINT_H
#define HLE_VK_IDHINT_H

#include <stdbool.h>
#include <stdint.h>

#define HLE_IDHINT_SETS 4096u       /* a power of two; two entries each */

typedef struct HleIdHint {
    uint32_t id[2 * HLE_IDHINT_SETS];   /* the id the entry is for */
    uint32_t miss[2 * HLE_IDHINT_SETS]; /* the generation it was missed in (slot 0) */
    uint16_t slot[2 * HLE_IDHINT_SETS]; /* its slot + 1; 0: not known */
    uint8_t last[HLE_IDHINT_SETS];      /* the entry of the two written last */
    uint32_t gen;                       /* an object was made: + 1 (starts at 1) */
} HleIdHint;

#define HLE_IDHINT_INIT { .gen = 1 }

/* id's entry, -1 if it has none */
static inline int hle_idhint_at(const HleIdHint *h, uint32_t id)
{
    const unsigned k = 2 * (id & (HLE_IDHINT_SETS - 1));
    return h->id[k] == id ? (int)k : h->id[k + 1] == id ? (int)k + 1 : -1;
}

/* The entry id takes: its own, else one that says nothing, else the one of
 * the two not written last */
static inline unsigned hle_idhint_take(HleIdHint *h, uint32_t id)
{
    const unsigned set = id & (HLE_IDHINT_SETS - 1), k = 2 * set;
    int e = hle_idhint_at(h, id);
    if (e < 0) {
        for (unsigned w = 0; w < 2 && e < 0; w++) {
            if (!h->slot[k + w] && h->miss[k + w] != h->gen) {
                e = (int)(k + w);
            }
        }
        if (e < 0) {
            e = (int)(k + (h->last[set] ^ 1u));
        }
    }
    h->last[set] = (uint8_t)((unsigned)e & 1u);
    h->id[e] = id;
    return (unsigned)e;
}

/* id's slot + 1 as remembered, 0 if it is not */
static inline unsigned hle_idhint_get(const HleIdHint *h, uint32_t id)
{
    const int e = hle_idhint_at(h, id);
    return e < 0 ? 0 : h->slot[e];
}

/* id was looked for since the last object was made, and not found */
static inline bool hle_idhint_missed(const HleIdHint *h, uint32_t id)
{
    const int e = hle_idhint_at(h, id);
    return e >= 0 && !h->slot[e] && h->miss[e] == h->gen;
}

/* id found at slot1 - 1 */
static inline void hle_idhint_set(HleIdHint *h, uint32_t id, unsigned slot1)
{
    const unsigned e = hle_idhint_take(h, id);
    h->slot[e] = (uint16_t)slot1;
    h->miss[e] = 0;
}

/* id looked for and not found */
static inline void hle_idhint_miss(HleIdHint *h, uint32_t id)
{
    const unsigned e = hle_idhint_take(h, id);
    h->slot[e] = 0;
    h->miss[e] = h->gen;
}

/* id's object at slot1 - 1 is gone: no longer remembered there */
static inline void hle_idhint_forget(HleIdHint *h, uint32_t id, unsigned slot1)
{
    const int e = hle_idhint_at(h, id);
    if (e >= 0 && h->slot[e] == slot1) {
        h->slot[e] = 0;
        h->miss[e] = 0;
    }
}

/* an object was made: an id missed before may be found now */
static inline void hle_idhint_made(HleIdHint *h)
{
    h->gen++;
}

#endif
