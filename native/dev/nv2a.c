/*
 * xbased native core — NV2A register space (0xFD000000, 16 MiB), first cut.
 *
 * A register file (reads return what was last written) plus the behavior the
 * XDK's D3D needs, added as the frontier shows it:
 *   PMC_BOOT_0      0x000000  chip identification
 *   PMC_INTR_0      0x000100  pending summary (bit 24 PCRTC, 20 PTIMER), read-only
 *   PMC_INTR_EN_0   0x000140  bit 0: interrupts to the CPU
 *   PTIMER          0x009400/0x009410  the time, low/high: the core clock
 *                   times 32 * DENOMINATOR (0x009210) / NUMERATOR (0x009200)
 *                   on the guest's clock, writable; ALARM_0 0x009420: INTR_0
 *                   0x009100 bit 0 when the low word reaches it (write 1 to
 *                   clear), INTR_EN_0 0x009140
 *   PFB_CSTATUS     0x10020C  installed memory size
 *   PCRTC_INTR_0    0x600100  bit 0 vblank, write 1 to clear
 *   PCRTC_INTR_EN_0 0x600140
 *   PCRTC_RASTER    0x600808  current scanline
 *   PFIFO_RUNOUT_STATUS 0x002400, CACHE1_STATUS 0x003214  always empty (low
 *                   mark), CACHE1_DMA_PUSH 0x003220 never busy: the HLE GPU
 *                   takes the push buffer, so the FIFO is idle whenever D3D
 *                   waits for it (Forza's wait-for-idle spins otherwise).
 *                   In push-buffer mode they follow the HLE's GET.
 *   NV_USER         0x800000  channel 0's DMA_PUT/GET/REF (+0x40/44/48), the
 *                   FIFO's CACHE1 DMA_PUT/GET/REF; a DMA_PUT write in
 *                   push-buffer mode kicks the HLE's decoder (hle_pb_kick)
 * Vblank comes from the host clock at 60 Hz. The interrupt goes out on bus
 * level 3. Written from the register map, without the LLE device.
 */
#include "native/dev/dev.h"
#include "hw/xbox/hle/host.h"
#include "hw/xbox/hle/pb/pbdraw.h"
#include "hw/xbox/hle/pb/pbmode.h"
#include "native/core/irq.h"
#include "native/core/mmio.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include "hw/xbox/hle/prop.h"
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#elif defined(NC_DESKTOP)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define NV2A_BASE 0xFD000000u
#define NV2A_SIZE 0x01000000u
#define NV2A_IRQ  3

#define NV_PMC_BOOT_0       0x000000u
#define NV_PMC_INTR_0       0x000100u
#define NV_PMC_INTR_EN_0    0x000140u
#define NV_PFIFO_RUNOUT_STATUS 0x002400u
#define NV_PFIFO_CACHE1_STATUS 0x003214u
#define NV_PFIFO_CACHE1_DMA_PUSH 0x003220u
#define NV_PFIFO_CACHE1_DMA_PUT  0x003240u
#define NV_PFIFO_CACHE1_DMA_GET  0x003244u
#define NV_PFIFO_CACHE1_REF      0x003248u
#define NV_PFIFO_CACHE1_DMA_SUBROUTINE 0x00324Cu
#define NV_PGRAPH_INTR      0x400100u
#define NV_PGRAPH_INTR_EN   0x400140u
#define NV_PGRAPH_STATUS    0x400700u
#define NV_PGRAPH_RDI_INDEX 0x400750u   /* select 24:16, word address 12:2 */
#define NV_PGRAPH_RDI_DATA  0x400754u
#define NV_PGRAPH_SURFACE   0x400710u
#define NV_PGRAPH_INCREMENT 0x40071Cu
#define NV_PGRAPH_FIFO      0x400720u
#define NV_USER_DMA_PUT     0x800040u
#define NV_USER_DMA_GET     0x800044u
#define NV_USER_REF         0x800048u
#define NV_PVIDEO_BUFFER    0x008700u   /* bit 0: buffer 0 in use (the overlay shows it) */
#define NV_PVIDEO_STOP      0x008704u
#define NV_PTIMER_INTR_0    0x009100u
#define NV_PTIMER_INTR_EN_0 0x009140u
#define NV_PTIMER_NUMERATOR   0x009200u
#define NV_PTIMER_DENOMINATOR 0x009210u
#define NV_PTIMER_TIME_0    0x009400u
#define NV_PTIMER_TIME_1    0x009410u
#define NV_PTIMER_ALARM_0   0x009420u
#define NV_PFB_CFG0         0x100200u
#define NV_PFB_CSTATUS      0x10020Cu
#define NV_PFB_WBC          0x100410u
#define NV_PCRTC_INTR_0     0x600100u
#define NV_PCRTC_INTR_EN_0  0x600140u
#define NV_PCRTC_START      0x600800u
#define NV_PCRTC_RASTER     0x600808u
#define NV_PRAMDAC_NVPLL_COEFF 0x680500u
#define NV_PRAMDAC_VPLL_COEFF  0x680508u

#define PMC_INTR_PCRTC   (1u << 24)
#define PMC_INTR_PTIMER  (1u << 20)
#define PMC_INTR_PGRAPH  (1u << 12)
#define PCRTC_VBLANK     1u
#define PTIMER_ALARM     1u
#define PFIFO_LOW_MARK   (1u << 4) /* RUNOUT_STATUS / CACHE1_STATUS: empty */
#define PFIFO_DMA_BUSY   (1u << 4) /* CACHE1_DMA_PUSH state */

#define FRAME_NS   16683333ull /* 59.94 Hz */
#define LINES      525u

static uint8_t *g_regs;
static uint64_t g_vblanks;
static uint64_t g_frame_ns = FRAME_NS;
static uint64_t g_last_vblank_ns;     /* when the last vblank came (the raster counts from it) */

/* The panel's vsync (the app's Choreographer, nc_dev_nv2a_display_vsync):
 * when the display's period is a whole multiple of a frame's (60 Hz, 120 Hz),
 * the vblank runs at it. Free running at 59.94 Hz beside a 60 Hz panel, two
 * frames now and then fell in one refresh and one was never shown (JSRF at
 * 60 fps: one a second). Its phase: half a period from where the presenter
 * picks frames up (nc_dev_nv2a_display_pickup), the most either may jitter
 * before a frame is shown twice and the next never. The presenter's loop is
 * paced by its window's buffer queue, not the vsync: on the RP5 it picked up
 * 1-3 ms before vblanks set VSYNC_LEAD before the vsync, and one ~3 ms late
 * pickup skipped a frame (NGB at 60 fps: one every ~20 s). VSYNC_LEAD is the
 * phase until pickups are seen. */
static uint64_t g_disp_vsync_ns, g_disp_period_ns;
/* The display mode's refresh period (nc_dev_nv2a_display_nominal; 0: not
 * known). The vsyncs seen apart from it aren't the panel's: a variable
 * refresh rate display (G-Sync, FreeSync) refreshes as frames come, and the
 * compositor's vblanks then follow the presents, or come every other tick
 * (Windows' DwmFlush, every 5.87 ms beside a 280 Hz mode). Locked to them,
 * the vblank followed its own presents, and the game ran at 57 or 62 fps:
 * it runs free instead. */
static uint64_t g_disp_nominal_ns;
/* The vsyncs as the presenter's swapchain has them (hle_host_display_sync:
 * the driver's own instants, where the compositor's above are a thread's
 * wake after each), the lead its timed presents keep, and when they came
 * (stale after VSYNC_STALE: the others again). */
static uint64_t g_sync_vsync_ns, g_sync_period_ns, g_sync_lead_ns, g_sync_fed_ns;
static bool g_vblank_locked;          /* next_vblank's last: locked to the vsyncs */
static bool g_vblank_timed;           /* ... and placed for the presenter's timed presents */
static int64_t g_pick_off_ns = -1;    /* pickups after a vsync (smoothed, 0..period) */
#define VSYNC_LEAD   3000000ll
#define VSYNC_STALE  200000000ull
/* Push-buffer mode (hle_host_pb_mode): the HLE consumes channel 0's FIFO. */
static bool g_pbmode;

/* debug: NC_NV2A_CENSUS=1 / debug.xbased.nvcensus=1 at boot: the registers the
 * guest reads and writes, by offset: each new one logged as it first comes
 * (the first 512), the busiest once a second (xbased-native-nv2a). */
#define CENSUS_SLOTS 4096u
static struct {
    bool on;
    struct {
        uint32_t off;               /* offset + 1; 0: free */
        uint32_t rd, wr, last;
    } r[CENSUS_SLOTS];
    unsigned nnew;
    uint64_t t_log;
} g_census;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t *reg(uint32_t off)
{
    return (uint32_t *)(g_regs + off);
}

static uint32_t ld(uint32_t off)
{
    return __atomic_load_n(reg(off), __ATOMIC_ACQUIRE);
}

static void census(uint32_t off, bool write, uint32_t v)
{
#if !XBASED_DIAG
    (void)off;                          /* (a build's made to investigate) */
    (void)write;
    (void)v;
#else
    uint32_t key = off & ~3u;
    unsigned i = (key * 2654435761u) >> 20 & (CENSUS_SLOTS - 1);
    for (unsigned k = 0; k < 16; k++, i = (i + 1) & (CENSUS_SLOTS - 1)) {
        if (g_census.r[i].off == key + 1) {
            break;
        }
        if (!g_census.r[i].off) {
            g_census.r[i].off = key + 1;
            if (g_census.nnew++ < 512) {
                nc_log("xbased-native-nv2a", "new register %06x: first %s %08x", key,
                       write ? "write" : "read", v);
            }
            break;
        }
    }
    if (write) {
        g_census.r[i].wr++;
    } else {
        g_census.r[i].rd++;
    }
    g_census.r[i].last = v;
#endif
}

/* Once a second from the vblank thread: the busiest registers since. */
static void census_log(void)
{
    struct { uint32_t n, off, rd, wr, last; } top[16] = { { 0 } };
    for (unsigned i = 0; i < CENSUS_SLOTS; i++) {
        uint32_t n = g_census.r[i].rd + g_census.r[i].wr;
        if (!g_census.r[i].off || n <= top[15].n) {
            continue;
        }
        int k = 15;
        while (k > 0 && top[k - 1].n < n) {
            top[k] = top[k - 1];
            k--;
        }
        top[k].n = n;
        top[k].off = g_census.r[i].off - 1;
        top[k].rd = g_census.r[i].rd;
        top[k].wr = g_census.r[i].wr;
        top[k].last = g_census.r[i].last;
        g_census.r[i].rd = g_census.r[i].wr = 0;
    }
    char buf[900];
    int o = 0;
    for (int k = 0; k < 16 && top[k].n && o < (int)sizeof(buf) - 48; k++) {
        o += snprintf(buf + o, sizeof(buf) - o, " %06x:r%u/w%u=%08x", top[k].off, top[k].rd,
                      top[k].wr, top[k].last);
    }
    buf[o < (int)sizeof(buf) ? o : (int)sizeof(buf) - 1] = 0;
    if (o) {
        nc_log("xbased-native-nv2a", "registers/s:%s", buf);
    }
}

/* The FIFO in push-buffer mode: empty when the HLE has consumed to PUT. */
static bool fifo_empty(void)
{
    return !g_pbmode || ld(NV_PFIFO_CACHE1_DMA_GET) == ld(NV_PFIFO_CACHE1_DMA_PUT);
}

uint64_t nc_ke_now_ns(void);           /* native/kernel/ke.c */
#if XBASED_DIAG
/* ---- debug: the GPU event trace (host.h hle_host_gpu_trace) ---- */
#define GTR_N 32768u
static struct {
    uint64_t ns;
    uint32_t kind, value;
} g_gtr[GTR_N];
static uint32_t g_gtr_pos;

void hle_host_gpu_trace(unsigned kind, uint32_t value)
{
    if (!g_pbmode) {
        return;
    }
    uint32_t i = __atomic_fetch_add(&g_gtr_pos, 1, __ATOMIC_RELAXED) % GTR_N;
    g_gtr[i].ns = nc_ke_now_ns();      /* the guest thread: once per kernel entry */
    g_gtr[i].kind = kind;
    g_gtr[i].value = value;
}

uint32_t hle_host_gpu_trace_mark(void)
{
    return __atomic_load_n(&g_gtr_pos, __ATOMIC_ACQUIRE);
}

void hle_host_gpu_trace_dump(const char *why, uint32_t mark)
{
    static const char *const k_names[] = {
        "?", "vblank", "pcrtc-clr", "pmc-en", "intr-clr", "fifo", "increment", "start",
        "trap", "flip-inc", "flip-stall", "resume", "dpc-queue", "dpc-run", "dpc-refused",
        "isr-ret", "irql",
    };
    uint32_t now = __atomic_load_n(&g_gtr_pos, __ATOMIC_ACQUIRE);
    uint32_t from = mark > 150 ? mark - 150 : 0, end = mark + 6000 < now ? mark + 6000 : now;
    if (now - from > GTR_N) {
        from = now - GTR_N;                     /* overwritten since */
    }
    uint32_t n = end > from ? end - from : 0;
    if (!n) {
        return;
    }
    const uint64_t t0 = g_gtr[(end - n) % GTR_N].ns;
    nc_log("xbased-native-trace", "%s: %u GPU events, from 150 before the mark", why, n);
    char line[400];
    int o = 0;
    for (uint32_t j = end - n; j != end; j++) {
        const uint32_t i = j % GTR_N;
        const uint32_t k = g_gtr[i].kind < sizeof(k_names) / sizeof(k_names[0]) ? g_gtr[i].kind : 0;
        o += snprintf(line + o, sizeof(line) - (size_t)o, " +%.2f %s %x",
                      (g_gtr[i].ns - t0) / 1e6, k_names[k], g_gtr[i].value);
        if (o > 300 || j + 1 == end) {
            nc_log("xbased-native-trace", "%s", line);
            o = 0;
        }
    }
}
#endif

static uint32_t pmc_intr(void)
{
    uint32_t v = 0;
    if (ld(NV_PCRTC_INTR_0) & ld(NV_PCRTC_INTR_EN_0)) {
        v |= PMC_INTR_PCRTC;
    }
    if (g_pbmode && (ld(NV_PGRAPH_INTR) & ld(NV_PGRAPH_INTR_EN))) {
        v |= PMC_INTR_PGRAPH;                       /* push-buffer mode: a software trap */
    }
    if (ld(NV_PTIMER_INTR_0) & ld(NV_PTIMER_INTR_EN_0) & PTIMER_ALARM) {
        v |= PMC_INTR_PTIMER;
    }
    return v;
}

/* The line as the registers have it, looked at and set as one step. The
 * vblank and alarm threads, the decoder (a trap) and the guest (its ISR
 * clearing what it handled) each change a register and then come here: with
 * the look and the set apart, one of them set the level it had seen before
 * another's change, after that one's: a line left low with an interrupt
 * pending (taken a vblank late), or high with none (its ISR run for nothing
 * until the next change). */
static pthread_mutex_t g_irq_mu = PTHREAD_MUTEX_INITIALIZER;

static void update_irq(void)
{
    pthread_mutex_lock(&g_irq_mu);
    nc_irq_set(NV2A_IRQ, (ld(NV_PMC_INTR_EN_0) & 1) && pmc_intr());
    pthread_mutex_unlock(&g_irq_mu);
}

/* ---- PTIMER's time ---- */

uint64_t nc_ke_host_ns(uint64_t guest_ns);  /* native/kernel/ke.c */

/* The time counts the core clock (16.666666 MHz * 28 / 2, NVPLL) times 32 *
 * DENOMINATOR / NUMERATOR: 0.99995 GHz at the console's 56966 / 7629, the
 * low 5 bits read 0, the high word 29 bits; a zero in either stops it. It
 * goes on the guest's clock from where it was last set or rescaled
 * (nxdk_low_level_nv2a_tests' ptimer_alarm_test: Time R/W, Rollover, Clock
 * Scaling Ratios; it was the guest's nanoseconds, fixed). */
#define PTIMER_CORE_HZ 233333324ull
#define PTIMER_NUM0    56966u
#define PTIMER_DEN0    7629u
static struct {
    pthread_mutex_t mu;
    uint64_t ticks, ns;             /* the time at the guest's ns (ns 0: at the first look) */
    uint64_t set_ns;                /* the guest's ns when it last set the time (TIME_0 / TIME_1),
                                       0: not since the start or the state's load */
    bool strict;                    /* the alarm was set (or the time) within ALARM_STRICT_NS
                                       of that: the console's rule for it (alarm_due) */
} g_pt = {PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, false};

/* (g_pt.mu held) */
static uint64_t pt_at(uint64_t now)
{
    if (!g_pt.ns) {
        g_pt.ticks = g_pt.ns = now;
    }
    const uint64_t num = ld(NV_PTIMER_NUMERATOR), den = ld(NV_PTIMER_DENOMINATOR);
    if (!num || !den || now <= g_pt.ns) {
        return g_pt.ticks;
    }
    return g_pt.ticks + (uint64_t)((unsigned __int128)(now - g_pt.ns) * (PTIMER_CORE_HZ * 32u * den) /
                                   (num * 1000000000ull));
}

/* The guest ns the time takes to go d further, rounded up; UINT64_MAX if it
 * stands still (g_pt.mu held) */
static uint64_t pt_ns_for(uint64_t d)
{
    const uint64_t num = ld(NV_PTIMER_NUMERATOR), den = ld(NV_PTIMER_DENOMINATOR);
    if (!num || !den) {
        return UINT64_MAX;
    }
    const unsigned __int128 q = PTIMER_CORE_HZ * 32u * den;
    return (uint64_t)(((unsigned __int128)d * num * 1000000000ull + q - 1) / q);
}

static uint64_t pt_now(void)
{
    pthread_mutex_lock(&g_pt.mu);
    const uint64_t t = pt_at(nc_ke_now_ns());
    pthread_mutex_unlock(&g_pt.mu);
    return t;
}

/* ---- PTIMER's alarm ---- */

/* The alarm interrupt comes when the time's low word reaches ALARM_0: D3D
 * times its vblank work with it in the modes whose vblank it doesn't take
 * from the encoder (Project Zero II's PAL disc, 60 Hz on SCART: its swaps
 * flip at the alarm's vblanks, and with no alarm the first one never did),
 * and its timer callbacks. The ptimer thread, started at the first alarm a
 * title sets, waits for it on the guest's clock, which stands still while
 * the emulation is paused and goes on from a savestate's.
 *
 * The console's alarm comes only as the low word gets to it: one set where
 * the time already is or behind it waits for the word to come round (4.3 s;
 * ptimer_alarm_test's Alarm Set In The Past, 0x10000000 behind, and
 * Unaligned Target, 0x201F over a time of 0x2000). Here one the time passed
 * less than ALARM_LATE ago comes at once: D3D sets its alarms ahead of a time
 * it read, and the emulation (a hitch of ours) can make that late, where its
 * handler checks the 64-bit time itself. The console's rule holds for an
 * alarm set right after the guest set the time (within ALARM_STRICT_NS:
 * the tests set the time, then an alarm against it). D3D sets the time too,
 * to 0 at its start (Prince of Persia - Warrior Within, Grand Theft Auto -
 * Vice City): with the rule kept for every guest that ever set it, their
 * late alarms waited for the low word to come round, the loading screens
 * black and the play stopped for seconds. */
#define ALARM_LATE      1000000000u /* in the time's units, ~1 s */
#define ALARM_STRICT_NS 10000000ull /* 10 ms of the guest's time */
#define ALARM_POLL_NS  20000000ull  /* the pause looked at this often */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_once_t once;
    bool rearm;                     /* ALARM_0 written (or a savestate's): due again; set at first,
                                     * the thread maybe looking before alarm_set */
    uint64_t due;                   /* guest ns; 0: none */
} g_alarm = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_ONCE_INIT, true, 0};

/* The guest ns at which the time's low word, at now, next reaches alarm
 * (UINT64_MAX: the time stands still) */
static uint64_t alarm_due(uint32_t alarm, uint64_t now)
{
    pthread_mutex_lock(&g_pt.mu);
    const uint32_t d = (alarm & ~0x1Fu) - ((uint32_t)pt_at(now) & ~0x1Fu);
    const bool late = !g_pt.strict && d >= (uint32_t)-ALARM_LATE;
    const uint64_t w = d == 0 || late ? 0 : pt_ns_for(d);
    pthread_mutex_unlock(&g_pt.mu);
    return w == UINT64_MAX ? UINT64_MAX : now + w;
}

/* The guest ns the low word takes to come round (UINT64_MAX: it doesn't) */
static uint64_t alarm_period(void)
{
    pthread_mutex_lock(&g_pt.mu);
    const uint64_t w = pt_ns_for(1ull << 32);
    pthread_mutex_unlock(&g_pt.mu);
    return w;
}

static void *ptimer_thread(void *arg)
{
    nc_pause_register("ptimer");
    pthread_mutex_lock(&g_alarm.mu);
    for (;;) {
        if (nc_paused()) {
            pthread_mutex_unlock(&g_alarm.mu);
            nc_pause_wait();
            pthread_mutex_lock(&g_alarm.mu);
            g_alarm.rearm = true;           /* a savestate's clock and alarm, maybe */
        }
        const uint64_t now = nc_ke_now_ns();
        if (g_alarm.rearm) {
            g_alarm.rearm = false;
            g_alarm.due = alarm_due(ld(NV_PTIMER_ALARM_0), now);
        }
        if (now >= g_alarm.due) {
            __atomic_fetch_or(reg(NV_PTIMER_INTR_0), PTIMER_ALARM, __ATOMIC_ACQ_REL);
            update_irq();
            const uint64_t p = alarm_period();  /* the low word comes round to it again */
            g_alarm.due = p == UINT64_MAX ? UINT64_MAX : g_alarm.due + p;
            continue;
        }
        const uint64_t cap = now_ns() + ALARM_POLL_NS;
        uint64_t until = g_alarm.due == UINT64_MAX ? cap : nc_ke_host_ns(g_alarm.due);
        until = until < cap ? until : cap;
        const struct timespec ts = {(time_t)(until / 1000000000ull), (long)(until % 1000000000ull)};
        pthread_cond_timedwait(&g_alarm.cv, &g_alarm.mu, &ts);
    }
    return NULL;
}

static void ptimer_start(void)
{
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_destroy(&g_alarm.cv);
    pthread_cond_init(&g_alarm.cv, &ca);
    pthread_condattr_destroy(&ca);
    pthread_t t;
    pthread_create(&t, NULL, ptimer_thread, NULL);
    pthread_detach(t);
    nc_log("xbased-native", "nv2a: PTIMER alarm in use (alarm %08x, interrupt %s)", ld(NV_PTIMER_ALARM_0),
           ld(NV_PTIMER_INTR_EN_0) & PTIMER_ALARM ? "on" : "off");
}

/* ALARM_0 written, or restored */
static void alarm_set(void)
{
    pthread_once(&g_alarm.once, ptimer_start);
    pthread_mutex_lock(&g_alarm.mu);
    g_alarm.rearm = true;
    pthread_cond_signal(&g_alarm.cv);
    pthread_mutex_unlock(&g_alarm.mu);
}

/* The time set or rescaled: when the alarm comes again, if anything waits
 * for it (a guest that sets the time itself starts the alarm too: INTR_0
 * latches what it crosses, armed or not; D3D's scale written at its start
 * doesn't, for a title that never sets an alarm) */
static bool g_alarm_started;
static void alarm_retime(bool start)
{
    if (start || __atomic_load_n(&g_alarm_started, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_alarm_started, true, __ATOMIC_RELEASE);
        alarm_set();
    }
}

/* INTR_0 or PMC's summary read: an alarm the time has reached is latched
 * now, as the console's is the moment the low word gets there (the thread's
 * wake is late by the host's timer, a millisecond or more on Windows: the
 * Interrupt Acknowledge test reads INTR_0 1 ms after the alarm) */
static void alarm_poll(void)
{
    if (!__atomic_load_n(&g_alarm_started, __ATOMIC_ACQUIRE)) {
        return;
    }
    bool fired = false;
    pthread_mutex_lock(&g_alarm.mu);
    const uint64_t now = nc_ke_now_ns();
    if (g_alarm.rearm) {
        g_alarm.rearm = false;
        g_alarm.due = alarm_due(ld(NV_PTIMER_ALARM_0), now);
    }
    if (now >= g_alarm.due) {
        __atomic_fetch_or(reg(NV_PTIMER_INTR_0), PTIMER_ALARM, __ATOMIC_ACQ_REL);
        const uint64_t p = alarm_period();
        g_alarm.due = p == UINT64_MAX ? UINT64_MAX : g_alarm.due + p;
        fired = true;
    }
    pthread_mutex_unlock(&g_alarm.mu);
    if (fired) {
        update_irq();
    }
}

/* TIME_0 / TIME_1 / NUMERATOR / DENOMINATOR written */
static void ptimer_write(uint32_t off, uint32_t v)
{
    const uint64_t now = nc_ke_now_ns();
    pthread_mutex_lock(&g_pt.mu);
    uint64_t t = pt_at(now);
    if (off == NV_PTIMER_TIME_0) {
        t = (t & ~0xFFFFFFFFull) | (v & ~0x1Fu);
        g_pt.set_ns = now ? now : 1;
        g_pt.strict = true;
    } else if (off == NV_PTIMER_TIME_1) {
        t = (t & 0xFFFFFFFFull) | (uint64_t)(v & 0x1FFFFFFFu) << 32;
        g_pt.set_ns = now ? now : 1;
        g_pt.strict = true;
    } else {
        __atomic_store_n(reg(off), v & 0xFFFFu, __ATOMIC_RELEASE);
    }
    g_pt.ticks = t;
    g_pt.ns = now;
    pthread_mutex_unlock(&g_pt.mu);
    alarm_retime(off == NV_PTIMER_TIME_0 || off == NV_PTIMER_TIME_1);
}

static uint64_t nv2a_read_reg(uint32_t off, unsigned size);

static uint64_t nv2a_read(void *opaque, uint32_t off, unsigned size)
{
    const uint64_t v = nv2a_read_reg(off, size);
    if (__builtin_expect(g_census.on, 0)) {
        census(off, false, (uint32_t)v);
    }
    return v;
}

static uint64_t nv2a_read_reg(uint32_t off, unsigned size)
{
    /* Idle FIFO bits, for any access width inside the register (D3D tests
     * CACHE1_STATUS with a byte read). */
    const uint32_t word = off & ~3u, shift = (off & 3u) * 8;
    if (size <= 4 && (off & 3u) + size <= 4) {
        uint32_t v;
        switch (word) {
        case NV_PFIFO_RUNOUT_STATUS:
        case NV_PFIFO_CACHE1_STATUS:
            v = fifo_empty() ? PFIFO_LOW_MARK : 0;
            break;
        case NV_PFIFO_CACHE1_DMA_PUSH:
            v = fifo_empty() ? ld(word) & ~PFIFO_DMA_BUSY : ld(word) | PFIFO_DMA_BUSY;
            break;
        case NV_USER_DMA_PUT:
        case NV_USER_DMA_GET:
        case NV_USER_REF:
            if (!g_pbmode) {
                goto plain;
            }
            v = ld(word - NV_USER_DMA_PUT + NV_PFIFO_CACHE1_DMA_PUT);
            break;
        case NV_PFB_WBC:
            v = 0; /* no write-combine flush pending (Riddick spins on it) */
            break;
        case NV_PGRAPH_STATUS:
            if (g_pbmode && fifo_empty()) {
                hle_pbd_idle_seen();    /* the guest waited for the GPU (pbkit's pb_busy) */
            }
            goto plain;
        default:
            goto plain;
        }
        return size == 4 ? v : (v >> shift) & ((1u << (size * 8)) - 1);
    }
plain:
    switch (off) {
    case NV_PMC_INTR_0:
        alarm_poll();
        return pmc_intr();
    case NV_PTIMER_INTR_0:
        alarm_poll();
        break;
    case NV_PGRAPH_RDI_DATA: {
        /* RDI: the word at RDI_INDEX's address in the RAM its select names,
         * the address one on after each read. The vertex constants (0x17,
         * 0xCC: xemu's names) give each constant's words w, z, y, x; a
         * program's writes to them under CXT_WRITE_EN are read so
         * (pbdraw.c cxt_after). Other RAMs read 0 here. */
        const uint32_t idx = ld(NV_PGRAPH_RDI_INDEX), sel = (idx >> 16) & 0x1FFu, addr = (idx >> 2) & 0x7FFu;
        __atomic_store_n(reg(NV_PGRAPH_RDI_INDEX), (idx & ~0x1FFCu) | (((addr + 1) & 0x7FFu) << 2),
                         __ATOMIC_RELEASE);
        return sel == 0x17 || sel == 0xCC ? hle_pbd_rdi_constant(addr) : 0;
    }
    case NV_PTIMER_TIME_0:
        return (uint32_t)pt_now() & ~0x1Fu;
    case NV_PTIMER_TIME_1:
        return (uint32_t)(pt_now() >> 32) & 0x1FFFFFFFu;
    case NV_PCRTC_RASTER: { /* the line since the last vblank */
        const uint64_t since = now_ns() - __atomic_load_n(&g_last_vblank_ns, __ATOMIC_ACQUIRE);
        return (uint32_t)((since % g_frame_ns) * LINES / g_frame_ns);
    }
    default:
        break;
    }
    uint64_t v = 0;
    memcpy(&v, g_regs + off, size);
    return v;
}

#define NV_PRMDIO_WRITE_INDEX 0x6813C8u     /* the DAC palette: the entry to write */
#define NV_PRMDIO_DATA        0x6813C9u     /* ... then its R, G, B bytes, auto-incrementing */

/* Push-buffer mode: D3D's gamma ramp goes to the DAC's palette (index 0,
 * then the 768 data bytes: Riddick's LTCG 0x4c0f66, its brightness); the
 * whole table becomes the display's ramp. Not modelled, the ramp was lost
 * and Riddick's gameplay drew darker than in D3D mode. */
static struct {
    uint8_t index, comp;
    uint8_t rgb[3][256];
} g_dac;

static void dac_write(uint32_t off, uint8_t v)
{
    if (off == NV_PRMDIO_WRITE_INDEX) {
        g_dac.index = v;
        g_dac.comp = 0;
        return;
    }
    g_dac.rgb[g_dac.comp][g_dac.index] = v;
    if (++g_dac.comp == 3) {
        g_dac.comp = 0;
        if (++g_dac.index == 0) {               /* the 256th entry: the table is whole */
            hle_pb_gamma((const uint8_t (*)[256])g_dac.rgb);
        }
    }
}

static void nv2a_write(void *opaque, uint32_t off, uint64_t value, unsigned size)
{
    if (__builtin_expect(g_census.on, 0)) {
        census(off, true, (uint32_t)value);
    }
    if (g_pbmode && size == 1 && (off == NV_PRMDIO_WRITE_INDEX || off == NV_PRMDIO_DATA)) {
        dac_write(off, (uint8_t)value);
        return;
    }
    if (g_pbmode && size == 4 && off == NV_PFIFO_CACHE1_DMA_SUBROUTINE) {
        __atomic_store_n(reg(off), (uint32_t)value, __ATOMIC_RELEASE);
        hle_pb_subroutine_written((uint32_t)value);     /* D3D left a run (4928) */
        return;
    }
    if (g_pbmode && size == 4 && (off == NV_PFIFO_CACHE1_DMA_PUT || off == NV_PFIFO_CACHE1_DMA_GET)) {
        static unsigned s_logged_c;             /* debug: the first direct CACHE1 writes */
        if (s_logged_c++ < 16) {
            nc_log("xbased-native", "nv2a: CACHE1 %s <- %08x (GET %08x PUT %08x)",
                   off == NV_PFIFO_CACHE1_DMA_PUT ? "PUT" : "GET", (uint32_t)value,
                   ld(NV_PFIFO_CACHE1_DMA_GET), ld(NV_PFIFO_CACHE1_DMA_PUT));
        }
        if (off == NV_PFIFO_CACHE1_DMA_GET) {
            hle_pb_get();                       /* a channel set-up: the scout syncs again */
        }
    }
    if (g_pbmode && size == 4 && (off == NV_USER_DMA_PUT || off == NV_USER_DMA_GET ||
                                  off == NV_USER_REF)) {
        static unsigned s_logged;               /* debug: the first channel writes */
        if (s_logged < 24 && off != NV_USER_REF) {
            s_logged++;
            nc_log("xbased-native", "nv2a: channel %s <- %08x (GET %08x PUT %08x)",
                   off == NV_USER_DMA_PUT ? "PUT" : "GET", (uint32_t)value,
                   ld(NV_PFIFO_CACHE1_DMA_GET), ld(NV_PFIFO_CACHE1_DMA_PUT));
        }
        if (off == NV_USER_DMA_PUT) {
            hle_pb_pushed((uint32_t)value);         /* before the decoder can see it */
        }
        __atomic_store_n(reg(off - NV_USER_DMA_PUT + NV_PFIFO_CACHE1_DMA_PUT), (uint32_t)value,
                         __ATOMIC_RELEASE);
        if (off == NV_USER_DMA_PUT) {
            hle_pb_put();
        } else if (off == NV_USER_DMA_GET) {
            hle_pb_get();
        }
        return;
    }
    if (g_pbmode && size == 4 && off == NV_PGRAPH_FIFO) {
        hle_host_gpu_trace(HLE_GTR_FIFO, (uint32_t)value);
        __atomic_store_n(reg(off), (uint32_t)value, __ATOMIC_RELEASE);
        hle_pb_kick();                              /* access back: the decoder goes on */
        return;
    }
    if (g_pbmode && size == 4 && (off == NV_PGRAPH_INTR || off == NV_PGRAPH_INTR_EN)) {
        /* PGRAPH's interrupt: the ISR clears what it handled (1s), and a
         * decoder waiting on a trap goes on */
        if (off == NV_PGRAPH_INTR) {
            hle_host_gpu_trace(HLE_GTR_INTR_CLR, (uint32_t)value);
            __atomic_fetch_and(reg(off), ~(uint32_t)value, __ATOMIC_ACQ_REL);
        } else {
            __atomic_store_n(reg(off), (uint32_t)value, __ATOMIC_RELEASE);
        }
        update_irq();
        hle_pb_kick();
        return;
    }
    if (g_pbmode && off == NV_PGRAPH_INCREMENT && size == 4) {
        /* the vblank ISR's flip: the read counter advances (modulo), and a
         * FLIP_STALL in the push buffer may go on */
        if (value & 2) {
            uint32_t s = ld(NV_PGRAPH_SURFACE), n;
            do {
                const uint32_t mod = (s >> 28) & 7, rd = (s >> 24) & 7;
                n = (s & ~0x07000000u) | ((mod ? (rd + 1) % mod : 0) << 24);
            } while (!__atomic_compare_exchange_n(reg(NV_PGRAPH_SURFACE), &s, n, false,
                                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
            static unsigned s_logged;
            if (s_logged++ < 32) {
                nc_log("xbased-native", "nv2a: flip read -> %u (write %u, display %08x)",
                       (n >> 24) & 7, (n >> 20) & 7, ld(NV_PCRTC_START));
            }
            hle_host_gpu_trace(HLE_GTR_INCREMENT, (n >> 24) & 7);
            hle_pb_flipped(ld(NV_PCRTC_START));
            hle_pb_kick();
        }
        return;
    }
    switch (off) {
    case NV_PMC_INTR_0:
        return; /* a summary of the units' registers */
    case NV_PCRTC_INTR_0:
        hle_host_gpu_trace(HLE_GTR_PCRTC_CLR, (uint32_t)value);
        __atomic_fetch_and(reg(off), ~(uint32_t)value, __ATOMIC_ACQ_REL);
        update_irq();
        return;
    case NV_PTIMER_INTR_0:
        __atomic_fetch_and(reg(off), ~(uint32_t)value, __ATOMIC_ACQ_REL);
        update_irq();
        return;
    case NV_PTIMER_INTR_EN_0:
        __atomic_store_n(reg(off), (uint32_t)value, __ATOMIC_RELEASE);
        update_irq();
        return;
    case NV_PTIMER_ALARM_0:
        /* (the low 5 bits read 0: ptimer_alarm_test's Register Bitmasks) */
        __atomic_store_n(reg(off), (uint32_t)value & ~0x1Fu, __ATOMIC_RELEASE);
        pthread_mutex_lock(&g_pt.mu);
        {
            const uint64_t now = nc_ke_now_ns();
            g_pt.strict = g_pt.set_ns && now - g_pt.set_ns < ALARM_STRICT_NS;
        }
        pthread_mutex_unlock(&g_pt.mu);
        __atomic_store_n(&g_alarm_started, true, __ATOMIC_RELEASE);
        alarm_set();
        return;
    case NV_PTIMER_TIME_0:
    case NV_PTIMER_TIME_1:
    case NV_PTIMER_NUMERATOR:
    case NV_PTIMER_DENOMINATOR:
        if (size == 4) {
            ptimer_write(off, (uint32_t)value);
            return;
        }
        break;
    case NV_PMC_INTR_EN_0:
    case NV_PCRTC_INTR_EN_0:
        if (off == NV_PMC_INTR_EN_0) {
            hle_host_gpu_trace(HLE_GTR_PMC_EN, (uint32_t)value);
        }
        __atomic_store_n(reg(off), (uint32_t)value, __ATOMIC_RELEASE);
        update_irq();
        return;
    case NV_PVIDEO_BUFFER:
        if (g_pbmode) {
            /* a buffer to show: in use (shown) until the overlay stops; D3D
             * stops it only when a buffer is in use (Forza 0x3bef1b) */
            __atomic_fetch_or(reg(off), (uint32_t)value, __ATOMIC_ACQ_REL);
            if (value & 1) {
                hle_pb_overlay_changed(true);
            }
            return;
        }
        break;
    case NV_PVIDEO_STOP:
        if (g_pbmode) {
            /* the overlay stops: no buffer pending, as D3D waits to see
             * (EnableOverlay(FALSE) spins on BUFFER until 0: Forza 0x3bef31) */
            __atomic_store_n(reg(off), (uint32_t)value, __ATOMIC_RELEASE);
            if (value & 1) {
                __atomic_store_n(reg(NV_PVIDEO_BUFFER), 0u, __ATOMIC_RELEASE);
                hle_pb_overlay_changed(false);
            }
            return;
        }
        break;
    case NV_PCRTC_START:
        hle_host_gpu_trace(HLE_GTR_START, (uint32_t)value);
        if (g_pbmode) {
            hle_pb_display_start((uint32_t)value);  /* the display shows that memory */
        }
        break;
    default:
        break;
    }
    memcpy(g_regs + off, &value, size);
}

void nc_dev_nv2a_display_vsync(uint64_t ns)
{
    const uint64_t last = __atomic_load_n(&g_disp_vsync_ns, __ATOMIC_ACQUIRE);
    if (last && ns > last) {
        const uint64_t d = ns - last;
        uint64_t p = __atomic_load_n(&g_disp_period_ns, __ATOMIC_RELAXED);
        /* a callback that came late spans whole periods */
        const uint64_t n = p ? (d + p / 2) / p : 1;
        const uint64_t one = n ? d / n : d;
        if (n >= 1 && n <= 4 && one > 5000000ull && one < 50000000ull) {
            p = p ? p + ((int64_t)one - (int64_t)p) / 16 : one;
            __atomic_store_n(&g_disp_period_ns, p, __ATOMIC_RELAXED);
        }
    }
    __atomic_store_n(&g_disp_vsync_ns, ns, __ATOMIC_RELEASE);
}

bool nc_dev_nv2a_vblank_locked(void)
{
    return __atomic_load_n(&g_vblank_locked, __ATOMIC_RELAXED);
}

bool nc_dev_nv2a_vblank_timed(void)
{
    return __atomic_load_n(&g_vblank_timed, __ATOMIC_RELAXED);
}

void hle_host_display_sync(uint64_t vsync_ns, uint64_t period_ns, uint64_t lead_ns)
{
    __atomic_store_n(&g_sync_vsync_ns, vsync_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&g_sync_period_ns, period_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&g_sync_lead_ns, lead_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&g_sync_fed_ns, now_ns(), __ATOMIC_RELEASE);
}

bool hle_host_vblank_timed(void)
{
    return nc_dev_nv2a_vblank_timed();
}

void nc_dev_nv2a_display_nominal(uint64_t period_ns)
{
    __atomic_store_n(&g_disp_nominal_ns, period_ns, __ATOMIC_RELAXED);
}

uint64_t nc_dev_nv2a_next_vsync(uint64_t after)
{
    const uint64_t vs = __atomic_load_n(&g_disp_vsync_ns, __ATOMIC_ACQUIRE);
    const uint64_t pd = __atomic_load_n(&g_disp_period_ns, __ATOMIC_RELAXED);
    if (!vs || !pd || (after > vs && after - vs > VSYNC_STALE)) {
        return 0;
    }
    return after < vs ? vs : vs + ((after - vs) / pd + 1) * pd;
}

/* x into (-pd/2, pd/2]. */
static int64_t wrap_half(int64_t x, int64_t pd)
{
    x %= pd;
    if (x <= -pd / 2) {
        x += pd;
    } else if (x > pd / 2) {
        x -= pd;
    }
    return x;
}

void nc_dev_nv2a_display_pickup(uint64_t ns)
{
    static int64_t logged = -1;
    const uint64_t vs = __atomic_load_n(&g_disp_vsync_ns, __ATOMIC_ACQUIRE);
    const int64_t pd = (int64_t)__atomic_load_n(&g_disp_period_ns, __ATOMIC_RELAXED);
    if (!vs || !pd) {
        return;
    }
    int64_t off = ((int64_t)ns - (int64_t)vs) % pd;
    off = off < 0 ? off + pd : off;
    int64_t est = __atomic_load_n(&g_pick_off_ns, __ATOMIC_RELAXED);
    est = est < 0 ? off : est + wrap_half(off - est, pd) / 16;
    est = (est % pd + pd) % pd;
    __atomic_store_n(&g_pick_off_ns, est, __ATOMIC_RELAXED);
    if (logged < 0 || llabs(wrap_half(est - logged, pd)) > 2000000) {
        logged = est;
        nc_log("xbased-native", "nv2a: the presenter picks frames up %.1f ms after the panel's "
               "vsync (period %.2f ms): vblanks %.1f ms after the vsync", est / 1e6, pd / 1e6,
               (double)((est + pd / 2) % pd) / 1e6);
    }
}

/* The next vblank after one at prev: a frame later, or locked to the panel
 * (its period times the vsyncs per frame, the phase nudged at most 0.5 ms a
 * vblank so the guest's clock never jumps). */
static uint64_t place_vblank(uint64_t prev, bool *locked, bool *timed)
{
    const uint64_t frame = __atomic_load_n(&g_frame_ns, __ATOMIC_RELAXED);
    uint64_t vs = __atomic_load_n(&g_disp_vsync_ns, __ATOMIC_ACQUIRE);
    uint64_t pd = __atomic_load_n(&g_disp_period_ns, __ATOMIC_RELAXED);
    /* the presenter's own, while they come: exact where the compositor's
     * are a thread's wake */
    const uint64_t now = now_ns();
    const uint64_t fed = __atomic_load_n(&g_sync_fed_ns, __ATOMIC_ACQUIRE);
    const bool exact = fed && now - fed < VSYNC_STALE && __atomic_load_n(&g_sync_period_ns, __ATOMIC_RELAXED);
    uint64_t lead = 0;
    if (exact) {
        vs = __atomic_load_n(&g_sync_vsync_ns, __ATOMIC_RELAXED);
        pd = __atomic_load_n(&g_sync_period_ns, __ATOMIC_RELAXED);
        lead = __atomic_load_n(&g_sync_lead_ns, __ATOMIC_RELAXED);
    }
    if (!vs || !pd || (!exact && now - vs > VSYNC_STALE)) {
        return prev + frame;                /* no panel (the app in the background) */
    }
    const uint64_t k = (frame + pd / 2) / pd;
    if (k < 1 || k > 4 || llabs((int64_t)(k * pd) - (int64_t)frame) > (int64_t)(frame / 20)) {
        return prev + frame;                /* 90 Hz and the like: free running */
    }
    const uint64_t nominal = __atomic_load_n(&g_disp_nominal_ns, __ATOMIC_RELAXED);
    if (nominal && llabs((int64_t)pd - (int64_t)nominal) > (int64_t)(nominal / 50)) {
        return prev + frame;                /* not the panel's vsyncs (variable refresh): free running */
    }
    *locked = true;
    const uint64_t next = prev + k * pd;
    const int64_t pick = __atomic_load_n(&g_pick_off_ns, __ATOMIC_RELAXED);
    int64_t at = pick < 0 ? -VSYNC_LEAD : pick + (int64_t)pd / 2;  /* after a vsync */
    if (lead) {
        /* The presenter shows a frame `lead` after its vblank (timed
         * presents): the vblank that much before the middle of a refresh, so
         * the present falls midway between two vsyncs, as far as can be from
         * both, and each frame is on for the same refreshes. Placed by where
         * the presenter happened to pick frames up, which the queue behind
         * it moves, a flip now and then fell on the pickup: one frame shown
         * twice and the next never (Burnout 3 at 60 Hz with V-Sync: once in
         * 45 s). */
        at = (int64_t)pd / 2 - (int64_t)(lead % pd);
        *timed = true;
    }
    const int64_t ph = wrap_half((int64_t)next - (int64_t)vs - at, (int64_t)pd);
    int64_t c = -ph / 8;
    c = c > 500000 ? 500000 : c < -500000 ? -500000 : c;
    return next + (uint64_t)c;
}

static uint64_t next_vblank(uint64_t prev)
{
    bool locked = false, timed = false;
    const uint64_t next = place_vblank(prev, &locked, &timed);
    __atomic_store_n(&g_vblank_locked, locked, __ATOMIC_RELAXED);
    __atomic_store_n(&g_vblank_timed, timed, __ATOMIC_RELAXED);
    return next;
}

/* Uncapped (the app's setting: XBASED_HLE_UNCAP; debug.xbased.uncap 1 / 0
 * on demand), read by the vblank thread now and then. */
static uint64_t g_cap_ns;           /* uncapped: a frame at most this often (0: no limit) */
static volatile int g_own_pace;     /* the title runs at its own rate whatever the options say */
static uint32_t g_title_id;         /* the title that boots (nc_dev_nv2a_title_pace) */

/* The titles that break with their frame rate lifted (Up to 60, Up to 120,
 * Unlocked, fast forward too): it stays their own, and their game settings
 * show it locked at Original (app/ui/dash_settings.c), with why. Only the
 * early vblanks go: a frame rate patch made for the game (a mod) paces it
 * itself on the vblanks as they come, a mod's vblank multiple included.
 * Every edition: the id and the certificate's title.
 *
 * Prince of Persia: Warrior Within draws at 30 fps (a presentation interval
 * of 2) and counts its time in vblanks: lifted, it runs fast (by half again
 * at Up to 45), and from 50 fps on its own threads race: one walks a list
 * of objects that still holds one another thread of the same priority has
 * freed meanwhile, its page decommitted, a fault no handler of the title's
 * takes, and the picture stops (from the savestate before the ship's hold:
 * every run at 50 and 60 within seconds, the same instruction; at its own 30
 * fps none in 120 s, though the object is freed there too). Its disc's speed
 * is the same race (native/kernel/io.c nc_io_title_device_mode). The Sands
 * of Time and The Two Thrones, on its engine, ran 60 s each at 60 fps
 * without a fault: not here. */
int nc_dev_nv2a_title_pace_mode(uint32_t title_id, const char *name)
{
    static const struct {
        uint32_t id;
        const char *name;   /* in the certificate's title, any case (NULL: the id only) */
    } k_titles[] = {
        {0x5553003B, "prince of persia ww"},
    };
    char low[64] = "";
    for (unsigned i = 0; name && name[i] && i < sizeof(low) - 1; i++) {
        low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    }
    for (unsigned i = 0; i < sizeof(k_titles) / sizeof(k_titles[0]); i++) {
        if ((k_titles[i].id && k_titles[i].id == title_id) || (k_titles[i].name && strstr(low, k_titles[i].name))) {
            return 1;
        }
    }
    return 0;
}

void nc_dev_nv2a_title_pace(uint32_t title_id, const char *name)
{
    g_title_id = title_id;
    g_own_pace = nc_dev_nv2a_title_pace_mode(title_id, name);
    if (g_own_pace) {
        nc_log("xbased-native", "nv2a: the frame rate stays %08x's own (%s): uncapped, its threads race",
               title_id, name ? name : "");
    }
}

static bool uncapped(void)
{
    static int on = -1, cap = -1;
    static unsigned polls;
    if (on < 0) {
        on = getenv("XBASED_HLE_UNCAP") != NULL;
        const char *e = getenv("XBASED_HLE_FPSCAP");
        cap = e ? atoi(e) : 0;
    }
    if (polls++ % 32 == 0) {
        char v[PROP_VALUE_MAX] = "";
        if (hle_prop_get("debug.xbased.uncap", v) > 0) {
            on = atoi(v) != 0;
        }
        /* fast forward lifts the limit while held (0), then sets it back */
        if (hle_prop_get("debug.xbased.fpscap", v) > 0) {
            cap = atoi(v);
        }
    }
    g_cap_ns = cap > 0 ? 1000000000ull / (uint64_t)cap : 0;
    return on && !g_own_pace;
}

#define EARLY_GAP_NS 1000000ull     /* uncapped: early vblanks at most this often */
#define EARLY_POLL_NS 250000ull

/* Uncapped push-buffer mode: the title's own D3D flips in its vblank
 * handler (and with a presentation interval of n, every n vblanks), so
 * while PGRAPH waits in FLIP_STALL (the buffer it would draw next is still
 * the one shown: the flip counters' read equals write) an early vblank
 * comes once the last one was handled; the frame rate then follows
 * rendering, as IMMEDIATE swaps do in D3D mode. Titles that count vblanks run fast, as frame-locked ones do
 * there. Returns when the vblank is due: next, or earlier. */
#ifdef NC_DESKTOP
/* Windows: the decoder's FLIP_STALL wakes the uncapped vblank thread at once
 * (WakeByAddress on the count) instead of at its next look, up to 250 us
 * later each frame: at 370 fps Conker's vCPU idled waiting for it. */
static volatile uint32_t g_flip_waits;

#ifndef _WIN32
/* Linux: WaitOnAddress / WakeByAddressAll as futexes */
static void WakeByAddressAll(void *p)
{
    syscall(SYS_futex, p, FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, NULL, NULL, 0);
}

static void WaitOnAddress(volatile void *p, void *expected, size_t n, unsigned ms)
{
    (void)n;
    const struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    syscall(SYS_futex, p, FUTEX_WAIT_PRIVATE, *(uint32_t *)expected, &ts, NULL, 0);
}
#endif

static void flip_waits(void)
{
    __atomic_add_fetch(&g_flip_waits, 1, __ATOMIC_RELEASE);
    WakeByAddressAll((void *)&g_flip_waits);
}
#endif

static uint64_t wait_vblank(uint64_t next, uint64_t last)
{
    if (g_pbmode && uncapped()) {
        /* A frame limit (Up to 60, Up to 120): a frame's first early vblank
         * no sooner than the limit's period after the last frame's. A frame
         * is a stall; its further early vblanks (a presentation interval of
         * 2 flips on the second) as they come. */
        static uint32_t s_stall;
        static uint64_t s_frame_t;
        for (uint64_t t = now_ns(); t < next; t = now_ns()) {
#ifdef NC_DESKTOP
            const uint32_t seen = __atomic_load_n(&g_flip_waits, __ATOMIC_ACQUIRE);
#endif
            const uint32_t stall = hle_pb_flip_stalls();
            const bool paced = !g_cap_ns || stall == s_stall || t - s_frame_t >= g_cap_ns;
            if (t - last >= EARLY_GAP_NS && hle_pb_flip_waiting() &&
                !(ld(NV_PCRTC_INTR_0) & PCRTC_VBLANK)) {
                if (paced) {
                    if (stall != s_stall) {
                        s_stall = stall;
                        /* on the limit's grid while frames keep up with it (the
                         * timer wakes a little late), else from now */
                        s_frame_t = g_cap_ns && t - s_frame_t < 2 * g_cap_ns ? s_frame_t + g_cap_ns : t;
                    }
                    return t;
                }
                /* the frame waits for the limit: till then, or the vblank due */
                const uint64_t until = next < s_frame_t + g_cap_ns ? next : s_frame_t + g_cap_ns;
                const struct timespec ts = {(time_t)(until / 1000000000ull), (long)(until % 1000000000ull)};
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
                continue;
            }
#ifdef NC_DESKTOP
            /* waiting for a stall (not for the guest's handler to take the
             * last vblank: that it is polled for): until the stall, or a ms
             * short of the vblank due (its wait's timeout is in whole ms) */
            if (!hle_pb_flip_waiting() && t - last >= EARLY_GAP_NS && next - t > 1500000) {
                WaitOnAddress((void *)&g_flip_waits, (void *)&seen, 4, (unsigned)((next - t - 500000) / 1000000));
                continue;
            }
#endif
            const uint64_t until = next < t + EARLY_POLL_NS ? next : t + EARLY_POLL_NS;
            const struct timespec ts = {(time_t)(until / 1000000000ull), (long)(until % 1000000000ull)};
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        }
        /* The vblank due, a waiting frame's first: under a limit, not before
         * the limit's period after the last frame's either. It came at the
         * display's rate, and a frame of a presentation interval of 2 had its
         * second at once: Prince of Persia - Warrior Within ran at 53 fps
         * under Up to 30, at 56 under Up to 60; a frame of 1 flipped at the
         * display's rate whatever the limit below it. */
        if (g_cap_ns && hle_pb_flip_waiting() && hle_pb_flip_stalls() != s_stall &&
            now_ns() < s_frame_t + g_cap_ns) {
            /* (held only while it would come before the limit allows: past
             * that, as it always came) */
            const uint64_t until = s_frame_t + g_cap_ns;
            for (uint64_t t = now_ns(); t < until && hle_pb_flip_waiting(); t = now_ns()) {
                const uint64_t u = until < t + EARLY_POLL_NS * 4 ? until : t + EARLY_POLL_NS * 4;
                const struct timespec ts = {(time_t)(u / 1000000000ull), (long)(u % 1000000000ull)};
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
            }
            if (hle_pb_flip_waiting() && hle_pb_flip_stalls() != s_stall) {
                s_stall = hle_pb_flip_stalls();
                s_frame_t = until;          /* on the limit's grid (the wake's overshoot left out) */
            }
            const uint64_t t = now_ns();
            return t > next ? t : next;
        }
        /* the vblank due takes a waiting frame on: the limit counts from it */
        if (hle_pb_flip_waiting() && hle_pb_flip_stalls() != s_stall) {
            s_stall = hle_pb_flip_stalls();
            s_frame_t = next;
        }
        return next;
    }
    const struct timespec ts = {(time_t)(next / 1000000000ull), (long)(next % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0) {
    }
    return next;
}

static void *vblank_thread(void *arg)
{
    nc_pause_register("vblank");
    uint64_t next = now_ns();
    for (;;) {
        if (nc_paused()) {
            /* the emulation paused: no vblanks, and from now on resuming
             * (not the ones missed meanwhile, one after the other) */
            nc_pause_wait();
            next = now_ns();
        }
        const uint64_t due = next_vblank(next);
        next = wait_vblank(due, next);
        __atomic_store_n(&g_last_vblank_ns, next, __ATOMIC_RELEASE);
        __atomic_fetch_or(reg(NV_PCRTC_INTR_0), PCRTC_VBLANK, __ATOMIC_ACQ_REL);
        __atomic_fetch_add(&g_vblanks, 1, __ATOMIC_RELAXED);
        hle_host_gpu_trace(HLE_GTR_VBLANK, (uint32_t)g_vblanks | (next < due ? 0x80000000u : 0));
        update_irq();
        if (g_pbmode) {
            hle_pb_vblank();    /* the scanout shows the memory, flipped or not */
        }
        if (g_census.on && next - g_census.t_log >= 1000000000ull) {
            g_census.t_log = next;
            if (XBASED_DIAG) {
                census_log();
            }
        }
    }
    return NULL;
}

void nc_dev_nv2a_set_ram(uint32_t bytes)
{
    if (g_regs) {
        nc_wr32(g_regs + NV_PFB_CSTATUS, bytes);
    }
}

void nc_dev_nv2a_init(void)
{
    g_regs = calloc(1, NV2A_SIZE + 16); /* pages are touched only where the guest writes (a word of slack past the end) */
    nc_wr32(g_regs + NV_PMC_BOOT_0, 0x02A000A1u);
    nc_wr32(g_regs + NV_PFB_CSTATUS, 64u << 20);
    /* What the console's own start-up leaves in them, for code that programs
     * the chip itself rather than through D3D (pbkit checks both before it
     * sets its channel up, and gives up on a core clock that isn't the
     * console's 233.33 MHz: 16.67 MHz * 28 / 2). */
    nc_wr32(g_regs + NV_PFB_CFG0, 3);                       /* memory partitions */
    nc_wr32(g_regs + NV_PRAMDAC_NVPLL_COEFF, 0x00011C01u);  /* P 1, N 28, M 1 */
    nc_wr32(g_regs + NV_PRAMDAC_VPLL_COEFF, 0x0003C20Du);
    /* PTIMER's scale at the console's ratio (D3D writes the same): ~1 GHz */
    nc_wr32(g_regs + NV_PTIMER_NUMERATOR, PTIMER_NUM0);
    nc_wr32(g_regs + NV_PTIMER_DENOMINATOR, PTIMER_DEN0);
    char v[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.xbased.nvcensus", v);
    const char *e = getenv("NC_NV2A_CENSUS");
    g_census.on = !strcmp(v, "1") || (e && !strcmp(e, "1"));
    const NcMmioDev dev = {"nv2a", NV2A_BASE, NV2A_SIZE, nv2a_read, nv2a_write, NULL};
    nc_mmio_register(&dev);
    pthread_t t;
    pthread_create(&t, NULL, vblank_thread, NULL);
    pthread_detach(t);
}

/* ---- the HLE GPU's seams (hw/xbox/hle/host.h) ---- */

/* A mod's faster vblank (mods/README.md refresh:): the video mode's rate
 * times this; a frame-locked title paced by vblanks then runs that much
 * faster unless the mod halves its steps. */
static unsigned g_mode_hz = 60, g_vblank_mul = 1;

void nc_dev_nv2a_refresh(unsigned hz)
{
    g_mode_hz = hz == 50 ? 50 : 60;
    const uint64_t f = (hz == 50 ? 20000000ull : FRAME_NS) / g_vblank_mul;
    if (__atomic_exchange_n(&g_frame_ns, f, __ATOMIC_RELAXED) != f) {
        nc_log("xbased-native", "nv2a: vblank at %u Hz (the video mode%s)", g_mode_hz * g_vblank_mul,
               g_vblank_mul > 1 ? ", a mod's rate" : "");
    }
}

void nc_dev_nv2a_display_mode(uint32_t pitch, uint32_t xfmt, unsigned hz, uint32_t fb)
{
    hle_pb_display_mode(pitch, xfmt, hz);
    if (fb) {
        hle_pb_display_fb(fb);
    }
}

static uint32_t g_av_mode;

void nc_dev_nv2a_av_mode_set(uint32_t mode)
{
    __atomic_store_n(&g_av_mode, mode, __ATOMIC_RELAXED);
}

uint32_t nc_dev_nv2a_av_mode(void)
{
    return __atomic_load_n(&g_av_mode, __ATOMIC_RELAXED);
}

static uint32_t g_av_title;

/* Each title's shape as last seen (1 4:3, 2 16:9), kept across runs in the
 * HLE cache's folder (shapes.txt, "<title id> <shape>" lines): a savestate
 * from before the shape was saved in it (NV2A v1, v2) has nothing that tells
 * at 480p, whose AV mode is the same for both, and its title doesn't set the
 * mode again. Buffy the Vampire Slayer and XIII, 4:3 at 480p, came up 16:9
 * (the console's setting) from every such state. */
#define SHAPES_MAX 4096

static unsigned shapes_read(uint32_t (*e)[2], unsigned cap)
{
    char path[512];
    snprintf(path, sizeof(path), "%sshapes.txt", hle_host_base_path());
    FILE *f = fopen(path, "r");
    unsigned n = 0;
    if (f) {
        unsigned id, shape;
        while (n < cap && fscanf(f, "%x %u", &id, &shape) == 2) {
            e[n][0] = id;
            e[n][1] = shape;
            n++;
        }
        fclose(f);
    }
    return n;
}

static void shape_remember(uint32_t id, uint32_t shape)
{
    static uint32_t s_id, s_shape;      /* the last one written (a title sets the mode again and again) */
    if (!id || !shape || (id == s_id && shape == s_shape)) {
        return;
    }
    s_id = id;
    s_shape = shape;
    static uint32_t e[SHAPES_MAX][2];
    unsigned n = shapes_read(e, SHAPES_MAX), i = 0;
    while (i < n && e[i][0] != id) {
        i++;
    }
    if (i < n && e[i][1] == shape) {
        return;
    }
    if (i == n) {
        if (n == SHAPES_MAX) {
            return;
        }
        n++;
    }
    e[i][0] = id;
    e[i][1] = shape;
    char path[512], tmp[520];
    snprintf(path, sizeof(path), "%sshapes.txt", hle_host_base_path());
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        return;
    }
    for (unsigned k = 0; k < n; k++) {
        fprintf(f, "%08X %u\n", e[k][0], e[k][1]);
    }
    fclose(f);
    remove(path);
    rename(tmp, path);
}

uint32_t nc_dev_nv2a_av_title_remembered(void)
{
    static uint32_t e[SHAPES_MAX][2];
    const unsigned n = shapes_read(e, SHAPES_MAX);
    for (unsigned i = 0; i < n; i++) {
        if (e[i][0] == g_title_id && (e[i][1] == 1 || e[i][1] == 2)) {
            return e[i][1];
        }
    }
    return 0;
}

void nc_dev_nv2a_av_title_set(uint32_t shape)
{
    __atomic_store_n(&g_av_title, shape, __ATOMIC_RELAXED);
    shape_remember(g_title_id, shape);
}

uint32_t nc_dev_nv2a_av_title(void)
{
    return __atomic_load_n(&g_av_title, __ATOMIC_RELAXED);
}

void nc_dev_nv2a_vblank_multiply(unsigned n)
{
    g_vblank_mul = n >= 1 && n <= 4 ? n : 1;
    nc_dev_nv2a_refresh(g_mode_hz);
}

uint64_t hle_host_vblank_count(void)
{
    return __atomic_load_n(&g_vblanks, __ATOMIC_RELAXED);
}

uint64_t hle_host_vblank_ns(void)
{
    return __atomic_load_n(&g_last_vblank_ns, __ATOMIC_ACQUIRE);
}

int64_t hle_host_vblank_period_ns(void)
{
    return (int64_t)__atomic_load_n(&g_frame_ns, __ATOMIC_RELAXED);
}

uint32_t hle_host_gpu_reg(uint32_t off)
{
    if (g_regs && (off & ~3u) == NV_PTIMER_TIME_0) {
        return (uint32_t)pt_now() & ~0x1Fu;     /* the time now (a notifier's stamp) */
    }
    if (g_regs && (off & ~3u) == NV_PTIMER_TIME_1) {
        return (uint32_t)(pt_now() >> 32) & 0x1FFFFFFFu;
    }
    return g_regs && off < NV2A_SIZE - 3 ? ld(off & ~3u) : 0;
}

uint64_t hle_host_ptimer(void)
{
    if (!g_regs) {
        return 0;
    }
    const uint64_t t = pt_now();
    return (t >> 32 & 0x1FFFFFFFu) << 32 | ((uint32_t)t & ~0x1Fu);
}

const uint32_t *hle_host_gpu_reg_ptr(uint32_t off)
{
    return g_regs && off < NV2A_SIZE - 3 ? reg(off & ~3u) : NULL;
}

void hle_host_gpu_reg_set(uint32_t off, uint32_t v)
{
    if (g_regs && off < NV2A_SIZE - 3) {
        __atomic_store_n(reg(off & ~3u), v, __ATOMIC_RELEASE);
    }
}

void hle_host_gpu_reg_mask(uint32_t off, uint32_t mask, uint32_t bits)
{
    if (!g_regs || off >= NV2A_SIZE - 3) {
        return;
    }
    uint32_t *r = reg(off & ~3u), v = __atomic_load_n(r, __ATOMIC_ACQUIRE);
    while (!__atomic_compare_exchange_n(r, &v, (v & ~mask) | (bits & mask), false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    }
}

void hle_host_gpu_irq_update(void)
{
    if (g_regs) {
        update_irq();
    }
}

void hle_host_pb_mode(bool on)
{
#ifdef NC_DESKTOP
    hle_pb_on_flip_wait(flip_waits);
#endif
    __atomic_store_n(&g_pbmode, on, __ATOMIC_RELEASE);
    nc_log("xbased-native", "nv2a: push-buffer mode %s", on ? "on" : "off");
}

uint64_t nc_dev_nv2a_vblanks(void)
{
    return __atomic_load_n(&g_vblanks, __ATOMIC_RELAXED);
}

/* The other device windows (APU, ACI, USB, NIC, flash): register files that
 * show up in the MMIO log until each gets a device. */
static uint8_t *g_misc;
#define MISC_BASE 0xFE000000u
#define MISC_SIZE 0x02000000u

static uint64_t misc_read(void *opaque, uint32_t off, unsigned size)
{
    uint64_t v = 0;
    memcpy(&v, g_misc + off, size);
    return v;
}

static void misc_write(void *opaque, uint32_t off, uint64_t value, unsigned size)
{
    memcpy(g_misc + off, &value, size);
}

void nc_dev_misc_init(void)
{
    g_misc = calloc(1, MISC_SIZE + 16);
    const NcMmioDev dev = {"misc", MISC_BASE, MISC_SIZE, misc_read, misc_write, NULL};
    nc_mmio_register(&dev);
}

/* ---------------------------------------------------------- savestates */

typedef struct NcNv2aState {
    uint64_t vblanks, frame_ns;
    uint32_t npages;
    uint32_t pt_set;                /* PTIMER (v2): g_pt */
    uint64_t pt_ticks, pt_ns;
    uint32_t av_mode, av_title;     /* (v3) the AV mode and the title's shape: g_av_mode, g_av_title */
} NcNv2aState;

typedef struct NcMiscState {
    uint32_t npages;
} NcMiscState;

#define NC_DEF_SECTION NV2A
#define NC_DEF_FILE "native/state/nv2a.def"
#include "native/state/def.h"
#undef NC_DEF_SECTION
#undef NC_DEF_FILE
#define NC_DEF_SECTION MISC
#define NC_DEF_FILE "native/state/misc.def"
#include "native/state/def.h"
#include "native/state/sections.h"

static bool page_zero(const uint8_t *p)
{
    const uint64_t *q = (const uint64_t *)p;
    for (unsigned i = 0; i < NC_PAGE / 8; i++) {
        if (q[i]) {
            return false;
        }
    }
    return true;
}

static uint32_t pages_put(NcBuf *b, const uint8_t *base, uint32_t size, bool count_only)
{
    uint32_t n = 0;
    for (uint32_t pg = 0; pg < size / NC_PAGE; pg++) {
        if (!page_zero(base + (size_t)pg * NC_PAGE)) {
            if (!count_only) {
                nc_buf_u32(b, pg);
                nc_buf_put(b, base + (size_t)pg * NC_PAGE, NC_PAGE);
            }
            n++;
        }
    }
    return n;
}

static bool pages_get(NcRd *r, uint8_t *base, uint32_t size, uint32_t n)
{
    for (uint32_t pg = 0; pg < size / NC_PAGE; pg++) {
        if (!page_zero(base + (size_t)pg * NC_PAGE)) {
            memset(base + (size_t)pg * NC_PAGE, 0, NC_PAGE);
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t pg = nc_rd_u32(r);
        if (pg >= size / NC_PAGE || !nc_rd_get(r, base + (size_t)pg * NC_PAGE, NC_PAGE)) {
            return false;
        }
    }
    return !r->fail;
}

bool nc_nv2a_state_save(NcBuf *b)
{
    pthread_mutex_lock(&g_pt.mu);
    const NcNv2aState st = {__atomic_load_n(&g_vblanks, __ATOMIC_RELAXED), g_frame_ns,
                            pages_put(NULL, g_regs, NV2A_SIZE, true), g_pt.set_ns != 0, g_pt.ticks, g_pt.ns,
                            nc_dev_nv2a_av_mode(), nc_dev_nv2a_av_title()};
    pthread_mutex_unlock(&g_pt.mu);
    nc_rec_put(b, &nc_rec_NcNv2aState, &st);
    pages_put(b, g_regs, NV2A_SIZE, false);
    return true;
}

bool nc_nv2a_state_load(NcRd *r, unsigned v)
{
    NcNv2aState st = {0, FRAME_NS, 0, 0, 0, 0, 0, 0};
    nc_rec_get(r, &nc_rec_NcNv2aState, &st, v);
    nc_dev_nv2a_av_mode_set(st.av_mode);    /* (v1 - v2: 0, not known; kstate.c looks for the shape) */
    nc_dev_nv2a_av_title_set(st.av_title);
    if (!pages_get(r, g_regs, NV2A_SIZE, st.npages)) {
        return false;
    }
    pthread_mutex_lock(&g_pt.mu);
    if (v < 2) {
        /* the time was the guest's ns: it goes on from there, at the
         * console's scale unless the title set one */
        if (!ld(NV_PTIMER_NUMERATOR) && !ld(NV_PTIMER_DENOMINATOR)) {
            nc_wr32(g_regs + NV_PTIMER_NUMERATOR, PTIMER_NUM0);
            nc_wr32(g_regs + NV_PTIMER_DENOMINATOR, PTIMER_DEN0);
        }
        st.pt_set = 0;
        st.pt_ticks = st.pt_ns = 0;     /* the first look: the guest's ns then */
    }
    g_pt.set_ns = 0;                    /* (pt_set: the time was set once; the rule's window
                                           is long past at a load) */
    g_pt.strict = false;
    g_pt.ticks = st.pt_ticks;
    g_pt.ns = st.pt_ns;
    pthread_mutex_unlock(&g_pt.mu);
    __atomic_store_n(&g_vblanks, st.vblanks, __ATOMIC_RELAXED);
    /* the state's video mode (50 Hz: a 20 ms frame), at this run's rate (the
     * mods on now: a state saved with or without them) */
    nc_dev_nv2a_refresh(st.frame_ns >= 18000000ull ? 50 : 60);
    g_last_vblank_ns = now_ns();
    g_disp_vsync_ns = 0;
    g_pick_off_ns = -1;
    if (ld(NV_PTIMER_ALARM_0) || ld(NV_PTIMER_INTR_EN_0) || st.pt_set) {
        __atomic_store_n(&g_alarm_started, true, __ATOMIC_RELEASE);
        alarm_set();                    /* due from the state's alarm, on its clock */
    }
    update_irq();
    return true;
}

bool nc_misc_state_save(NcBuf *b)
{
    const NcMiscState st = {pages_put(NULL, g_misc, MISC_SIZE, true)};
    nc_rec_put(b, &nc_rec_NcMiscState, &st);
    pages_put(b, g_misc, MISC_SIZE, false);
    return true;
}

bool nc_misc_state_load(NcRd *r, unsigned v)
{
    NcMiscState st = {0};
    nc_rec_get(r, &nc_rec_NcMiscState, &st, v);
    return pages_get(r, g_misc, MISC_SIZE, st.npages);
}
