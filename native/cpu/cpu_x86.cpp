/*
 * hakuX native core â€” the guest CPU on x86-64 hosts (Windows): a dynamic
 * translator from the title's x86-32 code to x86-64, behind the same
 * interface as the FEX embedding (cpu.h).
 *
 * The guest's 4 GiB live at host address 0 (core/vm_win.c), so a guest
 * address is a host address: translated instructions keep their guest
 * encoding, re-encoded for 64-bit mode with a 32-bit address size (0x67),
 * which wraps the effective address at 4 GiB as the guest's does. Guest
 * registers live in the host's: EAX-EDI in RAX-RDI, except ESP in R12 (the
 * host stack stays the host's). R15 points at the frame; R13, R14 and
 * R8-R11 are the translator's scratch. Flags are the host's own.
 *
 * What changes on the way: stack instructions (64-bit pushes, calls and
 * returns would move RSP by 8) become stores and loads through R12;
 * control flow ends a block and goes through the dispatcher or a chained
 * jump; FS-relative accesses add the KPCR's address (FS's base is fixed);
 * privileged and system instructions, the kernel calls' INT 0x3F and hooked
 * addresses leave to the core's system-op callback, as FEX's system mode
 * did; RDTSC runs on the host's TSC scaled to the Xbox's 733 MHz.
 *
 * Interrupt requests from other threads protect a poll page that every
 * block reads at its start: the fault (vectored exception handler) turns
 * the block's start into an NC_SYSOP_INTERRUPT exit. The x87 runs native,
 * at the precision the title's control word selects, so FEX's reduced-x87
 * switches have nothing to do here.
 */
#include "native/cpu/cpu.h"
#include "native/cpu/x64asm.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include "nc_winapi.h"  /* compat/linux: the Windows APIs used here, over POSIX */
#endif

#include <Zydis/Zydis.h>

#include <algorithm>
#include <string>
#include <type_traits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace x64;

/* ------------------------------------------------------------ the frame */

struct alignas(64) NcFrame {
    uint64_t gregs[16];      /* x86 order; [4] is ESP */
    uint64_t rip;
    uint64_t eflags;         /* the host's RFLAGS as the guest left them (IF: vif) */
    uint32_t vif;            /* the guest's interrupt flag */
    uint32_t interrupt_pending;
    uint32_t hook_bypass;
    uint32_t sysop_info;     /* op | size << 8 */
    uint64_t host_rsp;       /* the translated code's host stack pointer */
    uint64_t next_code;      /* where the restore stub goes */
    uint64_t cycle_offset;   /* host TSC ticks the guest's RDTSC leaves out */
    uint64_t tsc_ratio;      /* guest cycles per host tick, Q32 */
    uint32_t host_mxcsr;
    uint32_t x87_load;       /* fx's x87 part goes to the hardware at the next restore (1: another
                              * thread's or a fault's, the return predictions go too; 2: put aside
                              * for an interrupt's host code, they stay) */
    NcCpu *cpu;
    uint32_t shadow_reset;   /* the return predictions go at the next restore (the code they point into went) */
    uint32_t vac;            /* the guest's AC flag (the host's would check alignment in ring 3) */
    uint64_t pad1;
    alignas(16) uint8_t fx[512]; /* FXSAVE image: x87/MMX, MXCSR, XMM */
};

static_assert(offsetof(NcFrame, fx) % 16 == 0, "FXSAVE needs 16-byte alignment");

/* The FXSAVE image's fields. */
enum {
    FX_FCW = 0,
    FX_FSW = 2,
    FX_FTW = 4,      /* abridged: one bit per register, 1 = valid */
    FX_MXCSR = 24,
    FX_ST = 32,      /* 8 x 16 bytes */
    FX_XMM = 160,    /* 16 x 16 bytes (the guest has 8) */
};

/* A guest thread's state (NcCpuContext's blob). */
struct SavedState {
    uint64_t gregs[8];
    uint64_t rip;
    uint32_t eflags;         /* with IF */
    uint32_t pad;
    alignas(16) uint8_t fx[512];
};
static_assert(sizeof(SavedState) <= sizeof(NcCpuContext), "context blob too small");

static SavedState &Saved(NcCpuContext *c)
{
    return *reinterpret_cast<SavedState *>(c->blob);
}
static const SavedState &Saved(const NcCpuContext *c)
{
    return *reinterpret_cast<const SavedState *>(c->blob);
}

/* The host's RFLAGS bits a guest value may set (IF is virtual; TF, AC, NT,
 * IOPL, RF and VM stay the host's). */
#define GUEST_FLAGS 0x00200DD5u /* ID | OF DF | SF ZF AF PF CF */
#define GUEST_AC    0x00040000u /* AC: kept in the frame (vac), never in the host's flags */

/* ------------------------------------------------------ the translation */

struct InsnMap {
    uint32_t host_off;       /* from the block's code */
    uint32_t eip;
    uint8_t swap;            /* the low register R12 was exchanged with (1-7), 0 none */
    uint8_t borrow;          /* 1 + the low register that holds an FS operand's address, its own value in R13 */
};

struct Exit {
    uint8_t *rel32;          /* the jump to patch */
    uint8_t *stub;           /* its unchained target: mov r13d, target; jmp dispatch */
    uint32_t target;
};

struct Block {
    uint32_t eip, end;       /* guest [eip, end) */
    uint8_t *code, *code_end;
    bool bypass;             /* the hook at eip is skipped (nc_frame_bypass_hook) */
    bool dead;
#ifdef NC_CPU_DIAG
    std::vector<uint8_t> src; /* NC_SMC_CHECK: the guest bytes it was made from */
#endif
    std::vector<InsnMap> map;
    std::vector<Exit> exits;
};

struct L1Entry {
    uint32_t neg_eip;        /* -eip: lea ecx, [rcx + r13] is 0 on a hit */
    uint32_t pad;
    uint8_t *code;
};
#define L1_BITS 16
#define L1_SIZE (1u << L1_BITS)

#define REGION_POLL   0
#define REGION_L1     0x1000
#define REGION_STUBS  (REGION_L1 + L1_SIZE * sizeof(L1Entry))
#define STUBS_SIZE    0x10000
#define REGION_CODE   (REGION_STUBS + STUBS_SIZE)
#define CODE_SIZE     (192u << 20)
#define REGION_SIZE   (REGION_CODE + CODE_SIZE)

struct NcCpu {
    NcCpuConfig cfg {};
    uint32_t fs_base = 0;
    uint8_t *region = nullptr;
    volatile uint8_t *poll = nullptr;
    L1Entry *l1 = nullptr;
    uint8_t *code_begin = nullptr, *code_end = nullptr, *code_p = nullptr;
    /* stubs */
    uint8_t *s_enter = nullptr, *s_restore = nullptr, *s_exit = nullptr, *s_sysop = nullptr;
    uint8_t *s_lookup = nullptr, *s_dispatch = nullptr, *s_dispatch_rip = nullptr, *s_hlt = nullptr;
    uint8_t *s_ret_reset = nullptr;
    /* blocks */
    std::unordered_map<uint32_t, Block *> blocks;      /* by guest eip (not bypass) */
    std::unordered_map<uint32_t, Block *> bypass;      /* the bypass translations */
    std::unordered_map<uint32_t, std::vector<Block *>> pages; /* guest page -> blocks on it */
    /* a bit per guest page that has blocks: an invalidation of a range
     * without any (memory freed, nearly always) looks at a word per 64 pages */
    std::vector<uint64_t> page_bits = std::vector<uint64_t>(1u << 14);
    std::unordered_map<uint32_t, std::vector<std::pair<Block *, unsigned>>> incoming;
    std::vector<Block *> by_host;                       /* in code order */
    std::vector<Block *> graveyard;                     /* dead, freed at the next flush */
    NcFrame *frame = nullptr;
    DWORD thread_id = 0;
    volatile LONG poll_armed = 0;
    PVOID veh = nullptr;
    uint64_t translated = 0, flushes = 0;
    ZydisDecoder dec {};
};

static NcCpu *g_cpu;
/* Diagnostics, in builds made with NC_CPU_DIAG only (the tests' cputest; the
 * app with -DHAKUX_CPU_DIAG=ON): a release has none of it.
 * NC_SMC_CHECK=1: no chaining and no dispatcher table, so every block is
 * entered through the lookup, which compares the guest bytes it was made
 * from with what is there now, logs a block whose code changed without an
 * invalidation and makes it again. Slow (a title crawls), but it tells
 * whether a title rewrites code it has run, which nothing else here does. */
#ifdef NC_CPU_DIAG
static bool g_smc_check;
#else
#define g_smc_check false
#endif
static volatile LONG64 g_poll_faults, g_poll_lost, g_int_requests, g_int_sysops;

/* ------------------------------------------------------ host utilities */

static uint64_t rdtsc_now()
{
    unsigned lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* The host TSC's rate, measured against QueryPerformanceCounter. */
static uint64_t tsc_hz()
{
    static uint64_t hz;
    if (hz) {
        return hz;
    }
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    const uint64_t t0 = rdtsc_now();
    do {
        QueryPerformanceCounter(&b);
    } while ((uint64_t)(b.QuadPart - a.QuadPart) < (uint64_t)f.QuadPart / 50); /* 20 ms */
    const uint64_t t1 = rdtsc_now();
    hz = (uint64_t)((double)(t1 - t0) * (double)f.QuadPart / (double)(b.QuadPart - a.QuadPart));
    return hz;
}

static uint64_t tsc_ratio_q32()
{
    return (uint64_t)(((unsigned __int128)733333333u << 32) / tsc_hz());
}

/* ----------------------------------------------------- register mapping */

static const ZydisRegister k_r32[8] = {ZYDIS_REGISTER_EAX, ZYDIS_REGISTER_ECX, ZYDIS_REGISTER_EDX,
                                       ZYDIS_REGISTER_EBX, ZYDIS_REGISTER_R12D, ZYDIS_REGISTER_EBP,
                                       ZYDIS_REGISTER_ESI, ZYDIS_REGISTER_EDI};
static const ZydisRegister k_r64[8] = {ZYDIS_REGISTER_RAX, ZYDIS_REGISTER_RCX, ZYDIS_REGISTER_RDX,
                                       ZYDIS_REGISTER_RBX, ZYDIS_REGISTER_R12, ZYDIS_REGISTER_RBP,
                                       ZYDIS_REGISTER_RSI, ZYDIS_REGISTER_RDI};
static const ZydisRegister k_r16[8] = {ZYDIS_REGISTER_AX, ZYDIS_REGISTER_CX, ZYDIS_REGISTER_DX,
                                       ZYDIS_REGISTER_BX, ZYDIS_REGISTER_R12W, ZYDIS_REGISTER_BP,
                                       ZYDIS_REGISTER_SI, ZYDIS_REGISTER_DI};

#define FR(field) M(ZYDIS_REGISTER_R15, (int64_t)offsetof(NcFrame, field), 0)
#define FR8(field, add) M(ZYDIS_REGISTER_R15, (int64_t)offsetof(NcFrame, field) + (add), 8)
#define FR4(field, add) M(ZYDIS_REGISTER_R15, (int64_t)offsetof(NcFrame, field) + (add), 4)

static bool is_high8(ZydisRegister r)
{
    return r == ZYDIS_REGISTER_AH || r == ZYDIS_REGISTER_CH || r == ZYDIS_REGISTER_DH || r == ZYDIS_REGISTER_BH;
}

/* ESP's forms to R12's; anything else as it is. */
/* 0F xx: an MMX instruction on a Pentium III (MMX, and SSE's integer ones on
 * MMX registers), which a 66 prefix would make SSE2's elsewhere */
static bool nc_x86_mmx_opcode(uint8_t op)
{
    switch (op) {
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6E: case 0x6F:
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x7E: case 0x7F:
    case 0xC4: case 0xC5:
    case 0xD1: case 0xD2: case 0xD3: case 0xD5: case 0xD7: case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF:
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE7: case 0xE8: case 0xE9:
    case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE: case 0xEF:
    case 0xF1: case 0xF2: case 0xF3: case 0xF5: case 0xF6: case 0xF7: case 0xF8: case 0xF9: case 0xFA:
    case 0xFC: case 0xFD: case 0xFE:
        return true;
    default:
        return false;
    }
}

static ZydisRegister remap(ZydisRegister r, ZydisRegister esp32 = ZYDIS_REGISTER_R12D)
{
    if (r == ZYDIS_REGISTER_ESP) {
        return esp32;
    }
    if (r == ZYDIS_REGISTER_SP) {
        return esp32 == ZYDIS_REGISTER_R12D ? ZYDIS_REGISTER_R12W
                                            : (ZydisRegister)(ZYDIS_REGISTER_AX + (esp32 - ZYDIS_REGISTER_EAX));
    }
    return r;
}

/* ------------------------------------------------------------- stubs */

/* The guest's registers into the frame on the way to host code: the GPRs,
 * the flags, XMM0-7 and MXCSR into the FXSAVE image's places. The x87/MMX
 * registers stay in the hardware (x64 host code never touches them): what
 * needs them (a thread switch, nc_frame_save) reads them there, and a frame
 * loaded with another's sets x87_load for the restore. A whole FXSAVE and
 * FXRSTOR with an FNINIT cost ~400 cycles a system op, a quarter of a
 * kernel call. */
static void save_state(Asm &a)
{
    for (int i = 0; i < 8; i++) {
        a.op(ZYDIS_MNEMONIC_MOV, {FR8(gregs, i * 8), R(k_r64[i])});
    }
    a.op(ZYDIS_MNEMONIC_PUSHFQ, {});
    a.op(ZYDIS_MNEMONIC_POP, {FR8(eflags, 0)});
    for (int i = 0; i < 8; i++) {
        a.op(ZYDIS_MNEMONIC_MOVDQA, {M(ZYDIS_REGISTER_R15, (int64_t)offsetof(NcFrame, fx) + FX_XMM + 16 * i, 16),
                                     R((ZydisRegister)(ZYDIS_REGISTER_XMM0 + i))});
    }
    a.op(ZYDIS_MNEMONIC_STMXCSR, {M(ZYDIS_REGISTER_R15, (int64_t)offsetof(NcFrame, fx) + FX_MXCSR, 4)});
}

/* Host C code runs with the host's MXCSR and DF clear. */
static void host_env(Asm &a)
{
    a.op(ZYDIS_MNEMONIC_LDMXCSR, {FR4(host_mxcsr, 0)});
    a.op(ZYDIS_MNEMONIC_CLD, {});
}

static void call_abs(Asm &a, const void *fn)
{
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RAX), I((int64_t)(uintptr_t)fn)});
    a.op(ZYDIS_MNEMONIC_CALL, {R(ZYDIS_REGISTER_RAX)});
}

extern "C" uint8_t *nc_x86_sysop_entry(NcFrame *f);
extern "C" uint8_t *nc_x86_lookup_entry(NcFrame *f);

static bool build_stubs(NcCpu *cpu)
{
    Asm a(cpu->region + REGION_STUBS, cpu->region + REGION_STUBS + STUBS_SIZE);
    static const ZydisRegister callee[8] = {ZYDIS_REGISTER_RBX, ZYDIS_REGISTER_RBP, ZYDIS_REGISTER_RDI,
                                            ZYDIS_REGISTER_RSI, ZYDIS_REGISTER_R12, ZYDIS_REGISTER_R13,
                                            ZYDIS_REGISTER_R14, ZYDIS_REGISTER_R15};
    const int frame_alloc = 200; /* xmm6-15 at +32, shadow space at +0; RSP 16-aligned */

    /* restore: x87/SSE, GPRs, flags from the frame, then to next_code */
    a.p = (uint8_t *)(((uintptr_t)a.p + 63) & ~(uintptr_t)63);
    cpu->s_restore = a.p;
    {
        /* the x87 registers only when the frame got another's (a switch, a fault's) */
        a.op(ZYDIS_MNEMONIC_CMP, {FR4(x87_load, 0), I(0)});
        uint8_t *skip = a.jcc(0x4, nullptr); /* jz */
        a.op(ZYDIS_MNEMONIC_FXRSTOR, {M(ZYDIS_REGISTER_R15, offsetof(NcFrame, fx), 512)});
        a.op(ZYDIS_MNEMONIC_CMP, {FR4(x87_load, 0), I(2)});
        a.op(ZYDIS_MNEMONIC_MOV, {FR4(x87_load, 0), I(0)}); /* the flags stay the comparison's */
        uint8_t *same = a.jcc(0x4, nullptr); /* je: the same thread's, back from an interrupt */
        /* another thread's frame: the return predictions are the last one's */
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RSP), FR8(host_rsp, 0)});
        patch_rel32(same, a.p);
        patch_rel32(skip, a.p);
    }
    {
        /* the translations went (flush_all): the predictions' host return
         * addresses point into code that is no longer there, and a guest
         * return that still matched one ran whatever was translated over it */
        a.op(ZYDIS_MNEMONIC_CMP, {FR4(shadow_reset, 0), I(0)});
        uint8_t *keep = a.jcc(0x4, nullptr); /* jz */
        a.op(ZYDIS_MNEMONIC_MOV, {FR4(shadow_reset, 0), I(0)});
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RSP), FR8(host_rsp, 0)});
        patch_rel32(keep, a.p);
    }
    {
        /* calls never returned from (call $+5; pop) pile up: 4 MB of them
         * (the thread has 16) and the shadow stack goes */
        a.op(ZYDIS_MNEMONIC_LEA, {R(ZYDIS_REGISTER_RAX), M(ZYDIS_REGISTER_RSP, 4 << 20, 8)});
        a.op(ZYDIS_MNEMONIC_CMP, {R(ZYDIS_REGISTER_RAX), FR8(host_rsp, 0)});
        uint8_t *ok = a.jcc(0x3, nullptr); /* jae */
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RSP), FR8(host_rsp, 0)});
        patch_rel32(ok, a.p);
    }
    for (int i = 0; i < 8; i++) {
        a.op(ZYDIS_MNEMONIC_MOVDQA, {R((ZydisRegister)(ZYDIS_REGISTER_XMM0 + i)),
                                     M(ZYDIS_REGISTER_R15, (int64_t)offsetof(NcFrame, fx) + FX_XMM + 16 * i, 16)});
    }
    a.op(ZYDIS_MNEMONIC_LDMXCSR, {M(ZYDIS_REGISTER_R15, (int64_t)offsetof(NcFrame, fx) + FX_MXCSR, 4)});
    for (int i = 0; i < 8; i++) {
        a.op(ZYDIS_MNEMONIC_MOV, {R(k_r64[i]), FR8(gregs, i * 8)});
    }
    a.op(ZYDIS_MNEMONIC_PUSH, {FR8(eflags, 0)});
    a.op(ZYDIS_MNEMONIC_POPFQ, {});
    a.op(ZYDIS_MNEMONIC_JMP, {FR8(next_code, 0)});

    /* ret_reset: a return predicted wrong (an empty shadow stack's sentinel
     * taken for an entry): the shadow stack empty, to the dispatcher */
    a.p = (uint8_t *)(((uintptr_t)a.p + 63) & ~(uintptr_t)63);
    cpu->s_ret_reset = a.p;
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RSP), FR8(host_rsp, 0)});
    uint8_t *to_dispatch = a.jmp(nullptr);

    /* enter(frame): the C side's way in (nc_cpu_run). host_rsp is the base
     * of the return predictions' shadow stack (Translator ret): a sentinel
     * pair there (s_ret_reset, 0) that no guest return matches but one to
     * address 0, and that one goes to s_ret_reset anyway. */
    a.p = (uint8_t *)(((uintptr_t)a.p + 63) & ~(uintptr_t)63);
    cpu->s_enter = a.p;
    for (ZydisRegister r : callee) {
        a.op(ZYDIS_MNEMONIC_PUSH, {R(r)});
    }
    a.op(ZYDIS_MNEMONIC_SUB, {R(ZYDIS_REGISTER_RSP), I(frame_alloc)});
    for (int i = 0; i < 10; i++) {
        a.op(ZYDIS_MNEMONIC_MOVAPS, {M(ZYDIS_REGISTER_RSP, 32 + 16 * i, 16), R((ZydisRegister)(ZYDIS_REGISTER_XMM6 + i))});
    }
#ifdef _WIN32
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R15), R(ZYDIS_REGISTER_RCX)});
#else
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R15), R(ZYDIS_REGISTER_RDI)});
#endif
    a.op(ZYDIS_MNEMONIC_SUB, {R(ZYDIS_REGISTER_RSP), I(16)});
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RAX), I((int64_t)(uintptr_t)cpu->s_ret_reset)});
    a.op(ZYDIS_MNEMONIC_MOV, {M(ZYDIS_REGISTER_RSP, 0, 8), R(ZYDIS_REGISTER_RAX)});
    a.op(ZYDIS_MNEMONIC_MOV, {M(ZYDIS_REGISTER_RSP, 8, 8), I(0)});
    a.op(ZYDIS_MNEMONIC_MOV, {FR8(host_rsp, 0), R(ZYDIS_REGISTER_RSP)});
    a.jmp(cpu->s_restore);

    /* exit: back to enter's caller (the frame already saved) */
    a.p = (uint8_t *)(((uintptr_t)a.p + 63) & ~(uintptr_t)63);
    cpu->s_exit = a.p;
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RSP), FR8(host_rsp, 0)});
    a.op(ZYDIS_MNEMONIC_ADD, {R(ZYDIS_REGISTER_RSP), I(16)});
    for (int i = 0; i < 10; i++) {
        a.op(ZYDIS_MNEMONIC_MOVAPS, {R((ZydisRegister)(ZYDIS_REGISTER_XMM6 + i)), M(ZYDIS_REGISTER_RSP, 32 + 16 * i, 16)});
    }
    a.op(ZYDIS_MNEMONIC_ADD, {R(ZYDIS_REGISTER_RSP), I(frame_alloc)});
    for (int i = 7; i >= 0; i--) {
        a.op(ZYDIS_MNEMONIC_POP, {R(callee[i])});
    }
    a.op(ZYDIS_MNEMONIC_RET, {});

    /* hlt: the guest stopped (rip stored): save it all and leave */
    a.p = (uint8_t *)(((uintptr_t)a.p + 63) & ~(uintptr_t)63);
    cpu->s_hlt = a.p;
    save_state(a);
    a.op(ZYDIS_MNEMONIC_FXSAVE, {M(ZYDIS_REGISTER_R15, offsetof(NcFrame, fx), 512)});
    host_env(a);
    a.jmp(cpu->s_exit);

    /* sysop: rip and sysop_info stored; the core's callback, then wherever
     * the frame's EIP is (or out, for a null next) */
    a.p = (uint8_t *)(((uintptr_t)a.p + 63) & ~(uintptr_t)63);
    cpu->s_sysop = a.p;
    save_state(a);
    host_env(a);
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RCX), R(ZYDIS_REGISTER_R15)});
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RDI), R(ZYDIS_REGISTER_R15)}); /* System V's first argument */
    a.op(ZYDIS_MNEMONIC_SUB, {R(ZYDIS_REGISTER_RSP), I(32)}); /* the callee's home space, below the predictions */
    call_abs(a, (const void *)nc_x86_sysop_entry);
    a.op(ZYDIS_MNEMONIC_ADD, {R(ZYDIS_REGISTER_RSP), I(32)});
    a.op(ZYDIS_MNEMONIC_TEST, {R(ZYDIS_REGISTER_RAX), R(ZYDIS_REGISTER_RAX)});
    a.jcc(0x4, cpu->s_exit); /* jz */
    a.op(ZYDIS_MNEMONIC_MOV, {FR8(next_code, 0), R(ZYDIS_REGISTER_RAX)});
    a.jmp(cpu->s_restore);

    /* lookup: a dispatcher miss, the guest's EIP in r13d */
    a.p = (uint8_t *)(((uintptr_t)a.p + 63) & ~(uintptr_t)63);
    cpu->s_lookup = a.p;
    a.op(ZYDIS_MNEMONIC_MOV, {FR4(rip, 0), R(ZYDIS_REGISTER_R13D)});
    save_state(a);
    host_env(a);
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RCX), R(ZYDIS_REGISTER_R15)});
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RDI), R(ZYDIS_REGISTER_R15)});
    a.op(ZYDIS_MNEMONIC_SUB, {R(ZYDIS_REGISTER_RSP), I(32)});
    call_abs(a, (const void *)nc_x86_lookup_entry);
    a.op(ZYDIS_MNEMONIC_ADD, {R(ZYDIS_REGISTER_RSP), I(32)});
    a.op(ZYDIS_MNEMONIC_MOV, {FR8(next_code, 0), R(ZYDIS_REGISTER_RAX)});
    a.jmp(cpu->s_restore);

    /* dispatch: the guest's EIP in r13d, flags live. One probe of the L1
     * table without touching the flags: the entry holds -eip, and
     * lea ecx, [rcx + r13] is zero on a hit (JRCXZ). */
    a.p = (uint8_t *)(((uintptr_t)a.p + 63) & ~(uintptr_t)63);
    cpu->s_dispatch = a.p;
    patch_rel32(to_dispatch, cpu->s_dispatch);
    a.op(ZYDIS_MNEMONIC_MOVZX, {R(ZYDIS_REGISTER_R11D), R(ZYDIS_REGISTER_R13W)});
    a.op(ZYDIS_MNEMONIC_LEA, {R(ZYDIS_REGISTER_R14), A(cpu->l1, 0)});
    a.op(ZYDIS_MNEMONIC_LEA, {R(ZYDIS_REGISTER_R11), M(ZYDIS_REGISTER_NONE, 0, 0, ZYDIS_REGISTER_R11, 8)});
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R10), R(ZYDIS_REGISTER_RCX)});
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_ECX), M(ZYDIS_REGISTER_R14, 0, 4, ZYDIS_REGISTER_R11, 2)});
    a.op(ZYDIS_MNEMONIC_LEA, {R(ZYDIS_REGISTER_ECX), M(ZYDIS_REGISTER_RCX, 0, 0, ZYDIS_REGISTER_R13, 1)});
    a.byte(0xE3); /* jrcxz hit */
    uint8_t *jr = a.p;
    a.byte(0);
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RCX), R(ZYDIS_REGISTER_R10)});
    a.jmp(cpu->s_lookup);
    *jr = (uint8_t)(a.p - (jr + 1));
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RCX), R(ZYDIS_REGISTER_R10)});
    a.op(ZYDIS_MNEMONIC_JMP, {M(ZYDIS_REGISTER_R14, 8, 8, ZYDIS_REGISTER_R11, 2)});

    /* dispatch_rip: the frame's EIP (a fault's resume) */
    a.p = (uint8_t *)(((uintptr_t)a.p + 63) & ~(uintptr_t)63);
    cpu->s_dispatch_rip = a.p;
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R13D), FR4(rip, 0)});
    a.jmp(cpu->s_dispatch);

    if (a.failed) {
        nc_log("hakuX-native", "cpu: the stubs don't assemble");
        return false;
    }
    return true;
}

/* -------------------------------------------------------- the L1 table */

static void l1_set(NcCpu *cpu, uint32_t eip, uint8_t *code)
{
    if (g_smc_check) {
        return;
    }
    L1Entry &e = cpu->l1[eip & (L1_SIZE - 1)];
    e.neg_eip = (uint32_t)-eip;
    e.code = code;
}

static void l1_drop(NcCpu *cpu, uint32_t eip)
{
    L1Entry &e = cpu->l1[eip & (L1_SIZE - 1)];
    if (e.neg_eip == (uint32_t)-eip) {
        e.neg_eip = 0; /* eip 0 never runs: a miss */
        e.code = cpu->s_lookup;
    }
}

static void l1_clear(NcCpu *cpu)
{
    for (unsigned i = 0; i < L1_SIZE; i++) {
        cpu->l1[i].neg_eip = 0;
        cpu->l1[i].code = cpu->s_lookup;
    }
}

/* -------------------------------------------------------- translation */

/* A guest address the translator may read: committed memory. A page
 * without access (the HLE's) faults and is handled as the guest's read. */
/* The last page found committed: good for one translation (translate
 * forgets it as it starts), since the guest may unmap it afterwards. */
static thread_local uint64_t g_ok_page = UINT64_MAX;

static bool guest_readable(uint32_t va, uint32_t n)
{
    /* 64-bit: the last bytes under 4 GiB don't wrap to a range never checked */
    for (uint64_t p = va & ~0xFFFu; p < (uint64_t)va + n; p += 0x1000) {
        if (p == g_ok_page) {
            continue;
        }
        MEMORY_BASIC_INFORMATION mbi;
        if (p > 0xFFFFFFFFull || !VirtualQuery((void *)(uintptr_t)p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
            return false;
        }
        g_ok_page = p;
    }
    return true;
}

struct Translator {
    NcCpu *cpu;
    Asm a;
    Block *b;
    uint32_t eip;
    struct PendingExit {
        uint8_t *rel32;
        uint32_t target;
    };
    std::vector<PendingExit> exits;

    void exit_to(uint32_t target)
    {
        exits.push_back({a.jmp(cpu->s_lookup), target});
    }
    void exit_jcc(unsigned cc, uint32_t target)
    {
        exits.push_back({a.jcc(cc, cpu->s_lookup), target});
    }
    void dispatch_r13() { a.jmp(cpu->s_dispatch); }

    void sysop(uint32_t at, unsigned op, unsigned size)
    {
        a.op(ZYDIS_MNEMONIC_MOV, {FR4(rip, 0), I((int32_t)at)});
        a.op(ZYDIS_MNEMONIC_MOV, {FR4(sysop_info, 0), I((int32_t)(op | size << 8))});
        a.jmp(cpu->s_sysop);
    }

    /* push a 32-bit host register or immediate: the store first (a fault
     * leaves ESP as it was), then ESP */
    void push_r(ZydisRegister r32)
    {
        a.op(ZYDIS_MNEMONIC_MOV, {M(ZYDIS_REGISTER_R12D, -4, 4), R(r32)}, true);
        a.op(ZYDIS_MNEMONIC_LEA, {R(ZYDIS_REGISTER_R12D), M(ZYDIS_REGISTER_R12, -4, 0)});
    }
    void push_imm(uint32_t v)
    {
        a.op(ZYDIS_MNEMONIC_MOV, {M(ZYDIS_REGISTER_R12D, -4, 4), I((int32_t)v)}, true);
        a.op(ZYDIS_MNEMONIC_LEA, {R(ZYDIS_REGISTER_R12D), M(ZYDIS_REGISTER_R12, -4, 0)});
    }
    void esp_add(int32_t n)
    {
        a.op(ZYDIS_MNEMONIC_LEA, {R(ZYDIS_REGISTER_R12D), M(ZYDIS_REGISTER_R12, n, 0)});
    }

    /* A decoded memory operand as an encoder operand: ESP as R12, the
     * segment's base folded in (FS: the KPCR's; through r13d when the
     * operand has registers). False for what can't be done. */
    bool mem_operand(const ZydisDecodedOperand &o, ZydisEncoderOperand &out, int64_t extra = 0)
    {
        memset(&out, 0, sizeof(out));
        out.type = ZYDIS_OPERAND_TYPE_MEMORY;
        out.mem.base = remap(o.mem.base);
        out.mem.index = remap(o.mem.index);
        out.mem.scale = o.mem.index == ZYDIS_REGISTER_NONE ? 0 : o.mem.scale;
        out.mem.displacement = o.mem.disp.has_displacement ? o.mem.disp.value : 0;
        out.mem.displacement += extra;
        out.mem.size = (uint16_t)(o.size / 8);
        if (o.mem.segment == ZYDIS_REGISTER_FS && cpu->fs_base) {
            if (out.mem.base == ZYDIS_REGISTER_NONE && out.mem.index == ZYDIS_REGISTER_NONE) {
                out.mem.displacement = (int32_t)(uint32_t)((uint32_t)out.mem.displacement + cpu->fs_base);
            } else {
                ZydisEncoderOperand ea = out;
                ea.mem.size = 0;
                if (!a.op(ZYDIS_MNEMONIC_LEA, {R(ZYDIS_REGISTER_R13D), ea}, true)) {
                    return false;
                }
                out.mem.base = ZYDIS_REGISTER_R13D;
                out.mem.index = ZYDIS_REGISTER_NONE;
                out.mem.scale = 0;
                out.mem.displacement = (int32_t)cpu->fs_base;
            }
        }
        out.mem.displacement = (int32_t)(uint32_t)out.mem.displacement;
        return true;
    }

    /* r13d (or r13w, by size) = an operand (register, memory or immediate) */
    bool load_r13(const ZydisDecodedOperand &o, unsigned bytes)
    {
        const ZydisRegister dst = bytes == 2 ? ZYDIS_REGISTER_R13W : ZYDIS_REGISTER_R13D;
        if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            return a.op(ZYDIS_MNEMONIC_MOV, {R(dst), R(remap(o.reg.value))});
        }
        if (o.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            return a.op(ZYDIS_MNEMONIC_MOV, {R(dst), I((int32_t)o.imm.value.s)});
        }
        ZydisEncoderOperand m;
        return mem_operand(o, m) && a.op(ZYDIS_MNEMONIC_MOV, {R(dst), m}, true);
    }

    /* The instruction re-encoded for 64-bit mode (see the header). */
    bool generic(const ZydisDecodedInstruction &in, const ZydisDecodedOperand *ops)
    {
        ZydisEncoderRequest req;
        if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(&in, ops, in.operand_count_visible, &req))) {
            return false;
        }
        req.machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
        req.prefixes &= ~(ZYDIS_ATTRIB_HAS_SEGMENT_CS | ZYDIS_ATTRIB_HAS_SEGMENT_SS | ZYDIS_ATTRIB_HAS_SEGMENT_DS |
                          ZYDIS_ATTRIB_HAS_SEGMENT_ES | ZYDIS_ATTRIB_HAS_SEGMENT_FS | ZYDIS_ATTRIB_HAS_SEGMENT_GS |
                          ZYDIS_ATTRIB_HAS_BRANCH_NOT_TAKEN | ZYDIS_ATTRIB_HAS_BRANCH_TAKEN);
        bool mem = false, uses_esp = false, high8 = false;
        for (unsigned i = 0; i < in.operand_count; i++) {
            const ZydisDecodedOperand &o = ops[i];
            /* the stack used implicitly (an instruction the cases above
             * don't take): encoded as it is, it would push or pop the
             * host's RSP, the shadow stack the stubs and C calls run on */
            if (o.visibility == ZYDIS_OPERAND_VISIBILITY_HIDDEN &&
                ((o.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                  (o.mem.base == ZYDIS_REGISTER_ESP || o.mem.base == ZYDIS_REGISTER_SP)) ||
                 (o.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                  (o.reg.value == ZYDIS_REGISTER_ESP || o.reg.value == ZYDIS_REGISTER_SP)))) {
                return false;
            }
            if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                mem = true;
                if (o.mem.segment == ZYDIS_REGISTER_FS && o.visibility != ZYDIS_OPERAND_VISIBILITY_EXPLICIT &&
                    in.mnemonic != ZYDIS_MNEMONIC_LEA) {
                    return false; /* string instructions from FS */
                }
            }
        }
        req.address_size_hint = ZYDIS_ADDRESS_SIZE_HINT_32;
        for (unsigned i = 0; i < req.operand_count; i++) {
            ZydisEncoderOperand &eo = req.operands[i];
            const ZydisDecodedOperand &o = ops[i];
            if (eo.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                if (eo.reg.value == ZYDIS_REGISTER_ESP || eo.reg.value == ZYDIS_REGISTER_SP) {
                    uses_esp = true;
                }
                high8 |= is_high8(eo.reg.value);
            } else if (eo.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                if (o.mem.base == ZYDIS_REGISTER_ESP) {
                    uses_esp = true;
                }
            }
        }
        (void)mem;
        if (uses_esp && high8) {
            return generic_swapped(in, ops, req);
        }
        if (high8) {
            for (unsigned i = 0; i < req.operand_count; i++) {
                const ZydisDecodedOperand &o = ops[i];
                if (o.type == ZYDIS_OPERAND_TYPE_MEMORY && o.mem.segment == ZYDIS_REGISTER_FS && cpu->fs_base &&
                    in.mnemonic != ZYDIS_MNEMONIC_LEA &&
                    (o.mem.base != ZYDIS_REGISTER_NONE || o.mem.index != ZYDIS_REGISTER_NONE)) {
                    return generic_borrowed(in, ops, req);
                }
            }
        }
        for (unsigned i = 0; i < req.operand_count; i++) {
            ZydisEncoderOperand &eo = req.operands[i];
            const ZydisDecodedOperand &o = ops[i];
            if (eo.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                eo.reg.value = remap(eo.reg.value);
            } else if (eo.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                if (in.mnemonic == ZYDIS_MNEMONIC_LEA) {
                    ZydisDecodedOperand plain = o;
                    plain.mem.segment = ZYDIS_REGISTER_DS;
                    if (!mem_operand(plain, eo)) {
                        return false;
                    }
                    eo.mem.size = 4; /* the address's size (32) */
                } else if (!mem_operand(o, eo)) {
                    return false;
                }
                if (o.mem.segment == ZYDIS_REGISTER_FS && eo.mem.base == ZYDIS_REGISTER_R13D && high8) {
                    return false; /* FS through r13 with AH-DH: no encoding */
                }
            }
        }
        return a.encode(req);
    }

    /* A low register (EBP, ESI, EDI, EBX, EDX, ECX, EAX: its number) that
     * the instruction doesn't name, or -1. */
    int unnamed_low(const ZydisDecodedInstruction &in, const ZydisDecodedOperand *ops)
    {
        static const int order[] = {5, 6, 7, 3, 2, 1, 0};
        for (int c : order) {
            bool used = false;
            for (unsigned i = 0; i < in.operand_count && !used; i++) {
                const ZydisDecodedOperand &o = ops[i];
                auto names = [&](ZydisRegister r) {
                    return r != ZYDIS_REGISTER_NONE &&
                           ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, r) ==
                               (ZydisRegister)(ZYDIS_REGISTER_EAX + c);
                };
                used = (o.type == ZYDIS_OPERAND_TYPE_REGISTER && names(o.reg.value)) ||
                       (o.type == ZYDIS_OPERAND_TYPE_MEMORY && (names(o.mem.base) || names(o.mem.index)));
            }
            if (!used) {
                return c;
            }
        }
        return -1;
    }

    /* AH-DH (no REX) with an FS operand that has registers: its address
     * can't go through R13 (REX), so it borrows a low register the
     * instruction doesn't name, whose own value waits in R13 meanwhile (the
     * map's borrow, for a fault in between). It stopped the title before. */
    bool generic_borrowed(const ZydisDecodedInstruction &in, const ZydisDecodedOperand *ops, ZydisEncoderRequest &req)
    {
        const int t = unnamed_low(in, ops);
        if (t < 0) {
            return false;
        }
        const ZydisRegister t32 = (ZydisRegister)(ZYDIS_REGISTER_EAX + t);
        bool taken = false;
        for (unsigned i = 0; i < req.operand_count; i++) {
            ZydisEncoderOperand &eo = req.operands[i];
            const ZydisDecodedOperand &o = ops[i];
            if (eo.type != ZYDIS_OPERAND_TYPE_MEMORY) {
                continue;
            }
            if (o.mem.segment != ZYDIS_REGISTER_FS || taken) {
                return false; /* one memory operand, the FS one */
            }
            ZydisEncoderOperand ea;
            memset(&ea, 0, sizeof(ea));
            ea.type = ZYDIS_OPERAND_TYPE_MEMORY;
            ea.mem.base = o.mem.base;
            ea.mem.index = o.mem.index;
            ea.mem.scale = o.mem.index == ZYDIS_REGISTER_NONE ? 0 : o.mem.scale;
            ea.mem.displacement = o.mem.disp.has_displacement ? o.mem.disp.value : 0;
            a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R13), R(k_r64[t])});
            if (!a.op(ZYDIS_MNEMONIC_LEA, {R(t32), ea}, true)) {
                return false;
            }
            eo.mem.base = t32;
            eo.mem.index = ZYDIS_REGISTER_NONE;
            eo.mem.scale = 0;
            eo.mem.displacement = (int32_t)cpu->fs_base;
            taken = true;
        }
        if (!taken) {
            return false;
        }
        b->map.back().borrow = (uint8_t)(t + 1);
        const bool ok = a.encode(req);
        a.op(ZYDIS_MNEMONIC_MOV, {R(k_r64[t]), R(ZYDIS_REGISTER_R13)});
        return ok;
    }

    /* AH-DH (no REX) with ESP: ESP borrows a low register the instruction
     * doesn't name, exchanged with R12 around it. */
    bool generic_swapped(const ZydisDecodedInstruction &in, const ZydisDecodedOperand *ops, ZydisEncoderRequest &req)
    {
        static const int order[] = {5, 6, 7, 3, 2, 1, 0}; /* ebp esi edi ebx edx ecx eax */
        int t = -1;
        for (int c : order) {
            bool used = false;
            for (unsigned i = 0; i < in.operand_count && !used; i++) {
                const ZydisDecodedOperand &o = ops[i];
                auto names = [&](ZydisRegister r) {
                    return r != ZYDIS_REGISTER_NONE &&
                           ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, r) ==
                               (ZydisRegister)(ZYDIS_REGISTER_EAX + c);
                };
                used = (o.type == ZYDIS_OPERAND_TYPE_REGISTER && names(o.reg.value)) ||
                       (o.type == ZYDIS_OPERAND_TYPE_MEMORY && (names(o.mem.base) || names(o.mem.index)));
                if (c < 4 && o.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                    o.reg.value == (ZydisRegister)(ZYDIS_REGISTER_AH + c)) {
                    used = true;
                }
            }
            if (!used) {
                t = c;
                break;
            }
        }
        if (t < 0) {
            return false;
        }
        const ZydisRegister t32 = (ZydisRegister)(ZYDIS_REGISTER_EAX + t);
        for (unsigned i = 0; i < req.operand_count; i++) {
            ZydisEncoderOperand &eo = req.operands[i];
            const ZydisDecodedOperand &o = ops[i];
            if (eo.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                eo.reg.value = remap(eo.reg.value, t32);
            } else if (eo.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                if (o.mem.segment == ZYDIS_REGISTER_FS && (o.mem.base != ZYDIS_REGISTER_NONE ||
                                                           o.mem.index != ZYDIS_REGISTER_NONE)) {
                    return false;
                }
                eo.mem.base = remap(o.mem.base, t32);
                eo.mem.index = remap(o.mem.index, t32);
                if (o.mem.segment == ZYDIS_REGISTER_FS) {
                    eo.mem.displacement = (int32_t)((uint32_t)eo.mem.displacement + cpu->fs_base);
                }
            }
        }
        a.op(ZYDIS_MNEMONIC_XCHG, {R(ZYDIS_REGISTER_R12D), R(t32)});
        b->map.back().swap = (uint8_t)t;
        const bool ok = a.encode(req);
        a.op(ZYDIS_MNEMONIC_XCHG, {R(ZYDIS_REGISTER_R12D), R(t32)});
        return ok;
    }

    bool rdtsc()
    {
        a.op(ZYDIS_MNEMONIC_PUSHFQ, {});
        a.op(ZYDIS_MNEMONIC_RDTSC, {});
        a.op(ZYDIS_MNEMONIC_SHL, {R(ZYDIS_REGISTER_RDX), I(32)});
        a.op(ZYDIS_MNEMONIC_OR, {R(ZYDIS_REGISTER_RAX), R(ZYDIS_REGISTER_RDX)});
        a.op(ZYDIS_MNEMONIC_SUB, {R(ZYDIS_REGISTER_RAX), FR8(cycle_offset, 0)});
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RDX), FR8(tsc_ratio, 0)});
        a.op(ZYDIS_MNEMONIC_MUL, {R(ZYDIS_REGISTER_RDX)});
        a.op(ZYDIS_MNEMONIC_SHRD, {R(ZYDIS_REGISTER_RAX), R(ZYDIS_REGISTER_RDX), I(32)});
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RDX), R(ZYDIS_REGISTER_RAX)});
        a.op(ZYDIS_MNEMONIC_SHR, {R(ZYDIS_REGISTER_RDX), I(32)});
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_EAX), R(ZYDIS_REGISTER_EAX)});
        a.op(ZYDIS_MNEMONIC_POPFQ, {});
        return true;
    }

    bool pushfd()
    {
        a.op(ZYDIS_MNEMONIC_PUSHFQ, {});
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R13), M(ZYDIS_REGISTER_RSP, 0, 8)});
        a.op(ZYDIS_MNEMONIC_AND, {R(ZYDIS_REGISTER_R13D), I((int32_t)(GUEST_FLAGS | 0x2))});
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R11D), FR4(vif, 0)});
        a.op(ZYDIS_MNEMONIC_SHL, {R(ZYDIS_REGISTER_R11D), I(9)});
        a.op(ZYDIS_MNEMONIC_OR, {R(ZYDIS_REGISTER_R13D), R(ZYDIS_REGISTER_R11D)});
        /* AC as the guest last set it: a Pentium III has it (what tells a
         * 486 or later from a 386, the first step of CPU detection code) */
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R11D), FR4(vac, 0)});
        a.op(ZYDIS_MNEMONIC_SHL, {R(ZYDIS_REGISTER_R11D), I(18)});
        a.op(ZYDIS_MNEMONIC_OR, {R(ZYDIS_REGISTER_R13D), R(ZYDIS_REGISTER_R11D)});
        push_r(ZYDIS_REGISTER_R13D);
        a.op(ZYDIS_MNEMONIC_POPFQ, {});
        return true;
    }

    /* PUSHF and POPF with an operand-size prefix (66 9C, 66 9D): the low
     * word of the flags. Passed to the host as they were, they pushed and
     * popped 2 bytes of the host's RSP: America's Army - Rise of a Soldier
     * runs a 66 9C after its intro, the shadow stack went 2 bytes off, and
     * the next C call from the stubs crashed on its first aligned store. */
    bool pushf16()
    {
        a.op(ZYDIS_MNEMONIC_PUSHFQ, {});
        a.op(ZYDIS_MNEMONIC_POP, {R(ZYDIS_REGISTER_R10)});
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R13), R(ZYDIS_REGISTER_R10)});
        a.op(ZYDIS_MNEMONIC_AND, {R(ZYDIS_REGISTER_R13D), I((int32_t)((GUEST_FLAGS | 0x2) & 0xFFFF))});
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R11D), FR4(vif, 0)});
        a.op(ZYDIS_MNEMONIC_SHL, {R(ZYDIS_REGISTER_R11D), I(9)});
        a.op(ZYDIS_MNEMONIC_OR, {R(ZYDIS_REGISTER_R13D), R(ZYDIS_REGISTER_R11D)});
        a.op(ZYDIS_MNEMONIC_PUSH, {R(ZYDIS_REGISTER_R10)});
        a.op(ZYDIS_MNEMONIC_POPFQ, {});
        a.op(ZYDIS_MNEMONIC_MOV, {M(ZYDIS_REGISTER_R12D, -2, 2), R(ZYDIS_REGISTER_R13W)}, true);
        esp_add(-2);
        return true;
    }

    bool popf16()
    {
        a.op(ZYDIS_MNEMONIC_MOVZX, {R(ZYDIS_REGISTER_R13D), M(ZYDIS_REGISTER_R12D, 0, 2)}, true);
        esp_add(2);
        a.op(ZYDIS_MNEMONIC_PUSHFQ, {});
        a.op(ZYDIS_MNEMONIC_POP, {R(ZYDIS_REGISTER_R10)});
        a.op(ZYDIS_MNEMONIC_AND, {R(ZYDIS_REGISTER_R10D), I((int32_t)(GUEST_FLAGS & 0xFFFF0000u))});
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R11D), R(ZYDIS_REGISTER_R13D)});
        a.op(ZYDIS_MNEMONIC_SHR, {R(ZYDIS_REGISTER_R11D), I(9)});
        a.op(ZYDIS_MNEMONIC_AND, {R(ZYDIS_REGISTER_R11D), I(1)});
        a.op(ZYDIS_MNEMONIC_MOV, {FR4(vif, 0), R(ZYDIS_REGISTER_R11D)});
        a.op(ZYDIS_MNEMONIC_AND, {R(ZYDIS_REGISTER_R13D), I((int32_t)(GUEST_FLAGS & 0xFFFF))});
        a.op(ZYDIS_MNEMONIC_OR, {R(ZYDIS_REGISTER_R13D), I(0x202)});
        a.op(ZYDIS_MNEMONIC_OR, {R(ZYDIS_REGISTER_R13D), R(ZYDIS_REGISTER_R10D)});
        a.op(ZYDIS_MNEMONIC_PUSH, {R(ZYDIS_REGISTER_R13)});
        a.op(ZYDIS_MNEMONIC_POPFQ, {});
        return true;
    }

    bool popfd()
    {
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R13D), M(ZYDIS_REGISTER_R12D, 0, 4)}, true);
        esp_add(4);
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R11D), R(ZYDIS_REGISTER_R13D)});
        a.op(ZYDIS_MNEMONIC_SHR, {R(ZYDIS_REGISTER_R11D), I(9)});
        a.op(ZYDIS_MNEMONIC_AND, {R(ZYDIS_REGISTER_R11D), I(1)});
        a.op(ZYDIS_MNEMONIC_MOV, {FR4(vif, 0), R(ZYDIS_REGISTER_R11D)});
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R11D), R(ZYDIS_REGISTER_R13D)});
        a.op(ZYDIS_MNEMONIC_SHR, {R(ZYDIS_REGISTER_R11D), I(18)});
        a.op(ZYDIS_MNEMONIC_AND, {R(ZYDIS_REGISTER_R11D), I(1)});
        a.op(ZYDIS_MNEMONIC_MOV, {FR4(vac, 0), R(ZYDIS_REGISTER_R11D)});
        a.op(ZYDIS_MNEMONIC_AND, {R(ZYDIS_REGISTER_R13D), I((int32_t)GUEST_FLAGS)});
        a.op(ZYDIS_MNEMONIC_OR, {R(ZYDIS_REGISTER_R13D), I(0x202)});
        a.op(ZYDIS_MNEMONIC_PUSH, {R(ZYDIS_REGISTER_R13)});
        a.op(ZYDIS_MNEMONIC_POPFQ, {});
        return true;
    }

    /* Stores a 16-bit then a 32-bit value at a memory operand (SIDT/SGDT). */
    bool store_table_reg(const ZydisDecodedOperand &o, uint16_t limit, uint32_t base)
    {
        ZydisEncoderOperand m0, m2;
        if (!mem_operand(o, m0) || !mem_operand(o, m2, 2)) {
            return false;
        }
        m0.mem.size = 2;
        m2.mem.size = 4;
        return a.op(ZYDIS_MNEMONIC_MOV, {m0, I(limit)}, true) && a.op(ZYDIS_MNEMONIC_MOV, {m2, I((int32_t)base)}, true);
    }

    uint16_t selector(ZydisRegister s)
    {
        switch (s) {
        case ZYDIS_REGISTER_CS: return 0x08;
        case ZYDIS_REGISTER_FS: return 0x28;
        case ZYDIS_REGISTER_GS: return 0;
        default: return 0x10;
        }
    }

    /* One instruction; false ends the block after it (or before, *stop). */
    enum Result { CONTINUE, END, FAIL };

    Result insn(const ZydisDecodedInstruction &in, const ZydisDecodedOperand *ops)
    {
        const uint32_t next = eip + in.length;
        const ZydisDecodedOperand &o0 = ops[0];
        switch (in.mnemonic) {
        /* ---- control flow ---- */
        case ZYDIS_MNEMONIC_JMP:
            if (o0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                exit_to(next + (uint32_t)o0.imm.value.s);
                return END;
            }
            if (in.meta.branch_type == ZYDIS_BRANCH_TYPE_FAR) {
                sysop(eip, NC_SYSOP_OTHER, in.length);
                return END;
            }
            if (!load_r13(o0, 4)) {
                return FAIL;
            }
            dispatch_r13();
            return END;
        case ZYDIS_MNEMONIC_CALL:
            /* the guest's return address on its stack; on the host's, its
             * negation and a host call: a return to it is a host RET the
             * CPU predicts (RET below), back here, to a chained jump to the
             * return address's block */
            if (in.meta.branch_type == ZYDIS_BRANCH_TYPE_FAR) {
                sysop(eip, NC_SYSOP_OTHER, in.length);
                return END;
            }
            if (o0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                push_imm(next);
                a.op(ZYDIS_MNEMONIC_PUSH, {I((int32_t)(0u - next))});
                exits.push_back({a.call(cpu->s_lookup), next + (uint32_t)o0.imm.value.s});
                exit_to(next);
                return END;
            }
            if (!load_r13(o0, 4)) {
                return FAIL;
            }
            push_imm(next);
            a.op(ZYDIS_MNEMONIC_PUSH, {I((int32_t)(0u - next))});
            a.call(cpu->s_dispatch);
            exit_to(next);
            return END;
        case ZYDIS_MNEMONIC_RET: {
            if (in.meta.branch_type == ZYDIS_BRANCH_TYPE_FAR) {
                sysop(eip, NC_SYSOP_OTHER, in.length);
                return END;
            }
            const int32_t extra = o0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE ? (int32_t)(uint16_t)o0.imm.value.u : 0;
            a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R13D), M(ZYDIS_REGISTER_R12D, 0, 4)}, true);
            esp_add(4 + extra);
            /* the shadow stack's top predicts it: [rsp] the host return,
             * [rsp+8] the guest one negated; equal (lea ecx, [rcx + r13]
             * zero, the flags untouched): RET 8. Else the shadow stack goes
             * (a longjmp, a thread switch, a return address replaced) and
             * the dispatcher finds the block. */
            a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R10), R(ZYDIS_REGISTER_RCX)});
            a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_ECX), M(ZYDIS_REGISTER_RSP, 8, 4)});
            a.op(ZYDIS_MNEMONIC_LEA, {R(ZYDIS_REGISTER_ECX), M(ZYDIS_REGISTER_RCX, 0, 0, ZYDIS_REGISTER_R13, 1)});
            a.byte(0xE3); /* jrcxz hit */
            uint8_t *jr = a.p;
            a.byte(0);
            a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RCX), R(ZYDIS_REGISTER_R10)});
            a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RSP), FR8(host_rsp, 0)});
            dispatch_r13();
            *jr = (uint8_t)(a.p - (jr + 1));
            a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RCX), R(ZYDIS_REGISTER_R10)});
            a.op(ZYDIS_MNEMONIC_RET, {I(8)});
            return END;
        }
        case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_JLE:
        case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_JNLE:
        case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_JNP: case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_JNZ:
        case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_JZ: {
            const unsigned cc = in.opcode & 15; /* 7x and 0F 8x alike */
            exit_jcc(cc, next + (uint32_t)o0.imm.value.s);
            exit_to(next);
            return END;
        }
        case ZYDIS_MNEMONIC_JECXZ: case ZYDIS_MNEMONIC_JCXZ:
        case ZYDIS_MNEMONIC_LOOP: case ZYDIS_MNEMONIC_LOOPE: case ZYDIS_MNEMONIC_LOOPNE: {
            if (in.address_width == 16) {
                /* the count is CX (67 E0-E3): CX less one where it counts,
                 * its zero tested through RCX (JRCXZ), no flag touched;
                 * the guest's ECX waits in R10 */
                const bool counts = in.mnemonic != ZYDIS_MNEMONIC_JCXZ && in.mnemonic != ZYDIS_MNEMONIC_JECXZ;
                if (counts) {
                    a.op(ZYDIS_MNEMONIC_LEA, {R(ZYDIS_REGISTER_R11D), M(ZYDIS_REGISTER_RCX, -1, 0)});
                    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_CX), R(ZYDIS_REGISTER_R11W)});
                }
                a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R10), R(ZYDIS_REGISTER_RCX)});
                a.op(ZYDIS_MNEMONIC_MOVZX, {R(ZYDIS_REGISTER_ECX), R(ZYDIS_REGISTER_CX)});
                uint8_t *zero = nullptr, *flag = nullptr;
                a.byte(0xE3); /* jrcxz: the count is zero */
                zero = a.p;
                a.byte(0);
                if (in.mnemonic == ZYDIS_MNEMONIC_LOOPE || in.mnemonic == ZYDIS_MNEMONIC_LOOPNE) {
                    a.byte(in.mnemonic == ZYDIS_MNEMONIC_LOOPE ? 0x75 : 0x74); /* jnz / jz: its flag says stop */
                    flag = a.p;
                    a.byte(0);
                }
                /* not zero: LOOP's jump, JCXZ's fall through */
                a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RCX), R(ZYDIS_REGISTER_R10)});
                exit_to(counts ? next + (uint32_t)o0.imm.value.s : next);
                *zero = (uint8_t)(a.p - (zero + 1));
                if (flag) {
                    /* the flag's stop is LOOP's fall through; the zero count's too */
                    *flag = (uint8_t)(a.p - (flag + 1));
                }
                a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RCX), R(ZYDIS_REGISTER_R10)});
                exit_to(counts ? next : next + (uint32_t)o0.imm.value.s);
                return END;
            }
            if (in.address_width != 32) {
                return FAIL;
            }
            const uint8_t opc = in.mnemonic == ZYDIS_MNEMONIC_LOOP     ? 0xE2
                                : in.mnemonic == ZYDIS_MNEMONIC_LOOPE  ? 0xE1
                                : in.mnemonic == ZYDIS_MNEMONIC_LOOPNE ? 0xE0
                                                                      : 0xE3;
            /* 67 op +2; jmp +5 (not taken); jmp rel32 (taken) */
            a.bytes({0x67, opc, 0x02, 0xEB, 0x05});
            exit_to(next + (uint32_t)o0.imm.value.s);
            exit_to(next);
            return END;
        }
        case ZYDIS_MNEMONIC_IRETD: case ZYDIS_MNEMONIC_IRET: case ZYDIS_MNEMONIC_IRETQ:
        case ZYDIS_MNEMONIC_SYSENTER: case ZYDIS_MNEMONIC_SYSEXIT: case ZYDIS_MNEMONIC_SYSCALL:
        case ZYDIS_MNEMONIC_SYSRET:
            sysop(eip, NC_SYSOP_OTHER, in.length);
            return END;

        /* ---- the stack ---- */
        case ZYDIS_MNEMONIC_PUSH: {
            if (o0.type == ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetClass(o0.reg.value) == ZYDIS_REGCLASS_SEGMENT) {
                push_imm(selector(o0.reg.value));
                return CONTINUE;
            }
            if (in.operand_width == 16 && o0.type != ZYDIS_OPERAND_TYPE_MEMORY) {
                /* a word pushed (66 50+r, 66 6A / 68): ESP down by 2.
                 * Castlevania: Curse of Darkness pushes DX as its title
                 * screen ends: a system op, the core stopped there. */
                if (o0.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                    a.op(ZYDIS_MNEMONIC_MOV, {M(ZYDIS_REGISTER_R12D, -2, 2), R(remap(o0.reg.value))}, true);
                } else {
                    a.op(ZYDIS_MNEMONIC_MOV, {M(ZYDIS_REGISTER_R12D, -2, 2), I((int16_t)o0.imm.value.s)}, true);
                }
                esp_add(-2);
                return CONTINUE;
            }
            if (in.operand_width == 16) {
                /* a word pushed from memory (66 FF /6): its address taken
                 * with ESP as it was */
                if (!load_r13(o0, 2) ||
                    !a.op(ZYDIS_MNEMONIC_MOV, {M(ZYDIS_REGISTER_R12D, -2, 2), R(ZYDIS_REGISTER_R13W)}, true)) {
                    return FAIL;
                }
                esp_add(-2);
                return CONTINUE;
            }
            if (in.operand_width != 32) {
                return FAIL;
            }
            if (o0.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                push_r(remap(o0.reg.value));
            } else if (o0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                push_imm((uint32_t)o0.imm.value.s);
            } else {
                if (!load_r13(o0, 4)) {
                    return FAIL;
                }
                push_r(ZYDIS_REGISTER_R13D);
            }
            return CONTINUE;
        }
        case ZYDIS_MNEMONIC_POP: {
            if (o0.type == ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetClass(o0.reg.value) == ZYDIS_REGCLASS_SEGMENT) {
                esp_add(4);
                return CONTINUE;
            }
            if (in.operand_width == 16 && o0.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                /* a word popped (66 58+r): ESP up by 2 (SP itself: the value) */
                const ZydisRegister r = remap(o0.reg.value);
                a.op(ZYDIS_MNEMONIC_MOV, {R(r), M(ZYDIS_REGISTER_R12D, 0, 2)}, true);
                if (r != ZYDIS_REGISTER_R12W) {
                    esp_add(2);
                }
                return CONTINUE;
            }
            if (in.operand_width == 16 && o0.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                /* a word popped to memory (66 8F /0): as the 32-bit form
                 * below, the store first, its address the one after the pop */
                a.op(ZYDIS_MNEMONIC_MOVZX, {R(ZYDIS_REGISTER_R14D), M(ZYDIS_REGISTER_R12D, 0, 2)}, true);
                ZydisEncoderOperand m;
                const int64_t after = o0.mem.base == ZYDIS_REGISTER_ESP ? 2 : 0;
                if (!mem_operand(o0, m, after) || !a.op(ZYDIS_MNEMONIC_MOV, {m, R(ZYDIS_REGISTER_R14W)}, true)) {
                    return FAIL;
                }
                esp_add(2);
                return CONTINUE;
            }
            if (in.operand_width != 32) {
                return FAIL;
            }
            if (o0.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                const ZydisRegister r = remap(o0.reg.value);
                a.op(ZYDIS_MNEMONIC_MOV, {R(r), M(ZYDIS_REGISTER_R12D, 0, 4)}, true);
                if (r != ZYDIS_REGISTER_R12D) {
                    esp_add(4);
                }
                return CONTINUE;
            }
            /* the store before ESP moves: a store that faults (a write-
             * watched page) restarts the instruction from the frame the
             * fault saw, which must not have ESP popped already (a second
             * pop then, of the next slot). The address is the one after
             * the pop (ESP + 4 as the base); the value in R14, as an FS
             * operand's address takes R13. */
            a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R14D), M(ZYDIS_REGISTER_R12D, 0, 4)}, true);
            ZydisEncoderOperand m;
            const int64_t after = o0.mem.base == ZYDIS_REGISTER_ESP ? 4 : 0;
            if (!mem_operand(o0, m, after) || !a.op(ZYDIS_MNEMONIC_MOV, {m, R(ZYDIS_REGISTER_R14D)}, true)) {
                return FAIL;
            }
            esp_add(4);
            return CONTINUE;
        }
        case ZYDIS_MNEMONIC_PUSHAD:
            if (in.operand_width != 32) {
                return FAIL;
            }
            for (int i = 0; i < 8; i++) {
                a.op(ZYDIS_MNEMONIC_MOV, {M(ZYDIS_REGISTER_R12D, -4 * (i + 1), 4), R(k_r32[i])}, true);
            }
            esp_add(-32);
            return CONTINUE;
        case ZYDIS_MNEMONIC_POPAD:
            if (in.operand_width != 32) {
                return FAIL;
            }
            for (int i = 7; i >= 0; i--) {
                if (i != 4) {
                    a.op(ZYDIS_MNEMONIC_MOV, {R(k_r32[i]), M(ZYDIS_REGISTER_R12D, 4 * (7 - i), 4)}, true);
                }
            }
            esp_add(32);
            return CONTINUE;
        case ZYDIS_MNEMONIC_PUSHA:
            /* 66 60: the 16-bit registers, 2 bytes each, SP as it was
             * (Rockstar's timer reads wrap RDTSC in PUSHAW / POPAW: a system
             * op each until here) */
            for (int i = 0; i < 8; i++) {
                a.op(ZYDIS_MNEMONIC_MOV, {M(ZYDIS_REGISTER_R12D, -2 * (i + 1), 2), R(k_r16[i])}, true);
            }
            esp_add(-16);
            return CONTINUE;
        case ZYDIS_MNEMONIC_POPA:
            /* 66 61: the low halves only, SP's word skipped */
            for (int i = 7; i >= 0; i--) {
                if (i != 4) {
                    a.op(ZYDIS_MNEMONIC_MOV, {R(k_r16[i]), M(ZYDIS_REGISTER_R12D, 2 * (7 - i), 2)}, true);
                }
            }
            esp_add(16);
            return CONTINUE;
        case ZYDIS_MNEMONIC_PUSHFD:
            return pushfd() ? CONTINUE : FAIL;
        case ZYDIS_MNEMONIC_POPFD:
            return popfd() ? CONTINUE : FAIL;
        case ZYDIS_MNEMONIC_PUSHF:
            return pushf16() ? CONTINUE : FAIL;
        case ZYDIS_MNEMONIC_POPF:
            return popf16() ? CONTINUE : FAIL;
        case ZYDIS_MNEMONIC_LEAVE:
            if (in.operand_width != 32) {
                return FAIL;
            }
            a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R13D), M(ZYDIS_REGISTER_EBP, 0, 4)}, true);
            a.op(ZYDIS_MNEMONIC_LEA, {R(ZYDIS_REGISTER_R12D), M(ZYDIS_REGISTER_RBP, 4, 0)});
            a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_EBP), R(ZYDIS_REGISTER_R13D)});
            return CONTINUE;
        case ZYDIS_MNEMONIC_ENTER: {
            if (in.operand_width != 32 || ops[1].imm.value.u != 0) {
                return FAIL;
            }
            push_r(ZYDIS_REGISTER_EBP);
            a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_EBP), R(ZYDIS_REGISTER_R12D)});
            esp_add(-(int32_t)(uint16_t)o0.imm.value.u);
            return CONTINUE;
        }

        /* ---- system ---- */
        case ZYDIS_MNEMONIC_HLT:
            a.op(ZYDIS_MNEMONIC_MOV, {FR4(rip, 0), I((int32_t)eip)});
            a.jmp(cpu->s_hlt);
            return END;
        case ZYDIS_MNEMONIC_CLI: sysop(eip, NC_SYSOP_CLI, in.length); return END;
        case ZYDIS_MNEMONIC_STI: sysop(eip, NC_SYSOP_STI, in.length); return END;
        case ZYDIS_MNEMONIC_IN: sysop(eip, NC_SYSOP_IN, in.length); return END;
        case ZYDIS_MNEMONIC_OUT: sysop(eip, NC_SYSOP_OUT, in.length); return END;
        case ZYDIS_MNEMONIC_INSB: case ZYDIS_MNEMONIC_INSW: case ZYDIS_MNEMONIC_INSD:
            sysop(eip, NC_SYSOP_INS, in.length);
            return END;
        case ZYDIS_MNEMONIC_OUTSB: case ZYDIS_MNEMONIC_OUTSW: case ZYDIS_MNEMONIC_OUTSD:
            sysop(eip, NC_SYSOP_OUTS, in.length);
            return END;
        case ZYDIS_MNEMONIC_INT:
            sysop(eip, NC_SYSOP_INT, in.length);
            return END;
        case ZYDIS_MNEMONIC_INT3: sysop(eip, NC_SYSOP_INT3, in.length); return END;
        case ZYDIS_MNEMONIC_INTO: sysop(eip, NC_SYSOP_INTO, in.length); return END;
        case ZYDIS_MNEMONIC_INT1: sysop(eip, NC_SYSOP_INT1, in.length); return END;
        case ZYDIS_MNEMONIC_UD0: case ZYDIS_MNEMONIC_UD1: case ZYDIS_MNEMONIC_UD2:
            sysop(eip, NC_SYSOP_UD2, in.length);
            return END;
        case ZYDIS_MNEMONIC_RDMSR: sysop(eip, NC_SYSOP_RDMSR, in.length); return END;
        case ZYDIS_MNEMONIC_WRMSR: sysop(eip, NC_SYSOP_WRMSR, in.length); return END;
        case ZYDIS_MNEMONIC_RDPMC: sysop(eip, NC_SYSOP_RDPMC, in.length); return END;
        case ZYDIS_MNEMONIC_WBINVD: sysop(eip, NC_SYSOP_WBINVD, in.length); return END;
        case ZYDIS_MNEMONIC_INVD: sysop(eip, NC_SYSOP_INVD, in.length); return END;
        case ZYDIS_MNEMONIC_INVLPG: sysop(eip, NC_SYSOP_INVLPG, in.length); return END;
        case ZYDIS_MNEMONIC_CLTS: sysop(eip, NC_SYSOP_CLTS, in.length); return END;
        case ZYDIS_MNEMONIC_LGDT: sysop(eip, NC_SYSOP_LGDT, in.length); return END;
        case ZYDIS_MNEMONIC_LIDT: sysop(eip, NC_SYSOP_LIDT, in.length); return END;
        case ZYDIS_MNEMONIC_LLDT: sysop(eip, NC_SYSOP_LLDT, in.length); return END;
        case ZYDIS_MNEMONIC_LTR: sysop(eip, NC_SYSOP_LTR, in.length); return END;
        case ZYDIS_MNEMONIC_LMSW: sysop(eip, NC_SYSOP_LMSW, in.length); return END;
        case ZYDIS_MNEMONIC_CPUID: sysop(eip, NC_SYSOP_CPUID, in.length); return END;
        case ZYDIS_MNEMONIC_RDTSC:
            return rdtsc() ? CONTINUE : FAIL;
        case ZYDIS_MNEMONIC_SIDT:
            return store_table_reg(o0, cpu->cfg.idt_limit, cpu->cfg.idt_base) ? CONTINUE : FAIL;
        case ZYDIS_MNEMONIC_SGDT:
            return store_table_reg(o0, 0xFF, 0) ? CONTINUE : FAIL;
        case ZYDIS_MNEMONIC_SMSW:
            if (o0.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                a.op(ZYDIS_MNEMONIC_MOV, {R(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, remap(o0.reg.value)) ==
                                               ZYDIS_REGISTER_R12 ? ZYDIS_REGISTER_R12D : ZydisRegisterEncode(ZYDIS_REGCLASS_GPR32, ZydisRegisterGetId(o0.reg.value))),
                                           I(0x3B)});
            } else {
                ZydisEncoderOperand m;
                if (!mem_operand(o0, m)) {
                    return FAIL;
                }
                m.mem.size = 2;
                a.op(ZYDIS_MNEMONIC_MOV, {m, I(0x3B)}, true);
            }
            return CONTINUE;
        case ZYDIS_MNEMONIC_LDS: case ZYDIS_MNEMONIC_LES: {
            /* a far pointer loaded (C5 /r, C4 /r): the register takes the
             * offset; the selector's segment register stays the flat
             * model's, as MOV to one does */
            ZydisEncoderOperand m;
            if (o0.type != ZYDIS_OPERAND_TYPE_REGISTER || ops[1].type != ZYDIS_OPERAND_TYPE_MEMORY ||
                !mem_operand(ops[1], m)) {
                return FAIL;
            }
            m.mem.size = (uint16_t)(in.operand_width / 8);
            return a.op(ZYDIS_MNEMONIC_MOV, {R(remap(o0.reg.value)), m}, true) ? CONTINUE : FAIL;
        }
        case ZYDIS_MNEMONIC_STR: case ZYDIS_MNEMONIC_SLDT: case ZYDIS_MNEMONIC_LAR: case ZYDIS_MNEMONIC_LSL:
        case ZYDIS_MNEMONIC_VERR: case ZYDIS_MNEMONIC_VERW: case ZYDIS_MNEMONIC_RDTSCP:
        case ZYDIS_MNEMONIC_LFS: case ZYDIS_MNEMONIC_LGS:
        case ZYDIS_MNEMONIC_LSS: case ZYDIS_MNEMONIC_AAA: case ZYDIS_MNEMONIC_AAD: case ZYDIS_MNEMONIC_AAM:
        case ZYDIS_MNEMONIC_AAS: case ZYDIS_MNEMONIC_DAA: case ZYDIS_MNEMONIC_DAS: case ZYDIS_MNEMONIC_BOUND:
        case ZYDIS_MNEMONIC_ARPL: case ZYDIS_MNEMONIC_SALC:
            sysop(eip, NC_SYSOP_OTHER, in.length);
            return END;
        case ZYDIS_MNEMONIC_MOV: {
            /* control and debug registers, segment registers */
            for (unsigned i = 0; i < in.operand_count_visible; i++) {
                if (ops[i].type != ZYDIS_OPERAND_TYPE_REGISTER) {
                    continue;
                }
                const ZydisRegisterClass c = ZydisRegisterGetClass(ops[i].reg.value);
                if (c == ZYDIS_REGCLASS_CONTROL) {
                    sysop(eip, NC_SYSOP_MOVCR, in.length);
                    return END;
                }
                if (c == ZYDIS_REGCLASS_DEBUG) {
                    sysop(eip, NC_SYSOP_MOVDR, in.length);
                    return END;
                }
                if (c == ZYDIS_REGCLASS_SEGMENT) {
                    if (i == 0) {
                        return CONTINUE; /* a segment load: the flat model's selectors stay */
                    }
                    const uint16_t sel = selector(ops[1].reg.value);
                    if (o0.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                        const ZydisRegister r = remap(o0.reg.value);
                        const ZydisRegister r32 = r == ZYDIS_REGISTER_R12W || r == ZYDIS_REGISTER_R12D
                                                      ? ZYDIS_REGISTER_R12D
                                                      : ZydisRegisterEncode(ZYDIS_REGCLASS_GPR32, ZydisRegisterGetId(r));
                        a.op(ZYDIS_MNEMONIC_MOV, {R(r32), I(sel)});
                    } else {
                        ZydisEncoderOperand m;
                        if (!mem_operand(o0, m)) {
                            return FAIL;
                        }
                        m.mem.size = 2;
                        a.op(ZYDIS_MNEMONIC_MOV, {m, I(sel)}, true);
                    }
                    return CONTINUE;
                }
            }
            return generic(in, ops) ? CONTINUE : FAIL;
        }
        default:
            return generic(in, ops) ? CONTINUE : FAIL;
        }
    }
};

static void free_all_blocks(NcCpu *cpu)
{
    for (Block *b : cpu->by_host) {
        delete b;
    }
    for (Block *b : cpu->graveyard) {
        delete b;
    }
    cpu->by_host.clear();
    cpu->graveyard.clear();
    cpu->blocks.clear();
    cpu->bypass.clear();
    cpu->pages.clear();
    std::fill(cpu->page_bits.begin(), cpu->page_bits.end(), 0);
    cpu->incoming.clear();
}

/* Everything translated goes (the code buffer is full): only between
 * blocks, from the C side. */
static void flush_all(NcCpu *cpu)
{
    free_all_blocks(cpu);
    // dead blocks are in by_host too; free_all_blocks deleted them once
    l1_clear(cpu);
    cpu->code_p = cpu->code_begin;
    if (cpu->frame) {
        cpu->frame->shadow_reset = 1; /* the return predictions point into what went (s_restore) */
    }
    cpu->flushes++;
    nc_log("hakuX-native", "cpu: translation buffer flushed (%llu so far)", (unsigned long long)cpu->flushes);
}

static void link_block(NcCpu *cpu, Block *b)
{
    if (g_smc_check) {
        return; /* every exit through its stub and the lookup */
    }
    for (unsigned i = 0; i < b->exits.size(); i++) {
        Exit &e = b->exits[i];
        auto it = cpu->blocks.find(e.target);
        if (it != cpu->blocks.end() && !it->second->dead) {
            patch_rel32(e.rel32, it->second->code);
        }
        cpu->incoming[e.target].push_back({b, i});
    }
    if (b->bypass) {
        return;
    }
    auto in = cpu->incoming.find(b->eip);
    if (in != cpu->incoming.end()) {
        for (auto &site : in->second) {
            if (!site.first->dead) {
                patch_rel32(site.first->exits[site.second].rel32, b->code);
            }
        }
    }
}

static void kill_block(NcCpu *cpu, Block *b)
{
    if (b->dead) {
        return;
    }
    b->dead = true;
    /* Its own exits back to their stubs. Its code still runs once more when
     * a return predicted before it died comes back into it (a call below
     * made the invalidation: a protection change, memory freed), and a
     * chained exit then went on into the next block's dead code, the old
     * translation of what was just replaced: the incoming list below skips
     * sites in dead blocks. Through the stub the dispatcher finds the new. */
    for (Exit &e : b->exits) {
        patch_rel32(e.rel32, e.stub);
    }
    if (b->bypass) {
        cpu->bypass.erase(b->eip);
        return;
    }
    cpu->blocks.erase(b->eip);
    l1_drop(cpu, b->eip);
    auto in = cpu->incoming.find(b->eip);
    if (in != cpu->incoming.end()) {
        for (auto &site : in->second) {
            if (!site.first->dead) {
                Exit &e = site.first->exits[site.second];
                patch_rel32(e.rel32, e.stub);
            }
        }
    }
}

static Block *translate(NcCpu *cpu, uint32_t eip, bool bypass)
{
    if ((size_t)(cpu->code_end - cpu->code_p) < 64 * 1024) {
        flush_all(cpu);
    }
    g_ok_page = UINT64_MAX;
    Block *b = new Block();
    b->eip = eip;
    b->bypass = bypass;
    b->code = cpu->code_p;
    Translator t {cpu, Asm(cpu->code_p, cpu->code_end - 4096), b, eip, {}};
    Asm &a = t.a;

    /* the interrupt poll: a load from the poll page, flags untouched */
    if (!bypass) {
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R11B), A((const void *)cpu->poll, 1)});
    }

    const unsigned max_insns = 256;
    unsigned n = 0;
    bool ended = false;
    /* The guest bytes the block was made from end here: the instruction it
     * stops at without translating (a hook, one with no translation, one
     * that doesn't decode) is one of them. A block of only that, at a
     * page's first byte, was listed under no page, and no invalidation of
     * its code ever found it. */
    uint64_t dep_end = (uint64_t)eip + 1;
    bool probe = false;
    while (!ended) {
        const uint32_t at = t.eip;
        b->map.push_back({(uint32_t)(a.p - b->code), at, 0});
        const bool hooked = cpu->cfg.hook_query && !(bypass && n == 0) && cpu->cfg.hook_query(cpu->cfg.opaque, at);
        if (hooked) {
            dep_end = std::max(dep_end, (uint64_t)at + 1);
            t.sysop(at, NC_SYSOP_HOOK, 0);
            break;
        }
        if (n >= max_insns || (n && ((at ^ eip) & ~0xFFFu) && (at & 0xFFF) == 0 && n > 32)) {
            t.exit_to(at);
            break;
        }
        if (!guest_readable(at, 1)) {
            if (n) {
                t.exit_to(at);
            } else {
                /* fetching it faults as a guest read at eip: the core's
                 * page fault path (its page may be mapped by then) */
                a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R11B), M(ZYDIS_REGISTER_NONE, (int32_t)at, 1)}, true);
                t.exit_to(at);
                /* not this address's block: kept, its exit came back to
                 * itself for ever once the page was mapped */
                probe = true;
            }
            break;
        }
        const uint32_t avail = guest_readable(at, ZYDIS_MAX_INSTRUCTION_LENGTH) ? ZYDIS_MAX_INSTRUCTION_LENGTH
                                                                               : 0x1000 - (at & 0xFFF);
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        /* The Xbox's CPU is a Pentium III: no SSE2. An MMX instruction with
         * an operand-size prefix (66 0F xx) is MMX there, the prefix ignored;
         * on the host it would be SSE2's (XMM registers, 16 bytes, a 16-byte
         * aligned operand). Dr. Muto adds a pixel so (66 0F DC: PADDUSB) from
         * an unaligned address: SSE2's general protection fault, the title
         * gone. Decoded without the prefix, as the console runs it. */
        const uint8_t *ib = (const uint8_t *)(uintptr_t)at;
        const bool mmx66 = avail >= 3 && ib[0] == 0x66 && ib[1] == 0x0F && nc_x86_mmx_opcode(ib[2]);
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&cpu->dec, (const void *)(uintptr_t)(at + (mmx66 ? 1 : 0)),
                                                 avail - (mmx66 ? 1 : 0), &in, ops))) {
            dep_end = std::max(dep_end, (uint64_t)at + 1);
            t.sysop(at, NC_SYSOP_UD2, 0);
            break;
        }
        if (mmx66) {
            in.length++;    /* the prefix is the instruction's too */
        }
        uint8_t *const insn_start = a.p;
        const Translator::Result r = t.insn(in, ops);
        if (r == Translator::FAIL || a.failed) {
            a.p = insn_start;
            a.failed = false;
            b->map.back().swap = 0;
            b->map.back().borrow = 0;
            static unsigned logged;
            if (logged++ < 64) {
                char txt[96] = "?";
                ZydisFormatter fmt;
                ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);
                ZydisFormatterFormatInstruction(&fmt, &in, ops, in.operand_count_visible, txt, sizeof(txt), at, nullptr);
                nc_log("hakuX-native", "cpu: no translation for %08x: %s (a system op)", at, txt);
            }
            dep_end = std::max(dep_end, (uint64_t)at + in.length);
            t.sysop(at, NC_SYSOP_OTHER, in.length);
            break;
        }
        n++;
        t.eip = at + in.length;
        ended = r == Translator::END;
    }
    b->end = t.eip;
    if (t.eip > eip) {
        dep_end = std::max(dep_end, (uint64_t)t.eip);
    }

    /* the exits' stubs, after the code: mov r13d, target; jmp dispatch */
    for (auto &pe : t.exits) {
        uint8_t *stub = a.p;
        a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R13D), I((int32_t)pe.target)});
        a.jmp(cpu->s_dispatch);
        patch_rel32(pe.rel32, stub);
        b->exits.push_back({pe.rel32, stub, pe.target});
    }
    if (a.failed) {
        delete b;
        flush_all(cpu);
        return translate(cpu, eip, bypass);
    }
    b->code_end = a.p;
    cpu->code_p = (uint8_t *)(((uintptr_t)a.p + 15) & ~(uintptr_t)15);
    cpu->by_host.push_back(b);
#ifdef NC_CPU_DIAG
    if (g_smc_check && !probe && dep_end - eip <= 0x4000 && guest_readable(eip, (uint32_t)(dep_end - eip))) {
        b->src.assign((const uint8_t *)(uintptr_t)eip, (const uint8_t *)(uintptr_t)eip + (dep_end - eip));
    }
#endif
    if (probe) {
        b->dead = true; /* made again each time it is reached, until the page is there */
    } else {
        for (uint64_t p = eip >> 12; p <= ((dep_end - 1) >> 12) && p <= 0xFFFFFu; p++) {
            cpu->pages[(uint32_t)p].push_back(b);
            cpu->page_bits[p >> 6] |= 1ull << (p & 63);
        }
    }
    if (bypass && !probe) {
        cpu->bypass[eip] = b;
    } else if (!bypass && !probe) {
        cpu->blocks[eip] = b;
        l1_set(cpu, eip, b->code);
    }
    link_block(cpu, b);
    cpu->translated++;
    FlushInstructionCache(GetCurrentProcess(), b->code, (size_t)(b->code_end - b->code));
    return b;
}

static uint8_t *code_for(NcCpu *cpu, uint32_t eip)
{
    /* the dispatcher's table first: every system op returns through here */
    const L1Entry &e = cpu->l1[eip & (L1_SIZE - 1)];
    if (e.neg_eip == (uint32_t)-eip && eip) {
        return e.code;
    }
    auto it = cpu->blocks.find(eip);
    if (it != cpu->blocks.end()) {
        Block *b = it->second;
#ifdef NC_CPU_DIAG
        if (g_smc_check && !b->src.empty()) {
            g_ok_page = UINT64_MAX;
            const uint32_t n = (uint32_t)b->src.size();
            if (guest_readable(eip, n) && memcmp((const void *)(uintptr_t)eip, b->src.data(), n)) {
                static unsigned logged;
                if (logged++ < 200) {
                    uint32_t at = 0;
                    while (((const uint8_t *)(uintptr_t)eip)[at] == b->src[at]) {
                        at++;
                    }
                    nc_log("hakuX-native", "cpu: smc: the code at %08x changed with no invalidation (first at %08x, %u bytes)",
                           eip, eip + at, n);
                }
                kill_block(cpu, b);
                return translate(cpu, eip, false)->code;
            }
        }
#endif
        l1_set(cpu, eip, b->code);
        return b->code;
    }
    return translate(cpu, eip, false)->code;
}

/* Where the frame goes next: its EIP's translation, or the bypass one. */
static uint8_t *next_for(NcCpu *cpu, NcFrame *f)
{
    const uint32_t eip = (uint32_t)f->rip;
    if (f->hook_bypass && f->hook_bypass == eip) {
        f->hook_bypass = 0;
        auto it = cpu->bypass.find(eip);
        return it != cpu->bypass.end() ? it->second->code : translate(cpu, eip, true)->code;
    }
    return code_for(cpu, eip);
}

/* ------------------------------------------- instructions run in C */

static uint32_t parity_even(uint8_t v)
{
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (v & 1) ? 0 : 0x4;
}

/* SF, ZF and PF of a byte result into flags (the others as given). */
static uint64_t szp8(uint64_t fl, uint8_t v)
{
    fl &= ~(uint64_t)0xC4;
    return fl | (v & 0x80) | (v ? 0 : 0x40) | parity_even(v);
}

/* User-mode instructions 64-bit mode has no encoding for, which the
 * translator therefore can't pass to the host: the ASCII and decimal
 * adjusts, SALC, BOUND, INTO, ENTER with a nesting level and the 16-bit
 * LEAVE. They left to the core as system ops it didn't know, and the title
 * stopped there. Run here on the frame instead (the flags are the host's
 * image in f->eflags); true when done, EIP past the instruction. What
 * faults on a console (BOUND out of range, INTO with OF set, AAM 0) stays
 * the core's. */
static bool emulate(NcCpu *cpu, NcFrame *f)
{
    const uint32_t eip = (uint32_t)f->rip;
    g_ok_page = UINT64_MAX;
    if (!guest_readable(eip, 1)) {
        return false;
    }
    const uint32_t avail = guest_readable(eip, ZYDIS_MAX_INSTRUCTION_LENGTH) ? ZYDIS_MAX_INSTRUCTION_LENGTH
                                                                              : 0x1000 - (eip & 0xFFF);
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&cpu->dec, (const void *)(uintptr_t)eip, avail, &in, ops))) {
        return false;
    }
    uint64_t fl = f->eflags;
    const bool cf = fl & 0x1, af = fl & 0x10;
    uint32_t eax = (uint32_t)f->gregs[NC_EAX];
    uint8_t al = (uint8_t)eax, ah = (uint8_t)(eax >> 8);
    auto set_ax = [&](uint8_t h, uint8_t l) { f->gregs[NC_EAX] = (eax & 0xFFFF0000u) | (uint32_t)h << 8 | l; };
    switch (in.mnemonic) {
    case ZYDIS_MNEMONIC_AAM: {
        const uint8_t base = (uint8_t)ops[0].imm.value.u;
        if (!base) {
            return false; /* a divide error: the core's */
        }
        ah = al / base;
        al = al % base;
        set_ax(ah, al);
        fl = szp8(fl & ~(uint64_t)0x811, al); /* OF, AF, CF undefined: clear, as a P6 leaves them */
        break;
    }
    case ZYDIS_MNEMONIC_AAD: {
        const uint8_t base = (uint8_t)ops[0].imm.value.u;
        al = (uint8_t)(al + ah * base);
        set_ax(0, al);
        fl = szp8(fl & ~(uint64_t)0x811, al);
        break;
    }
    case ZYDIS_MNEMONIC_AAA:
    case ZYDIS_MNEMONIC_AAS: {
        uint16_t ax = (uint16_t)eax;
        const bool adjust = (al & 0x0F) > 9 || af;
        if (adjust) {
            ax = in.mnemonic == ZYDIS_MNEMONIC_AAA ? (uint16_t)(ax + 0x106) : (uint16_t)(ax - 6 - 0x100);
        }
        ax &= 0xFF0F;
        f->gregs[NC_EAX] = (eax & 0xFFFF0000u) | ax;
        fl = (fl & ~(uint64_t)0x11) | (adjust ? 0x11 : 0);
        break;
    }
    case ZYDIS_MNEMONIC_DAA:
    case ZYDIS_MNEMONIC_DAS: {
        const bool add = in.mnemonic == ZYDIS_MNEMONIC_DAA;
        const uint8_t old_al = al;
        bool ncf = false, naf = false;
        if ((al & 0x0F) > 9 || af) {
            ncf = cf || (add ? al > 0xF9 : al < 6);
            al = add ? (uint8_t)(al + 6) : (uint8_t)(al - 6);
            naf = true;
        }
        if (old_al > 0x99 || cf) {
            al = add ? (uint8_t)(al + 0x60) : (uint8_t)(al - 0x60);
            ncf = true;
        } else if (add) {
            ncf = false;
        }
        f->gregs[NC_EAX] = (eax & 0xFFFFFF00u) | al;
        fl = szp8((fl & ~(uint64_t)0x11) | (ncf ? 0x1 : 0) | (naf ? 0x10 : 0), al);
        break;
    }
    case ZYDIS_MNEMONIC_SALC:
        f->gregs[NC_EAX] = (eax & 0xFFFFFF00u) | (cf ? 0xFF : 0);
        break;
    case ZYDIS_MNEMONIC_INTO:
        if (fl & 0x800) {
            return false; /* the overflow trap: the core's */
        }
        break;
    case ZYDIS_MNEMONIC_BOUND: {
        /* index register against a pair of signed bounds in memory */
        const ZydisDecodedOperand &m = ops[1];
        if (ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER || m.type != ZYDIS_OPERAND_TYPE_MEMORY ||
            in.address_width != 32) {
            return false;
        }
        auto reg = [&](ZydisRegister r) -> uint32_t {
            return r == ZYDIS_REGISTER_NONE ? 0 : (uint32_t)f->gregs[ZydisRegisterGetId(r) & 7];
        };
        uint32_t ea = reg(m.mem.base) + reg(m.mem.index) * (m.mem.index == ZYDIS_REGISTER_NONE ? 0 : m.mem.scale) +
                      (m.mem.disp.has_displacement ? (uint32_t)m.mem.disp.value : 0);
        if (m.mem.segment == ZYDIS_REGISTER_FS) {
            ea += cpu->fs_base;
        }
        const unsigned bytes = in.operand_width / 8;
        if (!guest_readable(ea, 2 * bytes)) {
            return false;
        }
        int32_t idx, lo, hi;
        if (bytes == 2) {
            int16_t b[2];
            memcpy(b, (const void *)(uintptr_t)ea, 4);
            idx = (int16_t)reg(ops[0].reg.value);
            lo = b[0];
            hi = b[1];
        } else {
            int32_t b[2];
            memcpy(b, (const void *)(uintptr_t)ea, 8);
            idx = (int32_t)reg(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, ops[0].reg.value));
            lo = b[0];
            hi = b[1];
        }
        if (idx < lo || idx > hi) {
            return false; /* the bound range exception: the core's */
        }
        break;
    }
    case ZYDIS_MNEMONIC_ENTER: {
        /* with a nesting level (the plain one is translated): the outer
         * frames' pointers copied down, as the processor does */
        const uint32_t size = (uint16_t)ops[0].imm.value.u, level = (uint32_t)ops[1].imm.value.u & 31;
        if (in.operand_width != 32) {
            return false;
        }
        uint32_t esp = (uint32_t)f->gregs[NC_ESP], ebp = (uint32_t)f->gregs[NC_EBP];
        const uint32_t low = esp - 4 * (level + 1);
        if (low > esp || !guest_readable(low, 4 * (level + 1)) || (level > 1 && !guest_readable(ebp - 4 * (level - 1), 4 * (level - 1)))) {
            return false;
        }
        auto push = [&](uint32_t v) {
            esp -= 4;
            memcpy((void *)(uintptr_t)esp, &v, 4);
        };
        push(ebp);
        const uint32_t frame = esp;
        for (uint32_t i = 1; i < level; i++) {
            ebp -= 4;
            uint32_t v;
            memcpy(&v, (const void *)(uintptr_t)ebp, 4);
            push(v);
        }
        if (level) {
            push(frame);
        }
        f->gregs[NC_EBP] = frame;
        f->gregs[NC_ESP] = esp - size;
        break;
    }
    case ZYDIS_MNEMONIC_LEAVE: {
        /* 66 C9: ESP from EBP whole (the stack's address size is 32), BP
         * popped as a word (the operand's) */
        if (in.operand_width != 16) {
            return false;
        }
        const uint32_t esp = (uint32_t)f->gregs[NC_EBP];
        if (!guest_readable(esp, 2)) {
            return false;
        }
        uint16_t bp;
        memcpy(&bp, (const void *)(uintptr_t)esp, 2);
        f->gregs[NC_EBP] = ((uint32_t)f->gregs[NC_EBP] & 0xFFFF0000u) | bp;
        f->gregs[NC_ESP] = esp + 2;
        break;
    }
    default:
        return false;
    }
    f->eflags = fl;
    f->rip = eip + in.length;
    return true;
}

static void x87_copy(uint8_t *dst, const uint8_t *src);

/* The host's MXCSR for C code; the guest's x87 control word left alone. */
extern "C" uint8_t *nc_x86_sysop_entry(NcFrame *f)
{
    NcCpu *cpu = f->cpu;
    const uint32_t info = f->sysop_info;
    f->sysop_info = 0;
    const auto op = (enum NcSysOp)(info & 0xFF);
    const unsigned size = (info >> 8) & 0xFF;
    if (op == NC_SYSOP_INTERRUPT) {
        InterlockedIncrement64(&g_int_sysops);
        if (!f->x87_load) {
            /* An interrupt cuts into any guest code, MMX or x87 code with
             * all eight registers in use among it, and the host code that
             * serves it is not as free of the x87 as save_state hopes:
             * MinGW's printf formats floating point on it (long double),
             * and the core's statistics, formatted every few seconds inside
             * whichever system op comes then, pushed onto the guest's full
             * stack: one of its registers (MM7 first) came back as the
             * indefinite NaN. The registers go to the frame for the
             * interrupt's time and the hardware's are reset; the restore
             * loads them again. Other system ops are at instructions of
             * ordinary code (a kernel call, a port access), where the x87
             * stack has room. */
            alignas(16) uint8_t hw[512];
            __asm__ volatile("fxsave %0" : "=m"(hw));
            x87_copy(f->fx, hw);
            f->x87_load = 2;
            __asm__ volatile("fninit");
        }
    }
    if ((op == NC_SYSOP_OTHER || op == NC_SYSOP_INTO) && emulate(cpu, f)) {
        return next_for(cpu, f);
    }
    if (!cpu->cfg.sysop(cpu->cfg.opaque, f, op, size)) {
        nc_log("hakuX-native", "cpu: unhandled system op %d at %08x", (int)op, (uint32_t)f->rip);
        abort();
    }
    return next_for(cpu, f);
}

extern "C" uint8_t *nc_x86_lookup_entry(NcFrame *f)
{
    return next_for(f->cpu, f);
}

/* --------------------------------------------------------- the poll */

/* The page's protection and poll_armed change together: a request (any
 * thread) and the fault's disarming (the vCPU's) racing left the flag set
 * over a readable page, and no interrupt cut into guest code again (a
 * title spinning on its push buffer's space then waited forever). */
static SRWLOCK g_poll_lock = SRWLOCK_INIT;

static void poll_arm(NcCpu *cpu, bool on)
{
    AcquireSRWLockExclusive(&g_poll_lock);
    if (cpu->poll_armed != (LONG)on) {
        DWORD old;
        VirtualProtect((void *)cpu->poll, 0x1000, on ? PAGE_NOACCESS : PAGE_READONLY, &old);
        cpu->poll_armed = on;
    }
    ReleaseSRWLockExclusive(&g_poll_lock);
}

static Block *block_at_host(NcCpu *cpu, uintptr_t pc)
{
    if (pc < (uintptr_t)cpu->code_begin || pc >= (uintptr_t)cpu->code_p) {
        return nullptr;
    }
    auto it = std::upper_bound(cpu->by_host.begin(), cpu->by_host.end(), pc,
                               [](uintptr_t v, const Block *b) { return v < (uintptr_t)b->code; });
    if (it == cpu->by_host.begin()) {
        return nullptr;
    }
    Block *b = *(it - 1);
    return pc < (uintptr_t)b->code_end ? b : nullptr;
}

/* The guest instruction a host pc in a block is part of. */
static const InsnMap *insn_at(const Block *b, uintptr_t pc)
{
    const uint32_t off = (uint32_t)(pc - (uintptr_t)b->code);
    const InsnMap *best = nullptr;
    for (const InsnMap &m : b->map) {
        if (m.host_off <= off) {
            best = &m;
        } else {
            break;
        }
    }
    return best;
}

static LONG CALLBACK poll_handler(PEXCEPTION_POINTERS ep)
{
    NcCpu *cpu = g_cpu;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (!cpu || er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 ||
        (uintptr_t)er->ExceptionInformation[1] - (uintptr_t)cpu->poll >= 0x1000) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT *ctx = ep->ContextRecord;
    Block *b = block_at_host(cpu, ctx->Rip);
    poll_arm(cpu, false);
    InterlockedIncrement64(&g_poll_faults);
    if (!b) {
        InterlockedIncrement64(&g_poll_lost);
        return EXCEPTION_CONTINUE_EXECUTION; /* not a block's poll: just let it read */
    }
    NcFrame *f = cpu->frame;
    f->rip = b->eip;
    f->sysop_info = NC_SYSOP_INTERRUPT;
    ctx->Rip = (DWORD64)cpu->s_sysop;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* ------------------------------------------------------------ the API */

extern "C" uint32_t nc_frame_gpr(const NcFrame *f, int reg)
{
    return (uint32_t)f->gregs[reg];
}
extern "C" void nc_frame_set_gpr(NcFrame *f, int reg, uint32_t v)
{
    f->gregs[reg] = v;
}
extern "C" uint32_t nc_frame_eip(const NcFrame *f)
{
    return (uint32_t)f->rip;
}
extern "C" void nc_frame_set_eip(NcFrame *f, uint32_t eip)
{
    f->rip = eip;
}
extern "C" bool nc_frame_if(const NcFrame *f)
{
    return f->vif != 0;
}
extern "C" void nc_frame_set_if(NcFrame *f, bool on)
{
    f->vif = on;
}

/* The x87 part of an FXSAVE image: FCW..FDP (0-23) and ST0-7 (32-159). */
static void x87_copy(uint8_t *dst, const uint8_t *src)
{
    memcpy(dst, src, 24);
    memcpy(dst + FX_ST, src + FX_ST, FX_XMM - FX_ST);
}

extern "C" void nc_frame_save(const NcFrame *f, NcCpuContext *c)
{
    SavedState &s = Saved(c);
    if (!f->x87_load) {
        /* the x87 registers are the hardware's (save_state left them) */
        alignas(16) uint8_t hw[512];
        __asm__ volatile("fxsave %0" : "=m"(hw));
        NcFrame *mf = const_cast<NcFrame *>(f);
        x87_copy(mf->fx, hw);
    }
    for (int i = 0; i < 8; i++) {
        s.gregs[i] = (uint32_t)f->gregs[i];
    }
    s.rip = (uint32_t)f->rip;
    s.eflags = ((uint32_t)f->eflags & GUEST_FLAGS) | 0x2 | (f->vif ? 0x200 : 0) | (f->vac ? GUEST_AC : 0);
    memcpy(s.fx, f->fx, sizeof(s.fx));
}

extern "C" void nc_frame_load(NcFrame *f, const NcCpuContext *c)
{
    const SavedState &s = Saved(c);
    for (int i = 0; i < 8; i++) {
        f->gregs[i] = (uint32_t)s.gregs[i];
    }
    f->rip = (uint32_t)s.rip;
    f->eflags = (s.eflags & GUEST_FLAGS) | 0x202;
    f->vif = (s.eflags >> 9) & 1;
    f->vac = (s.eflags >> 18) & 1;
    memcpy(f->fx, s.fx, sizeof(f->fx));
    f->x87_load = 1;
}

static void fx_reset(uint8_t *fx)
{
    memset(fx, 0, 512);
    const uint16_t fcw = 0x037F; /* FNINIT's */
    const uint32_t mxcsr = 0x1F80;
    memcpy(fx + FX_FCW, &fcw, 2);
    memcpy(fx + FX_MXCSR, &mxcsr, 4);
    const uint32_t mask = 0xFFFF; /* MXCSR_MASK */
    memcpy(fx + 28, &mask, 4);
}

extern "C" void nc_context_init(NcCpu *cpu, NcCpuContext *c, uint32_t eip, uint32_t esp)
{
    memset(c, 0, sizeof(*c));
    SavedState &s = Saved(c);
    s.rip = eip;
    s.gregs[NC_ESP] = esp;
    s.eflags = 0x202;
    fx_reset(s.fx);
}

extern "C" void nc_context_set_gpr(NcCpuContext *c, int reg, uint32_t v)
{
    Saved(c).gregs[reg] = v;
}
extern "C" uint32_t nc_context_gpr(const NcCpuContext *c, int reg)
{
    return (uint32_t)Saved(c).gregs[reg];
}
extern "C" uint32_t nc_context_eip(const NcCpuContext *c)
{
    return (uint32_t)Saved(c).rip;
}
extern "C" void nc_context_set_eip(NcCpuContext *c, uint32_t eip)
{
    Saved(c).rip = eip;
}

/* NcCpuState keeps FEX's layout: flags[] one byte per EFLAGS bit (FEX's
 * flag array is indexed by bit), the x87 stack's eight 80-bit registers in
 * mm[] (16 bytes each), FSW in flags[40..41]. */
extern "C" void nc_context_export(const NcCpuContext *c, NcCpuState *st)
{
    const SavedState &s = Saved(c);
    memset(st, 0, sizeof(*st));
    st->rip = s.rip;
    for (int i = 0; i < 8; i++) {
        st->gregs[i] = s.gregs[i];
    }
    for (int bit = 0; bit < 32 && bit < 40; bit++) {
        st->flags[bit] = (s.eflags >> bit) & 1;
    }
    st->pf_raw = (s.eflags & 0x4) ? 0 : 1;
    st->af_raw = s.eflags & 0x10;
    memcpy(&st->flags[40], s.fx + FX_FSW, 2);
    memcpy(st->xmm, s.fx + FX_XMM, 8 * 16);
    memcpy(st->mm, s.fx + FX_ST, 8 * 16);
    memcpy(&st->mxcsr, s.fx + FX_MXCSR, 4);
    memcpy(&st->fcw, s.fx + FX_FCW, 2);
    st->ftw = s.fx[FX_FTW];
    st->cs_idx = 0x08;
    st->ds_idx = st->es_idx = st->ss_idx = 0x10;
    st->fs_idx = 0x28;
}

extern "C" void nc_context_import(NcCpuContext *c, const NcCpuState *st)
{
    memset(c, 0, sizeof(*c));
    SavedState &s = Saved(c);
    s.rip = (uint32_t)st->rip;
    for (int i = 0; i < 8; i++) {
        s.gregs[i] = (uint32_t)st->gregs[i];
    }
    uint32_t fl = 0;
    for (int bit = 0; bit < 32; bit++) {
        fl |= (uint32_t)(st->flags[bit] & 1) << bit;
    }
    s.eflags = (fl & (GUEST_FLAGS | GUEST_AC | 0x200)) | 0x2;
    fx_reset(s.fx);
    memcpy(s.fx + FX_FSW, &st->flags[40], 2);
    memcpy(s.fx + FX_XMM, st->xmm, 8 * 16);
    memcpy(s.fx + FX_ST, st->mm, 8 * 16);
    /* only the bits MXCSR has: a reserved one (a damaged state's) faults
     * the FXRSTOR that brings this context back, in the emulator's own code */
    const uint32_t mxcsr = st->mxcsr & 0xFFFFu;
    memcpy(s.fx + FX_MXCSR, &mxcsr, 4);
    memcpy(s.fx + FX_FCW, &st->fcw, 2);
    s.fx[FX_FTW] = st->ftw;
}

extern "C" NcCpu *nc_cpu_create(const NcCpuConfig *cfg, uint32_t fs_base)
{
    if (cfg->membase != nullptr) {
        nc_log("hakuX-native", "cpu: guest memory must be at host address 0 (it is at %p)", (void *)cfg->membase);
        return nullptr;
    }
    auto *cpu = new NcCpu();
    cpu->cfg = *cfg;
    cpu->fs_base = fs_base;
    ZydisDecoderInit(&cpu->dec, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);

    cpu->region = (uint8_t *)VirtualAlloc(nullptr, REGION_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!cpu->region) {
        nc_log("hakuX-native", "cpu: no translation buffer (%lu)", GetLastError());
        delete cpu;
        return nullptr;
    }
    cpu->poll = cpu->region + REGION_POLL;
    DWORD old;
    VirtualProtect((void *)cpu->poll, 0x1000, PAGE_READONLY, &old);
    cpu->l1 = (L1Entry *)(cpu->region + REGION_L1);
    cpu->code_begin = cpu->code_p = cpu->region + REGION_CODE;
    cpu->code_end = cpu->region + REGION_SIZE;
    if (!build_stubs(cpu)) {
        delete cpu;
        return nullptr;
    }
    l1_clear(cpu);

    auto *f = (NcFrame *)_aligned_malloc(sizeof(NcFrame), 64);
    memset(f, 0, sizeof(*f));
    f->cpu = cpu;
    f->vif = 1;
    f->eflags = 0x202;
    f->tsc_ratio = tsc_ratio_q32();
    f->cycle_offset = rdtsc_now();
    f->host_mxcsr = 0x1F80;
    fx_reset(f->fx);
    cpu->frame = f;
    g_cpu = cpu;
#ifdef NC_CPU_DIAG
    if (const char *e = getenv("NC_SMC_CHECK")) {
        g_smc_check = atoi(e) != 0;
        if (g_smc_check) {
            nc_log("hakuX-native", "cpu: NC_SMC_CHECK: every block checked against its code as it is entered (slow)");
        }
    }
#endif
    cpu->veh = AddVectoredExceptionHandler(1, poll_handler);
    nc_log("hakuX-native", "cpu: x86-64 translator, TSC %.1f MHz, KPCR at %08x", tsc_hz() / 1e6, fs_base);
    return cpu;
}

/* ------------------------------------------------------------ profile */

#ifdef _WIN32
/* HAKUX_PROF=<start s>,<seconds> (debug): the vCPU thread sampled at about
 * 2 kHz over that window (suspended, its RIP read), and where its time
 * went logged at the end: translated code (and its hottest guest blocks),
 * the stubs, this executable's functions (offsets for llvm-symbolizer:
 * tools/native/winprof.sh), other modules by name. */
extern "C" volatile int g_nc_idle_waiting; /* core/irq.c: the guest CPU's idle wait */
extern "C" volatile int g_nc_in_sysop;     /* kernel/kernel.c: inside a system op */

struct ProfArgs {
    NcCpu *cpu;
    unsigned start_s, secs;
};

static DWORD WINAPI prof_thread(void *arg)
{
    ProfArgs pa = *(ProfArgs *)arg;
    delete (ProfArgs *)arg;
    Sleep(pa.start_s * 1000);
    DWORD tid = pa.cpu->thread_id;
    if (const char *want = getenv("HAKUX_PROF_THREAD")) {
        /* another thread by its name (SetThreadDescription's), the same report */
        typedef HRESULT(WINAPI * GetDesc)(HANDLE, PWSTR *);
        auto get = (GetDesc)(void *)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "GetThreadDescription");
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {sizeof(te)};
        for (BOOL more = Thread32First(snap, &te); get && more; more = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId()) {
                continue;
            }
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            PWSTR d = nullptr;
            if (h && SUCCEEDED(get(h, &d)) && d) {
                char n[64];
                WideCharToMultiByte(CP_UTF8, 0, d, -1, n, sizeof(n), nullptr, nullptr);
                if (!strcmp(n, want)) {
                    tid = te.th32ThreadID;
                }
                LocalFree(d);
            }
            if (h) {
                CloseHandle(h);
            }
        }
        CloseHandle(snap);
        nc_log("hakuX-prof", "sampling thread \"%s\" (%lu)", want, tid);
    }
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!th) {
        return 0;
    }
    HMODULE self = GetModuleHandleW(nullptr);
    const auto *nt = (const IMAGE_NT_HEADERS *)((const uint8_t *)self + ((const IMAGE_DOS_HEADER *)self)->e_lfanew);
    const uintptr_t self_end = (uintptr_t)self + nt->OptionalHeader.SizeOfImage;
    /* guest: the blocks; insn: the guest instructions in them; under: where
     * in this executable a sample in another module (a wait: ntdll's) was
     * called from */
    std::unordered_map<uint32_t, unsigned> guest, exe, insn;
    std::unordered_map<std::string, unsigned> mods, under;
    uint32_t chain_pc[7];
    int chain_n = 0;
    unsigned n = 0, jit = 0, stubs = 0, in_exe = 0, other = 0, idle = 0, sysop = 0;
    const ULONGLONG end = GetTickCount64() + pa.secs * 1000ull;
    ULONG64 cyc0 = 0, cyc1 = 0;
    QueryThreadCycleTime(th, &cyc0);
    const uint64_t tsc0 = rdtsc_now();
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    while (GetTickCount64() < end) {
        QueryPerformanceCounter(&t);
        const LONGLONG next = t.QuadPart + f.QuadPart / 2000;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            const bool ok = GetThreadContext(th, &ctx);
            if (ok && !block_at_host(pa.cpu, ctx.Rip) &&
                (ctx.Rip < (uintptr_t)self || ctx.Rip >= self_end) &&
                (ctx.Rip < (uintptr_t)pa.cpu->region || ctx.Rip >= (uintptr_t)pa.cpu->region + REGION_SIZE)) {
                /* another module's: unwound (the thread still suspended) to this executable */
                /* nothing allocated while it is suspended (it may hold the heap's lock) */
                CONTEXT c = ctx;
                int in_self = 0;
                for (int k = 0; k < 32 && c.Rip && in_self < 7; k++) {
                    if (c.Rip >= (uintptr_t)self && c.Rip < self_end) {
                        chain_pc[in_self++] = (uint32_t)(c.Rip - (uintptr_t)self);
                    }
                    DWORD64 base = 0;
                    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
                    if (!fe) {
                        c.Rip = *(DWORD64 *)c.Rsp;
                        c.Rsp += 8;
                    } else {
                        void *hd = nullptr;
                        DWORD64 est = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &est, nullptr);
                    }
                }
                chain_n = in_self;
            }
            ResumeThread(th);
            if (chain_n) {
                /* this executable's first frames under the other module's, callee first */
                char key[96];
                int o = 0;
                for (int k = 0; k < chain_n; k++) {
                    o += snprintf(key + o, sizeof(key) - (size_t)o, "%s%x", k ? "<" : "", chain_pc[k]);
                }
                under[key]++;
                chain_n = 0;
            }
            if (ok) {
                const uintptr_t pc = ctx.Rip;
                n++;
                Block *b = block_at_host(pa.cpu, pc);
                sysop += g_nc_in_sysop != 0;
                if (tid == pa.cpu->thread_id && g_nc_idle_waiting) {
                    idle++;
                } else if (b) {
                    jit++;
                    guest[b->eip]++;
                    if (const InsnMap *m = insn_at(b, pc)) {
                        insn[m->eip]++;
                    }
                } else if (pc >= (uintptr_t)pa.cpu->region + REGION_STUBS && pc < (uintptr_t)pa.cpu->region + REGION_CODE) {
                    stubs++;
                } else {
                    HMODULE m = nullptr;
                    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)pc, &m);
                    if (m == self) {
                        in_exe++;
                        exe[(uint32_t)((pc - (uintptr_t)self) & ~0xFull)]++;
                    } else {
                        other++;
                        char name[MAX_PATH] = "?";
                        if (m) {
                            GetModuleFileNameA(m, name, sizeof(name));
                        }
                        const char *base = strrchr(name, '\\');
                        mods[base ? base + 1 : name]++;
                    }
                }
            }
        }
        do {
            QueryPerformanceCounter(&t);
        } while (t.QuadPart < next);
    }
    QueryThreadCycleTime(th, &cyc1);
    const uint64_t tsc1 = rdtsc_now();
    CloseHandle(th);
    if (!n) {
        return 0;
    }
    nc_log("hakuX-prof", "the vCPU thread ran %.1f%% of the time (%.0f M cycles a second)",
           100.0 * (double)(cyc1 - cyc0) / (double)(tsc1 - tsc0), (double)(cyc1 - cyc0) / pa.secs / 1e6);
    auto pct = [n](unsigned v) { return 100.0 * v / n; };
    nc_log("hakuX-prof", "%u samples: idle (waiting for an interrupt) %.1f%%; busy: translated code %.1f%%, stubs %.1f%%, "
           "hakux.exe %.1f%%, other modules %.1f%% (in system ops %.1f%%)", n, pct(idle), pct(jit), pct(stubs),
           pct(in_exe), pct(other), pct(sysop));
    auto top = [&](auto &map, const char *what, unsigned max) {
        std::vector<std::pair<unsigned, typename std::decay_t<decltype(map)>::key_type>> v;
        for (auto &e : map) {
            v.push_back({e.second, e.first});
        }
        std::sort(v.begin(), v.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
        for (unsigned i = 0; i < v.size() && i < max; i++) {
            if constexpr (std::is_same_v<std::decay_t<decltype(v[i].second)>, std::string>) {
                nc_log("hakuX-prof", "  %s %s %.1f%%", what, v[i].second.c_str(), pct(v[i].first));
            } else {
                nc_log("hakuX-prof", "  %s %x %.1f%%", what, (unsigned)v[i].second, pct(v[i].first));
            }
        }
    };
    top(mods, "module", 8);
    top(under, "other modules under exe+", 12);
    top(exe, "exe+", 40);
    top(guest, "guest block", 20);
    /* the hottest guest instructions, decoded from guest memory as it is now */
    std::vector<std::pair<unsigned, uint32_t>> hot;
    for (auto &e : insn) {
        hot.push_back({e.second, e.first});
    }
    std::sort(hot.begin(), hot.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
    ZydisFormatter fmt;
    ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);
    for (unsigned i = 0; i < hot.size() && i < 40; i++) {
        const uint32_t at = hot[i].second;
        char txt[96] = "?";
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (guest_readable(at, 16) &&
            ZYAN_SUCCESS(ZydisDecoderDecodeFull(&pa.cpu->dec, (const void *)(uintptr_t)at, 16, &in, ops))) {
            ZydisFormatterFormatInstruction(&fmt, &in, ops, in.operand_count_visible, txt, sizeof(txt), at, nullptr);
        }
        nc_log("hakuX-prof", "  insn %08x %.1f%%  %s", at, pct(hot[i].first), txt);
    }
    return 0;
}

#endif

extern "C" void nc_cpu_prof_start(unsigned secs)
{
#ifndef _WIN32
    (void)secs;
    nc_log("hakuX-native", "cpu: the profile is Windows-only");
    return;
#endif
#ifdef _WIN32
    if (g_cpu && g_cpu->thread_id && secs) {
        CreateThread(nullptr, 0, prof_thread, new ProfArgs{g_cpu, 0, secs}, 0, nullptr);
    }
#endif
}

extern "C" void nc_cpu_run(NcCpu *cpu, const NcCpuContext *c)
{
    NcFrame *f = cpu->frame;
    cpu->thread_id = GetCurrentThreadId();
#if HAKUX_DIAG && defined(_WIN32)
    /* HAKUX_PROF (a build made to investigate): the environment was read at
     * every entry here, one for each kernel call's return */
    static bool prof_started;
    const char *pe = prof_started ? nullptr : getenv("HAKUX_PROF");
    unsigned ps = 0, pn = 0;
    if (pe && sscanf(pe, "%u,%u", &ps, &pn) == 2 && pn) {
        prof_started = true;
        CreateThread(nullptr, 0, prof_thread, new ProfArgs{cpu, ps, pn}, 0, nullptr);
    }
#endif
    nc_frame_load(f, c);
    f->next_code = (uint64_t)(uintptr_t)next_for(cpu, f);
    reinterpret_cast<void (*)(NcFrame *)>(cpu->s_enter)(f);
}

extern "C" void nc_cpu_invalidate(NcCpu *cpu, uint32_t va, uint32_t len)
{
    if (!len) {
        return;
    }
    const uint32_t first = va >> 12;
    const uint32_t last = (uint32_t)std::min<uint64_t>(((uint64_t)va + len - 1) >> 12, 0xFFFFFu);
    for (uint32_t w = first >> 6; w <= (last >> 6); w++) {
        uint64_t bits = cpu->page_bits[w];
        if (w == (first >> 6)) {
            bits &= ~0ull << (first & 63);
        }
        if (w == (last >> 6)) {
            bits &= ~0ull >> (63 - (last & 63));
        }
        while (bits) {
            const unsigned bit = (unsigned)__builtin_ctzll(bits);
            bits &= bits - 1;
            cpu->page_bits[w] &= ~(1ull << bit);
            auto it = cpu->pages.find((w << 6) | bit);
            if (it == cpu->pages.end()) {
                continue;
            }
            for (Block *b : it->second) {
                kill_block(cpu, b);
            }
            cpu->pages.erase(it);
        }
    }
}

#ifdef NC_CPU_DIAG
/* Tests: a smaller translation buffer, to reach its flush. */
extern "C" void nc_cpu_set_code_limit(NcCpu *cpu, size_t bytes)
{
    if (bytes >= (128u << 10) && bytes <= CODE_SIZE) {
        cpu->code_end = cpu->code_begin + bytes;
        if (cpu->code_p > cpu->code_end) {
            flush_all(cpu); /* between runs: what is translated is past the new end */
        }
    }
}
#endif

extern "C" bool nc_cpu_x87_single_check(NcCpu *cpu)
{
    return false; /* the host's x87 rounds as the control word says */
}

extern "C" void nc_cpu_request_interrupt(NcCpu *cpu)
{
    NcFrame *f = cpu->frame;
    __atomic_store_n(&f->interrupt_pending, 1u, __ATOMIC_RELEASE);
    InterlockedIncrement64(&g_int_requests);
    if (!cpu->poll_armed) {
        poll_arm(cpu, true);
    }
}

extern "C" void nc_cpu_cycle_offset_add(NcCpu *cpu, uint64_t ticks)
{
    cpu->frame->cycle_offset += ticks;
}

extern "C" uint64_t nc_cpu_host_cycles(void)
{
    return rdtsc_now();
}

extern "C" uint64_t nc_cpu_guest_cycles(NcCpu *cpu)
{
    return rdtsc_now() - cpu->frame->cycle_offset;
}

extern "C" void nc_cpu_guest_cycles_set(NcCpu *cpu, uint64_t ticks)
{
    cpu->frame->cycle_offset = rdtsc_now() - ticks;
}

extern "C" bool nc_cpu_x87_single(NcCpu *cpu)
{
    return false;
}

extern "C" void nc_cpu_x87_single_restore(NcCpu *cpu)
{
}

extern "C" void nc_frame_clear_interrupt(NcFrame *f)
{
    __atomic_store_n(&f->interrupt_pending, 0u, __ATOMIC_RELAXED);
}

extern "C" void nc_frame_bypass_hook(NcFrame *f)
{
    f->hook_bypass = (uint32_t)f->rip;
}

extern "C" uint16_t nc_frame_cs(const NcFrame *f)
{
    return 0x08;
}

extern "C" uint32_t nc_frame_eflags(NcFrame *f)
{
    return ((uint32_t)f->eflags & GUEST_FLAGS) | 0x2 | (f->vif ? 0x200 : 0) | (f->vac ? GUEST_AC : 0);
}

extern "C" void nc_frame_set_eflags(NcFrame *f, uint32_t eflags)
{
    f->eflags = (eflags & GUEST_FLAGS) | 0x202;
    f->vif = (eflags >> 9) & 1;
    f->vac = (eflags >> 18) & 1;
}

extern "C" uint64_t *nc_frame_regs(NcFrame *f)
{
    return f->gregs;
}

extern "C" uint64_t *nc_frame_rip(NcFrame *f)
{
    return &f->rip;
}

extern "C" uint32_t nc_cpu_guest_eip_at(NcCpu *cpu, uintptr_t host_pc)
{
    if (!cpu) {
        return 0;
    }
    Block *b = block_at_host(cpu, host_pc);
    const InsnMap *m = b ? insn_at(b, host_pc) : nullptr;
    return m ? m->eip : (uint32_t)cpu->frame->rip;
}

extern "C" bool nc_cpu_sample_pc(NcCpu *cpu, uintptr_t host_pc, uint32_t *eip)
{
    if (!cpu) {
        *eip = 0;
        return false;
    }
    Block *b = block_at_host(cpu, host_pc);
    const InsnMap *m = b ? insn_at(b, host_pc) : nullptr;
    *eip = m ? m->eip : (uint32_t)cpu->frame->rip;
    return m != nullptr;
}

/* The faulting access's direction, which a CONTEXT doesn't hold: the
 * front end's exception handler notes it before the core looks. */
static __thread int t_fault_write;

extern "C" void nc_cpu_fault_note_write(int write)
{
    t_fault_write = write;
}

extern "C" bool nc_cpu_fault_is_write(const void *ucontext)
{
    return t_fault_write != 0;
}

extern "C" NcFrame *nc_cpu_fault_frame(NcCpu *cpu, void *ucontext)
{
    CONTEXT *ctx = static_cast<CONTEXT *>(ucontext);
    if (!cpu || GetCurrentThreadId() != cpu->thread_id) {
        return nullptr;
    }
    Block *b = block_at_host(cpu, ctx->Rip);
    const InsnMap *m = b ? insn_at(b, ctx->Rip) : nullptr;
    if (!m) {
        return nullptr;
    }
    NcFrame *f = cpu->frame;
    const DWORD64 regs[8] = {ctx->Rax, ctx->Rcx, ctx->Rdx, ctx->Rbx, ctx->R12, ctx->Rbp, ctx->Rsi, ctx->Rdi};
    for (int i = 0; i < 8; i++) {
        f->gregs[i] = (uint32_t)regs[i];
    }
    if (m->swap) {
        /* faulted between the exchanges: ESP is in the borrowed register */
        const uint64_t t = f->gregs[m->swap];
        f->gregs[m->swap] = f->gregs[NC_ESP];
        f->gregs[NC_ESP] = t;
    }
    if (m->borrow) {
        /* the register holds an FS operand's address: its value is in R13 */
        f->gregs[m->borrow - 1] = (uint32_t)ctx->R13;
    }
    f->rip = m->eip;
    f->eflags = (ctx->EFlags & GUEST_FLAGS) | 0x202;
    memcpy(f->fx, &ctx->FltSave, sizeof(f->fx));
    f->x87_load = 1;
    return f;
}

extern "C" void nc_cpu_fault_resume(NcCpu *cpu, void *ucontext)
{
    CONTEXT *ctx = static_cast<CONTEXT *>(ucontext);
    NcFrame *f = cpu->frame;
    f->next_code = (uint64_t)(uintptr_t)cpu->s_dispatch_rip;
    ctx->Rip = (DWORD64)cpu->s_restore;
    ctx->Rsp = f->host_rsp;
    ctx->R15 = (DWORD64)f;
}

extern "C" bool nc_cpu_unaligned_fault(NcCpu *cpu, int code, void *ucontext)
{
    return false; /* x86 hosts do unaligned atomics */
}

extern "C" bool nc_cpu_callret_fault(NcCpu *cpu, uintptr_t fault_addr, void *ucontext)
{
    return false; /* no call-ret shadow stack */
}

/* Whether a host pc is translated code (the front end's fault reports). */
extern "C" bool nc_cpu_in_code(NcCpu *cpu, uintptr_t pc)
{
    return cpu && pc >= (uintptr_t)cpu->region && pc < (uintptr_t)cpu->region + REGION_SIZE;
}

extern "C" uintptr_t nc_cpu_context_pc(const void *ucontext)
{
    return (uintptr_t)static_cast<const CONTEXT *>(ucontext)->Rip;
}

extern "C" NcCpu *nc_cpu_current(void)
{
    return g_cpu;
}
