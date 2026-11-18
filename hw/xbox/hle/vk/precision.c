/*
 * xbased HLE GPU — the GPU precision (see precision.h).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/vk/precision.h"

#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/host.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_option = HLE_PREC_AUTO;
static bool g_fast;
static bool g_measuring;
static char g_id[96];               /* "vendor:device:driver" of the GPU, its verdicts' key */
static char g_path[320];            /* <base>/gpu_precision.txt */
static __thread int t_building;     /* in a driver call that compiles */
static __thread int t_linking;      /* in one from pipeline libraries (gpl.c) */
static __thread uint64_t t_pipe;    /* the pipeline this thread builds (hle_precision_pipe_set) */
#define MAX_BAD_PIPES 64
static uint64_t g_bad_pipes[MAX_BAD_PIPES];   /* (read once, hle_precision_init) */
static unsigned g_nbad_pipes;
static void bad_pipes_read(void);
static HleMutex g_b_mu;             /* the exact builds' measure (made in hle_precision_init, before any worker) */

/* the measurement: busy GPU time in whole seconds of wall time */
static struct {
    uint64_t t0, sec_start, busy_ns;
    unsigned frames, bound_secs;        /* (frames: the game's, hle_precision_swap) */
    bool decided;
} g_m;

#define WARMUP_NS  (20ull * 1000000000ull)  /* loading and first shaders aside */
#define BOUND_SECS 15u                      /* seconds at the GPU's limit in a row */

void hle_precision_set_option(int mode)
{
    g_option = mode == HLE_PREC_EXACT || mode == HLE_PREC_FAST ? mode : HLE_PREC_AUTO;
}

bool hle_precision_fast(void)
{
    return g_fast;
}

bool hle_precision_measuring(void)
{
    return g_measuring;
}

/* The line of this GPU and driver in the verdicts' file: its verdict and
 * reason (false: none). */
static bool verdict_read_id(const char *id, char *verdict, size_t vn, char *reason, size_t rn)
{
    FILE *f = fopen(g_path, "r");
    if (!f) {
        return false;
    }
    char line[512];
    bool got = false;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        const size_t il = strlen(id);
        if (strncmp(line, id, il) || line[il] != ' ') {
            continue;
        }
        const char *v = line + il + 1;
        const char *sp = strchr(v, ' ');
        snprintf(verdict, vn, "%.*s", sp ? (int)(sp - v) : (int)strlen(v), v);
        snprintf(reason, rn, "%s", sp ? sp + 1 : "");
        got = true;         /* (the last line of it wins) */
    }
    fclose(f);
    return got;
}

static bool verdict_read(char *verdict, size_t vn, char *reason, size_t rn)
{
    return verdict_read_id(g_id, verdict, vn, reason, rn);
}

/* This GPU and driver's verdict, kept: the file's other lines, then this one
 * (id: g_id, or g_id + "/gpl" for the pipeline libraries' own). */
static void verdict_write_id(const char *id, const char *verdict, const char *reason)
{
    char *keep = NULL;
    size_t kn = 0;
    FILE *f = fopen(g_path, "r");
    if (f) {
        char line[512];
        const size_t il = strlen(id);
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, id, il) && line[il] == ' ') {
                continue;
            }
            const size_t l = strlen(line);
            char *n = realloc(keep, kn + l + 1);
            if (!n) {
                break;
            }
            keep = n;
            memcpy(keep + kn, line, l + 1);
            kn += l;
        }
        fclose(f);
    }
    f = fopen(g_path, "w");
    if (f) {
        if (keep) {
            fputs(keep, f);
        }
        fprintf(f, "%s %s %s\n", id, verdict, reason);
        fclose(f);
    }
    free(keep);
}

static void verdict_write(const char *verdict, const char *reason)
{
    verdict_write_id(g_id, verdict, reason);
}

static void gpl_id(char *out, size_t n)
{
    snprintf(out, n, "%s/gpl", g_id);
}

/* AMD's proprietary driver on GCN (Polaris, Vega): the Windows driver branch
 * kept for them since 2023 (23.19), whose shader compiler crashed on the
 * exact pixel shaders (driver 0x800117, Adrenalin 24.9.1 and 26.1.1 alike).
 * GCN runs 64-lane waves only: its subgroups are 64 at least, where RDNA's
 * are 32 too (VK_EXT_subgroup_size_control, core in 1.3). */
static bool amd_gcn_proprietary(VkPhysicalDevice phys, const VkPhysicalDeviceProperties *p, uint32_t driver_id,
                                char *why, size_t wn)
{
    if (p->vendorID != 0x1002 || driver_id != VK_DRIVER_ID_AMD_PROPRIETARY) {
        return false;
    }
    VkPhysicalDeviceSubgroupSizeControlProperties sg = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg };
    vkGetPhysicalDeviceProperties2(phys, &p2);
    if (sg.minSubgroupSize >= 64) {
        snprintf(why, wn, "AMD's driver for GCN GPUs (Polaris, Vega: 64-lane waves only)");
        return true;
    }
    if (!sg.minSubgroupSize && p->driverVersion <= 0x800117u) {
        snprintf(why, wn, "AMD's driver branch for GCN GPUs (driver 0x%x)", p->driverVersion);
        return true;
    }
    return false;
}

void hle_precision_init(VkPhysicalDevice phys)
{
    hle_mutex_init(&g_b_mu);
    VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
    vkGetPhysicalDeviceProperties2(phys, &p2);
    const VkPhysicalDeviceProperties *p = &p2.properties;
    snprintf(g_id, sizeof(g_id), "%04x:%04x:%08x", p->vendorID, p->deviceID, p->driverVersion);
    snprintf(g_path, sizeof(g_path), "%sgpu_precision.txt", hle_host_base_path());
    char why[256] = "", verdict[16] = "", kept[256] = "";
    if (g_option == HLE_PREC_EXACT) {
        g_fast = false;
        snprintf(why, sizeof(why), "chosen in the settings");
    } else if (g_option == HLE_PREC_FAST) {
        g_fast = true;
        snprintf(why, sizeof(why), "chosen in the settings");
    } else if (amd_gcn_proprietary(phys, p, drv.driverID, why, sizeof(why))) {
        g_fast = true;
    } else if (sysconf(_SC_NPROCESSORS_ONLN) > 0 && sysconf(_SC_NPROCESSORS_ONLN) <= 4) {
        g_fast = true;
        snprintf(why, sizeof(why), "a CPU of %ld threads: the exact shaders' builds would take the emulation's",
                 sysconf(_SC_NPROCESSORS_ONLN));
    } else if (verdict_read(verdict, sizeof(verdict), kept, sizeof(kept)) && !strcmp(verdict, "fast")) {
        g_fast = true;
        snprintf(why, sizeof(why), "%s", kept);
    } else {
        g_fast = false;
        g_measuring = true;     /* auto, exact: the run tells whether the GPU keeps up */
        snprintf(why, sizeof(why), "the GPU and driver have no known trouble with it");
    }
    VkPhysicalDeviceSubgroupSizeControlProperties sg = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES };
    VkPhysicalDeviceProperties2 q2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg };
    vkGetPhysicalDeviceProperties2(phys, &q2);
    hle_log("xbased-hle-back", "precision: %s (%s, %s) [gpu %s, driver id %u, subgroups %u-%u]",
            g_fast ? "fast" : "exact", g_option == HLE_PREC_AUTO ? "auto" : "set", why, g_id, drv.driverID,
            sg.minSubgroupSize, sg.maxSubgroupSize);
    /* the dashboard shows auto's verdict and why */
    char last[400];
    bad_pipes_read();
    snprintf(last, sizeof(last), "%slast_precision.txt", hle_host_base_path());
    FILE *f = fopen(last, "w");
    if (f) {
        fprintf(f, "%s\n%s\n", g_fast ? "fast" : "exact", why);
        fclose(f);
    }
}

void hle_precision_gpu_time(uint64_t ns)
{
    if (!g_measuring || g_m.decided) {
        return;
    }
    const uint64_t now = hle_now_ns();
    if (!g_m.t0) {
        g_m.t0 = g_m.sec_start = now;
    }
    g_m.busy_ns += ns;
    if (now - g_m.sec_start < 1000000000ull) {
        return;
    }
    const double secs = (double)(now - g_m.sec_start) / 1e9;
    const double busy = (double)g_m.busy_ns / (double)(now - g_m.sec_start);
    const double fps = __atomic_exchange_n(&g_m.frames, 0, __ATOMIC_RELAXED) / secs;
    const bool warm = now - g_m.t0 >= WARMUP_NS;
    /* at its limit: busy nearly all the time and the frames short of 60 (an
     * uncapped game keeps the GPU busy by design, over 60) */
    g_m.bound_secs = warm && busy >= 0.95 && fps < 57.0 ? g_m.bound_secs + 1 : 0;
    g_m.sec_start = now;
    g_m.busy_ns = 0;
    if (g_m.bound_secs >= BOUND_SECS) {
        g_m.decided = true;
        char reason[160];
        snprintf(reason, sizeof(reason), "measured: the GPU at its limit with the exact shaders (%.0f%% busy, %.0f "
                 "frames a second)", busy * 100.0, fps);
        verdict_write("fast", reason);
        hle_log("xbased-hle-back", "precision: the GPU can't keep up with the exact shaders (%.0f%% busy for %u s, %.0f "
                "fps): fast from the next run (auto)", busy * 100.0, BOUND_SECS, fps);
    }
}

static struct {
    double total_ms, max_ms;
    unsigned n;
} g_b;

void hle_precision_exact_build(double ms)
{
    if (!g_measuring || g_m.decided) {
        return;
    }
    hle_mutex_lock(&g_b_mu);           /* (the workers build a few at a time) */
    g_b.total_ms += ms;
    g_b.max_ms = ms > g_b.max_ms ? ms : g_b.max_ms;
    g_b.n++;
    const bool slow = !g_m.decided && (g_b.max_ms >= 5000.0 || g_b.total_ms >= 20000.0);
    if (slow) {
        g_m.decided = true;
    }
    const double total = g_b.total_ms, mx = g_b.max_ms;
    const unsigned n = g_b.n;
    hle_mutex_unlock(&g_b_mu);
    if (slow) {
        char reason[200];
        snprintf(reason, sizeof(reason), "measured: the exact shaders took too long to build here (%u in %.1f s, "
                 "the longest %.1f s)", n, total / 1000.0, mx / 1000.0);
        verdict_write("fast", reason);
        hle_log("xbased-hle-back", "precision: the exact shaders take too long to build here (%u in %.1f s, the "
                "longest %.1f s): fast from the next run (auto)", n, total / 1000.0, mx / 1000.0);
    }
}

void hle_precision_swap(void)
{
    if (g_measuring && !g_m.decided) {
        __atomic_add_fetch(&g_m.frames, 1, __ATOMIC_RELAXED);
    }
}

void hle_precision_build_enter(void)
{
    t_building++;
}

void hle_precision_build_leave(void)
{
    t_building--;
}

void hle_precision_link_enter(void)
{
    t_linking++;
}

void hle_precision_link_leave(void)
{
    t_linking--;
}

bool hle_precision_gpl_blocked(char *why, size_t wn)
{
    char id[128], v[32];
    gpl_id(id, sizeof(id));
    return g_path[0] && verdict_read_id(id, v, sizeof(v), why, wn) && !strcmp(v, "off");
}

void hle_precision_pipe_set(uint64_t key)
{
    t_pipe = key;
}

bool hle_precision_pipe_blocked(uint64_t key)
{
    for (unsigned i = 0; i < g_nbad_pipes; i++) {
        if (g_bad_pipes[i] == key) {
            return true;
        }
    }
    return false;
}

/* This GPU and driver's pipelines that crashed it ("<id>/pipe:<key> off"
 * lines of the verdicts' file) */
static void bad_pipes_read(void)
{
    FILE *f = g_path[0] ? fopen(g_path, "r") : NULL;
    if (!f) {
        return;
    }
    char line[512], pre[128];
    snprintf(pre, sizeof(pre), "%s/pipe:", g_id);
    const size_t pl = strlen(pre);
    while (fgets(line, sizeof(line), f) && g_nbad_pipes < MAX_BAD_PIPES) {
        unsigned long long key;
        char v[16];
        if (!strncmp(line, pre, pl) && sscanf(line + pl, "%llx %15s", &key, v) == 2 && !strcmp(v, "off")) {
            g_bad_pipes[g_nbad_pipes++] = key;
        }
    }
    fclose(f);
    if (g_nbad_pipes) {
        hle_log("xbased-hle-back", "precision: %u pipeline(s) not built on this GPU and driver (they crashed its "
                "compiler in an earlier run): their draws are skipped", g_nbad_pipes);
    }
}

void hle_precision_crashed(void)
{
    if (t_building > 0 && t_pipe && g_path[0]) {
        /* the pipeline itself, kept out from the next run whatever else
         * changes (a crash on fast too: nothing else to fall back to) */
        char id[160];
        snprintf(id, sizeof(id), "%s/pipe:%016llx", g_id, (unsigned long long)t_pipe);
        verdict_write_id(id, "off", "the driver crashed building it in an earlier run");
    }
    if (t_linking > 0 && g_path[0]) {
        /* in the libraries' link: whole pipelines from the next run, the
         * precision as it was (a link compiles none of the shaders anew; it
         * crashed on fast too) */
        char id[128];
        gpl_id(id, sizeof(id));
        verdict_write_id(id, "off", "the driver crashed linking a shader from its parts in an earlier run");
        return;
    }
    if (t_building <= 0 || !g_path[0] || g_option != HLE_PREC_AUTO || g_fast) {
        return;
    }
    verdict_write("fast", "the driver crashed building a shader in an earlier run");
}
