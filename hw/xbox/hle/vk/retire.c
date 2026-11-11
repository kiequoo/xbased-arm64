/*
 * hakuX HLE GPU — things let go that the GPU may still use (retire.h).
 */
#include "hw/xbox/hle/vk/retire.h"

#include <stdlib.h>

void hle_retire_reap(HleRetireTab *t, uint64_t completed, void (*gone)(const HleRetired *e, void *ctx),
                     void *ctx)
{
    unsigned j = 0;
    for (unsigned i = 0; i < t->n; i++) {
        if (t->e[i].after == HLE_RETIRE_PARKED || t->e[i].after > completed) {
            t->e[j++] = t->e[i];
            continue;
        }
        gone(&t->e[i], ctx);
    }
    t->n = j;
}

void hle_retire_stamp(HleRetireTab *t, uint64_t value)
{
    for (unsigned i = 0; i < t->n; i++) {
        if (t->e[i].after == HLE_RETIRE_PARKED) {
            t->e[i].after = value;
        }
    }
}

bool hle_retire_put(HleRetireTab *t, const HleRetired *r, void (*full)(HleRetireTab *t, void *ctx),
                    void *ctx)
{
    if (t->n == t->cap) {
        if (t->n >= t->max && full) {
            full(t, ctx);
        }
        if (t->n == t->cap) {
            /* the first table, or one the wait freed nothing of: the entries
             * retired since the last submission stay through it */
            const unsigned cap = t->cap ? 2 * t->cap : t->max;
            HleRetired *e = cap > t->cap ? realloc(t->e, (size_t)cap * sizeof(*e)) : NULL;
            if (!e) {
                return false;
            }
            t->e = e;
            t->cap = cap;
        }
    }
    t->e[t->n++] = *r;
    return true;
}
