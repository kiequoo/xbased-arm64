/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "native/dev/apu/apu_int.h"
#include "dsp_rec.h"

static const int16_t ep_silence[256][2] = { 0 };

/* xbased: the GP's program didn't finish its frames (an instruction the
 * core lacks, a loop it can't leave): the VP's own mix from then on */
static bool g_gp_out_failed;
/* the GP ran a program: its output from then on, through its resets (a
 * new effects image) too, silent while it's held, as the console's */
static bool g_gp_out_started;
static unsigned g_gp_unfinished;

void mcpx_apu_dsp_reset_output(void)
{
    g_gp_out_started = false;
    g_gp_out_failed = false;
    g_gp_unfinished = 0;
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    static int last_known_preference = -1;

    /* xbased: the GP's output, as the console's, when it runs a program and
     * the recompiler (dsp_rec.cpp) runs it: its speaker outputs carry the
     * title's own routing and effects (a voice only sent to an effect, the
     * 5.1 mix). The EP, which only encodes them for the digital out, as
     * before. The preference (NC_APU_DSP=1) runs both, the EP's output
     * heard; XBASED_DSP_REC=0, the VP's own mix as before */
    const bool gp_on = (d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPRST) &&
                       (d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPDSPRST);
    if (!g_apu_config.use_dsp && gp_on && !g_gp_out_failed && dsp_rec_ready(d->gp.dsp)) {
        g_gp_out_started = true;
    }
    int want = 0;
    if (g_apu_config.use_dsp) {
        want = 2;
    } else if (g_gp_out_started && !g_gp_out_failed) {
        want = 1;
    }

    if (last_known_preference == want) {
        return;
    }

    if (want == 2) {
        d->monitor.point = MCPX_APU_DEBUG_MON_GP_OR_EP;
        d->gp.realtime = true;
        d->ep.realtime = true;
    } else if (want == 1) {
        d->monitor.point = MCPX_APU_DEBUG_MON_GP;
        d->gp.realtime = true;
        d->ep.realtime = false;
    } else {
        d->monitor.point = MCPX_APU_DEBUG_MON_VP;
        d->gp.realtime = false;
        d->ep.realtime = false;
    }

    if (last_known_preference >= 0 || want) {
        nc_log("xbased-apu", "audio: %s", want == 2 ? "the GP's and the EP's programs (NC_APU_DSP=1)" :
               want == 1 ? "the title's DSP program (GP), compiled" : "the voices' own mix");
    }
    last_known_preference = want;
}

static void scatter_gather_rw(MCPXAPUState *d, uint64_t sge_base,
                              unsigned int max_sge, uint8_t *ptr, uint32_t addr,
                              size_t len, bool dir)
{
    unsigned int page_entry = addr / APU_PAGE_SIZE;
    unsigned int offset_in_page = addr % APU_PAGE_SIZE;
    unsigned int bytes_to_copy = APU_PAGE_SIZE - offset_in_page;

    while (len > 0) {
        assert(page_entry <= max_sge);

        uint32_t prd_address = apu_ram_ld32(sge_base + page_entry * 8 + 0);
        // uint32_t prd_control = apu_ram_ld32(//                                     sge_base + page_entry * 8 + 4);

        uint64_t paddr = prd_address + offset_in_page;

        if (bytes_to_copy > len) {
            bytes_to_copy = len;
        }

        /* xbased: the page's address is the guest table's; a piece past RAM
         * is skipped (a read leaves those bytes as they were) */
        const bool in_ram = APU_CHECK(paddr + bytes_to_copy <= d->ram_size);
        if (in_ram && dir) {
            memcpy(&d->ram_ptr[paddr], ptr, bytes_to_copy);
            hle_host_mem_set_dirty(paddr, bytes_to_copy);
#if defined(__aarch64__)
            /* Wake any TCG thread parked in WFE on this cache line.
             * APU DSP writes audio voice descriptors that include
             * "voice done" / "buffer drained" flags the guest polls
             * via cmp+jne self-loops (e.g. PGR2 0x1efc00). The ARM
             * cache-coherent invalidation triggered by the memcpy
             * above SHOULD trip the spinner's exclusive monitor, but
             * empirically misses on some scheduler / interrupt
             * patterns. An explicit SEV is reliable and cheap.
             *
             * SEV broadcasts to all cores' event registers; any WFE
             * waiter (on this or any other core) wakes immediately
             * and re-checks the polled value. */
            __asm__ __volatile__("sev" ::: "memory");
#endif
        } else if (in_ram) {
            memcpy(ptr, &d->ram_ptr[paddr], bytes_to_copy);
        }

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration, we are page aligned */
        page_entry += 1;
        bytes_to_copy = APU_PAGE_SIZE;
        offset_in_page = 0;
    }
}

static void gp_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    // fprintf(stderr, "GP %s scratch 0x%x bytes (0x%x words) at %x (0x%x words)\n", dir ? "writing to" : "reading from", len, len/4, addr, addr/4);
    scatter_gather_rw(d, d->regs[NV_PAPU_GPSADDR], d->regs[NV_PAPU_GPSMAXSGE],
                      ptr, addr, len, dir);
}

static void ep_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    // fprintf(stderr, "EP %s scratch 0x%x bytes (0x%x words) at %x (0x%x words)\n", dir ? "writing to" : "reading from", len, len/4, addr, addr/4);
    scatter_gather_rw(d, d->regs[NV_PAPU_EPSADDR], d->regs[NV_PAPU_EPSMAXSGE],
                      ptr, addr, len, dir);
}

static uint32_t circular_scatter_gather_rw(MCPXAPUState *d, uint64_t sge_base,
                                           unsigned int max_sge, uint8_t *ptr,
                                           uint32_t base, uint32_t end,
                                           uint32_t cur, size_t len, bool dir)
{
    /* xbased: the FIFO's registers are the guest's; with cur outside
     * [base, end) nothing could be copied (end == cur: forever) */
    if (!APU_CHECK(base <= cur && cur < end)) {
        return cur;
    }
    while (len > 0) {
        unsigned int bytes_to_copy = end - cur;

        if (bytes_to_copy > len) {
            bytes_to_copy = len;
        }

        DPRINTF("circular scatter gather %s in range 0x%x - 0x%x at 0x%x of "
                "length 0x%x / 0x%lx bytes\n",
                dir ? "write" : "read", base, end, cur, bytes_to_copy, len);

        assert((cur >= base) && ((cur + bytes_to_copy) <= end));
        scatter_gather_rw(d, sge_base, max_sge, ptr, cur, bytes_to_copy, dir);

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration we might have to wrap */
        cur += bytes_to_copy;
        if (cur >= end) {
            assert(cur == end);
            cur = base;
        }
    }

    return cur;
}

static void gp_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    uint64_t cur_reg;
    if (dir) {
        assert(index < GP_OUTPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_GPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPOFCUR0 + 0x10 * index;
    } else {
        assert(index < GP_INPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_GPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    // fprintf(stderr, "GP %s fifo #%d, base = %x, end = %x, cur = %x, len = %x\n",
    //     dir ? "writing to" : "reading from", index,
    //     base, end, cur, len);

    /* DSP hangs if current >= end; but forces current >= base */
    assert(cur < end);
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_GPFADDR], d->regs[NV_PAPU_GPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static bool ep_sink_samples(MCPXAPUState *d, uint8_t *ptr, size_t len)
{
    if (d->monitor.point == MCPX_APU_DEBUG_MON_AC97) {
        return false;
    } else if ((d->monitor.point == MCPX_APU_DEBUG_MON_EP) ||
        (d->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP)) {
        /* xbased: len is the DMA's, from the guest's DSP code */
        if (APU_CHECK(len == sizeof(d->monitor.frame_buf))) {
            memcpy(d->monitor.frame_buf, ptr, len);
        }
    }

    return true;
}

static void ep_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    uint64_t cur_reg;
    if (dir) {
        assert(index < EP_OUTPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_EPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPOFCUR0 + 0x10 * index;
    } else {
        assert(index < EP_INPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_EPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    // fprintf(stderr, "EP %s fifo #%d, base = %x, end = %x, cur = %x, len = %x\n",
    //     dir ? "writing to" : "reading from", index,
    //     base, end, cur, len);

    if (dir && index == 0) {
        bool did_sink = ep_sink_samples(d, ptr, len);
        if (did_sink) {
            /* Since we are sinking, push silence out */
            if (!APU_CHECK(len <= sizeof(ep_silence))) {
                return;
            }
            ptr = (uint8_t*)ep_silence;
        }
    }

    /* DSP hangs if current >= end; but forces current >= base */
    if (cur >= end) {
        cur = cur % (end - base);
    }
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_EPFADDR], d->regs[NV_PAPU_EPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static void proc_rst_write(DSPState *dsp, uint32_t oldval, uint32_t val)
{
    if (!(val & NV_PAPU_GPRST_GPRST) || !(val & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_reset(dsp);
    } else if (
        (!(oldval & NV_PAPU_GPRST_GPRST) || !(oldval & NV_PAPU_GPRST_GPDSPRST))
        && ((val & NV_PAPU_GPRST_GPRST) && (val & NV_PAPU_GPRST_GPDSPRST))) {
        dsp_bootstrap(dsp);
    }
}

/* Global Processor - programmable DSP */
uint64_t mcpx_apu_gp_read(void *opaque, uint32_t addr, unsigned int size)
{
    MCPXAPUState *d = opaque;

    assert(size == 4);
    assert(addr % 4 == 0);

    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_GPXMEM ... NV_PAPU_GPXMEM + 0x1000 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPXMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'X', xaddr);
        // fprintf(stderr, "read GP NV_PAPU_GPXMEM [%x] -> %x\n", xaddr, r);
        break;
    }
    case NV_PAPU_GPMIXBUF ... NV_PAPU_GPMIXBUF + 0x400 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPMIXBUF) / 4;
        r = dsp_read_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE + xaddr);
        // fprintf(stderr, "read GP NV_PAPU_GPMIXBUF [%x] -> %x\n", xaddr, r);
        break;
    }
    case NV_PAPU_GPYMEM ... NV_PAPU_GPYMEM + 0x800 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_GPYMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'Y', yaddr);
        // fprintf(stderr, "read GP NV_PAPU_GPYMEM [%x] -> %x\n", yaddr, r);
        break;
    }
    case NV_PAPU_GPPMEM ... NV_PAPU_GPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_GPPMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'P', paddr);
        // fprintf(stderr, "read GP NV_PAPU_GPPMEM [%x] -> %x\n", paddr, r);
        break;
    }
    default:
        /* an access at the window's end can run past the array */
        if (addr < ARRAY_SIZE(d->gp.regs)) {
            r = d->gp.regs[addr];
        }
        break;
    }
    DPRINTF("mcpx apu GP: read [0x%" PRIx64 "] -> 0x%lx\n", addr, r);

    return r;
}

/* xbased: the DSP memories are 24 bits wide and a host write's top byte is
 * not stored (DirectSound can write sign-extended words; the DSP core
 * asserts on them) */
#define DSP_WORD(v) ((uint32_t)(v) & 0xFFFFFF)

void mcpx_apu_gp_write(void *opaque, uint32_t addr, uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    pthread_mutex_lock(&d->lock);

    assert(size == 4);
    assert(addr % 4 == 0);

    DPRINTF("mcpx apu GP: [0x%" PRIx64 "] = 0x%lx\n", addr, val);

    switch (addr) {
    case NV_PAPU_GPXMEM ... NV_PAPU_GPXMEM + 0x1000 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPXMEM) / 4;
        // fprintf(stderr, "gp write xmem %x = %x\n", xaddr, val);
        dsp_write_memory(d->gp.dsp, 'X', xaddr, DSP_WORD(val));
        break;
    }
    case NV_PAPU_GPMIXBUF ... NV_PAPU_GPMIXBUF + 0x400 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPMIXBUF) / 4;
        // fprintf(stderr, "gp write xmixbuf %x = %x\n", xaddr, val);
        dsp_write_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE + xaddr, DSP_WORD(val));
        break;
    }
    case NV_PAPU_GPYMEM ... NV_PAPU_GPYMEM + 0x800 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_GPYMEM) / 4;
        // fprintf(stderr, "gp write ymem %x = %x\n", yaddr, val);
        dsp_write_memory(d->gp.dsp, 'Y', yaddr, DSP_WORD(val));
        break;
    }
    case NV_PAPU_GPPMEM ... NV_PAPU_GPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_GPPMEM) / 4;
        // fprintf(stderr, "gp write pmem %x = %x\n", paddr, val);
        dsp_write_memory(d->gp.dsp, 'P', paddr, DSP_WORD(val));
        break;
    }
    case NV_PAPU_GPRST:
        proc_rst_write(d->gp.dsp, d->gp.regs[NV_PAPU_GPRST], val);
        d->gp.regs[NV_PAPU_GPRST] = val;
        break;
    default:
        if (addr < ARRAY_SIZE(d->gp.regs)) {
            d->gp.regs[addr] = val;
        }
        break;
    }

    pthread_mutex_unlock(&d->lock);
}

/* Encode Processor - encoding DSP */
uint64_t mcpx_apu_ep_read(void *opaque, uint32_t addr, unsigned int size)
{
    MCPXAPUState *d = opaque;

    assert(size == 4);
    assert(addr % 4 == 0);

    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_EPXMEM ... NV_PAPU_EPXMEM + 0xC00 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_EPXMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'X', xaddr);
        // fprintf(stderr, "read EP  NV_PAPU_EPXMEM [%x] -> %x\n", xaddr, r);
        break;
    }
    case NV_PAPU_EPYMEM ... NV_PAPU_EPYMEM + 0x100 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_EPYMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'Y', yaddr);
        // fprintf(stderr, "read EP  NV_PAPU_EPYMEM [%x] -> %x\n", yaddr, r);
        break;
    }
    case NV_PAPU_EPPMEM ... NV_PAPU_EPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_EPPMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'P', paddr);
        // fprintf(stderr, "read EP  NV_PAPU_EPPMEM [%x] -> %x\n", paddr, r);
        break;
    }
    default:
        if (addr < ARRAY_SIZE(d->ep.regs)) {
            r = d->ep.regs[addr];
        }
        break;
    }
    DPRINTF("mcpx apu EP: read [0x%" PRIx64 "] -> 0x%lx\n", addr, r);

    return r;
}

void mcpx_apu_ep_write(void *opaque, uint32_t addr, uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    pthread_mutex_lock(&d->lock);

    assert(size == 4);
    assert(addr % 4 == 0);

    DPRINTF("mcpx apu EP: [0x%" PRIx64 "] = 0x%lx\n", addr, val);

    switch (addr) {
    case NV_PAPU_EPXMEM ... NV_PAPU_EPXMEM + 0xC00 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_EPXMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'X', xaddr, DSP_WORD(val));
        // fprintf(stderr, "ep write xmem %x = %x\n", xaddr, val);
        break;
    }
    case NV_PAPU_EPYMEM ... NV_PAPU_EPYMEM + 0x100 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_EPYMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'Y', yaddr, DSP_WORD(val));
        // fprintf(stderr, "ep write ymem %x = %x\n", yaddr, val);
        break;
    }
    case NV_PAPU_EPPMEM ... NV_PAPU_EPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_EPPMEM) / 4;
        // fprintf(stderr, "ep write pmem %x = %x\n", paddr, val);
        dsp_write_memory(d->ep.dsp, 'P', paddr, DSP_WORD(val));
        break;
    }
    case NV_PAPU_EPRST:
        proc_rst_write(d->ep.dsp, d->ep.regs[NV_PAPU_EPRST], val);
        d->ep.regs[NV_PAPU_EPRST] = val;
        d->ep_frame_div = 0; /* FIXME: Still unsure about frame sync */
        break;
    default:
        if (addr < ARRAY_SIZE(d->ep.regs)) {
            d->ep.regs[addr] = val;
        }
        break;
    }

    pthread_mutex_unlock(&d->lock);
}



#define GP_FRAME_CYCLES (160000000 / 1500)

/* ---- dev (XBASED_DSP_SHADOW=1, with the GP until idle): each GP frame on
 * the interpreter as well, from the same state, its writes to RAM and the
 * FIFOs set aside (and seen by its own reads), and both compared: the
 * recompiler's check on the games' own programs */

typedef struct ShadowIo {
    uint8_t fifo, dir;
    uint32_t addr, len, hash;
    uint32_t off; /* the reference's reads: their data in rdd */
} ShadowIo;

typedef struct ShadowOv {
    uint32_t addr, len, off;
} ShadowOv;

static struct {
    int on;
    DSPState *ref;
    ShadowIo *log[2];
    int n[2], cap[2];
    ShadowOv *ov;
    int ov_n, ov_cap;
    uint8_t *ovd;
    size_t ovd_n, ovd_cap;
    uint8_t *rdd;
    size_t rdd_n, rdd_cap;
    uint64_t frames, bad, runs_capped;
    int said;
} g_shadow = {.on = -1};

static uint32_t shadow_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static void shadow_log(int who, int fifo, bool dir, uint32_t addr, const uint8_t *p, size_t len)
{
    if (g_shadow.n[who] == g_shadow.cap[who]) {
        g_shadow.cap[who] = g_shadow.cap[who] ? 2 * g_shadow.cap[who] : 256;
        g_shadow.log[who] = realloc(g_shadow.log[who], g_shadow.cap[who] * sizeof(ShadowIo));
    }
    ShadowIo *e = &g_shadow.log[who][g_shadow.n[who]++];
    e->fifo = fifo;
    e->dir = dir;
    e->addr = addr;
    e->len = (uint32_t)len;
    e->hash = shadow_hash(p, len);
    e->off = 0;
    if (who == 0 && !dir) {
        /* the guest's CPU writes this memory as the frames run: the real
         * run reads what the reference read */
        if (g_shadow.rdd_n + len > g_shadow.rdd_cap) {
            g_shadow.rdd_cap = (g_shadow.rdd_n + len) * 2;
            g_shadow.rdd = realloc(g_shadow.rdd, g_shadow.rdd_cap);
        }
        memcpy(g_shadow.rdd + g_shadow.rdd_n, p, len);
        e->off = (uint32_t)g_shadow.rdd_n;
        g_shadow.rdd_n += len;
    }
}

/* the real run's read: the reference's data when it's the same transfer */
static bool shadow_replay(int fifo, uint32_t addr, uint8_t *ptr, size_t len)
{
    const int i = g_shadow.n[1];
    if (i >= g_shadow.n[0]) return false;
    const ShadowIo *e = &g_shadow.log[0][i];
    if (e->fifo != fifo || e->dir || e->addr != addr || e->len != len) return false;
    memcpy(ptr, g_shadow.rdd + e->off, len);
    return true;
}

static void shadow_ref_scratch(void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    if (dir) {
        shadow_log(0, 0, dir, addr, ptr, len);
        if (g_shadow.ov_n == g_shadow.ov_cap) {
            g_shadow.ov_cap = g_shadow.ov_cap ? 2 * g_shadow.ov_cap : 64;
            g_shadow.ov = realloc(g_shadow.ov, g_shadow.ov_cap * sizeof(ShadowOv));
        }
        if (g_shadow.ovd_n + len > g_shadow.ovd_cap) {
            g_shadow.ovd_cap = (g_shadow.ovd_n + len) * 2;
            g_shadow.ovd = realloc(g_shadow.ovd, g_shadow.ovd_cap);
        }
        memcpy(g_shadow.ovd + g_shadow.ovd_n, ptr, len);
        g_shadow.ov[g_shadow.ov_n++] = (ShadowOv){addr, (uint32_t)len, (uint32_t)g_shadow.ovd_n};
        g_shadow.ovd_n += len;
        return;
    }
    gp_scratch_rw(opaque, ptr, addr, len, false);
    for (int i = 0; i < g_shadow.ov_n; i++) {
        const ShadowOv *o = &g_shadow.ov[i];
        const uint32_t lo = MAX(addr, o->addr);
        const uint32_t hi = MIN(addr + (uint32_t)len, o->addr + o->len);
        if (lo < hi) {
            memcpy(ptr + (lo - addr), g_shadow.ovd + o->off + (lo - o->addr), hi - lo);
        }
    }
    shadow_log(0, 0, dir, addr, ptr, len);
}

static void shadow_ref_fifo(void *opaque, uint8_t *ptr, unsigned int index, size_t len, bool dir)
{
    if (!dir) {
        gp_fifo_rw(opaque, ptr, index, len, false);
    }
    shadow_log(0, 1, dir, index, ptr, len);
}

static void shadow_real_scratch(void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    if (dir || !shadow_replay(0, addr, ptr, len)) gp_scratch_rw(opaque, ptr, addr, len, dir);
    shadow_log(1, 0, dir, addr, ptr, len);
}

static void shadow_real_fifo(void *opaque, uint8_t *ptr, unsigned int index, size_t len, bool dir)
{
    if (dir || !shadow_replay(1, index, ptr, len)) {
        gp_fifo_rw(opaque, ptr, index, len, dir);
    } else {
        /* the FIFO's position moves on as the read would */
        uint8_t tmp[4096];
        if (len <= sizeof(tmp)) gp_fifo_rw(opaque, tmp, index, len, dir);
    }
    shadow_log(1, 1, dir, index, ptr, len);
}

/* the reference run; the FIFOs' read positions put back for the real one */
static void shadow_begin(MCPXAPUState *d)
{
    DSPState *g = d->gp.dsp;
    if (!g_shadow.ref) {
        g_shadow.ref = malloc(sizeof(DSPState));
    }
    DSPState *ref = g_shadow.ref;
    memcpy(ref, g, sizeof(*ref));
    ref->dma.core = &ref->core;
    ref->dma.scratch_rw = shadow_ref_scratch;
    ref->dma.fifo_rw = shadow_ref_fifo;
    ref->rec = NULL;
    g_shadow.n[0] = g_shadow.n[1] = 0;
    g_shadow.ov_n = 0;
    g_shadow.ovd_n = 0;
    g_shadow.rdd_n = 0;

    uint32_t cur[GP_INPUT_FIFO_COUNT];
    for (int i = 0; i < GP_INPUT_FIFO_COUNT; i++) {
        cur[i] = d->regs[NV_PAPU_GPIFCUR0 + 0x10 * i];
    }
    int guard = 0;
    do {
        dsp_run_interp(ref, 1000);
    } while (!ref->core.is_idle && ++guard < 50 * GP_FRAME_CYCLES / 500);
    if (!ref->core.is_idle) {
        g_shadow.runs_capped++;
    }
    for (int i = 0; i < GP_INPUT_FIFO_COUNT; i++) {
        d->regs[NV_PAPU_GPIFCUR0 + 0x10 * i] = cur[i];
    }
    g->dma.scratch_rw = shadow_real_scratch;
    g->dma.fifo_rw = shadow_real_fifo;
}

static void shadow_end(MCPXAPUState *d)
{
    DSPState *g = d->gp.dsp, *ref = g_shadow.ref;
    g->dma.scratch_rw = gp_scratch_rw;
    g->dma.fifo_rw = gp_fifo_rw;
    const dsp_core_t *a = &ref->core, *b = &g->core;
    char what[160] = "";
    if (a->pc != b->pc) {
        snprintf(what, sizeof(what), "pc %04x %04x", a->pc, b->pc);
    }
    for (int i = 0; !what[0] && i < DSP_REG_MAX; i++) {
        if (a->registers[i] != b->registers[i]) {
            snprintf(what, sizeof(what), "reg %02x: %06x %06x", i, a->registers[i], b->registers[i]);
        }
    }
    if (!what[0] && memcmp(a->stack, b->stack, sizeof(a->stack))) snprintf(what, sizeof(what), "stack");
    for (int i = 0; !what[0] && i < DSP_XRAM_SIZE; i++) {
        if (a->xram[i] != b->xram[i]) snprintf(what, sizeof(what), "x:%04x %06x %06x", i, a->xram[i], b->xram[i]);
    }
    for (int i = 0; !what[0] && i < DSP_YRAM_SIZE; i++) {
        if (a->yram[i] != b->yram[i]) snprintf(what, sizeof(what), "y:%04x %06x %06x", i, a->yram[i], b->yram[i]);
    }
    for (int i = 0; !what[0] && i < DSP_MIXBUFFER_SIZE; i++) {
        if (a->mixbuffer[i] != b->mixbuffer[i]) snprintf(what, sizeof(what), "mix:%03x %06x %06x", i, a->mixbuffer[i], b->mixbuffer[i]);
    }
    if (!what[0] && memcmp(a->pram, b->pram, sizeof(a->pram))) snprintf(what, sizeof(what), "pram");
    if (!what[0] && (a->loop_rep != b->loop_rep || a->pc_on_rep != b->pc_on_rep || a->is_idle != b->is_idle ||
                     a->interrupt_state != b->interrupt_state || a->interrupt_counter != b->interrupt_counter)) {
        snprintf(what, sizeof(what), "rep/idle/irq");
    }
    if (!what[0] && (ref->dma.configuration != g->dma.configuration || ref->dma.control != g->dma.control ||
                     ref->dma.start_block != g->dma.start_block || ref->dma.next_block != g->dma.next_block ||
                     ref->dma.error != g->dma.error || ref->dma.eol != g->dma.eol || ref->interrupts != g->interrupts)) {
        snprintf(what, sizeof(what), "dma: control %x %x", ref->dma.control, g->dma.control);
    }
    if (!what[0] && g_shadow.n[0] != g_shadow.n[1]) {
        snprintf(what, sizeof(what), "transfers: %d %d", g_shadow.n[0], g_shadow.n[1]);
    }
    for (int i = 0; !what[0] && i < g_shadow.n[0]; i++) {
        const ShadowIo *x = &g_shadow.log[0][i], *y = &g_shadow.log[1][i];
        if (x->fifo != y->fifo || x->dir != y->dir || x->addr != y->addr || x->len != y->len || x->hash != y->hash) {
            snprintf(what, sizeof(what), "transfer %d (%s %s %x+%x)", i, x->fifo ? "fifo" : "scratch",
                     x->dir ? "write" : "read", x->addr, x->len);
        }
    }
    g_shadow.frames++;
    if (what[0]) {
        g_shadow.bad++;
        if (g_shadow.said < 40) {
            g_shadow.said++;
            nc_log("xbased-apu", "dsp shadow: frame %llu differs: %s", (unsigned long long)g_shadow.frames, what);
        }
    }
    if (g_shadow.frames % 1500 == 0) {
        nc_log("xbased-apu", "dsp shadow: %llu frames, %llu differ (%llu reference runs past the cap)",
               (unsigned long long)g_shadow.frames, (unsigned long long)g_shadow.bad,
               (unsigned long long)g_shadow.runs_capped);
    }
}

void mcpx_apu_dsp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Write VP results to the GP DSP MIXBUF (X:GP_DSP_MIXBUF_BASE is the
     * core's mix buffer: the words stored directly) */
    _Static_assert(GP_DSP_MIXBUF_BASE == DSP_MIXBUFFER_BASE &&
                   NUM_MIXBINS * NUM_SAMPLES_PER_FRAME == DSP_MIXBUFFER_SIZE,
                   "the mixbins fill the GP's mix buffer");
    uint32_t *mixbuf = d->gp.dsp->core.mixbuffer;
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        /* the VP's own mix: the mixbins are all zero (mcpx_apu_vp_frame) */
        memset(mixbuf, 0, NUM_MIXBINS * NUM_SAMPLES_PER_FRAME * sizeof(*mixbuf));
    } else {
        for (int mixbin = 0; mixbin < NUM_MIXBINS; mixbin++) {
            for (int sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) {
                mixbuf[mixbin * NUM_SAMPLES_PER_FRAME + sample] =
                    float_to_24b(mixbins[mixbin][sample]);
            }
        }
    }

    bool ep_enabled = (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPRST) &&
                      (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPDSPRST);

    /* Run GP */
    if ((d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPRST) &&
        (d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_start_frame(d->gp.dsp);
        d->gp.dsp->core.is_idle = false;
        d->gp.dsp->core.cycle_count = 0;
        /* xbased: while a vCPU waits (mcpx_apu_spin_wait), up to one
         * hardware frame of cycles (160 MHz / 1500 Hz) */
        bool boost = !d->gp.realtime &&
                     apu_ld(&d->gp_boost_until_us) >
                         apu_host_us();
        if (g_shadow.on < 0) {
            const char *e = getenv("XBASED_DSP_SHADOW");
            g_shadow.on = e && e[0] == '1';
        }
        const bool shadow = g_shadow.on && d->gp.realtime;
        if (shadow) {
            shadow_begin(d);
        }
        if (d->gp.realtime) {
            /* to its end in one run, up to 8 hardware frames of cycles: a
             * program the core can't finish (an instruction it lacks) no
             * longer holds the APU's thread */
            d->gp.dsp->save_cycles = 0;
            dsp_run(d->gp.dsp, 16 * GP_FRAME_CYCLES);
        } else {
            do {
                dsp_run(d->gp.dsp, 1000);
            } while (!d->gp.dsp->core.is_idle &&
                     boost && d->gp.dsp->core.cycle_count < GP_FRAME_CYCLES);
        }
        if (d->gp.realtime && !d->gp.dsp->core.is_idle &&
            d->monitor.point == MCPX_APU_DEBUG_MON_GP) {
            if (++g_gp_unfinished == 30) {
                g_gp_out_failed = true;
                nc_log("xbased-apu", "the title's DSP program doesn't finish its frames here "
                       "(at p:%04x): the voices' own mix from now on", d->gp.dsp->core.pc);
                mcpx_apu_update_dsp_preference(d);
            }
        }
        if (shadow) {
            shadow_end(d);
        }
        static int stats = -1;
        if (stats < 0) {
            const char *e = getenv("XBASED_APU_STATS");
            stats = e && e[0] == '1';
        }
        if (stats) {
            dsp_rec_frame_stats(d->gp.dsp);
        }

        /* 5.1: the GP's six speaker outputs (X:0x1400, 32 samples a channel,
         * FL FR C LFE BL BR as DirectSound's mixbins), what its EP encodes
         * for the Dolby Digital out; the VP's own mix has its own (vp.c) */
        if (g_apu_out_channels == 6 && d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
            int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
            for (int c = 0; c < 6; c++) {
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                    uint32_t v = dsp_read_memory(d->gp.dsp, 'X', 0x1400 + c * 0x20 + i);
                    g_apu_frame6[off + i][c] = v >> 8;
                }
            }
        }
        /* Stereo: the six folded into the front pair, as the console's EP
         * downmixes them for its analog out whatever the speaker setting (the
         * XDK's multichannel white paper: the centre and the surrounds into
         * the front speakers at reduced volume, the LFE dropped; the voices'
         * own mix folds the same, vp.c). A title sends to the centre in
         * stereo too: Dynasty Warriors 5, booted in stereo, puts its
         * officers' lines there (#402), and the front pair alone lost them.
         * A frame whose six are the pair three times (DirectSound's stereo
         * layout, seen in Max Payne's) is the pair as it is: folding it
         * would play it 7.6 dB louder. */
        if ((d->monitor.point == MCPX_APU_DEBUG_MON_GP) ||
            (d->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP && !ep_enabled)) {
            int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
            int32_t s[6][NUM_SAMPLES_PER_FRAME];
            bool pair3 = true;
            for (int c = 0; c < 6; c++) {
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                    const uint32_t v = dsp_read_memory(d->gp.dsp, 'X', 0x1400 + c * 0x20 + i);
                    s[c][i] = (int32_t)(v << 8) >> 8;   /* 24 bits, signed */
                }
            }
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME && pair3; i++) {
                pair3 = s[2][i] == s[0][i] && s[4][i] == s[0][i] && s[3][i] == s[1][i] && s[5][i] == s[1][i];
            }
            /* -3 dB (0.70710678) in 16.16 */
            const int64_t side = pair3 ? 0 : 46341;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                const int64_t l = ((int64_t)s[0][i] << 16) + side * ((int64_t)s[2][i] + s[4][i]);
                const int64_t r = ((int64_t)s[1][i] << 16) + side * ((int64_t)s[2][i] + s[5][i]);
                const int64_t l16 = l >> 24, r16 = r >> 24;
                d->monitor.frame_buf[off + i][0] = (int16_t)(l16 > 32767 ? 32767 : l16 < -32768 ? -32768 : l16);
                d->monitor.frame_buf[off + i][1] = (int16_t)(r16 > 32767 ? 32767 : r16 < -32768 ? -32768 : r16);
            }
        }
    }

    /* Run EP */
    if ((d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPRST) &&
        (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPDSPRST)) {
        if (d->ep_frame_div % 8 == 0) {
            dsp_start_frame(d->ep.dsp);
            d->ep.dsp->core.is_idle = false;
            d->ep.dsp->core.cycle_count = 0;
            do {
                dsp_run(d->ep.dsp, 1000);
            } while (!d->ep.dsp->core.is_idle && d->ep.realtime);
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    d->gp.dsp = dsp_init(d, gp_scratch_rw, gp_fifo_rw);
    for (int i = 0; i < DSP_PRAM_SIZE; i++) {
        d->gp.dsp->core.pram[i] = 0xCACACACA;
    }
    memset(d->gp.dsp->core.pram_opcache, 0,
           sizeof(d->gp.dsp->core.pram_opcache));
    d->gp.dsp->is_gp = true;
    d->gp.dsp->core.is_gp = true;
    d->gp.dsp->core.is_idle = false;
    d->gp.dsp->core.cycle_count = 0;

    d->ep.dsp = dsp_init(d, ep_scratch_rw, ep_fifo_rw);
    for (int i = 0; i < DSP_PRAM_SIZE; i++) {
        d->ep.dsp->core.pram[i] = 0xCACACACA;
    }
    memset(d->ep.dsp->core.pram_opcache, 0,
           sizeof(d->ep.dsp->core.pram_opcache));
    for (int i = 0; i < DSP_XRAM_SIZE; i++) {
        d->ep.dsp->core.xram[i] = 0xCACACACA;
    }
    for (int i = 0; i < DSP_YRAM_SIZE; i++) {
        d->ep.dsp->core.yram[i] = 0xCACACACA;
    }
    d->ep.dsp->is_gp = false;
    d->ep.dsp->core.is_gp = false;
    d->ep.dsp->core.is_idle = false;
    d->ep.dsp->core.cycle_count = 0;

    /* Until DSP is more performant, a switch to decide whether or not we should
     * use the full audio pipeline or not.
     */
    mcpx_apu_update_dsp_preference(d);

    /* dev: the recompiler against the interpreter on random programs */
    const char *st = getenv("XBASED_DSP_SELFTEST");
    if (st && atoi(st) > 0) {
        const char *seed = getenv("XBASED_DSP_SELFTEST_SEED");
        dsp_rec_selftest(atoi(st), seed ? (uint32_t)strtoul(seed, NULL, 0) : 1);
    }
}
