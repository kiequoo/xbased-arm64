/*
 * hakuX HLE GPU — an object's slot by its id, remembered (hw/xbox/hle/vk/idhint.h).
 */
#include "hle_test.h"

#include "hw/xbox/hle/vk/idhint.h"

static HleIdHint g_h = HLE_IDHINT_INIT;

/* An id of any size has its entry: the ids past the table's size too. */
static void test_any_id(void)
{
    static const uint32_t ids[] = { 1, 2, 8191, 8192, 8193, 20000, 1u << 20, 0x7FFFFFFFu, 0xFFFFFFFFu };
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        HT_EQ_U32(hle_idhint_get(&g_h, ids[i]), 0);
        HT_CHECK(!hle_idhint_missed(&g_h, ids[i]));
        hle_idhint_set(&g_h, ids[i], i + 1);
        HT_EQ_U32(hle_idhint_get(&g_h, ids[i]), i + 1);
        HT_CHECK(!hle_idhint_missed(&g_h, ids[i]));
    }
}

/* Two ids with the same low bits each keep an entry (two live objects used in
 * turn); a third puts out the one written before the other, and an id never
 * reads another's slot. */
static void test_shared_low_bits(void)
{
    const uint32_t a = 5, b = 5 + HLE_IDHINT_SETS, c = 5 + 7 * HLE_IDHINT_SETS;
    hle_idhint_set(&g_h, a, 11);
    HT_EQ_U32(hle_idhint_get(&g_h, a), 11);
    HT_EQ_U32(hle_idhint_get(&g_h, b), 0);
    HT_EQ_U32(hle_idhint_get(&g_h, c), 0);
    hle_idhint_set(&g_h, b, 12);
    for (unsigned i = 0; i < 4; i++) {              /* in turn: both stay */
        HT_EQ_U32(hle_idhint_get(&g_h, a), 11);
        HT_EQ_U32(hle_idhint_get(&g_h, b), 12);
    }
    hle_idhint_miss(&g_h, c);                       /* a was written before b: a goes */
    HT_CHECK(hle_idhint_missed(&g_h, c));
    HT_EQ_U32(hle_idhint_get(&g_h, a), 0);
    HT_CHECK(!hle_idhint_missed(&g_h, a));
    HT_EQ_U32(hle_idhint_get(&g_h, b), 12);
    HT_EQ_U32(hle_idhint_get(&g_h, c), 0);
    hle_idhint_made(&g_h);                          /* c's miss is stale: its entry says nothing */
    hle_idhint_set(&g_h, a, 11);                    /* and a takes it, b kept */
    HT_EQ_U32(hle_idhint_get(&g_h, a), 11);
    HT_EQ_U32(hle_idhint_get(&g_h, b), 12);
}

/* A miss holds until an object is made; a found id is no miss. */
static void test_miss(void)
{
    const uint32_t id = 123456;
    hle_idhint_miss(&g_h, id);
    HT_CHECK(hle_idhint_missed(&g_h, id));
    HT_EQ_U32(hle_idhint_get(&g_h, id), 0);
    hle_idhint_made(&g_h);
    HT_CHECK(!hle_idhint_missed(&g_h, id));
    hle_idhint_miss(&g_h, id);
    HT_CHECK(hle_idhint_missed(&g_h, id));
    hle_idhint_set(&g_h, id, 3);
    HT_CHECK(!hle_idhint_missed(&g_h, id));
    HT_EQ_U32(hle_idhint_get(&g_h, id), 3);
}

/* Forgetting takes the id's own entry away, and no other's. */
static void test_forget(void)
{
    const uint32_t a = 77, b = 77 + HLE_IDHINT_SETS;
    hle_idhint_set(&g_h, a, 9);
    hle_idhint_forget(&g_h, b, 9);                  /* another id's: stays */
    HT_EQ_U32(hle_idhint_get(&g_h, a), 9);
    hle_idhint_forget(&g_h, a, 8);                  /* another slot's: stays */
    HT_EQ_U32(hle_idhint_get(&g_h, a), 9);
    hle_idhint_forget(&g_h, a, 9);
    HT_EQ_U32(hle_idhint_get(&g_h, a), 0);
    HT_CHECK(!hle_idhint_missed(&g_h, a));          /* gone is not "looked for and missing" */
}

int main(void)
{
    HT_RUN(test_any_id);
    HT_RUN(test_shared_low_bits);
    HT_RUN(test_miss);
    HT_RUN(test_forget);
    HT_DONE();
}
