/*
 * xbased (xbased for PC) — the dev tools' control of a run (win_devctl.h).
 * Dev builds only (XBASED_DEVCTL).
 */
#ifdef XBASED_DEVCTL
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "native/app/win_devctl.h"
#include "native/app/win_auto.h"
#include "native/core/irq.h"
#include "native/core/mods.h"
#include "native/core/nc.h"
#include "native/core/vm.h"
#include "native/cpu/cpu.h"
#include "native/dev/dev.h"
#include "native/kernel/kernel.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/vk/back.h"
#include "hw/xbox/hle/vk/devcap.h"
#include "hw/xbox/hle/vk/swapchain.h"

#include <Zydis/Zydis.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>

void nc_audio_set_paused(bool on);
void nc_present_stats(float *fps, float *avg_ms, float *last_ms);

#define REPLY_MAX (300u << 10)
#define WATCH_MAX 16
#define WATCH_EVENTS 256
#define FREEZE_MAX 16
#define WRITE_MAX 4096

static struct {
    NcCore *core;
    char data_dir[MAX_PATH];
    char token[128];
    int port;
    uint64_t t0;                        /* GetTickCount64 at init */
    /* the window thread's command: posted, done */
    CRITICAL_SECTION mu;
    CONDITION_VARIABLE cv;
    char wcmd[1024];
    int wstate;                         /* 0 none, 1 posted, 2 done */
    char wreply[2048];
    /* the loop's pause, and a step */
    bool paused;
    uint64_t step_to;                   /* the swap the step ends at, 0 none */
    /* memory: a write for the CPU's thread, the frozen values */
    uint32_t w_va, w_len;
    uint8_t w_buf[WRITE_MAX];
    volatile LONG w_state;              /* 0 none, 1 posted, 2 done (ok), 3 failed */
    struct {
        uint32_t va, len;
        uint8_t v[16];
    } fz[FREEZE_MAX];
    unsigned nfz;
    /* watches: sampled by their own thread */
    struct {
        uint32_t va, len;
        uint8_t last[16];
        bool have;
        struct {
            uint64_t ms, vblank, swap;
            uint32_t thread;
            uint8_t old[16], now[16];
        } ev[WATCH_EVENTS];
        unsigned nev, lost;
    } wt[WATCH_MAX];
    unsigned nwt;
    CRITICAL_SECTION wmu;
} g_dc;

/* ---- the reply ---------------------------------------------------------------- */

typedef struct Out {
    char *p;
    size_t n, cap;
} Out;

static void out(Out *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(o->p + o->n, o->cap > o->n ? o->cap - o->n : 0, fmt, ap);
    va_end(ap);
    if (k > 0) {
        o->n = o->n + (size_t)k < o->cap ? o->n + (size_t)k : o->cap - 1;
    }
}

/* a JSON string's text (quotes added) */
static void out_str(Out *o, const char *s)
{
    out(o, "\"");
    for (; *s && o->n + 8 < o->cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out(o, "\\%c", c);
        } else if (c < 0x20) {
            out(o, "\\u%04x", c);
        } else {
            o->p[o->n++] = (char)c;
            o->p[o->n] = 0;
        }
    }
    out(o, "\"");
}

static void out_hex(Out *o, const uint8_t *b, unsigned n)
{
    out(o, "\"");
    for (unsigned i = 0; i < n && o->n + 4 < o->cap; i++) {
        out(o, "%02x", b[i]);
    }
    out(o, "\"");
}

static unsigned parse_hex(const char *s, uint8_t *b, unsigned max)
{
    unsigned n = 0;
    while (s[0] && s[1] && n < max) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) {
            break;
        }
        b[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

/* ---- guest memory --------------------------------------------------------------- */

/* n bytes from guest VA va (phys: a physical address) into b; false where unmapped */
static bool mem_get(uint32_t va, uint8_t *b, unsigned n, bool phys)
{
    const NcVm *vm = &g_dc.core->vm;
    for (unsigned i = 0; i < n; i++) {
        const uint32_t pa = phys ? va + i : nc_vm_phys(vm, va + i);
        if (pa == UINT32_MAX || pa >= vm->ram_size) {
            return false;
        }
        b[i] = vm->phys[pa];
    }
    return true;
}

/* (the guest CPU's thread, each vblank: mods.c) the posted write, the frozen values */
static void on_vblank(void)
{
    if (InterlockedCompareExchange(&g_dc.w_state, 1, 1) == 1) {
        const bool ok = nc_mods_write(g_dc.w_va, g_dc.w_buf, g_dc.w_len);
        InterlockedExchange(&g_dc.w_state, ok ? 2 : 3);
    }
    for (unsigned i = 0; i < g_dc.nfz; i++) {
        nc_mods_write(g_dc.fz[i].va, g_dc.fz[i].v, g_dc.fz[i].len);
    }
}

static bool mem_put(uint32_t va, const uint8_t *b, unsigned n)
{
    if (nc_paused()) {                  /* the CPU waits: no vblank comes, nothing races */
        return nc_mods_write(va, b, n);
    }
    memcpy(g_dc.w_buf, b, n);
    g_dc.w_va = va;
    g_dc.w_len = n;
    InterlockedExchange(&g_dc.w_state, 1);
    for (int i = 0; i < 200 && InterlockedCompareExchange(&g_dc.w_state, 1, 1) == 1; i++) {
        Sleep(5);
    }
    const LONG st = InterlockedExchange(&g_dc.w_state, 0);
    return st == 2;
}

static uint32_t va_of(uint32_t pa)
{
    const NcVm *vm = &g_dc.core->vm;
    const uint32_t v = vm->rmap[pa / NC_PAGE];
    return v ? (v & ~(NC_PAGE - 1)) | (pa & (NC_PAGE - 1)) : NC_CONTIG_WINDOW | pa;
}

static DWORD WINAPI watch_main(void *arg)
{
    (void)arg;
    for (;;) {
        Sleep(1);
        EnterCriticalSection(&g_dc.wmu);
        for (unsigned i = 0; i < g_dc.nwt; i++) {
            uint8_t now[16];
            if (!mem_get(g_dc.wt[i].va, now, g_dc.wt[i].len, false)) {
                continue;
            }
            if (g_dc.wt[i].have && memcmp(now, g_dc.wt[i].last, g_dc.wt[i].len)) {
                if (g_dc.wt[i].nev < WATCH_EVENTS) {
                    __typeof__(g_dc.wt[0].ev[0]) *e = &g_dc.wt[i].ev[g_dc.wt[i].nev++];
                    e->ms = GetTickCount64() - g_dc.t0;
                    e->vblank = nc_dev_nv2a_vblanks();
                    e->swap = hle_back_swap_count();
                    const NcThread *t = g_dc.core->kernel.current;
                    e->thread = t ? t->id : 0;
                    memcpy(e->old, g_dc.wt[i].last, g_dc.wt[i].len);
                    memcpy(e->now, now, g_dc.wt[i].len);
                } else {
                    g_dc.wt[i].lost++;
                }
            }
            memcpy(g_dc.wt[i].last, now, g_dc.wt[i].len);
            g_dc.wt[i].have = true;
        }
        LeaveCriticalSection(&g_dc.wmu);
    }
    return 0;
}

/* ---- the guest's threads ------------------------------------------------------------ */

static const char *const k_tstate[] = { "free", "ready", "running", "waiting", "suspended", "terminated" };
static const char *const k_greg[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };

static void cmd_threads(Out *o)
{
    NcKernel *k = &g_dc.core->kernel;
    NcFrame *f = g_nc_paused_frame;
    out(o, "{\"ok\":true,\"paused_frame\":%s,\"current\":%u,\"threads\":[", f ? "true" : "false",
        k->current ? k->current->id : 0);
    bool first = true;
    for (unsigned i = 0; i < k->threads_hi && i < NC_MAX_THREADS; i++) {
        const NcThread *t = &k->threads[i];
        if (t->state == NC_T_FREE) {
            continue;
        }
        uint32_t r[8], eip;
        const bool live = t == k->current && f;
        if (live) {
            for (int g = 0; g < 8; g++) {
                r[g] = nc_frame_gpr(f, g);
            }
            eip = nc_frame_eip(f);
        } else {
            NcCpuState st;
            nc_context_export(&t->ctx, &st);
            for (int g = 0; g < 8; g++) {
                r[g] = (uint32_t)st.gregs[g];
            }
            eip = (uint32_t)st.rip;
        }
        out(o, "%s{\"id\":%u,\"ethread\":\"%08x\",\"state\":\"%s\",\"priority\":%d,\"suspend\":%u,"
               "\"stack\":[\"%08x\",\"%08x\"],\"waits\":%u,\"regs_exact\":%s,\"eip\":\"%08x\"",
            first ? "" : ",", t->id, t->ethread, t->state < 6 ? k_tstate[t->state] : "?", t->priority, t->suspend,
            t->stack_limit, t->stack_base, t->state == NC_T_WAITING ? t->wait.count : 0,
            live || t != k->current ? "true" : "false", eip);
        for (int g = 0; g < 8; g++) {
            out(o, ",\"%s\":\"%08x\"", k_greg[g], r[g]);
        }
        /* its APCs: queued, one running (and how deep), the wait alertable */
        out(o, ",\"apcs\":%u,\"in_apc\":%s,\"apc_nest\":%u,\"alertable\":%s", t->napcs,
            t->apc.in_apc ? "true" : "false", t->apc_nest,
            t->state == NC_T_WAITING && t->wait.alertable ? "true" : "false");
        /* the EBP chain: return addresses */
        out(o, ",\"frames\":[");
        uint32_t bp = r[5];
        for (int d = 0; d < 12 && bp; d++) {
            uint32_t w[2];
            if (!mem_get(bp, (uint8_t *)w, 8, false) || w[0] <= bp) {
                break;
            }
            out(o, "%s\"%08x\"", d ? "," : "", w[1]);
            bp = w[0];
        }
        out(o, "]}");
        first = false;
    }
    out(o, "]}");
}

static void cmd_disasm(Out *o, uint32_t va, unsigned count)
{
    uint8_t b[16 * 64];
    count = count > 64 ? 64 : count ? count : 16;
    unsigned have = 0;
    while (have < sizeof(b) && mem_get(va + have, b + have, 1, false)) {
        have++;
    }
    ZydisDecoder dec;
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    ZydisFormatter fmt;
    ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);
    out(o, "{\"ok\":true,\"insns\":[");
    unsigned at = 0;
    for (unsigned i = 0; i < count && at < have; i++) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, b + at, have - at, &in, op))) {
            out(o, "%s[\"%08x\",\"%02x\",\"(bad)\"]", i ? "," : "", va + at, b[at]);
            at++;
            continue;
        }
        char text[128];
        ZydisFormatterFormatInstruction(&fmt, &in, op, in.operand_count_visible, text, sizeof(text), va + at, NULL);
        out(o, "%s[\"%08x\",", i ? "," : "", va + at);
        out_hex(o, b + at, in.length);
        out(o, ",");
        out_str(o, text);
        out(o, "]");
        at += in.length;
    }
    out(o, "]}");
}

/* ---- commands ---------------------------------------------------------------------- */

/* (the window's thread) one of its commands */
static void run_window(const char *c, char *reply, size_t cap)
{
    if (!strncmp(c, "shot ", 5)) {
        hle_present_shot(c + 5);
        snprintf(reply, cap, "{\"ok\":true}");
    } else if (!strncmp(c, "act ", 4)) {
        nc_auto_act(c + 4);
        snprintf(reply, cap, "{\"ok\":true}");
    } else if (!strncmp(c, "mouse ", 6)) {
        float dx = 0, dy = 0;
        unsigned b = 0;
        int held = -1;
        sscanf(c + 6, "%f %f %u %d", &dx, &dy, &b, &held);
        nc_win_dev_mouse(dx, dy, b, held);
        snprintf(reply, cap, "{\"ok\":true}");
    } else if (!strcmp(c, "pause") || !strcmp(c, "resume")) {
        g_dc.paused = c[0] == 'p';
        g_dc.step_to = 0;
        snprintf(reply, cap, "{\"ok\":true,\"paused\":%s}", g_dc.paused ? "true" : "false");
    } else if (!strncmp(c, "step ", 5)) {
        const unsigned n = (unsigned)strtoul(c + 5, NULL, 10);
        g_dc.step_to = hle_back_swap_count() + (n ? n : 1);
        g_dc.paused = false;
        snprintf(reply, cap, "{\"ok\":true,\"until_swap\":%llu}", (unsigned long long)g_dc.step_to);
    } else {
        snprintf(reply, cap, "{\"ok\":false,\"error\":\"unknown window command\"}");
    }
}

void nc_devctl_tick(bool *paused, bool minimized)
{
    if (!g_dc.core) {
        return;
    }
    /* the loop's own pause (the hotkey's) seen first */
    static bool last = false;
    if (*paused != last) {
        g_dc.paused = *paused;
    }
    EnterCriticalSection(&g_dc.mu);
    if (g_dc.wstate == 1) {
        run_window(g_dc.wcmd, g_dc.wreply, sizeof(g_dc.wreply));
        g_dc.wstate = 2;
        WakeAllConditionVariable(&g_dc.cv);
    }
    LeaveCriticalSection(&g_dc.mu);
    if (g_dc.step_to && hle_back_swap_count() >= g_dc.step_to) {
        g_dc.step_to = 0;
        g_dc.paused = true;
    }
    if (g_dc.paused != *paused) {
        *paused = g_dc.paused;
        nc_kernel_set_paused(&g_dc.core->kernel, *paused || minimized);
        nc_audio_set_paused(*paused || minimized);
        nc_log("xbased-dev", "%s (dev control)", *paused ? "paused" : "resumed");
    }
    last = *paused;
}

/* (the server's thread) a window command, waited for */
static void post_window(const char *c, Out *o)
{
    EnterCriticalSection(&g_dc.mu);
    snprintf(g_dc.wcmd, sizeof(g_dc.wcmd), "%s", c);
    g_dc.wstate = 1;
    bool done = true;
    while (g_dc.wstate != 2) {
        if (!SleepConditionVariableCS(&g_dc.cv, &g_dc.mu, 5000)) {
            done = false;
            break;
        }
    }
    if (done) {
        out(o, "%s", g_dc.wreply);
    } else {
        out(o, "{\"ok\":false,\"error\":\"the window's loop did not take it (a menu open?)\"}");
    }
    g_dc.wstate = 0;
    LeaveCriticalSection(&g_dc.mu);
}

static void cmd_status(Out *o)
{
    float fps = 0, avg = 0, lastf = 0;
    nc_present_stats(&fps, &avg, &lastf);
    const HleXbeInfo *x = &g_hle.title.xbe;
    char stats[2048];
    hle_devcap_stats(stats, sizeof(stats));
    out(o, "{\"ok\":true,\"pid\":%lu,\"title_id\":\"%08x\",\"paused\":%s,\"stepping\":%s,\"uptime_ms\":%llu,"
           "\"swaps\":%llu,\"vblanks\":%llu,\"fps\":%.2f,\"frame_ms\":%.2f,\"last_ms\":%.2f,\"data_dir\":",
        GetCurrentProcessId(), x->title_id, g_dc.paused ? "true" : "false", g_dc.step_to ? "true" : "false",
        (unsigned long long)(GetTickCount64() - g_dc.t0), (unsigned long long)hle_back_swap_count(),
        (unsigned long long)nc_dev_nv2a_vblanks(), fps, avg, lastf);
    out_str(o, g_dc.data_dir);
    out(o, ",\"draw\":{%s}}", stats);
}

static void cmd_mem(Out *o, char *a)
{
    char sub[16] = "";
    unsigned long va = 0, len = 0;
    int pos = 0;
    sscanf(a, "%15s %lx%n", sub, &va, &pos);
    if (!strcmp(sub, "read") || !strcmp(sub, "pread")) {
        len = strtoul(a + pos, NULL, 0);
        len = len > 65536 ? 65536 : len ? len : 16;
        uint8_t *b = malloc(len);
        if (!b || !mem_get((uint32_t)va, b, (unsigned)len, sub[0] == 'p')) {
            out(o, "{\"ok\":false,\"error\":\"not mapped\"}");
        } else {
            out(o, "{\"ok\":true,\"va\":\"%08lx\",\"len\":%lu,\"hex\":", va, len);
            out_hex(o, b, (unsigned)len);
            out(o, "}");
        }
        free(b);
    } else if (!strcmp(sub, "write")) {
        uint8_t b[WRITE_MAX];
        while (a[pos] == ' ') {
            pos++;
        }
        const unsigned n = parse_hex(a + pos, b, sizeof(b));
        if (!n) {
            out(o, "{\"ok\":false,\"error\":\"no bytes\"}");
        } else {
            const bool ok = mem_put((uint32_t)va, b, n);
            out(o, "{\"ok\":%s,\"written\":%u%s}", ok ? "true" : "false", ok ? n : 0,
                ok ? "" : ",\"error\":\"not mapped, or no vblank came\"");
        }
    } else if (!strcmp(sub, "find")) {
        /* (the pattern is the first word after "find": va took it as hex) */
        char pat[512];
        unsigned long max = 64;
        sscanf(a, "%15s %511s %lu", sub, pat, &max);
        uint8_t b[256];
        const unsigned n = parse_hex(pat, b, sizeof(b));
        const NcVm *vm = &g_dc.core->vm;
        out(o, "{\"ok\":true,\"hits\":[");
        unsigned hits = 0;
        for (uint32_t pa = 0; n && pa + n <= vm->ram_size && hits < max; pa++) {
            if (vm->phys[pa] == b[0] && !memcmp(vm->phys + pa, b, n)) {
                out(o, "%s[\"%08x\",\"%08x\"]", hits ? "," : "", va_of(pa), pa);
                hits++;
            }
        }
        out(o, "],\"count\":%u}", hits);
    } else {
        out(o, "{\"ok\":false,\"error\":\"mem read|pread|write|find\"}");
    }
}

static void cmd_watch(Out *o, const char *a)
{
    EnterCriticalSection(&g_dc.wmu);
    unsigned long va = 0, len = 4;
    if (!strncmp(a, "add ", 4) && sscanf(a + 4, "%lx %lu", &va, &len) >= 1 && g_dc.nwt < WATCH_MAX) {
        memset(&g_dc.wt[g_dc.nwt], 0, sizeof(g_dc.wt[0]));
        g_dc.wt[g_dc.nwt].va = (uint32_t)va;
        g_dc.wt[g_dc.nwt].len = len > 16 ? 16 : len ? (uint32_t)len : 4;
        g_dc.nwt++;
        out(o, "{\"ok\":true,\"watches\":%u}", g_dc.nwt);
    } else if (!strcmp(a, "clear")) {
        g_dc.nwt = 0;
        out(o, "{\"ok\":true}");
    } else {
        out(o, "{\"ok\":true,\"watches\":[");
        for (unsigned i = 0; i < g_dc.nwt; i++) {
            out(o, "%s{\"va\":\"%08x\",\"len\":%u,\"now\":", i ? "," : "", g_dc.wt[i].va, g_dc.wt[i].len);
            out_hex(o, g_dc.wt[i].last, g_dc.wt[i].len);
            out(o, ",\"lost\":%u,\"changes\":[", g_dc.wt[i].lost);
            for (unsigned e = 0; e < g_dc.wt[i].nev; e++) {
                out(o, "%s{\"ms\":%llu,\"vblank\":%llu,\"swap\":%llu,\"thread\":%u,\"old\":", e ? "," : "",
                    (unsigned long long)g_dc.wt[i].ev[e].ms, (unsigned long long)g_dc.wt[i].ev[e].vblank,
                    (unsigned long long)g_dc.wt[i].ev[e].swap, g_dc.wt[i].ev[e].thread);
                out_hex(o, g_dc.wt[i].ev[e].old, g_dc.wt[i].len);
                out(o, ",\"new\":");
                out_hex(o, g_dc.wt[i].ev[e].now, g_dc.wt[i].len);
                out(o, "}");
            }
            out(o, "]}");
            g_dc.wt[i].nev = 0;         /* (read: from now on) */
            g_dc.wt[i].lost = 0;
        }
        out(o, "]}");
    }
    LeaveCriticalSection(&g_dc.wmu);
}

static void command(char *c, Out *o)
{
    if (!strcmp(c, "ping")) {
        out(o, "{\"ok\":true,\"pong\":true}");
    } else if (!strcmp(c, "status")) {
        cmd_status(o);
    } else if (!strcmp(c, "stats")) {
        char stats[2048];
        hle_devcap_stats(stats, sizeof(stats));
        out(o, "{\"ok\":true,%s}", stats);
    } else if (!strncmp(c, "shot ", 5) || !strncmp(c, "act ", 4) || !strcmp(c, "pause") || !strcmp(c, "resume") ||
               !strncmp(c, "step ", 5) || !strncmp(c, "mouse ", 6)) {
        post_window(c, o);
    } else if (!strcmp(c, "quit")) {
        post_window("act quit", o);
    } else if (!strncmp(c, "mem ", 4)) {
        cmd_mem(o, c + 4);
    } else if (!strncmp(c, "watch ", 6) || !strcmp(c, "watch")) {
        cmd_watch(o, c[5] ? c + 6 : "get");
    } else if (!strncmp(c, "freeze ", 7)) {
        unsigned long va;
        int pos = 0;
        if (sscanf(c + 7, "%lx %n", &va, &pos) >= 1 && g_dc.nfz < FREEZE_MAX) {
            const unsigned n = parse_hex(c + 7 + pos, g_dc.fz[g_dc.nfz].v, 16);
            if (n) {
                g_dc.fz[g_dc.nfz].va = (uint32_t)va;
                g_dc.fz[g_dc.nfz].len = n;
                g_dc.nfz++;
            }
        }
        out(o, "{\"ok\":true,\"frozen\":%u}", g_dc.nfz);
    } else if (!strcmp(c, "unfreeze")) {
        g_dc.nfz = 0;
        out(o, "{\"ok\":true}");
    } else if (!strcmp(c, "threads")) {
        cmd_threads(o);
    } else if (!strncmp(c, "disasm ", 7)) {
        unsigned long va = 0, n = 16;
        sscanf(c + 7, "%lx %lu", &va, &n);
        cmd_disasm(o, (uint32_t)va, (unsigned)n);
    } else if (!strncmp(c, "prop get ", 9)) {
        char v[PROP_VALUE_MAX] = "";
        __system_property_get(c + 9, v);
        out(o, "{\"ok\":true,\"value\":");
        out_str(o, v);
        out(o, "}");
    } else if (!strncmp(c, "prop set ", 9)) {
        char name[PROP_NAME_MAX] = "";
        int pos = 0;
        sscanf(c + 9, "%31s %n", name, &pos);
        nc_win_prop_set(name, c[9 + pos] ? c + 9 + pos : NULL);
        out(o, "{\"ok\":true}");
    } else if (!strncmp(c, "capture ", 8)) {
        char path[512] = "";
        int detail = -1;
        sscanf(c + 8, "%511s %d", path, &detail);
        hle_devcap_arm(path, detail);
        out(o, "{\"ok\":true,\"from_swap\":%llu}", (unsigned long long)hle_back_swap_count());
    } else if (!strncmp(c, "frames ", 7)) {
        uint64_t next = 0;
        out(o, "{\"ok\":true,\"rows\":[");
        o->n += hle_devcap_frames(strtoull(c + 7, NULL, 10), o->p + o->n, o->cap - o->n - 64, &next);
        out(o, "],\"next\":%llu}", (unsigned long long)next);
    } else if (!strncmp(c, "skip", 4)) {
        hle_devcap_skip(c[4] == ' ' ? c + 5 : "");
        out(o, "{\"ok\":true}");
    } else {
        out(o, "{\"ok\":false,\"error\":\"unknown command\"}");
    }
}

static DWORD WINAPI server_main(void *arg)
{
    (void)arg;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)g_dc.port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    /* (a run started again, a savestate's load: the run before holds the port
     * until it hands the window over, then exits; taken once it lets go) */
    SOCKET ls = INVALID_SOCKET;
    for (int tries = 0; tries < 120; tries++) {
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls != INVALID_SOCKET && !bind(ls, (struct sockaddr *)&sa, sizeof(sa)) && !listen(ls, 2)) {
            break;
        }
        if (ls != INVALID_SOCKET) {
            closesocket(ls);
            ls = INVALID_SOCKET;
        }
        Sleep(250);
    }
    if (ls == INVALID_SOCKET) {
        nc_log("xbased-dev", "dev control: port %d not taken (%d)", g_dc.port, WSAGetLastError());
        return 0;
    }
    nc_log("xbased-dev", "dev control on 127.0.0.1:%d%s", g_dc.port, g_dc.token[0] ? " (token)" : "");
    char *reply = malloc(REPLY_MAX);
    for (;;) {
        SOCKET s = accept(ls, NULL, NULL);
        if (s == INVALID_SOCKET) {
            continue;
        }
        bool authed = !g_dc.token[0];
        char line[8192];
        size_t have = 0;
        for (;;) {
            const int r = recv(s, line + have, (int)(sizeof(line) - 1 - have), 0);
            if (r <= 0) {
                break;
            }
            have += (size_t)r;
            line[have] = 0;
            char *nl;
            while ((nl = strchr(line, '\n')) != NULL) {
                *nl = 0;
                if (nl > line && nl[-1] == '\r') {
                    nl[-1] = 0;
                }
                Out o = { reply, 0, REPLY_MAX };
                reply[0] = 0;
                if (!authed) {
                    authed = !strncmp(line, "auth ", 5) && !strcmp(line + 5, g_dc.token);
                    out(&o, authed ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"auth\"}");
                } else {
                    command(line, &o);
                }
                out(&o, "\n");
                for (size_t sent = 0; sent < o.n;) {
                    const int w = send(s, reply + sent, (int)(o.n - sent), 0);
                    if (w <= 0) {
                        break;
                    }
                    sent += (size_t)w;
                }
                const size_t used = (size_t)(nl + 1 - line);
                memmove(line, nl + 1, have - used + 1);
                have -= used;
            }
            if (have >= sizeof(line) - 1) {
                have = 0;               /* (a line too long: dropped) */
            }
        }
        closesocket(s);
    }
    return 0;
}

void nc_devctl_init(const char *data_dir, NcCore *core)
{
    g_dc.core = core;
    g_dc.t0 = GetTickCount64();
    snprintf(g_dc.data_dir, sizeof(g_dc.data_dir), "%s", data_dir);
    const char *p = getenv("XBASED_DEVCTL_PORT"), *t = getenv("XBASED_DEVCTL_TOKEN");
    g_dc.port = p && *p ? atoi(p) : 47860;
    snprintf(g_dc.token, sizeof(g_dc.token), "%s", t ? t : "");
    InitializeCriticalSection(&g_dc.mu);
    InitializeCriticalSection(&g_dc.wmu);
    InitializeConditionVariable(&g_dc.cv);
    nc_mods_vblank_hook = on_vblank;
    CreateThread(NULL, 0, server_main, NULL, 0, NULL);
    CreateThread(NULL, 0, watch_main, NULL, 0, NULL);
}
#endif
