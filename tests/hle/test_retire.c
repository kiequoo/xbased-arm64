/*
 * hakuX HLE GPU — the retired things' table (hw/xbox/hle/vk/retire.c).
 */
#include "hle_test.h"

#include "hw/xbox/hle/vk/retire.h"

#include <stdlib.h>

static unsigned g_gone, g_full;
static uint64_t g_gone_sum;

static void gone(const HleRetired *e, void *ctx)
{
    (void)ctx;
    g_gone++;
    g_gone_sum += e->h[0];
}

/* The owner's wait: everything submitted is done (value `*ctx`), reaped */
static void full(HleRetireTab *t, void *ctx)
{
    g_full++;
    hle_retire_reap(t, *(const uint64_t *)ctx, gone, NULL);
}

static HleRetired ent(uint64_t h)
{
    HleRetired r = { .h = { h, 0, 0, 0 }, .slot = (uint32_t)h, .after = HLE_RETIRE_PARKED };
    return r;
}

/* Retired, stamped by the submission, gone when the timeline passes it. */
static void test_lifecycle(void)
{
    HleRetireTab t = { .max = 4 };
    uint64_t done = 0;
    g_gone = g_full = 0;
    g_gone_sum = 0;
    for (uint64_t i = 1; i <= 3; i++) {
        HleRetired r = ent(i);
        HT_CHECK(hle_retire_put(&t, &r, full, &done));
    }
    HT_EQ_U32(t.n, 3);
    hle_retire_reap(&t, 100, gone, NULL);           /* parked: none goes, whatever completed */
    HT_EQ_U32(t.n, 3);
    HT_EQ_U32(g_gone, 0);
    hle_retire_stamp(&t, 7);
    HleRetired r4 = ent(4);
    HT_CHECK(hle_retire_put(&t, &r4, full, &done)); /* retired after the submission: parked */
    hle_retire_reap(&t, 6, gone, NULL);             /* the timeline short of 7 */
    HT_EQ_U32(g_gone, 0);
    hle_retire_reap(&t, 7, gone, NULL);
    HT_EQ_U32(g_gone, 3);
    HT_EQ_U32((uint32_t)g_gone_sum, 1 + 2 + 3);
    HT_EQ_U32(t.n, 1);
    HT_EQ_U32((uint32_t)t.e[0].h[0], 4);            /* the parked one stays */
    HT_EQ_U32(g_full, 0);
    free(t.e);
}

/* A full table waits and reaps before it takes another. */
static void test_full_waits(void)
{
    HleRetireTab t = { .max = 4 };
    uint64_t done = 0;
    g_gone = g_full = 0;
    for (uint64_t i = 1; i <= 4; i++) {
        HleRetired r = ent(i);
        HT_CHECK(hle_retire_put(&t, &r, full, &done));
    }
    hle_retire_stamp(&t, 9);
    done = 9;                                       /* the wait will find them done */
    HleRetired r5 = ent(5);
    HT_CHECK(hle_retire_put(&t, &r5, full, &done));
    HT_EQ_U32(g_full, 1);
    HT_EQ_U32(g_gone, 4);
    HT_EQ_U32(t.n, 1);
    HT_EQ_U32(t.cap, 4);                            /* no growth: the wait made room */
    free(t.e);
}

/* More retired between two submissions than the table holds: the wait frees
 * none of them (parked), and the table grows. It was written past its end. */
static void test_parked_overflow(void)
{
    HleRetireTab t = { .max = 4 };
    uint64_t done = 1000;
    g_gone = g_full = 0;
    for (uint64_t i = 1; i <= 23; i++) {
        HleRetired r = ent(i);
        HT_CHECK(hle_retire_put(&t, &r, full, &done));
        HT_CHECK(t.n <= t.cap);
    }
    HT_EQ_U32(t.n, 23);
    HT_EQ_U32(g_gone, 0);
    HT_CHECK(g_full >= 1);
    for (unsigned i = 0; i < t.n; i++) {
        HT_EQ_U32((uint32_t)t.e[i].h[0], i + 1);
        HT_EQ_U32(t.e[i].slot, i + 1);
    }
    hle_retire_stamp(&t, 5);
    hle_retire_reap(&t, 5, gone, NULL);
    HT_EQ_U32(g_gone, 23);
    HT_EQ_U32(t.n, 0);
    /* and room again without a wait */
    g_full = 0;
    HleRetired r = ent(99);
    HT_CHECK(hle_retire_put(&t, &r, full, &done));
    HT_EQ_U32(g_full, 0);
    free(t.e);
}

int main(void)
{
    HT_RUN(test_lifecycle);
    HT_RUN(test_full_waits);
    HT_RUN(test_parked_overflow);
    HT_DONE();
}
