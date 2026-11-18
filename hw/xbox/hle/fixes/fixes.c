/*
 * xbased HLE GPU — title fixes (see fixes.h and docs/hle-gpu-layers.md).
 *
 * The only file of the GPU layer that looks at which title is running (with
 * fixes_rt.c, which starts it). A new fix is a bit in fixes.h and a row here;
 * a title's own fix is a row whose predicate compares the title id. This
 * file is pure: tests/hle/test_fixes.c runs it.
 */
#include "hw/xbox/hle/fixes/fixes.h"

#include <stdio.h>
#include <string.h>

uint32_t g_hle_fixes;

typedef struct HleFix {
    uint32_t bit;
    const char *name;
    bool (*on)(const HleXbeInfo *x);    /* the titles it is on for; NULL: none */
} HleFix;

/* The titles that link Direct3D */
static bool d3d(const HleXbeInfo *x)
{
    return hle_xbe_d3d_lib(x) != NULL;
}

/* ... of build 4928 (the one the push buffer's runs are known for) */
static bool d3d_4928(const HleXbeInfo *x)
{
    const HleXbeLib *l = hle_xbe_d3d_lib(x);
    return l && l->build == 4928;
}

/* One title (its XBE certificate's id), for a title_* row:
 *   static bool title_example(const HleXbeInfo *x) { return x->title_id == 0x4d530004; }
 */

static const HleFix k_fixes[] = {
    { HLE_FIX_D3D_SNAP_BIAS, "d3d_snap_bias", d3d },
    { HLE_FIX_D3D_LAZY_TARGETS, "d3d_lazy_targets", d3d },
    { HLE_FIX_D3D_LAZY_PUSHES, "d3d_lazy_pushes", d3d },
    { HLE_FIX_D3D_LAZY_REPORTS, "d3d_lazy_reports", d3d },
    { HLE_FIX_D3D_METHOD_RUNS, "d3d_method_runs", d3d_4928 },
    { HLE_FIX_D3D_LAZY_ZALIAS, "d3d_lazy_zalias", d3d },
};

#define N_FIXES (sizeof(k_fixes) / sizeof(k_fixes[0]))

uint32_t hle_fixes_defaults(const HleXbeInfo *x)
{
    uint32_t m = 0;
    for (size_t i = 0; i < N_FIXES; i++) {
        if (k_fixes[i].on && k_fixes[i].on(x)) {
            m |= k_fixes[i].bit;
        }
    }
    return m;
}

void hle_fixes_names(uint32_t mask, char *out, unsigned len)
{
    unsigned n = 0;
    if (!len) {
        return;
    }
    out[0] = 0;
    for (size_t i = 0; i < N_FIXES; i++) {
        if (mask & k_fixes[i].bit) {
            n += (unsigned)snprintf(out + n, n < len ? len - n : 0, "%s%s", n ? "," : "",
                                    k_fixes[i].name);
        }
    }
    if (!n) {
        snprintf(out, len, "-");
    }
}

uint32_t hle_fixes_apply(uint32_t mask, const char *spec, unsigned *bad)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec ? spec : "");
    for (char *save = NULL, *t = strtok_r(buf, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
        const bool off = *t == '-';
        const char *name = *t == '-' || *t == '+' ? t + 1 : t;
        uint32_t bits = 0;
        if (!strcmp(name, "all")) {
            for (size_t i = 0; i < N_FIXES; i++) {
                bits |= k_fixes[i].bit;
            }
        }
        for (size_t i = 0; i < N_FIXES; i++) {
            if (!strcmp(name, k_fixes[i].name)) {
                bits = k_fixes[i].bit;
            }
        }
        if (!bits && bad) {
            (*bad)++;
        }
        mask = off ? mask & ~bits : mask | bits;
    }
    return mask;
}
