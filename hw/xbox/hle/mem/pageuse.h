/*
 * hakuX HLE GPU — the last GPU use per guest page: the timeline value of
 * the last submission that reads it (the write barrier, mem/barrier.c).
 */
#ifndef HW_XBOX_HLE_MEM_PAGEUSE_H
#define HW_XBOX_HLE_MEM_PAGEUSE_H

#include "hw/xbox/hle/hle_pure.h"

#define HLE_PAGE_SHIFT 12

typedef struct HlePageUse {
    uint64_t *v;                /* last GPU use per page */
    uint32_t npages;
    uint64_t max;               /* highest value ever marked */
} HlePageUse;

bool hle_pageuse_init(HlePageUse *p, uint64_t ram_bytes);
void hle_pageuse_fini(HlePageUse *p);
void hle_pageuse_mark(HlePageUse *p, uint32_t pa, uint32_t len, uint64_t value);
uint64_t hle_pageuse_get(const HlePageUse *p, uint32_t pa, uint32_t len);

/* How often each guest page was freed (given back by the title: its memory
 * is another owner's from then on). A range's count moves when any of its
 * pages is freed, so a count taken while an owner had the range tells later
 * whether it still has it. Counted and read from any thread. */
typedef struct HlePageFree {
    uint32_t *n;                /* frees per page */
    uint32_t npages;
} HlePageFree;

bool hle_pagefree_init(HlePageFree *p, uint64_t ram_bytes);
void hle_pagefree_fini(HlePageFree *p);
/* [pa, pa + len) freed */
void hle_pagefree_mark(HlePageFree *p, uint32_t pa, uint32_t len);
/* the frees of [pa, pa + len)'s pages, summed (0 for an empty range) */
uint32_t hle_pagefree_count(const HlePageFree *p, uint32_t pa, uint32_t len);

/* Whether [alo, alo + alen) and [blo, blo + blen) share a byte (lengths in
 * 64 bits: a range's end may pass 4 GiB) */
static inline bool hle_ranges_overlap(uint32_t alo, uint64_t alen, uint32_t blo, uint64_t blen)
{
    return alen && blen && alo < blo + blen && blo < alo + alen;
}

#endif
