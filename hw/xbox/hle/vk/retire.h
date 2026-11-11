/*
 * hakuX HLE GPU — things let go that the GPU may still use: kept until the
 * timeline passes the work that may use them.
 *
 * An entry is parked when it is retired (HLE_RETIRE_PARKED: the recording
 * still open may use it), stamped with the value of the submission that
 * closes that recording, and gone once the timeline completes that value.
 * The table holds `max` entries before its owner waits for the GPU, and more
 * only while that wait frees none: the parked ones stay through it.
 *
 * No Vulkan here (the handles as 64-bit words), so the host tests run it.
 */
#ifndef HLE_VK_RETIRE_H
#define HLE_VK_RETIRE_H

#include <stdbool.h>
#include <stdint.h>

#define HLE_RETIRE_PARKED UINT64_MAX

typedef struct HleRetired {
    uint64_t h[4];              /* the owner's handles */
    uint32_t slot;              /* the owner's slot, 0 none */
    uint64_t after;             /* HLE_RETIRE_PARKED, or the timeline value it goes after */
} HleRetired;

typedef struct HleRetireTab {
    HleRetired *e;
    unsigned n, cap;
    unsigned max;               /* entries before the owner waits (set once, > 0) */
} HleRetireTab;

/* The entries whose value the timeline has completed, each given to gone()
 * and dropped; the parked ones and the later ones stay, in their order. */
void hle_retire_reap(HleRetireTab *t, uint64_t completed, void (*gone)(const HleRetired *e, void *ctx),
                     void *ctx);

/* The parked entries take value. */
void hle_retire_stamp(HleRetireTab *t, uint64_t value);

/* r into the table. With `max` entries or more in a full table, full() runs
 * first (the owner waits for everything submitted and reaps); a table still
 * full after it grows. False when there is no memory for that: r is not in
 * the table (what it names is not destroyed, its slot not reused). */
bool hle_retire_put(HleRetireTab *t, const HleRetired *r, void (*full)(HleRetireTab *t, void *ctx),
                    void *ctx);

#endif
