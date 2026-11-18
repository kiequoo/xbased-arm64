/*
 * xbased native core — library code that waits on emulated hardware.
 *
 * DirectSound hands the APU's GP DSP a command through a mailbox in memory
 * and spins until the DSP takes it:
 *
 *     add eax, 0x800
 *     cmp dword [eax+0x10], 0     ; the previous command still pending
 *     jne $-6
 *
 * The emulated GP runs a slice of its program per audio frame unless a CPU
 * waits on it (mcpx_apu_spin_wait: frames then run to completion), so the
 * DSP reaches its command check only every ~40 ms, and the spinning thread
 * (PGR2's audio thread, above the main thread's priority) holds the CPU all
 * that time. QEMU's TCG recognises the spin and tells the APU; here the loop
 * jumps to a stub that asks the kernel (NC_KORDINAL_APU_SPIN) to hurry the
 * APU and wait for the mailbox, the CPU's thread blocked on the host rather
 * than spinning in translated code. The library is the same in every title
 * seen (one copy, in DSOUND). debug.xbased.apuspin=0 turns it off.
 */
#include "native/kernel/kernel.h"
#include "native/dev/dev.h"

#include <stdio.h>
#include <string.h>

#include <sys/system_properties.h>
#include "hw/xbox/hle/prop.h"

#include "hw/xbox/hle/core/xbe.h"

static const uint8_t k_mailbox_wait[] = {0x05, 0x00, 0x08, 0x00, 0x00,  /* add eax, 0x800 */
                                         0x83, 0x78, 0x10, 0x00,        /* cmp dword [eax+0x10], 0 */
                                         0x75, 0xFA};                   /* jne the cmp */
#define LOOP_OFF 5 /* the cmp */

static bool apu_spin_on(void)
{
    char v[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.xbased.apuspin", v);
    return strcmp(v, "0") != 0;
}

/* The stub for the loop at va (its cmp; 6 bytes):
 *   L: cmp dword [eax+0x10], 0 ; je out ; int 3Fh APU_SPIN ; jmp L
 *   out: jmp va+6 */
static uint32_t mailbox_stub(NcKernel *k, uint32_t va)
{
    uint8_t b[32];
    unsigned n = 0;
    b[n++] = 0x83, b[n++] = 0x78, b[n++] = 0x10, b[n++] = 0x00;
    b[n++] = 0x74, b[n++] = 6; /* je out */
    b[n++] = 0xCD, b[n++] = NC_KCALL_VECTOR;
    b[n++] = NC_KORDINAL_APU_SPIN & 0xFF, b[n++] = NC_KORDINAL_APU_SPIN >> 8;
    b[n++] = 0xEB, b[n++] = (uint8_t)-12; /* jmp L (from offset 12) */
    const unsigned out = n;                /* 12: the je's target */
    const uint32_t stub = nc_kernel_code_alloc(k, out + 5);
    if (!stub) {
        return 0;
    }
    b[n++] = 0xE9;
    const uint32_t rel = (va + 6) - (stub + out + 5);
    memcpy(b + n, &rel, 4);
    n += 4;
    memcpy(nc_vm_host(k->vm, stub), b, n);
    return stub;
}

static bool section_mapped(const NcVm *vm, uint32_t va, uint32_t len)
{
    for (uint32_t p = va & ~(NC_PAGE - 1); p < va + len; p += NC_PAGE) {
        if (nc_vm_phys(vm, p) == UINT32_MAX) {
            return false;
        }
    }
    return true;
}

void nc_kernel_title_fixups(NcKernel *k, const HleXbeInfo *xbe)
{
    for (unsigned i = 0; i < xbe->nsections; i++) {
        const HleXbeSection *s = &xbe->sections[i];
        if (s->vsize && section_mapped(k->vm, s->va, s->vsize)) {
            nc_kernel_section_fixups(k, s->va, s->vsize, s->name);
        }
    }
    if (!apu_spin_on()) {
        nc_log("xbased-native", "fixups: DirectSound mailbox waits left spinning (debug.xbased.apuspin=0)");
        return;
    }
    unsigned patched = 0;
    for (unsigned i = 0; i < xbe->nsections; i++) {
        const HleXbeSection *s = &xbe->sections[i];
        if (!s->vsize || !section_mapped(k->vm, s->va, s->vsize)) {
            continue; /* not loaded at boot */
        }
        const uint8_t *base = nc_vm_host(k->vm, s->va);
        const uint8_t *p = base, *end = base + s->vsize;
        while ((p = memmem(p, (size_t)(end - p), k_mailbox_wait, sizeof(k_mailbox_wait))) != NULL) {
            const uint32_t va = s->va + (uint32_t)(p - base) + LOOP_OFF;
            const uint32_t stub = mailbox_stub(k, va);
            if (stub) {
                uint8_t *site = nc_vm_host(k->vm, va);
                const uint32_t rel = stub - (va + 5);
                site[0] = 0xE9;
                memcpy(site + 1, &rel, 4);
                site[5] = 0x90;
                patched++;
                nc_log("xbased-native", "fixups: DirectSound mailbox wait at %08x (%s) waits on the APU (stub %08x)",
                       va, s->name, stub);
            }
            p += sizeof(k_mailbox_wait);
        }
    }
    if (!patched) {
        nc_log("xbased-native", "fixups: no DirectSound mailbox wait found");
    }
}

/* XMV's decoder close waits for each of its audio streams to stop before it
 * frees the decoder, where its packets' pdwCompletedSize and pdwStatus are:
 *
 *     call [ecx+0Ch]               ; IDirectSoundStream_GetStatus
 *     test byte [ebp+0Ah], 1       ; DSSTREAMSTATUS_PLAYING
 *     je next
 *     push 1
 *     call Sleep
 *     call DirectSoundDoWork       ; the flushed packets complete here
 *     dec edi                      ; the same stream again
 *   next:
 *     inc edi
 *
 * It counts on the stream still playing at its first look: the console's APU
 * releases a voice turned off at its next frame, 0.67 ms on, and the CPU gets
 * from the flush to here in microseconds. Here the guest can take
 * milliseconds (the code that skips a movie compiled as it first runs), the
 * release's DPC came first, GetStatus said stopped, DirectSoundDoWork never
 * ran and the decoder was freed with the flushed packets still to complete;
 * their completion in the stream's last Release wrote into the freed heap
 * (Soldier of Fortune II, an intro movie skipped). The je goes to a stub that
 * runs DirectSoundDoWork before the next stream: a stopped stream's flushed
 * packets complete there, as when the first look saw it playing. Patched as
 * the code loads: at boot and in XeLoadSection (XMV is a section of its own
 * in some titles, loaded for a movie). */
static const uint8_t k_xmv_wait[] = {0xFF, 0x51, 0x0C,              /* call [ecx+0Ch] */
                                     0xF6, 0x45, 0x0A, 0x01,        /* test byte [ebp+0Ah], 1 */
                                     0x74, 0x0D,                    /* je next */
                                     0x6A, 0x01,                    /* push 1 */
                                     0xE8};                         /* call Sleep */
#define XMV_JE      7   /* the je, patched with a jmp to the stub */
#define XMV_SLEEP   11  /* call Sleep (rel32) */
#define XMV_DOWORK  16  /* call DirectSoundDoWork (rel32) */
#define XMV_DEC     21  /* dec edi: the same stream again */
#define XMV_NEXT    22  /* inc edi: the next stream */

#define XMV_STUB    (2 + 2 + 5 + 5 + 5 + 5 + 5)

/* The stubs made, by the je's VA: a section loaded again reuses its own,
 * when its bytes are still the ones it would get (a savestate's load brings
 * the kernel area of the run that saved it). */
static struct {
    uint32_t site, stub;
} g_xmv_stubs[8];

static uint32_t rel_target(NcKernel *k, uint32_t call)
{
    return call + 5 + nc_vm_rd32(k->vm, call + 1);
}

static void put_rel(uint8_t *b, unsigned *n, uint8_t op, uint32_t from, uint32_t to)
{
    b[(*n)++] = op;
    const uint32_t rel = to - (from + *n + 4);
    memcpy(b + *n, &rel, 4);
    *n += 4;
}

/* The stub for the loop at m (the call [ecx+0Ch]) at stub; the flags of the test still set:
 *   je np ; push 1 ; call Sleep ; call DirectSoundDoWork ; jmp m+21 (dec edi)
 *   np: call DirectSoundDoWork ; jmp m+22 (inc edi) */
static void xmv_stub_bytes(NcKernel *k, uint32_t m, uint32_t stub, uint8_t b[XMV_STUB])
{
    const uint32_t sleep = rel_target(k, m + XMV_SLEEP), dowork = rel_target(k, m + XMV_DOWORK);
    unsigned n = 0;
    b[n++] = 0x74, b[n++] = 2 + 5 + 5 + 5; /* je np */
    b[n++] = 0x6A, b[n++] = 0x01;          /* push 1 */
    put_rel(b, &n, 0xE8, stub, sleep);
    put_rel(b, &n, 0xE8, stub, dowork);
    put_rel(b, &n, 0xE9, stub, m + XMV_DEC);
    put_rel(b, &n, 0xE8, stub, dowork);    /* np: */
    put_rel(b, &n, 0xE9, stub, m + XMV_NEXT);
}

static uint32_t xmv_stub(NcKernel *k, uint32_t m)
{
    const uint32_t site = m + XMV_JE;
    const unsigned slots = sizeof(g_xmv_stubs) / sizeof(g_xmv_stubs[0]);
    uint8_t b[XMV_STUB];
    unsigned slot = slots;
    for (unsigned i = 0; i < slots && slot == slots; i++) {
        if (g_xmv_stubs[i].site == site) {
            slot = i;
            xmv_stub_bytes(k, m, g_xmv_stubs[i].stub, b);
            if (!memcmp(nc_vm_host(k->vm, g_xmv_stubs[i].stub), b, XMV_STUB)) {
                return g_xmv_stubs[i].stub;
            }
        } else if (!g_xmv_stubs[i].site) {
            slot = i;
        }
    }
    const uint32_t stub = nc_kernel_code_alloc(k, XMV_STUB);
    if (!stub) {
        nc_log("xbased-native", "fixups: XMV's stream wait at %08x left as it is (no room for its stub)", m);
        return 0;
    }
    xmv_stub_bytes(k, m, stub, b);
    memcpy(nc_vm_host(k->vm, stub), b, XMV_STUB);
    if (slot < slots) {
        g_xmv_stubs[slot].site = site;
        g_xmv_stubs[slot].stub = stub;
    }
    return stub;
}

static bool xmv_wait_on(void)
{
    char v[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.xbased.xmvwait", v);
    return strcmp(v, "0") != 0;
}

/* D3DDevice_SetTimerCallback(time, callback, context) refuses a time the
 * TSC has passed (D3DERR_TIME_IN_PAST) and schedules nothing:
 *
 *     rdtsc ; add edi(eax), 40 ; adc ...      ; now + 40 ticks
 *     cmp esi, ecx ; ja ok ; jb past ; cmp ecx, edi ; ja ok
 *   past:
 *     mov cl, bl ; call [KfLowerIrql] ; pop edi ; pop esi ; pop ebp
 *     mov eax, 8876082Ah ; pop ebx ; ret 16
 *   ok:
 *     sub ecx, eax ; sbb esi, edx ; mov edi, ecx ; mov eax, [esp+32] ...
 *
 * A title's periodic callback that schedules its next time from the last
 * (Def Jam: Fight for NY's 60 Hz input timer, its main loop's only update
 * source) stops for good when one call runs a period late: the console's
 * DPC is never that late, the emulation can be (the guest's CPU held up by
 * the host while its time runs on). Its game then never updated again: the
 * intro's movie never ended. The refusal becomes a delay of 0 ticks: the
 * callback comes at once and the title catches up. Patched as the code
 * loads; debug.xbased.d3dtimer=0 leaves it as it is. */
static const uint8_t k_timer_past[] = {0x0F, 0x31, 0x8B, 0xF8, 0x83, 0xC7, 0x28, 0x8B, 0xCA, 0x13, 0xCE,
                                       0x8B, 0x74, 0x24, 0x18, 0x3B, 0xF1, 0x8B, 0x4C, 0x24, 0x14,
                                       0x77, 0x1A, 0x72, 0x04, 0x3B, 0xCF, 0x77, 0x14,
                                       0x8A, 0xCB, 0xFF, 0x15};
static const uint8_t k_timer_past_tail[] = {0x5F, 0x5E, 0x5D, 0xB8, 0x2A, 0x08, 0x76, 0x88, 0x5B, 0xC2, 0x10, 0x00,
                                            0x2B, 0xC8, 0x1B, 0xF2, 0x8B, 0xF9, 0x8B, 0x44, 0x24, 0x20};
#define TIMER_PAST     0x1D  /* past: mov cl, bl */
#define TIMER_TAIL     0x25  /* after the call [KfLowerIrql] */
#define TIMER_PUSH     0x37  /* ok's mov eax, [esp+32]: the arguments pushed */

static bool d3d_timer_on(void)
{
    char v[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.xbased.d3dtimer", v);
    return strcmp(v, "0") != 0;
}

/* past: xor edi, edi ; xor esi, esi ; jmp ok's push (0 ticks from now) */
static unsigned timer_fixups(NcKernel *k, uint32_t va, uint32_t len, const char *name)
{
    if (!d3d_timer_on()) {
        return 0;
    }
    unsigned patched = 0;
    uint8_t *base = nc_vm_host(k->vm, va), *end = base + len, *p = base;
    while ((p = memmem(p, (size_t)(end - p), k_timer_past, sizeof(k_timer_past))) != NULL) {
        if (end - p >= TIMER_TAIL + (long)sizeof(k_timer_past_tail) &&
            !memcmp(p + TIMER_TAIL, k_timer_past_tail, sizeof(k_timer_past_tail))) {
            uint8_t *site = p + TIMER_PAST;
            site[0] = 0x31, site[1] = 0xFF;                     /* xor edi, edi */
            site[2] = 0x31, site[3] = 0xF6;                     /* xor esi, esi */
            site[4] = 0xEB, site[5] = TIMER_PUSH - (TIMER_PAST + 6); /* jmp ok's push */
            patched++;
            nc_log("xbased-native", "fixups: D3D timer callback at %08x (%s) comes at once when its time "
                   "has passed", va + (uint32_t)(p - base), name);
        }
        p += sizeof(k_timer_past);
    }
    return patched;
}

/* The XMV stream waits in [va, va + len): how many were patched. */
static unsigned xmv_fixups(NcKernel *k, uint32_t va, uint32_t len, const char *name)
{
    if (!xmv_wait_on()) {
        return 0;                       /* debug.xbased.xmvwait=0: XMV as it is (A/B) */
    }
    unsigned patched = 0;
    const uint8_t *base = nc_vm_host(k->vm, va), *end = base + len, *p = base;
    while ((p = memmem(p, (size_t)(end - p), k_xmv_wait, sizeof(k_xmv_wait))) != NULL) {
        const uint32_t m = va + (uint32_t)(p - base);
        if (end - p > XMV_NEXT && p[XMV_DOWORK] == 0xE8 && p[XMV_DEC] == 0x4F && p[XMV_NEXT] == 0x47) {
            const uint32_t stub = xmv_stub(k, m);
            if (stub) {
                uint8_t *site = nc_vm_host(k->vm, m + XMV_JE);
                const uint32_t rel = stub - (m + XMV_JE + 5);
                site[0] = 0xE9;
                memcpy(site + 1, &rel, 4);
                memset(site + 5, 0x90, XMV_DEC - XMV_JE - 5);
                patched++;
                nc_log("xbased-native", "fixups: XMV's stream wait at %08x (%s) runs DirectSoundDoWork for a "
                       "stopped stream too (stub %08x)", m, name, stub);
            }
        }
        p += sizeof(k_xmv_wait);
    }
    return patched;
}

/* The library code patched as it loads, in [va, va + len): how many sites. */
static unsigned section_fixups(NcKernel *k, uint32_t va, uint32_t len, const char *name)
{
    return xmv_fixups(k, va, len, name) + timer_fixups(k, va, len, name);
}

void nc_kernel_section_fixups(NcKernel *k, uint32_t va, uint32_t len, const char *name)
{
    section_fixups(k, va, len, name);
}

/* After a savestate's load: the sections loaded then, as the run that saved
 * them had them (a build before a fixup, unpatched). */
void nc_kernel_loaded_fixups(NcKernel *k)
{
    const uint32_t base = k->xbe_base;
    const uint32_t nsec = nc_vm_rd32(k->vm, base + 0x11C), secs = nc_vm_rd32(k->vm, base + 0x120);
    for (uint32_t i = 0; i < nsec && i < 256; i++) {
        const uint32_t s = secs + i * 0x38;
        const uint32_t va = nc_vm_rd32(k->vm, s + 0x04), vsize = nc_vm_rd32(k->vm, s + 0x08);
        if (!nc_vm_rd32(k->vm, s + 0x18) || !vsize || !section_mapped(k->vm, va, vsize)) {
            continue;                   /* not loaded */
        }
        const uint32_t name = nc_vm_rd32(k->vm, s + 0x14);
        char nm[9] = "?";
        if (name && nc_vm_phys(k->vm, name) != UINT32_MAX) {
            snprintf(nm, sizeof(nm), "%s", (const char *)nc_vm_host(k->vm, name));
        }
        if (section_fixups(k, va, vsize, nm) && k->cpu) {
            nc_cpu_invalidate(k->cpu, va, vsize);
        }
    }
}

void nc_kernel_apu_spin(NcKernel *k, NcFrame *f)
{
    const uint32_t word = nc_frame_gpr(f, NC_EAX) + 0x10;
    if (nc_vm_phys(k->vm, word) != UINT32_MAX) {
        nc_dev_apu_spin_wait(nc_vm_host(k->vm, word));
    }
    k->apu_spins++;
    nc_frame_set_eip(f, nc_frame_eip(f) + 4); /* past the int: the stub checks again */
}
