/*
 * hakuX HLE GPU — upload arena allocator.
 */
#include "hw/xbox/hle/front/arena.h"

void hle_arena_init(HleArena *a, uint32_t size)
{
    memset(a, 0, sizeof(*a));
    a->size = size;
}

static HleArenaBlk *last_blk(HleArena *a)
{
    return &a->blk[(a->b0 + a->nb - 1) % HLE_ARENA_MAX_BLK];
}

uint32_t hle_arena_alloc(HleArena *a, uint32_t len, uint32_t align, uint64_t value)
{
    if (!len || len > a->size || !align) {
        return HLE_ARENA_FULL;
    }
    if (!a->nb) {
        a->head = 0;
        a->used = 0;
    }
    uint32_t tail = a->nb ? a->blk[a->b0].start : 0;
    uint32_t start = a->head;
    uint32_t off = (align & (align - 1)) ? (start + align - 1) / align * align
                                         : (start + align - 1) & ~(align - 1);
    bool wrapped = false;
    if (a->nb && start == tail) {
        return HLE_ARENA_FULL;                      /* head caught up: full */
    }
    if (!a->nb || start > tail) {                   /* free: [head,size)+[0,tail) */
        if (off + len > a->size) {
            if (len > tail) {
                return HLE_ARENA_FULL;
            }
            off = 0;
            wrapped = true;
        }
    } else if (off + len > tail) {                  /* free: [head,tail) */
        return HLE_ARENA_FULL;
    }
    uint32_t end = off + len;
    uint32_t bytes = wrapped ? (a->size - start) + end : end - start;
    HleArenaBlk *lb = a->nb ? last_blk(a) : NULL;
    if (lb && !wrapped && !a->split && lb->value == value && lb->end == start) {
        lb->end = end;
        lb->bytes += bytes;
    } else {
        if (a->nb == HLE_ARENA_MAX_BLK) {
            return HLE_ARENA_FULL;
        }
        a->nb++;
        HleArenaBlk *b = last_blk(a);
        b->start = start;
        b->end = end;
        b->bytes = bytes;
        b->value = value;
        b->made = a->made++;
    }
    a->split = false;
    a->used += bytes;
    a->head = end;
    return off;
}

void hle_arena_retag(HleArena *a, uint64_t since, uint64_t value)
{
    for (unsigned i = 0; i < a->nb; i++) {
        HleArenaBlk *b = &a->blk[(a->b0 + i) % HLE_ARENA_MAX_BLK];
        if (b->made >= since && b->value < value) {
            b->value = value;           /* the newest blocks: values stay in order */
        }
    }
}

void hle_arena_coalesce(HleArena *a)
{
    while (a->nb >= 2) {
        HleArenaBlk *l = last_blk(a);
        HleArenaBlk *p = &a->blk[(a->b0 + a->nb - 2) % HLE_ARENA_MAX_BLK];
        if (p->value != l->value || p->end != l->start || p->end < p->start || l->end < l->start) {
            break;
        }
        p->end = l->end;
        p->bytes += l->bytes;
        a->nb--;
    }
}

void hle_arena_retire(HleArena *a, uint64_t completed)
{
    while (a->nb && a->blk[a->b0].value <= completed) {
        a->used -= a->blk[a->b0].bytes;
        a->b0 = (a->b0 + 1) % HLE_ARENA_MAX_BLK;
        a->nb--;
    }
    if (!a->nb) {
        a->head = 0;
        a->used = 0;
    }
}
