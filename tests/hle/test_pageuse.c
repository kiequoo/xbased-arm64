#include "hw/xbox/hle/mem/pageuse.h"
#include "hle_test.h"

static void test_pageuse(void)
{
    HlePageUse p;
    HT_CHECK(hle_pageuse_init(&p, 1u << 20));       /* 256 pages */
    hle_pageuse_mark(&p, 0x1800, 0x2000, 7);        /* pages 1..3 */
    hle_pageuse_mark(&p, 0x3000, 0x10, 4);          /* page 3 keeps max 7 */
    HT_CHECK(hle_pageuse_get(&p, 0x0000, 0x1000) == 0);
    HT_CHECK(hle_pageuse_get(&p, 0x3FFC, 4) == 7);
    HT_CHECK(hle_pageuse_get(&p, 0x0000, 0x100000) == 7);
    HT_CHECK(hle_pageuse_get(&p, 0xFFFFF000, 0x1000) == 0);   /* outside */
    HT_CHECK(p.max == 7);
    hle_pageuse_fini(&p);
}

/* A range's count moves when any page of it is freed, and only then. */
static void test_pagefree(void)
{
    HlePageFree p;
    HT_CHECK(hle_pagefree_init(&p, 1u << 20));      /* 256 pages */
    const uint32_t pa = 0x5800, len = 0x2850;       /* a 112 x 23 target of 32 bits: pages 5..8 */
    const uint32_t c0 = hle_pagefree_count(&p, pa, len);
    HT_EQ_U32(c0, 0);
    hle_pagefree_mark(&p, 0x4000, 0x1000);          /* the page before */
    hle_pagefree_mark(&p, 0x9000, 0x3000);          /* the pages after */
    HT_EQ_U32(hle_pagefree_count(&p, pa, len), c0);
    hle_pagefree_mark(&p, 0x8000, 0x1000);          /* its last page */
    const uint32_t c1 = hle_pagefree_count(&p, pa, len);
    HT_CHECK(c1 != c0);
    hle_pagefree_mark(&p, 0x5FFF, 1);               /* one byte of its first page */
    const uint32_t c2 = hle_pagefree_count(&p, pa, len);
    HT_CHECK(c2 != c1);
    hle_pagefree_mark(&p, 0, 1u << 20);             /* everything */
    HT_EQ_U32(hle_pagefree_count(&p, pa, len), c2 + 4);
    HT_EQ_U32(hle_pagefree_count(&p, pa, 0), 0);
    /* outside the memory, and a range that runs past its end */
    hle_pagefree_mark(&p, 0xFFFFF000u, 0x1000);
    HT_EQ_U32(hle_pagefree_count(&p, 0xFFFFF000u, 0x1000), 0);
    HT_EQ_U32(hle_pagefree_count(&p, 0xFF000, 0x10000000u), 1);     /* its one page inside */
    hle_pagefree_fini(&p);
    HlePageFree none = { 0 };                       /* never set up: nothing counted */
    hle_pagefree_mark(&none, 0, 0x1000);
    HT_EQ_U32(hle_pagefree_count(&none, 0, 0x1000), 0);
}

static void test_overlap(void)
{
    HT_CHECK(hle_ranges_overlap(0x1000, 0x1000, 0x1FFF, 1));
    HT_CHECK(!hle_ranges_overlap(0x1000, 0x1000, 0x2000, 1));
    HT_CHECK(!hle_ranges_overlap(0x1000, 0x1000, 0x0FFF, 1));
    HT_CHECK(hle_ranges_overlap(0x1000, 0x1000, 0x0FFF, 2));
    HT_CHECK(hle_ranges_overlap(0x1000, 1, 0, 0x100000000ull));
    HT_CHECK(!hle_ranges_overlap(0x1000, 0, 0, 0x100000000ull));
    HT_CHECK(!hle_ranges_overlap(0x1000, 0x1000, 0x1800, 0));
    /* ends past 4 GiB */
    HT_CHECK(hle_ranges_overlap(0xFFFFF000u, 0x2000, 0xFFFFFFFFu, 1));
    HT_CHECK(!hle_ranges_overlap(0xFFFFF000u, 0x2000, 0x0, 0x1000));
}

int main(void)
{
    HT_RUN(test_pageuse);
    HT_RUN(test_pagefree);
    HT_RUN(test_overlap);
    HT_DONE();
}
