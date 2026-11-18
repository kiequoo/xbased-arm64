/*
 * xbased HLE GPU — title fixes at run time: the title starts, the overrides
 * (see fixes.h and docs/hle-gpu-layers.md).
 */
#include "hw/xbox/hle/fixes/fixes.h"

#include <stdlib.h>
#include <string.h>

#include "hw/xbox/hle/hle.h"
#if defined(__ANDROID__) || defined(_WIN32) || defined(NC_DESKTOP)
#include "hw/xbox/hle/prop.h"
#endif

static uint32_t g_defaults;

static void set(uint32_t mask, const char *why)
{
    char on[256], def[256];
    g_hle_fixes = mask;
    hle_fixes_names(mask, on, sizeof(on));
    hle_fixes_names(g_defaults, def, sizeof(def));
    hle_log("xbased-hle-mode", "title fixes: %s (title defaults %s%s%s)", on, def, why ? "; " : "",
            why ? why : "");
}

void hle_fixes_init(const HleXbeInfo *x)
{
    g_defaults = hle_fixes_defaults(x);
    const char *e = getenv("XBASED_HLE_FIXES");
    unsigned bad = 0;
    uint32_t m = hle_fixes_apply(g_defaults, e, &bad);
#if defined(__ANDROID__) || defined(_WIN32) || defined(NC_DESKTOP)
    char v[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.xbased.fixes", v);
    if (v[0]) {
        m = hle_fixes_apply(m, v, &bad);
    }
#endif
    if (bad) {
        hle_log("xbased-hle-mode", "fixes: %u names in the override are not fixes (ignored)", bad);
    }
    set(m, e || bad ? "XBASED_HLE_FIXES" : NULL);
}

void hle_fixes_poll(void)
{
#if defined(__ANDROID__) || defined(_WIN32) || defined(NC_DESKTOP)
    static char last[PROP_VALUE_MAX];
    char v[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.xbased.fixes", v);
    if (strcmp(v, last)) {
        strcpy(last, v);
        unsigned bad = 0;
        set(hle_fixes_apply(hle_fixes_apply(g_defaults, getenv("XBASED_HLE_FIXES"), &bad), v, &bad),
            v[0] ? "debug.xbased.fixes" : NULL);
    }
#endif
}
