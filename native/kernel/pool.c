/*
 * xbased native core — kernel pool.
 *
 * Guest memory for ExAllocatePool and kernel objects, in physical RAM
 * reached through the contiguous window: an initial range, then 64 KiB
 * chunks added as it fills. First-fit over a sorted free list kept on the
 * host; blocks are 16-byte aligned.
 */
#include "native/kernel/kernel.h"

#define POOL_ALIGN 16u
#define POOL_CHUNK 0x10000u

static NcRange g_first;                 /* the initial range: it stays (nc_pool_give_back) */

void nc_pool_init(NcKernel *k, uint32_t va, uint32_t len)
{
    g_first = (NcRange){va, len};
    k->pool_va = va;
    k->pool_len = len;
    k->pool_free[0] = (NcRange){va, len};
    k->pool_nfree = 1;
    memset(nc_vm_host(k->vm, va), 0, len);
}

/* More pool: a chunk of free physical RAM joins the free list. */
static bool pool_grow(NcKernel *k, uint32_t size)
{
    const uint32_t chunk = nc_align_up(size > POOL_CHUNK ? size : POOL_CHUNK, POOL_CHUNK);
    const uint32_t pa = nc_vm_alloc_phys_high(k->vm, chunk);
    if (pa == UINT32_MAX) {
        return false;
    }
    memset(nc_vm_host(k->vm, NC_CONTIG_WINDOW | pa), 0, chunk);
    k->pool_len += chunk;
    nc_pool_free(k, NC_CONTIG_WINDOW | pa, chunk);
    return true;
}

uint32_t nc_pool_alloc(NcKernel *k, uint32_t size)
{
    size = nc_align_up(size ? size : 1, POOL_ALIGN);
    for (unsigned i = 0; i < k->pool_nfree; i++) {
        NcRange *r = &k->pool_free[i];
        if (r->len < size) {
            continue;
        }
        uint32_t va = r->va;
        r->va += size;
        r->len -= size;
        if (!r->len) {
            memmove(r, r + 1, (k->pool_nfree - i - 1) * sizeof(*r));
            k->pool_nfree--;
        }
        memset(nc_vm_host(k->vm, va), 0, size);
        return va;
    }
    if (pool_grow(k, size)) {
        return nc_pool_alloc(k, size);
    }
    nc_log("xbased-native", "pool: out of kernel pool for %u bytes", size);
    return 0;
}

void nc_pool_free(NcKernel *k, uint32_t va, uint32_t size)
{
    size = nc_align_up(size ? size : 1, POOL_ALIGN);
    unsigned i = 0;
    while (i < k->pool_nfree && k->pool_free[i].va < va) {
        i++;
    }
    bool join_prev = i > 0 && k->pool_free[i - 1].va + k->pool_free[i - 1].len == va;
    bool join_next = i < k->pool_nfree && va + size == k->pool_free[i].va;
    if (join_prev && join_next) {
        k->pool_free[i - 1].len += size + k->pool_free[i].len;
        memmove(&k->pool_free[i], &k->pool_free[i + 1], (k->pool_nfree - i - 1) * sizeof(NcRange));
        k->pool_nfree--;
    } else if (join_prev) {
        k->pool_free[i - 1].len += size;
    } else if (join_next) {
        k->pool_free[i].va = va;
        k->pool_free[i].len += size;
    } else if (k->pool_nfree < NC_POOL_RANGES) {
        memmove(&k->pool_free[i + 1], &k->pool_free[i], (k->pool_nfree - i) * sizeof(NcRange));
        k->pool_free[i] = (NcRange){va, size};
        k->pool_nfree++;
    } else {
        k->pool_lost += size;
    }
}

/* The pages the free list holds whole, outside the initial range (the
 * objects made at boot are there), back to RAM; the bytes given back. The
 * chunks the pool grew by are never freed otherwise: a contiguous
 * allocation that finds no window calls this (mm.c contig_alloc). */
uint32_t nc_pool_give_back(NcKernel *k)
{
    uint32_t given = 0;
    const uint32_t first_end = g_first.va + g_first.len;
    for (unsigned i = 0; i < k->pool_nfree; i++) {
        NcRange *r = &k->pool_free[i];
        const uint32_t end = r->va + r->len;
        uint32_t a = nc_align_up(r->va, NC_PAGE), b = end & ~(NC_PAGE - 1);
        if (a < first_end && b > g_first.va) {
            if (a >= g_first.va) {
                a = first_end;
            } else {
                b = g_first.va;
            }
        }
        if (a >= b) {
            continue;
        }
        const bool head = a > r->va, tail = b < end;
        if (head && tail) {
            if (k->pool_nfree == NC_POOL_RANGES) {
                continue;
            }
            memmove(r + 2, r + 1, (k->pool_nfree - i - 1) * sizeof(*r));
            k->pool_nfree++;
            r[1] = (NcRange){b, end - b};
            r->len = a - r->va;
            i++;
        } else if (head) {
            r->len = a - r->va;
        } else if (tail) {
            r->va = b;
            r->len = end - b;
        } else {
            memmove(r, r + 1, (k->pool_nfree - i - 1) * sizeof(*r));
            k->pool_nfree--;
            i--;
        }
        nc_vm_free_phys(k->vm, a - NC_CONTIG_WINDOW, b - a);
        given += b - a;
    }
    if (given) {
        k->pool_len -= k->pool_len >= g_first.len + given ? given : 0;
        static unsigned s_logged;
        const unsigned n = ++s_logged;
        if (n <= 4 || !(n & (n - 1))) {
            nc_log("xbased-native", "pool: %u KiB with nothing in them back to RAM (#%u)", given >> 10, n);
        }
    }
    return given;
}
