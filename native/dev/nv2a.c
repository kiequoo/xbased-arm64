/*
 * xbased native core — NV2A register space (0xFD000000, 16 MiB), first cut.
 *
 * A register file (reads return what was last written) plus the behavior the
 * XDK's D3D needs, added as the frontier shows it:
 *   PMC_BOOT_0      0x000000  chip identification
 *   PMC_INTR_0      0x000100  pending summary (bit 24 PCRTC, 20 PTIMER), read-only
 *   PMC_INTR_EN_0   0x000140  bit 0: interrupts to the CPU
 *   PTIMER          0x009400/0x009410  free-running time (ns, the guest's
 *                   clock), low/high; ALARM_0 0x009420: INTR_0 0x009100 bit
 *                   0 when the low word reaches it (write 1 to clear),
 *                   INTR_EN_0 0x009140
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
#define NV_PTIMER_TIME_0    0x009400u
#define NV_PTIMER_TIME_1    0x009410u
#define NV_PTIMER_ALARM_0   0x009420u
#define NV_PFB_CSTATUS      0x10020Cu
#define NV_PFB_WBC          0x100410u
#define NV_PCRTC_INTR_0     0x600100u
#define NV_PCRTC_INTR_EN_0  0x600140u
#define NV_PCRTC_START      0x600800u
#define NV_PCRTC_RASTER     0x600808u

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

/* ---- PTIMER's alarm ---- */

uint64_t nc_ke_host_ns(uint64_t guest_ns);  /* native/kernel/ke.c */

/* The alarm interrupt comes when the time's low word reaches ALARM_0: D3D
 * times its vblank work with it in the modes whose vblank it doesn't take
 * from the encoder (Project Zero II's PAL disc, 60 Hz on SCART: its swaps
 * flip at the alarm's vblanks, and with no alarm the first one never did),
 * and its timer callbacks. The ptimer thread, started at the first alarm a
 * title sets, waits for it on the guest's clock, which stands still while
 * the emulation is paused and goes on from a savestate's. An alarm the time
 * passed less than ALARM_LATE_NS ago comes at once, not after the low word
 * wraps (4.3 s): the guest set it late, by its own slowness or ours, and
 * D3D's handler checks the 64-bit time itself. */
#define ALARM_LATE_NS  1000000000u
#define ALARM_POLL_NS  20000000ull  /* the pause looked at this often */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_once_t once;
    bool rearm;                     /* ALARM_0 written (or a savestate's): due again; set at first,
                                     * the thread maybe looking before alarm_set */
    uint64_t due;                   /* guest ns; 0: none */
} g_alarm = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_ONCE_INIT, true, 0};

/* When the time's low word, at now, next reaches alarm */
static uint64_t alarm_due(uint32_t alarm, uint64_t now)
{
    const uint32_t d = alarm - ((uint32_t)now & ~0x1Fu);
    return d >= (uint32_t)-ALARM_LATE_NS ? now : now + d;
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
            g_alarm.due += 1ull << 32;      /* the low word comes round to it again */
            continue;
        }
        uint64_t until = nc_ke_host_ns(g_alarm.due);
        const uint64_t cap = now_ns() + ALARM_POLL_NS;
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
        default:
            goto plain;
        }
        return size == 4 ? v : (v >> shift) & ((1u << (size * 8)) - 1);
    }
plain:
    switch (off) {
    case NV_PMC_INTR_0:
        return pmc_intr();
    case NV_PTIMER_TIME_0:
        return (uint32_t)nc_ke_now_ns() & ~0x1Fu;
    case NV_PTIMER_TIME_1:
        return (uint32_t)(nc_ke_now_ns() >> 32) & 0x1FFFFFFFu;
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
            hle_pb_flip_read();
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
        __atomic_store_n(reg(off), (uint32_t)value, __ATOMIC_RELEASE);
        alarm_set();
        return;
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

/* How long before a vsync a timed present goes (display_grid): half a
 * refresh at 60 Hz and below, above it PRESENT_AFTER past the vsync before,
 * up to PRESENT_ROOM (120 Hz 6.8 ms, 144 Hz 5.4, 240 Hz 2.7). */
#define PRESENT_AFTER 1500000ll
#define PRESENT_ROOM  8333333ll

static int64_t present_room(uint64_t pd)
{
    const int64_t h = (int64_t)pd / 2, r = (int64_t)pd - PRESENT_AFTER;
    const int64_t d = r < PRESENT_ROOM ? r : PRESENT_ROOM;
    return d > h ? d : h;
}

/* The display's vsyncs as the vblank keeps to them: one's instant, the
 * period, and the phase a vblank takes after a vsync (*timed: placed for the
 * presenter's timed presents); false while there are none to keep to (no
 * panel: the app in the background; not the panel's own: a variable refresh
 * rate display's, which follow the presents). */
static bool display_grid(uint64_t *vs_out, uint64_t *pd_out, int64_t *at_out, bool *timed)
{
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
        return false;
    }
    const uint64_t nominal = __atomic_load_n(&g_disp_nominal_ns, __ATOMIC_RELAXED);
    if (nominal && llabs((int64_t)pd - (int64_t)nominal) > (int64_t)(nominal / 50)) {
        return false;
    }
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
         * 45 s). Not nearer the vsync for less latency: a quarter of a
         * refresh before it (4.2 ms at 60 Hz, 5 ms off the 14 from a vblank
         * to the screen), presents made at that instant like their
         * neighbours missed it, 3 to 5 in 12 s (Crash Bandicoot at Up to 60,
         * a high priority queue for the presenter the same): the screen
         * takes a present some ms before its vsync, by the GPU's load.
         * Above 60 Hz half a refresh is less than that (4.2 ms at 120 Hz:
         * Crash at Up to 30, presents midway like their neighbours, 2 in
         * 12 s a refresh late): there the present goes PRESENT_AFTER after
         * the vsync before, as near the next one as that leaves (timed, a
         * present comes no earlier than its place), PRESENT_ROOM at most. */
        at = (int64_t)pd - present_room(pd) - (int64_t)(lead % pd);
    }
    *vs_out = vs;
    *pd_out = pd;
    *at_out = at;
    if (timed) {
        *timed = lead != 0;
    }
    return true;
}

/* The first instant at or after t of the grid display_grid gives. */
static uint64_t grid_at_or_after(uint64_t t, uint64_t vs, uint64_t pd, int64_t at)
{
    const int64_t base = (int64_t)vs + at, d = (int64_t)t - base, p = (int64_t)pd;
    const int64_t n = d >= 0 ? (d + p - 1) / p : -((-d) / p);
    return (uint64_t)(base + n * p);
}

/* The next vblank after one at prev: a frame later, or locked to the panel
 * (its period times the vsyncs per frame, the phase nudged at most 0.5 ms a
 * vblank so the guest's clock never jumps). */
/* How near whole refreshes a frame (or a limit's pace) must be to keep to
 * them: 1 %, a 59.94 Hz vblank beside a 60 Hz display (0.1 %) and a display's
 * own clock a little off. At 5 % a 50 Hz title's vblank kept to three
 * refreshes of a 144 Hz display (20.8 ms) or five of 240 Hz: it ran at 48
 * fps, 4 % slow (Crash Bandicoot's European disc at Original). */
#define LOCK_PART 100

static uint64_t place_vblank(uint64_t prev, bool *locked, bool *timed)
{
    const uint64_t frame = __atomic_load_n(&g_frame_ns, __ATOMIC_RELAXED);
    uint64_t vs, pd;
    int64_t at;
    bool lead;
    if (!display_grid(&vs, &pd, &at, &lead)) {
        return prev + frame;                /* no panel's vsyncs to keep to: free running */
    }
    const uint64_t k = (frame + pd / 2) / pd;
    if (k < 1 || k > 4 || llabs((int64_t)(k * pd) - (int64_t)frame) > (int64_t)(frame / LOCK_PART)) {
        return prev + frame;                /* 90 Hz and the like: free running */
    }
    *locked = true;
    *timed = lead;
    const uint64_t next = prev + k * pd;
    const int64_t ph = wrap_half((int64_t)next - (int64_t)vs - at, (int64_t)pd);
    int64_t c = -ph / 8;
    c = c > 500000 ? 500000 : c < -500000 ? -500000 : c;
    return next + (uint64_t)c;
}

static bool cap_grid(uint64_t *vs, uint64_t *pd, int64_t *at, bool *timed);

static uint64_t next_vblank(uint64_t prev)
{
    bool locked = false, timed = false;
    const uint64_t next = place_vblank(prev, &locked, &timed);
    uint64_t vs, pd;
    int64_t at;
    bool cap_timed = false;
    if (!timed && cap_grid(&vs, &pd, &at, &cap_timed)) {
        timed = cap_timed;              /* the limit's frames on the vsyncs, the vblank not */
    }
    __atomic_store_n(&g_vblank_locked, locked, __ATOMIC_RELAXED);
    __atomic_store_n(&g_vblank_timed, timed, __ATOMIC_RELAXED);
    return next;
}

/* Uncapped (the app's setting: XBASED_HLE_UNCAP; debug.xbased.uncap 1 / 0
 * on demand), read by the vblank thread now and then. */
static uint64_t g_cap_ns;           /* uncapped: a frame at most this often (0: no limit) */
static uint64_t g_frame_due_ns;     /* uncapped: the instant the limit's last frame stands for (frame_due) */
static uint64_t g_trap_slot_ns;     /* a trap's handler's next flip: its slot (hle_host_flip_slot; 0: none) */
static uint64_t g_trap_every_ns;    /* ... and its pace */
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

static volatile int g_uncap_on;     /* uncapped(), as it last read: the frame rate lifted */

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
    __atomic_store_n(&g_cap_ns, cap > 0 ? 1000000000ull / (uint64_t)cap : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_uncap_on, on && !g_own_pace, __ATOMIC_RELAXED);
    return on && !g_own_pace;
}

/* A frame limit of whole refreshes of a display the vblank keeps to (Up to
 * 30 or 60 on a 60 Hz one, Unlocked's limit at the display's rate): its
 * frames flip on the display's vsyncs (wait_vblank), whatever the vblank's
 * own rate. *timed: placed for the presenter's timed presents (V-Sync), so
 * it is told the frames are on the vsyncs, as at Original. A 50 Hz title
 * (Crash Bandicoot's European disc) at Up to 60 ran so with its vblank free
 * beside the display: the presenter was told nothing, queued each picture
 * behind the one before, and once one missed its vsync every one after it
 * was a refresh late, for good (the lead driven to its most, 33 ms from a
 * flip to the screen instead of 13).
 *
 * A limit of a whole fraction of a refresh (Up to 120 on a 60 Hz display)
 * likewise, on the vsyncs' grid cut in as many steps (*pd the step): the
 * frame of the step the display's grid has is the one on screen (its
 * present midway between two vsyncs), the others' presents a moment after
 * a vsync, replaced before the next. Free of the grid, its presents fell
 * anywhere in a refresh and the one shown changed from one to the next;
 * queued, with V-Sync, they ran into the same late state (Crash Bandicoot:
 * 34 ms from a flip to the screen). */
#define CAP_STEP_AFTER 500000ll     /* a step's present after a vsync: past it, sure */

/* The grid frames `every` apart flip on (display_grid's, or its steps for a
 * fraction of a refresh); false: none (no display grid, or a pace of no
 * whole refreshes or fraction of one). */
static bool every_grid(uint64_t every, uint64_t *vs, uint64_t *pd, int64_t *at, bool *timed)
{
    if (!every || !display_grid(vs, pd, at, timed)) {
        return false;
    }
    const uint64_t m = (every + *pd / 2) / *pd;
    if (m >= 1 && llabs((int64_t)(m * *pd) - (int64_t)every) <= (int64_t)(every / LOCK_PART)) {
        return true;
    }
    /* (half a 60 Hz refresh rounds to one as often as to none) */
    const uint64_t n = (*pd + every / 2) / every;
    if (n < 2 || n > 4 || llabs((int64_t)(*pd / n) - (int64_t)every) > (int64_t)(every / LOCK_PART)) {
        return false;
    }
    /* steps of pd/n from the display grid's place, all a moment later: the
     * one before a vsync's is then past it too */
    *pd /= n;
    *at += CAP_STEP_AFTER;
    return true;
}

static bool cap_grid(uint64_t *vs, uint64_t *pd, int64_t *at, bool *timed)
{
    return g_pbmode && __atomic_load_n(&g_uncap_on, __ATOMIC_RELAXED) &&
           every_grid(__atomic_load_n(&g_cap_ns, __ATOMIC_RELAXED), vs, pd, at, timed);
}

/* The least time between the flips a trap's handler does (D3D's IMMEDIATE
 * presents, hw/xbox/hle/pb/pbmode.c pace_flip): a vblank's at Original (the
 * console's display rate, 60 or 50 Hz, a mod's multiple included), the
 * limit's lifted (Up to 30, 60, 120, the global one), 0 for none (Unlocked
 * without a limit, fast forward). */
int64_t hle_host_flip_min_ns(void)
{
    if (__atomic_load_n(&g_uncap_on, __ATOMIC_RELAXED)) {
        return (int64_t)__atomic_load_n(&g_cap_ns, __ATOMIC_RELAXED);
    }
    return (int64_t)__atomic_load_n(&g_frame_ns, __ATOMIC_RELAXED);
}

/* The flips a trap's handler does on the display's vsyncs too, where the
 * pace is whole refreshes of a display the vblank keeps to (display_grid):
 * on a grid of their own beside the display's, as the limit's frames were
 * (wait_vblank), they fell anywhere in a refresh (Total Immersion Racing at
 * Original, 60 Hz with V-Sync: in 12 s 11 pictures on for two refreshes and
 * 11 never shown; on the grid, every one for one). A pace of a fraction of
 * a refresh (Up to 120 on 60 Hz) on its steps (every_grid). */
int64_t hle_host_flip_slot(int64_t want, int64_t every)
{
    uint64_t vs, pd;
    int64_t at;
    int64_t slot = want;
    if (every > 0 && want > 0 && every_grid((uint64_t)every, &vs, &pd, &at, NULL)) {
        slot = (int64_t)grid_at_or_after((uint64_t)want - pd / 2, vs, pd, at);
    }
    __atomic_store_n(&g_trap_every_ns, every > 0 ? (uint64_t)every : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_trap_slot_ns, every > 0 && slot > 0 ? (uint64_t)slot : 0, __ATOMIC_RELEASE);
    return slot;
}

#define EARLY_GAP_NS 1000000ull     /* uncapped: early vblanks at most this often */
#define EARLY_POLL_NS 250000ull
#define FRAME_DUE_SPAN 4000000ull   /* a limit's frame's further early vblanks come within this of its first */

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

/* A frame limit on the display's grid (wait_vblank): the vsync's place a
 * frame waiting at t flips at, the limit's refreshes after the last frame's
 * (at last), or one just gone by (the timer woke late: a quarter of a
 * refresh). */
/* A limit's frame, its early vblank's instant: its place on the limit's
 * grid when the timer woke just after it, not the wake (as a vblank's own
 * instant is the grid's, not its thread's wake): the presenter shows the
 * frame a lead after that instant, and a variable refresh rate display at
 * that time. With the wake, Otogi at Up to 30 on a 280 Hz G-Sync display
 * went on screen 32.9 to 34.8 ms apart, at Original 33.4 to 33.7. */
#define ON_LIMIT_WAKE 1000000ull

/* A limit's frame: the instant its first early vblank stands for, which its
 * flip is due at whichever of its vblanks it comes on (hle_host_flip_vblank_ns):
 * a presentation interval of 2 flips on the second, EARLY_GAP_NS and a poll
 * after the first, and its picture went on screen that much past its place
 * (Otogi at Up to 30 on a 120 Hz display: presents 3 ms before a vsync, not
 * 4.2, one picture in eight a refresh late). */
static uint64_t frame_due(uint64_t at)
{
    __atomic_store_n(&g_frame_due_ns, at, __ATOMIC_RELEASE);
    return at;
}

static uint64_t on_limit(uint64_t t, uint64_t at)
{
    return at <= t && t - at <= ON_LIMIT_WAKE ? at : t;
}

static uint64_t cap_slot(uint64_t t, uint64_t last, uint64_t vs, uint64_t pd, int64_t at)
{
    const uint64_t cap = __atomic_load_n(&g_cap_ns, __ATOMIC_RELAXED);
    const uint64_t earliest = last + cap - pd / 2;
    uint64_t from = t > pd / 4 ? t - pd / 4 : 0;
    from = from > earliest ? from : earliest;
    return grid_at_or_after(from, vs, pd, at);
}

static uint64_t wait_vblank(uint64_t next, uint64_t last)
{
    if (g_pbmode && uncapped()) {
        /* A frame limit (Up to 60, Up to 120): a frame's first early vblank
         * no sooner than the limit's period after the last frame's. A frame
         * is a stall; its further early vblanks (a presentation interval of
         * 2 flips on the second) as they come. */
        static uint32_t s_stall;
        static uint64_t s_frame_t;
        /* A limit of whole refreshes of the display (Up to 30 or 60 on a
         * 60 Hz one, Unlocked's limit at the display's rate): each frame's
         * first early vblank on the display's vsyncs, where the vblank keeps
         * to them (display_grid), at least the limit's refreshes after the
         * last frame's. On a grid of its own, the limit's period beside the
         * display's, the frames fell anywhere in a refresh, and those near a
         * vsync were shown a refresh early or late: Otogi at Up to 30 with
         * one picture in four on for 50 or 17 ms instead of 33 (12 s at 60 Hz
         * with V-Sync), Crash Bandicoot's 60 fps at Up to 60 without V-Sync
         * one in four 8 or 26 ms; another run's at the display's limit, on
         * the same grid, none, its phase by chance. */
        uint64_t gvs = 0, gpd = 0;
        int64_t gat = 0;
        bool gtimed;
        const bool aligned = cap_grid(&gvs, &gpd, &gat, &gtimed);
        for (uint64_t t = now_ns(); t < next; t = now_ns()) {
#ifdef NC_DESKTOP
            const uint32_t seen = __atomic_load_n(&g_flip_waits, __ATOMIC_ACQUIRE);
#endif
            const uint32_t stall = hle_pb_flip_stalls();
            const bool paced = !g_cap_ns || stall == s_stall || t - s_frame_t >= g_cap_ns;
            if (t - last >= EARLY_GAP_NS && hle_pb_flip_waiting() &&
                !(ld(NV_PCRTC_INTR_0) & PCRTC_VBLANK)) {
                if (aligned && stall != s_stall) {
                    const uint64_t slot = cap_slot(t, s_frame_t, gvs, gpd, gat);
                    if (t + EARLY_POLL_NS >= slot) {
                        s_stall = stall;
                        s_frame_t = slot;
                        frame_due(slot);
                        return t;
                    }
                    const uint64_t until = next < slot ? next : slot;
                    const struct timespec ts = {(time_t)(until / 1000000000ull), (long)(until % 1000000000ull)};
                    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
                    continue;
                }
                if (paced) {
                    if (stall != s_stall) {
                        s_stall = stall;
                        /* on the limit's grid while frames keep up with it (the
                         * timer wakes a little late), else from now */
                        s_frame_t = g_cap_ns && t - s_frame_t < 2 * g_cap_ns ? s_frame_t + g_cap_ns : t;
                        return frame_due(on_limit(t, s_frame_t));
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
        if (aligned && hle_pb_flip_waiting() && hle_pb_flip_stalls() != s_stall) {
            /* the vblank due, a waiting frame's first, on the display's grid
             * too: held till its vsync's place if it comes before */
            const uint64_t slot = cap_slot(now_ns(), s_frame_t, gvs, gpd, gat);
            for (uint64_t t = now_ns(); t + EARLY_POLL_NS < slot && hle_pb_flip_waiting(); t = now_ns()) {
                const uint64_t u = slot < t + EARLY_POLL_NS * 4 ? slot : t + EARLY_POLL_NS * 4;
                const struct timespec ts = {(time_t)(u / 1000000000ull), (long)(u % 1000000000ull)};
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
            }
            const uint64_t t = now_ns();
            if (hle_pb_flip_waiting() && hle_pb_flip_stalls() != s_stall) {
                s_stall = hle_pb_flip_stalls();
                s_frame_t = slot;
                frame_due(slot > next ? slot : next);
            }
            return t > next ? t : next;
        }
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
            const uint64_t t = now_ns();
            if (hle_pb_flip_waiting() && hle_pb_flip_stalls() != s_stall) {
                s_stall = hle_pb_flip_stalls();
                s_frame_t = until;          /* on the limit's grid (the wake's overshoot left out) */
                return frame_due(on_limit(t, until > next ? until : next));
            }
            return t > next ? t : next;
        }
        /* the vblank due takes a waiting frame on: the limit counts from it */
        if (hle_pb_flip_waiting() && hle_pb_flip_stalls() != s_stall) {
            s_stall = hle_pb_flip_stalls();
            s_frame_t = next;
            frame_due(next);
        }
        return next;
    }
    const struct timespec ts = {(time_t)(next / 1000000000ull), (long)(next % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0) {
    }
    return next;
}

/* A vblank thread that woke late (held up: a loaded PC, a laptop's power
 * saving putting the run on its slow cores and its timers together) sent
 * the vblanks it had missed one after the other, as fast as the guest took
 * them; a game that flips at each vblank flipped at that rate (made to stall
 * for 150 ms: 7 vblanks 0.0 ms apart after it), and its fps count read over
 * its rate after reading under it. The console's vblanks come at their time
 * whatever the CPU does, and a guest that takes its interrupt late takes one:
 * the vblanks missed by more than a period are dropped, the grid kept, and
 * the first few reported (each its lateness). */
static uint64_t late_skip(uint64_t at, uint64_t step)
{
    const uint64_t t = now_ns();
    if (!step || t <= at + step) {
        return at;
    }
    const uint64_t n = (t - at) / step;
    static unsigned s_logged;
    const unsigned c = ++s_logged;
    if (c <= 8 || !(c & (c - 1))) {
        nc_log("xbased-native", "nv2a: the vblank thread woke %.1f ms late: %llu vblanks dropped, not sent "
               "one after the other (#%u)", (double)(t - at) / 1e6, (unsigned long long)n, c);
    }
    return at + n * step;
}

static void *vblank_thread(void *arg)
{
    nc_pause_register("vblank");
#ifdef _WIN32
    /* the console's clock: before the emulation's busy threads (the vCPU's
     * is above normal), which a loaded PC can't run all at once */
    nc_win_thread_priority(2);
#endif
    uint64_t next = now_ns();
    for (;;) {
        if (nc_paused()) {
            /* the emulation paused: no vblanks, and from now on resuming
             * (not the ones missed meanwhile, one after the other) */
            nc_pause_wait();
            next = now_ns();
        }
        const uint64_t prev = next;
        const uint64_t due = next_vblank(prev);
        next = wait_vblank(due, prev);
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
        next = late_skip(next, due > prev ? due - prev : __atomic_load_n(&g_frame_ns, __ATOMIC_RELAXED));
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

/* A flip's vblank, the one its frame is due on screen from: the last one's,
 * or the next one's for a flip in the last quarter of a frame, which on the
 * console is on screen whole from the next scan (a vblank handler's flip
 * comes just after its vblank). Total Immersion Racing flips its IMMEDIATE
 * presents 1.4 ms before each vblank: due at the one before, its pictures
 * were ready 17 ms after it, later than any lead, and went to the screen as
 * they came, untimed, wherever its flips fell against the vsyncs (3 ms
 * before one: in one run of 12 s 19 never shown). */
uint64_t hle_host_flip_vblank_ns(void)
{
    const uint64_t last = __atomic_load_n(&g_last_vblank_ns, __ATOMIC_ACQUIRE);
    const uint64_t frame = __atomic_load_n(&g_frame_ns, __ATOMIC_RELAXED);
    const uint64_t now = now_ns();
    /* a trap's handler's flip on its pace: due at its slot, the place the
     * pace gave it, a little before or after (held to it, or let go less
     * than an eighth of the pace early): at 120 Hz Total Immersion Racing's
     * slots at Up to 30 fell between two vblanks, its flips went to the one
     * before, 8 ms earlier, ready 12 ms after it and 18 ms to the screen
     * (12 where they fell on vblanks); on a variable refresh rate display
     * the pace's own instants */
    /* (a flip that came late, up to half its pace past the slot it is on:
     * pbmode.c pace_flip) */
    const uint64_t ts = __atomic_load_n(&g_trap_slot_ns, __ATOMIC_ACQUIRE);
    const uint64_t te = __atomic_load_n(&g_trap_every_ns, __ATOMIC_RELAXED);
    const uint64_t late = te / 2 > FRAME_DUE_SPAN ? te / 2 + EARLY_POLL_NS : FRAME_DUE_SPAN;
    static uint64_t s_given;    /* the trap's pace's slot given last */
    if (ts && now + FRAME_DUE_SPAN >= ts && now < ts + late) {
        /* a slot is one picture's: one that comes before the pace has
         * moved on (a late frame on its slot, the next drawn at once after
         * it) is due at the slot after the last one given (it was due at the
         * same instant as that one, or a slot before it, one never on
         * screen) */
        const uint64_t d = te && ts <= s_given && s_given - ts < te ? s_given + te : ts;
        s_given = d;
        return d;
    }
    /* On the pace, outside its slot's window (the decoder met the flip
     * before pace_flip, or a while after): the slot after the last one given,
     * the one a picture a slot keeps to, when that is within a slot of now.
     * The vblank's rules below gave such flips an instant off the pace's grid
     * (at Up to 60 the console's 59.94 Hz vblank drifts past the limit's 60:
     * Republic Commando had one picture due 2 to 10 ms after the one before
     * and the next 30 to 23 after it, a few times in 30 s), or the last
     * one's. */
    if (ts && te && s_given && now < ts + 3 * te && now + te > s_given + te && now < s_given + 2 * te) {
        s_given += te;
        return s_given;
    }
    /* a limit's frame: due at its first early vblank's place (frame_due) */
    const uint64_t fd = __atomic_load_n(&g_frame_due_ns, __ATOMIC_ACQUIRE);
    uint64_t d = fd && __atomic_load_n(&g_uncap_on, __ATOMIC_RELAXED) && now + EARLY_POLL_NS >= fd && now < fd + FRAME_DUE_SPAN
                     ? fd
                     : last && now >= last + frame - frame / 4 && now < last + frame + frame / 2 ? last + frame : last;
    /* and a flip on the trap's pace those rules put at the slot given last,
     * before it or less than half a slot after it (a late frame drawn at once
     * after the one before: due at its instant, one of the two never on
     * screen) is on the slot after it */
    if (ts && te && s_given && now < ts + 3 * te && d < s_given + te / 2 && s_given + te < now + 2 * te) {
        s_given += te;
        d = s_given;
    }
    return d;
}

int64_t hle_host_vblank_period_ns(void)
{
    return (int64_t)__atomic_load_n(&g_frame_ns, __ATOMIC_RELAXED);
}

uint32_t hle_host_gpu_reg(uint32_t off)
{
    return g_regs && off < NV2A_SIZE - 3 ? ld(off & ~3u) : 0;
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
    uint32_t pt_set;                /* (v2: the PTIMER rework's, kept 0 here for the record's layout) */
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
    const NcNv2aState st = {__atomic_load_n(&g_vblanks, __ATOMIC_RELAXED), g_frame_ns,
                            pages_put(NULL, g_regs, NV2A_SIZE, true), 0, 0, 0,
                            nc_dev_nv2a_av_mode(), nc_dev_nv2a_av_title()};
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
    __atomic_store_n(&g_vblanks, st.vblanks, __ATOMIC_RELAXED);
    /* the state's video mode (50 Hz: a 20 ms frame), at this run's rate (the
     * mods on now: a state saved with or without them) */
    nc_dev_nv2a_refresh(st.frame_ns >= 18000000ull ? 50 : 60);
    g_last_vblank_ns = now_ns();
    g_disp_vsync_ns = 0;
    g_pick_off_ns = -1;
    if (ld(NV_PTIMER_ALARM_0) || ld(NV_PTIMER_INTR_EN_0)) {
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
