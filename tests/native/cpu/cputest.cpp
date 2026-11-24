/*
 * Host test of the PC's guest CPU (native/cpu/cpu_x86.cpp, the x86-32 to
 * x86-64 translator) against the host processor itself.
 *
 * Every test is a piece of 32-bit code. It runs twice from the same
 * registers, flags, x87/MMX/SSE state and memory: once on the processor, in
 * compatibility mode (a far jump to the 32-bit code segment that 64-bit
 * Windows and Linux give every process), and once through the translator.
 * The results must be equal: the eight registers, the flags an instruction
 * defines, the x87, MMX and SSE registers with their control and status
 * words, the data area and the stack. So the reference is real hardware,
 * not what this test's author believes an instruction does. (Flags an
 * instruction leaves undefined are the host's, not a Pentium III's: tests
 * mask them.)
 *
 * Tests that can't run on the processor (FS-relative accesses, which need
 * the KPCR's base; segment register values) run the same code without the
 * part the translator adds, or check values given here. Known gaps are
 * listed as such and don't fail the run.
 *
 *   cmake --build <build> --target cputest && <build>/cputest [-v] [name...]
 *   <build>/cputest --bench     what the translator's transitions cost
 *
 * Exit code: the number of failed tests.
 */
#include "native/cpu/cpu.h"
#include "native/cpu/x64asm.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

/* ------------------------------------------- what cpu_x86.cpp links to */

static bool g_verbose;

extern "C" {
volatile int g_nc_idle_waiting;
volatile int g_nc_in_sysop;
void nc_log(const char *tag, const char *fmt, ...)
{
    if (!g_verbose) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  [%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
}

/* ------------------------------------------------------ the guest's memory */

enum : uint32_t {
    BASE = 0x10000000u,
    CODE = BASE + 0x1000,       /* the test's code */
    STOP = BASE + 0xF000,       /* a HLT: where a translated run ends */
    DATA = BASE + 0x10000,
    DATA_SIZE = 0x10000,
    FSBASE = DATA + 0x8000,     /* the translator's FS base (the KPCR's place) */
    STACK = BASE + 0x20000,
    STACK_SIZE = 0x10000,
    ESP0 = STACK + 0xC000,
    TRAMP = BASE + 0x30000,     /* the way into and out of compatibility mode */
    TOTAL = 0x40000,
};

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

struct State {
    uint32_t r[8];
    uint32_t eflags;
    uint32_t pad[7];            /* zero: the flags are pushed as a quadword */
    alignas(16) uint8_t fx[512];
};

/* What is compared of the x87/MMX/SSE state. */
struct Fp {
    uint16_t fcw, fsw;
    uint8_t ftw;
    uint32_t mxcsr;
    uint8_t st[8][10];
    uint8_t xmm[8][16];
};

static Fp fp_of_fx(const uint8_t *fx)
{
    Fp f;
    memset(&f, 0, sizeof(f));
    memcpy(&f.fcw, fx + 0, 2);
    memcpy(&f.fsw, fx + 2, 2);
    f.ftw = fx[4];
    memcpy(&f.mxcsr, fx + 24, 4);
    for (int i = 0; i < 8; i++) {
        memcpy(f.st[i], fx + 32 + 16 * i, 10);
        memcpy(f.xmm[i], fx + 160 + 16 * i, 16);
    }
    return f;
}

static uint64_t g_rng;
static uint32_t rnd()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static void fill_mem(uint64_t seed)
{
    uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
    uint8_t *d = (uint8_t *)(uintptr_t)DATA;
    for (uint32_t i = 0; i < DATA_SIZE; i += 8) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        memcpy(d + i, &x, 8);
    }
    memset((void *)(uintptr_t)STACK, 0x5A, STACK_SIZE);
}

/* ---------------------------------------------- the processor's own run */

static State *g_nat_out;
static uint64_t *g_host_rsp;
static uint8_t *g_host_fx;      /* the host's own x87/SSE state (XMM6-15 are the caller's) */
static uint8_t *g_landing;
static void (*g_native)(const State *);

/* native(in): the registers, flags and x87/SSE state loaded, a far jump to
 * the code at CODE in the 32-bit segment; the test's last instruction is a
 * far jump back to the landing, which stores it all in g_nat_out. */
static bool build_native()
{
    using namespace x64;
    uint8_t *t = (uint8_t *)(uintptr_t)TRAMP;
    g_host_rsp = (uint64_t *)(t + 0x700);
    g_host_fx = t + 0xC00;
    g_nat_out = (State *)(t + 0x800);
    Asm a(t, t + 0x600);
    static const ZydisRegister saved[8] = {ZYDIS_REGISTER_RBX, ZYDIS_REGISTER_RBP, ZYDIS_REGISTER_RDI,
                                           ZYDIS_REGISTER_RSI, ZYDIS_REGISTER_R12, ZYDIS_REGISTER_R13,
                                           ZYDIS_REGISTER_R14, ZYDIS_REGISTER_R15};
    static const ZydisRegister r32[8] = {ZYDIS_REGISTER_EAX, ZYDIS_REGISTER_ECX, ZYDIS_REGISTER_EDX,
                                         ZYDIS_REGISTER_EBX, ZYDIS_REGISTER_ESP, ZYDIS_REGISTER_EBP,
                                         ZYDIS_REGISTER_ESI, ZYDIS_REGISTER_EDI};
    g_native = reinterpret_cast<void (*)(const State *)>(a.p);
    for (ZydisRegister r : saved) {
        a.op(ZYDIS_MNEMONIC_PUSH, {R(r)});
    }
#ifdef _WIN32
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R8), R(ZYDIS_REGISTER_RCX)});
#else
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_R8), R(ZYDIS_REGISTER_RDI)});
#endif
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RAX), I((int64_t)(uintptr_t)g_host_rsp)});
    a.op(ZYDIS_MNEMONIC_MOV, {M(ZYDIS_REGISTER_RAX, 0, 8), R(ZYDIS_REGISTER_RSP)});
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RAX), I((int64_t)(uintptr_t)g_host_fx)});
    a.op(ZYDIS_MNEMONIC_FXSAVE, {M(ZYDIS_REGISTER_RAX, 0, 512)});
    a.op(ZYDIS_MNEMONIC_FXRSTOR, {M(ZYDIS_REGISTER_R8, offsetof(State, fx), 512)});
    a.op(ZYDIS_MNEMONIC_PUSH, {M(ZYDIS_REGISTER_R8, offsetof(State, eflags), 8)});
    a.op(ZYDIS_MNEMONIC_POPFQ, {});
    for (int i = 0; i < 8; i++) {
        if (i != ESP) {
            a.op(ZYDIS_MNEMONIC_MOV, {R(r32[i]), M(ZYDIS_REGISTER_R8, 4 * i, 4)});
        }
    }
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_ESP), M(ZYDIS_REGISTER_R8, 4 * ESP, 4)});
    a.bytes({0xFF, 0x2D, 0, 0, 0, 0});  /* jmp far [rip + 0]: the pointer follows */
    a.u32(CODE);
    a.bytes({0x23, 0x00});

    /* the landing: 64-bit code again, on the guest's stack still */
    g_landing = a.p;
    a.byte(0xA3);                       /* mov [moffs64], eax */
    a.u64((uint64_t)(uintptr_t)&g_nat_out->r[EAX]);
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RAX), I((int64_t)(uintptr_t)g_nat_out)});
    for (int i = 1; i < 8; i++) {
        a.op(ZYDIS_MNEMONIC_MOV, {M(ZYDIS_REGISTER_RAX, 4 * i, 4), R(r32[i])});
    }
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RCX), I((int64_t)(uintptr_t)g_host_rsp)});
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RSP), M(ZYDIS_REGISTER_RCX, 0, 8)});
    a.op(ZYDIS_MNEMONIC_PUSHFQ, {});
    a.op(ZYDIS_MNEMONIC_POP, {R(ZYDIS_REGISTER_RCX)});
    a.op(ZYDIS_MNEMONIC_MOV, {M(ZYDIS_REGISTER_RAX, offsetof(State, eflags), 4), R(ZYDIS_REGISTER_ECX)});
    a.op(ZYDIS_MNEMONIC_FXSAVE, {M(ZYDIS_REGISTER_RAX, offsetof(State, fx), 512)});
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RCX), I((int64_t)(uintptr_t)g_host_fx)});
    a.op(ZYDIS_MNEMONIC_FXRSTOR, {M(ZYDIS_REGISTER_RCX, 0, 512)});
    a.op(ZYDIS_MNEMONIC_CLD, {});
    for (int i = 7; i >= 0; i--) {
        a.op(ZYDIS_MNEMONIC_POP, {R(saved[i])});
    }
    a.op(ZYDIS_MNEMONIC_RET, {});
    return !a.failed;
}

/* ------------------------------------------------------- the translator's */

static NcCpu *g_cpu;
static NcCpuContext g_ctx_out;
static uint32_t g_epilogue;     /* the test's last instruction (the far jump back) */
static int g_stop_op;           /* 0: the run reached the epilogue; else the system op it left with */
static uint32_t g_stop_eip;

static bool g_bench_irq;        /* the benchmark: an interrupt after each system op */

static bool on_sysop(void *opaque, NcFrame *f, enum NcSysOp op, unsigned size)
{
    const uint32_t eip = nc_frame_eip(f);
    if (op == NC_SYSOP_INTERRUPT) {
        /* what host code may do while it serves an interrupt: MinGW's printf
         * formats floating point on the x87 */
        nc_frame_clear_interrupt(f);
        __asm__ volatile("fldz\n fld1\n fldpi\n fstp %%st(0)\n fstp %%st(0)\n fstp %%st(0)" : : : "st", "st(1)", "st(2)", "st(3)",
                         "st(4)", "st(5)", "st(6)", "st(7)");
        return true;
    }
    if (op == NC_SYSOP_CLI) {
        /* the benchmark's system op: nothing done, back to the next instruction */
        nc_frame_set_eip(f, eip + size);
        if (g_bench_irq) {
            nc_cpu_request_interrupt(g_cpu);
        }
        return true;
    }
    if (op == NC_SYSOP_OTHER && eip != g_epilogue) {
        const uint8_t *ib = (const uint8_t *)(uintptr_t)eip;
        if (ib[0] == 0x0F && ib[1] == 0x00 && (ib[2] & 0xF8) == 0xE0) {
            /* VERR r16, a system op here and harmless on the processor: a
             * test's way to have an interrupt taken at the next block */
            nc_cpu_request_interrupt(g_cpu);
            nc_frame_set_eip(f, eip + size);
            return true;
        }
        if (ib[0] == 0x0F && ib[1] == 0x00 && (ib[2] & 0xF8) == 0xE8) {
            /* VERW r16, likewise: the code was changed, as a kernel call
             * that invalidates it would tell the CPU from inside a call */
            nc_cpu_invalidate(g_cpu, BASE, 0x10000);
            nc_frame_set_eip(f, eip + size);
            return true;
        }
    }
    if (op == NC_SYSOP_OTHER && eip == g_epilogue) {
        g_stop_op = 0;
    } else {
        g_stop_op = (int)op;
        g_stop_eip = eip;
    }
    nc_frame_save(f, &g_ctx_out);
    nc_frame_set_eip(f, STOP);
    return true;
}

static void translated(const State &in, State &out)
{
    NcCpuState st;
    memset(&st, 0, sizeof(st));
    st.rip = CODE;
    for (int i = 0; i < 8; i++) {
        st.gregs[i] = in.r[i];
    }
    for (int bit = 0; bit < 32; bit++) {
        st.flags[bit] = (in.eflags >> bit) & 1;
    }
    memcpy(&st.flags[40], in.fx + 2, 2);
    memcpy(st.xmm, in.fx + 160, 8 * 16);
    memcpy(st.mm, in.fx + 32, 8 * 16);
    memcpy(&st.mxcsr, in.fx + 24, 4);
    memcpy(&st.fcw, in.fx + 0, 2);
    st.ftw = in.fx[4];
    NcCpuContext ctx;
    nc_context_import(&ctx, &st);
    nc_cpu_invalidate(g_cpu, BASE, 0x10000);    /* the code changed: the interface's contract */
    g_stop_op = -1;
    nc_cpu_run(g_cpu, &ctx);

    NcCpuState so;
    nc_context_export(&g_ctx_out, &so);
    memset(&out, 0, sizeof(out));
    for (int i = 0; i < 8; i++) {
        out.r[i] = (uint32_t)so.gregs[i];
    }
    for (int bit = 0; bit < 32; bit++) {
        out.eflags |= (uint32_t)(so.flags[bit] & 1) << bit;
    }
    memcpy(out.fx + 0, &so.fcw, 2);
    memcpy(out.fx + 2, &so.flags[40], 2);
    out.fx[4] = so.ftw;
    memcpy(out.fx + 24, &so.mxcsr, 4);
    memcpy(out.fx + 32, so.mm, 8 * 16);
    memcpy(out.fx + 160, so.xmm, 8 * 16);
}

/* ---------------------------------------------------------------- tests */

struct Code {
    std::vector<uint8_t> b;
    Code() = default;
    Code(std::initializer_list<uint8_t> v) : b(v) {}
    Code &operator()(std::initializer_list<uint8_t> v)
    {
        b.insert(b.end(), v);
        return *this;
    }
    Code &d(uint32_t v)         /* a doubleword */
    {
        for (int i = 0; i < 4; i++) {
            b.push_back((uint8_t)(v >> (8 * i)));
        }
        return *this;
    }
    Code &w(uint16_t v)
    {
        b.push_back((uint8_t)v);
        b.push_back((uint8_t)(v >> 8));
        return *this;
    }
    Code &at(size_t off)        /* NOPs up to an offset */
    {
        while (b.size() < off) {
            b.push_back(0x90);
        }
        return *this;
    }
    Code &rel(uint32_t target)  /* a rel32 to an offset of this code */
    {
        return d(target - (uint32_t)(b.size() + 4));
    }
};

#define ARITH 0x8D5u            /* OF SF ZF AF PF CF */
#define FLAGS_ALL 0xCD5u        /* and DF */

struct Test {
    std::string name;
    Code code;
    uint32_t fmask = FLAGS_ALL;
    int iters = 6;
    bool no_native = false;     /* translated only: check() says */
    const char *gap = nullptr;  /* a known gap: reported, not failed */
    Code native;                /* other bytes for the processor's run (empty: the same) */
    std::function<void(State &)> setup;
    std::function<bool(const State &in, const State &out, std::string &why)> check;
    std::vector<std::pair<uint32_t, uint32_t>> ignore; /* memory not compared: address, length */
    bool fp = true;             /* the x87/SSE state compared */
    size_t code_limit = 0;      /* the translation buffer's size for this test */
};

static std::vector<Test> g_tests;

static Test &T(const char *name, Code code)
{
    g_tests.emplace_back();
    Test &t = g_tests.back();
    t.name = name;
    t.code = std::move(code);
    return t;
}

static void put_code(const Code &c)
{
    uint8_t *p = (uint8_t *)(uintptr_t)BASE;
    memset(p, 0xCC, 0x10000);
    memcpy((void *)(uintptr_t)CODE, c.b.data(), c.b.size());
    uint8_t *e = (uint8_t *)(uintptr_t)CODE + c.b.size();
    g_epilogue = CODE + (uint32_t)c.b.size();
    e[0] = 0xEA;                /* jmp far 0x33:landing */
    const uint32_t land = (uint32_t)(uintptr_t)g_landing;
    memcpy(e + 1, &land, 4);
    e[5] = 0x33;
    e[6] = 0x00;
    *(uint8_t *)(uintptr_t)STOP = 0xF4;
}

static void default_state(State &s)
{
    memset(&s, 0, sizeof(s));
    s.r[EAX] = rnd();
    s.r[ECX] = rnd();
    s.r[EDX] = rnd();
    s.r[EBX] = DATA + 0x1000;
    s.r[ESP] = ESP0;
    s.r[EBP] = DATA + 0x4000;
    s.r[ESI] = DATA + 0x2000;
    s.r[EDI] = DATA + 0x3000;
    s.eflags = 0x202 | (rnd() & ARITH);
    const uint16_t fcw = 0x037F;
    const uint32_t mxcsr = 0x1F80;
    memcpy(s.fx + 0, &fcw, 2);
    memcpy(s.fx + 24, &mxcsr, 4);
    for (int i = 0; i < 8 * 16; i += 4) {
        const uint32_t v = rnd();
        memcpy(s.fx + 160 + i, &v, 4);
    }
}

static std::string hex(const uint8_t *p, size_t n)
{
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; i++) {
        snprintf(b, sizeof(b), "%02x", p[i]);
        s += b;
    }
    return s;
}

static bool ignored(const Test &t, uint32_t addr)
{
    for (auto &r : t.ignore) {
        if (addr >= r.first && addr < r.first + r.second) {
            return true;
        }
    }
    return false;
}

/* One run of a test's iteration; "" when both results agree. */
static std::string run_once(const Test &t, uint64_t seed)
{
    static const char *const rn[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    g_rng = seed * 0x2545F4914F6CDD1Dull + 0x1234567;
    State in;
    default_state(in);
    if (t.setup) {
        t.setup(in);
    }
    std::vector<uint8_t> mem_n(DATA_SIZE + STACK_SIZE), mem_t(DATA_SIZE + STACK_SIZE);
    State out_n, out_t;
    char buf[256];
    std::string why;

    if (!t.no_native) {
        fill_mem(seed);
        put_code(t.native.b.empty() ? t.code : t.native);
        g_native(&in);
        out_n = *g_nat_out;
        memcpy(mem_n.data(), (void *)(uintptr_t)DATA, DATA_SIZE);
        memcpy(mem_n.data() + DATA_SIZE, (void *)(uintptr_t)STACK, STACK_SIZE);
    }

    fill_mem(seed);
    put_code(t.code);
    if (t.code_limit) {
        nc_cpu_set_code_limit(g_cpu, t.code_limit);
    }
    translated(in, out_t);
    memcpy(mem_t.data(), (void *)(uintptr_t)DATA, DATA_SIZE);
    memcpy(mem_t.data() + DATA_SIZE, (void *)(uintptr_t)STACK, STACK_SIZE);
    if (g_stop_op) {
        const uint8_t *ib = (const uint8_t *)(uintptr_t)g_stop_eip;
        snprintf(buf, sizeof(buf), "left with system op %d at +0x%x (%s)", g_stop_op, g_stop_eip - CODE,
                 hex(ib, 4).c_str());
        return buf;
    }
    if (t.check && !t.check(in, out_t, why)) {
        return "check: " + why;
    }
    if (t.no_native) {
        return "";
    }
    for (int i = 0; i < 8; i++) {
        if (out_n.r[i] != out_t.r[i]) {
            snprintf(buf, sizeof(buf), "%s %08x, the processor's %08x; ", rn[i], out_t.r[i], out_n.r[i]);
            why += buf;
        }
    }
    if ((out_n.eflags ^ out_t.eflags) & t.fmask) {
        snprintf(buf, sizeof(buf), "flags %03x, the processor's %03x (of %03x); ", out_t.eflags & t.fmask,
                 out_n.eflags & t.fmask, t.fmask);
        why += buf;
    }
    if (t.fp) {
        const Fp a = fp_of_fx(out_t.fx), b = fp_of_fx(out_n.fx);
        if (a.fcw != b.fcw || a.fsw != b.fsw || a.ftw != b.ftw || a.mxcsr != b.mxcsr) {
            snprintf(buf, sizeof(buf), "fcw %04x fsw %04x ftw %02x mxcsr %04x, the processor's %04x %04x %02x %04x; ",
                     a.fcw, a.fsw, a.ftw, a.mxcsr, b.fcw, b.fsw, b.ftw, b.mxcsr);
            why += buf;
        }
        for (int i = 0; i < 8; i++) {
            if (memcmp(a.st[i], b.st[i], 10)) {
                why += "st" + std::to_string(i) + " " + hex(a.st[i], 10) + ", the processor's " + hex(b.st[i], 10) + "; ";
            }
            if (memcmp(a.xmm[i], b.xmm[i], 16)) {
                why += "xmm" + std::to_string(i) + " " + hex(a.xmm[i], 16) + ", the processor's " + hex(b.xmm[i], 16) + "; ";
            }
        }
    }
    unsigned diffs = 0;
    for (uint32_t i = 0; i < DATA_SIZE + STACK_SIZE; i++) {
        if (mem_n[i] != mem_t[i]) {
            const uint32_t addr = i < DATA_SIZE ? DATA + i : STACK + (i - DATA_SIZE);
            if (ignored(t, addr)) {
                continue;
            }
            if (!diffs++) {
                snprintf(buf, sizeof(buf), "memory at %s+0x%x: %s, the processor's %s; ", i < DATA_SIZE ? "data" : "stack",
                         i < DATA_SIZE ? i : i - DATA_SIZE, hex(&mem_t[i], 8).c_str(), hex(&mem_n[i], 8).c_str());
                why += buf;
            }
        }
    }
    if (diffs > 1) {
        why += std::to_string(diffs) + " bytes differ; ";
    }
    return why;
}

/* ------------------------------------------------------------ the tests */

static void define_tests()
{
    /* ---- the stack ---- */
    T("push and pop r32", {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,      /* push eax..edi (esp too) */
                           0x58, 0x59, 0x5A, 0x5B, 0x5D, 0x5E, 0x5F, 0x58});    /* pop them elsewhere */
    T("pop esp", Code({0xB8}).d(ESP0 - 0x100)({0x50, 0x5C}));                   /* mov eax, imm; push eax; pop esp */
    T("push esp, pop [esp]", {0x54, 0x8F, 0x04, 0x24, 0x58});                   /* push esp; pop [esp]; pop eax */
    T("push imm", Code({0x6A, 0x80, 0x68}).d(0x12345678)({0x66, 0x6A, 0x80, 0x66, 0x68}).w(0xBEEF)({0x66, 0x6A, 0x7F}));
    T("push and pop r16", {0x66, 0x50, 0x66, 0x51, 0x66, 0x54, 0x66, 0x57,      /* push ax, cx, sp, di */
                           0x66, 0x5B, 0x66, 0x5A, 0x66, 0x59, 0x66, 0x5E});    /* pop bx, dx, cx, si */
    T("pop sp", Code({0x66, 0x68}).w((uint16_t)(ESP0 - 0x80))({0x66, 0x5C}));    /* push word; pop sp */
    T("push and pop m32", {0xFF, 0x33,                  /* push [ebx] */
                           0xFF, 0x74, 0x24, 0x04,      /* push [esp+4] */
                           0xFF, 0x34, 0x24,            /* push [esp] */
                           0x8F, 0x03,                  /* pop [ebx] */
                           0x8F, 0x44, 0x24, 0x08,      /* pop [esp+8] */
                           0x8F, 0x04, 0x24,            /* pop [esp] */
                           0x8F, 0x46, 0x10,            /* pop [esi+16] */
                           0x58});
    T("push and pop m16", {0x66, 0xFF, 0x33,                    /* push word [ebx] */
                           0x66, 0xFF, 0x74, 0x24, 0x02,        /* push word [esp+2] */
                           0x66, 0x8F, 0x03,                    /* pop word [ebx] */
                           0x66, 0x8F, 0x44, 0x24, 0x04,        /* pop word [esp+4] */
                           0x66, 0xFF, 0x36,                    /* push word [esi] */
                           0x66, 0x8F, 0x04, 0x24,              /* pop word [esp] */
                           0x66, 0x58});                        /* pop ax */
    T("pushad, popad", {0x60, 0x31, 0xC0, 0x31, 0xC9, 0x31, 0xD2, 0x31, 0xDB, 0x31, 0xED, 0x31, 0xF6, 0x31, 0xFF, 0x61});
    T("pushad kept", {0x60, 0x31, 0xC0, 0x83, 0xC4, 0x20});                     /* the frame stays on the stack */
    T("pushaw, popaw", {0x66, 0x60, 0x31, 0xC0, 0x31, 0xC9, 0x31, 0xD2, 0x31, 0xDB, 0x31, 0xED, 0x31, 0xF6, 0x31, 0xFF,
                        0x66, 0x61});
    T("pushaw kept", {0x66, 0x60, 0x83, 0xC4, 0x10});
    T("pushfd, popfd", Code({0x9C,                                              /* pushfd */
                             0x81, 0x34, 0x24}).d(ARITH)                        /* xor dword [esp], the flags */
                           ({0x9D, 0x9C, 0x58}));                               /* popfd; pushfd; pop eax */
    T("pushfd with DF", {0xFD, 0x9C, 0x58, 0xFC, 0x9C, 0x59});                  /* std; pushfd; pop eax; cld; pushfd; pop ecx */
    T("pushf, popf (16 bits)", Code({0x66, 0x9C,                                /* pushf */
                                     0x66, 0x81, 0x34, 0x24}).w(ARITH)          /* xor word [esp], the flags */
                                   ({0x66, 0x9D, 0x9C, 0x58, 0x66, 0x9C, 0x66, 0x59}));
    T("the ID flag toggles", Code({0x9C, 0x81, 0x34, 0x24}).d(0x200000)({0x9D, 0x9C, 0x58,     /* flip ID, read it */
                                   0x50, 0x81, 0x34, 0x24}).d(0x200000)({0x9D}));              /* and back */
    T("the AC flag toggles", Code({0x9C, 0x81, 0x0C, 0x24}).d(0x40000)({0x9D, 0x9C, 0x58,      /* set AC, read it */
                                   0x50, 0x81, 0x24, 0x24}).d(~0x40000u)({0x9D, 0x9C, 0x59})); /* clear it, read again */
    T("enter and leave", {0xC8, 0x20, 0x00, 0x00, 0x89, 0xE8, 0xC9});           /* enter 32, 0; mov eax, ebp; leave */
    T("enter with a nesting level", {0xC8, 0x10, 0x00, 0x03,                    /* enter 16, 3 */
                                     0x89, 0xE0, 0x89, 0xE9,                    /* mov eax, esp; mov ecx, ebp */
                                     0xC9});
    T("enter with nesting level 1", {0xC8, 0x08, 0x00, 0x01, 0x89, 0xE0, 0xC9});
    T("leave (16 bits)", {0x66, 0xC9});         /* ESP from EBP whole, in another 64 KiB than the stack */

    /* ---- calls and returns ---- */
    T("call, ret", Code({0xE8}).rel(7)({0xEB, 0x02})            /* call f; jmp end */
                       .at(7)({0x40, 0xC3}));                   /* f: inc eax; ret */
    T("call reg, call [esp], ret imm", Code({0xBE}).d(CODE + 0x20)({0xFF, 0xD6,         /* mov esi, f; call esi */
                                             0x68}).d(CODE + 0x24)({0xFF, 0x14, 0x24,   /* push g; call [esp] */
                                             0xEB, 0x20})                               /* jmp end */
                                           .at(0x20)({0x40, 0xC3})                      /* f: inc eax; ret */
                                           .at(0x24)({0x41, 0xC2, 0x04, 0x00})          /* g: inc ecx; ret 4 */
                                           .at(0x31));
    T("call next, pop (no return)", Code({0xE8}).rel(5)({0x58,                  /* call $+5; pop eax */
                                          0xE8}).rel(11)({0x59}));              /* again; pop ecx */
    T("a return address replaced", Code({0xE8}).rel(5)({0x58,                   /* call $+5; pop eax */
                                         0x68}).d(CODE + 0x10)({0xC3})          /* push target; ret */
                                       .at(0x10)({0x43}));                      /* target: inc ebx */
    T("nested calls", Code({0xE8}).rel(0x10)({0xEB, 0x30})                      /* call a; jmp end */
                          .at(0x10)({0x40, 0xE8}).rel(0x20)({0x40, 0xC3})       /* a: inc eax; call b; inc eax; ret */
                          .at(0x20)({0x41, 0xE8}).rel(0x30)({0x41, 0xC3})       /* b: inc ecx; call c; inc ecx; ret */
                          .at(0x30)({0x42, 0xF3, 0xC3})                         /* c: inc edx; rep ret */
                          .at(0x37));
    T("jmp reg and through memory", Code({0xBE}).d(CODE + 0x10)({0xFF, 0xE6})   /* mov esi, t1; jmp esi */
                                        .at(0x10)({0x40, 0xC7, 0x03}).d(CODE + 0x20)({0xFF, 0x23})   /* inc eax; mov [ebx], t2; jmp [ebx] */
                                        .at(0x20)({0x41}));

    /* ---- conditions ---- */
    {
        Code c;
        for (uint8_t cc = 0; cc < 16; cc++) {
            c({(uint8_t)(0x70 | cc), 0x01, 0x40});                              /* jcc +1; inc eax */
            c({0x0F, (uint8_t)(0x80 | cc), 0x01, 0x00, 0x00, 0x00, 0x41});      /* jcc near +1; inc ecx */
            c({0x0F, (uint8_t)(0x90 | cc), 0xC2});                              /* setcc dl */
            c({0x0F, (uint8_t)(0x40 | cc), 0xDE});                              /* cmovcc ebx, esi */
        }
        T("jcc, setcc, cmovcc", c).iters = 24;
    }
    T("loop, jecxz", {0xB9, 0x05, 0x00, 0x00, 0x00,     /* mov ecx, 5 */
                      0x40, 0xE2, 0xFD,                 /* l: inc eax; loop l */
                      0xE3, 0x01, 0x42,                 /* jecxz +1; inc edx */
                      0x41, 0xE3, 0x01, 0x43});         /* inc ecx; jecxz +1; inc ebx */
    T("loope, loopne", {0xB9, 0x04, 0x00, 0x00, 0x00, 0x31, 0xC0,       /* mov ecx, 4; xor eax, eax */
                        0x40, 0x83, 0xF8, 0x02, 0xE0, 0xFA,             /* l: inc eax; cmp eax, 2; loopne l */
                        0xB9, 0x04, 0x00, 0x00, 0x00,
                        0x39, 0xC0, 0xE1, 0xFC});                       /* m: cmp eax, eax; loope m */
    {
        Test &t = T("jcxz and loop on CX (67)", {0xB9, 0x03, 0x00, 0x01, 0x00,  /* mov ecx, 0x10003 */
                                                 0x40, 0x67, 0xE2, 0xFC,        /* l: inc eax; loop l (on cx) */
                                                 0x67, 0xE3, 0x01, 0x42,        /* jcxz +1; inc edx */
                                                 0xB9, 0x04, 0x00, 0x02, 0x00, 0x31, 0xC0,              /* mov ecx, 0x20004; xor eax, eax */
                                                 0x40, 0x83, 0xF8, 0x02, 0x67, 0xE0, 0xF9,              /* l: inc eax; cmp eax, 2; loopne l */
                                                 0x39, 0xC0, 0x67, 0xE1, 0xFB,                          /* m: cmp eax, eax; loope m */
                                                 0x41, 0x67, 0xE3, 0x01, 0x43});                        /* inc ecx; jcxz +1; inc ebx */
        (void)t;
    }

    /* ---- ESP as an operand ---- */
    T("ESP with AH, CH, DH, BH", {0x50, 0x51, 0x52, 0x53,       /* four pushes to work on */
                                  0x8A, 0x64, 0x24, 0x04,       /* mov ah, [esp+4] */
                                  0x00, 0x6C, 0x24, 0x08,       /* add [esp+8], ch */
                                  0x88, 0x74, 0x24, 0x0C,       /* mov [esp+12], dh */
                                  0x8A, 0x3C, 0x24,             /* mov bh, [esp] */
                                  0x86, 0x64, 0x24, 0x05,       /* xchg ah, [esp+5] */
                                  0x28, 0x7C, 0x24, 0x01,       /* sub [esp+1], bh */
                                  0x38, 0x6C, 0x24, 0x02,       /* cmp [esp+2], ch */
                                  0x83, 0xC4, 0x10}).iters = 12;
    T("ESP arithmetic", {0x83, 0xEC, 0x10,              /* sub esp, 16 */
                         0x8D, 0x64, 0x24, 0xF0,        /* lea esp, [esp-16] */
                         0x89, 0xE0,                    /* mov eax, esp */
                         0x66, 0x89, 0xE1,              /* mov cx, sp */
                         0x0F, 0xB7, 0xD4,              /* movzx edx, sp */
                         0x94, 0x94,                    /* xchg eax, esp twice */
                         0x87, 0xE1, 0x87, 0xE1,        /* xchg ecx, esp twice */
                         0x66, 0x83, 0xEC, 0x08,        /* sub sp, 8 */
                         0x66, 0x83, 0xC4, 0x08,        /* add sp, 8 */
                         0x44, 0x4C,                    /* inc esp; dec esp */
                         0x8D, 0x74, 0x24, 0x04,        /* lea esi, [esp+4] */
                         0x8D, 0x7C, 0x4C, 0x08,        /* lea edi, [esp+ecx*2+8] */
                         0xC7, 0x04, 0x24, 0x10, 0x00, 0x00, 0x00,      /* mov dword [esp], 16 */
                         0x03, 0x24, 0x24,              /* add esp, [esp] */
                         0x83, 0xEC, 0x10,              /* sub esp, 16 */
                         0x83, 0xC4, 0x20});
    T("inc and dec short forms", {0x40, 0x41, 0x42, 0x43, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4D, 0x4E, 0x4F,
                                  0x66, 0x40, 0x66, 0x49});

    /* ---- arithmetic and flags (re-encoded: the host's own) ---- */
    T("alu 32", {0x01, 0xC8,                    /* add eax, ecx */
                 0x11, 0xD0,                    /* adc eax, edx */
                 0x29, 0xCA,                    /* sub edx, ecx */
                 0x19, 0xC1,                    /* sbb ecx, eax */
                 0xF7, 0xD8,                    /* neg eax */
                 0x0F, 0xAF, 0xC2,              /* imul eax, edx */
                 0x6B, 0xC9, 0x7B,              /* imul ecx, ecx, 123 */
                 0x0F, 0xC8,                    /* bswap eax */
                 0x0F, 0xC1, 0xCA,              /* xadd edx, ecx */
                 0x09, 0xC8, 0x21, 0xD0, 0x31, 0xCA, 0x85, 0xC0}).iters = 16;
    T("mul and div", {0xF7, 0xE1,               /* mul ecx */
                      0x83, 0xC9, 0x01,         /* or ecx, 1 */
                      0x31, 0xD2,               /* xor edx, edx */
                      0xF7, 0xF1,               /* div ecx */
                      0x99,                     /* cdq */
                      0xD1, 0xF9,               /* sar ecx, 1 */
                      0x83, 0xC9, 0x01,
                      0xF7, 0xF9,               /* idiv ecx */
                      0x98, 0x66, 0x98, 0x66, 0x99}).fmask = 0;          /* the flags after them are undefined */
    T("shifts and rotates", {0xD3, 0xE0,                /* shl eax, cl */
                             0x0F, 0xA5, 0xD0,          /* shld eax, edx, cl */
                             0x0F, 0xAC, 0xC2, 0x07,    /* shrd edx, eax, 7 */
                             0xC1, 0xC8, 0x07,          /* ror eax, 7 */
                             0xD1, 0xD1,                /* rcl ecx, 1 */
                             0xD1, 0xDA,                /* rcr edx, 1 */
                             0xD1, 0xE0, 0xD1, 0xE8,    /* shl eax, 1; shr eax, 1 */
                             0xD1, 0xF8}).iters = 16;   /* sar eax, 1 (count 1: every flag defined) */
    T("bits", {0x25, 0x1F, 0x00, 0x00, 0x00,            /* and eax, 31 */
               0x0F, 0xAB, 0x03,                        /* bts [ebx], eax */
               0x0F, 0xB3, 0x43, 0x04,                  /* btr [ebx+4], eax */
               0x0F, 0xA3, 0xC1,                        /* bt ecx, eax */
               0x0F, 0xBA, 0xFA, 0x05,                  /* btc edx, 5 */
               0x83, 0xC9, 0x01, 0x0F, 0xBC, 0xF1,      /* or ecx, 1; bsf esi, ecx */
               0x0F, 0xBD, 0xF9}).fmask = 0x1;          /* bsr edi, ecx */
    T("alu 8 and 16 with high bytes", {0x00, 0xE0,              /* add al, ah */
                                       0x66, 0x01, 0xC8,        /* add ax, cx */
                                       0x86, 0xE0,              /* xchg al, ah */
                                       0x10, 0xF5,              /* adc ch, dh */
                                       0x0F, 0xB6, 0xC4,        /* movzx eax, ah */
                                       0x0F, 0xBE, 0xCD,        /* movsx ecx, ch */
                                       0x28, 0x33,              /* sub [ebx], dh */
                                       0x02, 0x7B, 0x01,        /* add bh, [ebx+1] */
                                       0xFE, 0xC4, 0xFE, 0xCD}).iters = 12;     /* inc ah; dec ch */
    T("lahf, sahf, flag instructions", {0x9F, 0x80, 0xF4, 0xD5, 0x9E,           /* lahf; xor ah, 0xD5; sahf */
                                        0xF5, 0xF8, 0xF9, 0xF5});               /* cmc; clc; stc; cmc */
    T("cmpxchg, cmpxchg8b, lock", {0x0F, 0xB1, 0x0B,            /* cmpxchg [ebx], ecx */
                                   0x8B, 0x03, 0x0F, 0xB1, 0x13,                /* mov eax, [ebx]; cmpxchg [ebx], edx */
                                   0x0F, 0xC7, 0x4B, 0x10,      /* cmpxchg8b [ebx+16] */
                                   0xF0, 0x0F, 0xC1, 0x03,      /* lock xadd [ebx], eax */
                                   0xF0, 0xFF, 0x43, 0x04,      /* lock inc dword [ebx+4] */
                                   0x87, 0x4B, 0x08});          /* xchg [ebx+8], ecx */

    /* ---- addressing ---- */
    T("mov moffs and absolute", Code({0xA1}).d(DATA + 0x10)({0xA3}).d(DATA + 0x20)({0xA0}).d(DATA + 0x31)({0xA2}).d(DATA + 0x41)
                                    ({0x66, 0xA1}).d(DATA + 0x52)
                                    ({0x8B, 0x34, 0x8D}).d(DATA + 0x100)        /* mov esi, [ecx*4 + abs] */
                                    ({0x8B, 0x0D}).d(DATA + 0x60)({0x89, 0x15}).d(DATA + 0x70)
                                    ).setup = [](State &s) { s.r[ECX] &= 0xFF; };
    T("segment prefixes on flat data", {0x3E, 0x8B, 0x03,               /* mov eax, ds:[ebx] */
                                        0x26, 0x89, 0x4B, 0x04,         /* mov es:[ebx+4], ecx */
                                        0x36, 0x8B, 0x53, 0x08,         /* mov edx, ss:[ebx+8] */
                                        0x2E, 0x8B, 0x73, 0x0C});       /* mov esi, cs:[ebx+12] */
    T("xlat, cwd family", {0xD7, 0x98, 0x66, 0x98, 0x99, 0x66, 0x99});
    {
        Test &t = T("16-bit addressing (67)", {0x67, 0x8B, 0x07});       /* mov eax, [bx] */
        t.no_native = true;
        t.gap = "memory operands with a 16-bit address size aren't translated";
    }

    /* ---- strings ---- */
    T("rep movs, stos, cmps, scas, lods", {0xFC, 0xB9, 0x10, 0x00, 0x00, 0x00, 0xF3, 0xA4,      /* cld; rep movsb x16 */
                                           0xB9, 0x05, 0x00, 0x00, 0x00, 0xF3, 0xA5,            /* rep movsd x5 */
                                           0xB9, 0x03, 0x00, 0x00, 0x00, 0xF3, 0x66, 0xA5,      /* rep movsw x3 */
                                           0xB9, 0x07, 0x00, 0x00, 0x00, 0xF3, 0xAB,            /* rep stosd x7 */
                                           0xAC, 0x66, 0xAD, 0xAD,                              /* lodsb; lodsw; lodsd */
                                           0xB9, 0x20, 0x00, 0x00, 0x00, 0xF3, 0xA6,            /* repe cmpsb */
                                           0xB9, 0x20, 0x00, 0x00, 0x00, 0xF2, 0xAE,            /* repne scasb */
                                           0xAA, 0xA7});                                        /* stosb; cmpsd */
    T("strings backwards", {0xFD, 0xB9, 0x10, 0x00, 0x00, 0x00, 0xF3, 0xA4,     /* std; rep movsb */
                            0xB9, 0x04, 0x00, 0x00, 0x00, 0xF3, 0xAB,           /* rep stosd */
                            0xAC, 0xAE, 0xFC});                                 /* lodsb; scasb; cld */

    /* ---- what 64-bit mode has no encoding for ---- */
    T("daa", {0x27}).fmask = 0x0D5;
    g_tests.back().iters = 64;
    T("das", {0x2F}).fmask = 0x0D5;
    g_tests.back().iters = 64;
    T("aaa", {0x37}).fmask = 0x011;
    g_tests.back().iters = 64;
    T("aas", {0x3F}).fmask = 0x011;
    g_tests.back().iters = 64;
    T("aam", {0xD4, 0x0A, 0x89, 0xC1, 0xD4, 0x07}).fmask = 0x0C4;        /* aam; mov ecx, eax; aam 7 */
    g_tests.back().iters = 32;
    T("aad", {0xD5, 0x0A, 0x89, 0xC1, 0x89, 0xD0, 0xD5, 0x10}).fmask = 0x0C4;    /* aad; mov ecx, eax; mov eax, edx; aad 16 */
    g_tests.back().iters = 32;
    T("salc", {0xD6, 0x89, 0xC1, 0xF5, 0xD6}).iters = 8;                 /* salc; mov ecx, eax; cmc; salc */
    T("bound in range", Code({0xC7, 0x03}).d(0x80000000)({0xC7, 0x43, 0x04}).d(0x7FFFFFFF)       /* the widest bounds */
                            ({0x62, 0x03,                                                         /* bound eax, [ebx] */
                              0x66, 0xC7, 0x43, 0x08}).w(0x8000)({0x66, 0xC7, 0x43, 0x0A}).w(0x7FFF)
                            ({0x66, 0x62, 0x4B, 0x08}));                                          /* bound cx, [ebx+8] */
    T("into without overflow", {0x31, 0xC0, 0xCE, 0x40});                /* xor eax, eax; into; inc eax */
    T("les, lds", Code({0x66, 0xC7, 0x43, 0x04}).w(0x2B)                 /* a selector the host has, for its own run */
                      ({0xC4, 0x03,                                      /* les eax, [ebx] */
                        0x66, 0xC7, 0x43, 0x14}).w(0x2B)({0xC5, 0x4B, 0x10}));   /* lds ecx, [ebx+16] */

    /* ---- x87, MMX, SSE ---- */
    T("x87 arithmetic", {0xD9, 0x03,            /* fld dword [ebx] */
                         0xDD, 0x43, 0x08,      /* fld qword [ebx+8] */
                         0xDE, 0xC1,            /* faddp */
                         0xD9, 0xE1,            /* fabs */
                         0xD9, 0xFA,            /* fsqrt */
                         0xDB, 0x43, 0x10,      /* fild dword [ebx+16] */
                         0xDC, 0xC9,            /* fmul st1, st0 */
                         0xD9, 0xC9,            /* fxch */
                         0xDB, 0x7B, 0x20,      /* fstp tbyte [ebx+32] */
                         0xDD, 0x5B, 0x30,      /* fstp qword [ebx+48] */
                         0xDF, 0xE0});          /* fnstsw ax */
    T("x87 control word, rounding", {0x66, 0xC7, 0x03, 0x7F, 0x0C,      /* mov word [ebx], 0x0C7F: chop, single precision */
                                     0xD9, 0x2B,                        /* fldcw [ebx] */
                                     0xD9, 0xE8, 0xD9, 0xEB, 0xDE, 0xF9,        /* fld1; fldpi; fdivp */
                                     0xDB, 0x5B, 0x10,                  /* fistp dword [ebx+16] */
                                     0xD9, 0xEB, 0xD8, 0xC8,            /* fldpi; fmul st0 */
                                     0xDD, 0x5B, 0x18,                  /* fstp qword [ebx+24] */
                                     0xD9, 0x7B, 0x20});                /* fnstcw [ebx+32] */
    T("x87 compare, fcmov, transcendental", {0xD9, 0xE8, 0xD9, 0xEE,    /* fld1; fldz */
                                             0xDB, 0xF1,                /* fcomi st1 */
                                             0xDA, 0xC1,                /* fcmovb st0, st1 */
                                             0xD9, 0xEB, 0xD9, 0xFE,    /* fldpi; fsin */
                                             0xD9, 0xFF,                /* fcos */
                                             0xDD, 0x5B, 0x10,          /* fstp qword [ebx+16] */
                                             0xDE, 0xD9,                /* fcompp */
                                             0xDF, 0xE0});              /* fnstsw ax */
    {
        Test &t = T("fnstenv, fnsave, frstor", {0xD9, 0xE8, 0xD9, 0xEB,         /* fld1; fldpi */
                                                0xD9, 0x73, 0x40,               /* fnstenv [ebx+64] */
                                                0xDD, 0xB3, 0x00, 0x01, 0x00, 0x00,     /* fnsave [ebx+256] */
                                                0xD9, 0xEE,                     /* fldz */
                                                0xDD, 0xA3, 0x00, 0x01, 0x00, 0x00,     /* frstor [ebx+256] */
                                                0xDB, 0x7B, 0x20});             /* fstp tbyte [ebx+32] */
        /* the last instruction's and operand's pointers: the host's, in translated code */
        t.ignore = {{DATA + 0x1000 + 64 + 12, 16}, {DATA + 0x1000 + 256 + 12, 16}};
    }
    {
        Test &t = T("fxsave, fxrstor", {0xD9, 0xE8,                     /* fld1 */
                                        0x0F, 0xAE, 0x83, 0x00, 0x02, 0x00, 0x00,       /* fxsave [ebx+512] */
                                        0xD9, 0xEE, 0x0F, 0x57, 0xC0,   /* fldz; xorps xmm0, xmm0 */
                                        0x0F, 0xAE, 0x8B, 0x00, 0x02, 0x00, 0x00});     /* fxrstor [ebx+512] */
        /* pointers as above; XMM8-15's and the reserved bytes: 64-bit mode's FXSAVE writes more of the image */
        t.ignore = {{DATA + 0x1000 + 512 + 6, 18}, {DATA + 0x1000 + 512 + 288, 224}};
    }
    T("mmx", {0x0F, 0x6F, 0x03,                 /* movq mm0, [ebx] */
              0x0F, 0x6F, 0x4B, 0x08,           /* movq mm1, [ebx+8] */
              0x0F, 0xFC, 0xC1,                 /* paddb mm0, mm1 */
              0x0F, 0xDC, 0x43, 0x10,           /* paddusb mm0, [ebx+16] */
              0x0F, 0xD5, 0xC8,                 /* pmullw mm1, mm0 */
              0x0F, 0x73, 0xD1, 0x03,           /* psrlq mm1, 3 */
              0x0F, 0x7F, 0x43, 0x20,           /* movq [ebx+32], mm0 */
              0x0F, 0x7E, 0xC8,                 /* movd eax, mm1 */
              0x0F, 0xE7, 0x4B, 0x28,           /* movntq [ebx+40], mm1 */
              0x0F, 0x77});                     /* emms */
    {
        /* 66 0F xx is the MMX instruction on a Pentium III; the host's own run takes it without the prefix */
        Test &t = T("mmx with an operand-size prefix", {0x0F, 0x6F, 0x03,
                                                        0x66, 0x0F, 0xDC, 0x43, 0x11,   /* paddusb mm0, [ebx+17] (unaligned) */
                                                        0x66, 0x0F, 0xFC, 0x43, 0x21,   /* paddb mm0, [ebx+33] */
                                                        0x66, 0x0F, 0xEF, 0x43, 0x31,   /* pxor mm0, [ebx+49] */
                                                        0x0F, 0x7F, 0x43, 0x40, 0x0F, 0x77});
        t.native = {0x0F, 0x6F, 0x03, 0x0F, 0xDC, 0x43, 0x11, 0x0F, 0xFC, 0x43, 0x21, 0x0F, 0xEF, 0x43, 0x31,
                    0x0F, 0x7F, 0x43, 0x40, 0x0F, 0x77};
    }
    T("sse", {0x0F, 0x10, 0x03,                 /* movups xmm0, [ebx] */
              0x0F, 0x28, 0x4B, 0x10,           /* movaps xmm1, [ebx+16] */
              0x0F, 0x58, 0xC1,                 /* addps xmm0, xmm1 */
              0x0F, 0x59, 0xC8,                 /* mulps xmm1, xmm0 */
              0x0F, 0xC6, 0xC1, 0x1B,           /* shufps xmm0, xmm1, 0x1B */
              0xF3, 0x0F, 0x51, 0xD0,           /* sqrtss xmm2, xmm0 */
              0x0F, 0x2F, 0xC1,                 /* comiss xmm0, xmm1 */
              0xF3, 0x0F, 0x2C, 0xC0,           /* cvttss2si eax, xmm0 */
              0x0F, 0x29, 0x43, 0x20,           /* movaps [ebx+32], xmm0 */
              0x0F, 0xAE, 0x5B, 0x30,           /* stmxcsr [ebx+48] */
              0x0F, 0x2B, 0x4B, 0x40,           /* movntps [ebx+64], xmm1 */
              0x0F, 0x18, 0x03, 0x0F, 0xAE, 0xF8});     /* prefetchnta [ebx]; sfence */
    T("ldmxcsr", {0xC7, 0x03, 0x80, 0x7F, 0x00, 0x00,   /* mov dword [ebx], 0x7F80: round to zero */
                  0x0F, 0xAE, 0x13,                     /* ldmxcsr [ebx] */
                  0xF3, 0x0F, 0x2D, 0x43, 0x10});       /* cvtss2si eax, [ebx+16] */
    T("nops", {0x90, 0x0F, 0x1F, 0x40, 0x00, 0xF3, 0x90, 0x66, 0x90, 0x8D, 0x76, 0x00});

    {
        /* all eight MMX registers in use (the x87 stack full) when an
         * interrupt comes and its host code uses the x87 */
        Code c;
        for (uint8_t i = 0; i < 8; i++) {
            c({0x0F, 0x6F, (uint8_t)(0x43 | i << 3), (uint8_t)(8 * i)});       /* movq mm<i>, [ebx + 8i] */
        }
        c({0x0F, 0x00, 0xE0});                                                  /* verr ax: an interrupt next */
        for (uint8_t i = 0; i < 8; i++) {
            c({0x0F, 0xFC, (uint8_t)(0xC0 | i << 3 | ((i + 1) & 7))});          /* paddb mm<i>, mm<i+1> */
            c({0x0F, 0x7F, (uint8_t)(0x43 | i << 3), (uint8_t)(0x80 + 8 * i)}); /* movq [ebx + 0x80 + 8i], mm<i> */
        }
        c({0x0F, 0x77});
        T("an interrupt in MMX code, the host using the x87", c).fmask = 0;
    }
    {
        /* the same under x87 code, eight values deep */
        Code c({0xD9, 0xE8, 0xD9, 0xEB, 0xD9, 0xEE, 0xD9, 0xE8, 0xD9, 0xEB, 0xD9, 0xEA, 0xD9, 0xE9, 0xD9, 0xEC,   /* eight loads */
                0x0F, 0x00, 0xE0});
        for (uint8_t i = 0; i < 8; i++) {
            c({0xDB, 0x7B, (uint8_t)(0x10 * i)});                               /* fstp tbyte [ebx + 16i] */
        }
        T("an interrupt in x87 code eight deep, the host using the x87", c).fmask = 0;
    }

    /* ---- FS: the KPCR's base added (the host's own run takes absolute addresses) ---- */
    {
        Test &t = T("FS-relative accesses", Code({0x64, 0xA1}).d(0x10)                  /* mov eax, fs:[0x10] */
                                                ({0x64, 0x89, 0x15}).d(0x30)            /* mov fs:[0x30], edx */
                                                ({0x64, 0x8B, 0x0E})                    /* mov ecx, fs:[esi] */
                                                ({0x64, 0xFF, 0x35}).d(0x18)            /* push fs:[0x18] */
                                                ({0x64, 0x8F, 0x05}).d(0x40)            /* pop fs:[0x40] */
                                                ({0x64, 0x8A, 0x25}).d(0x52)            /* mov ah, fs:[0x52] */
                                                ({0x64, 0x01, 0x46, 0x08})              /* add fs:[esi+8], eax */
                                                ({0x64, 0xFF, 0x76, 0x0C, 0x5F}));      /* push fs:[esi+12]; pop edi */
        t.native = Code({0xA1}).d(FSBASE + 0x10)({0x89, 0x15}).d(FSBASE + 0x30)
                       ({0x8B, 0x8E}).d(FSBASE)({0xFF, 0x35}).d(FSBASE + 0x18)({0x8F, 0x05}).d(FSBASE + 0x40)
                       ({0x8A, 0x25}).d(FSBASE + 0x52)({0x01, 0x86}).d(FSBASE + 8)({0xFF, 0xB6}).d(FSBASE + 12)({0x5F});
        t.setup = [](State &s) { s.r[ESI] = 0x100; };
    }
    {
        Test &t = T("FS with a register and AH, CH, DH or BH", Code({0x64, 0x8A, 0x66, 0x02})  /* mov ah, fs:[esi+2] */
                                                                   ({0x64, 0x00, 0x2E}));       /* add fs:[esi], ch */
        t.native = Code({0x8A, 0xA6}).d(FSBASE + 2)({0x00, 0xAE}).d(FSBASE);
        t.setup = [](State &s) { s.r[ESI] = 0x100; };
    }

    /* ---- segment registers: the flat model's selectors, given here ---- */
    {
        Test &t = T("segment register values", {0x8C, 0xC8,             /* mov eax, cs */
                                                0x8C, 0xD9,             /* mov ecx, ds */
                                                0x8C, 0xD2,             /* mov edx, ss */
                                                0x8C, 0xC6,             /* mov esi, es */
                                                0x1E, 0x5F,             /* push ds; pop edi */
                                                0x8E, 0xD9, 0x06, 0x07});       /* mov ds, ecx; push es; pop es */
        t.no_native = true;
        t.check = [](const State &in, const State &out, std::string &why) {
            if (out.r[EAX] != 0x08 || out.r[ECX] != 0x10 || out.r[EDX] != 0x10 || out.r[ESI] != 0x10 ||
                out.r[EDI] != 0x10 || out.r[ESP] != in.r[ESP]) {
                why = "cs, ds, ss, es or the stack pointer";
                return false;
            }
            return true;
        };
    }

    /* ---- code that changes ---- */
    {
        /* writes over an instruction further on in its own block */
        Test &t = T("code written over ahead in the block", Code({0xC6, 0x05}).d(CODE + 8)({0x07,      /* mov byte [imm], 7 */
                                                                  0xB8}).d(1));                         /* mov eax, 1 -> 7 */
        t.gap = "translated code isn't invalidated when the title writes over it";
    }

    {
        /* a function changes its caller's next instruction and the code is
         * invalidated before it returns (what NtProtectVirtualMemory does
         * for a title that patches itself): the return comes back into the
         * caller's block, dead by then, and must go on in the new code */
        Code c({0xB9, 0x02, 0x00, 0x00, 0x00, 0xEB, 0x00});     /* mov ecx, 2; jmp l (the call starts its block,
                                                                 * made before the one after it) */
        c.at(0x07)({0xE8}).rel(0x20);                           /* l: call f */
        c.at(0x0C)({0xB8, 0x01, 0x00, 0x00, 0x00,               /* mov eax, 1 (2 once f has changed it) */
                    0x49, 0x75, 0xF3,                           /* dec ecx; jnz l */
                    0xEB, 0x1C});                               /* jmp end */
        c.at(0x20)({0x83, 0xF9, 0x01, 0x75, 0x0A,               /* f: cmp ecx, 1; jne ret */
                    0xC6, 0x05}).d(CODE + 0x0D)({0x02,          /* mov byte [the immediate], 2 */
                    0x0F, 0x00, 0xE8,                           /* verw ax: the invalidation */
                    0xC3});                                     /* ret */
        c.at(0x32);
        T("code invalidated under a call returns into the new code", c);
    }

    /* ---- the translation buffer's flush under live calls ---- */
    {
        /* twelve nested calls deep, 6000 small functions run for the first
         * time (the buffer, cut to 128 KiB, fills several times), then the
         * returns through the frames from before */
        Code c({0xB8, 0x00, 0x00, 0x00, 0x00, 0xE8});
        c.rel(0x100);
        c({0xE9}).rel(0xD000);                                  /* to the end */
        for (int k = 0; k < 12; k++) {
            c.at(0x100 + 0x10 * k)({0xE8}).rel(k < 11 ? 0x100 + 0x10 * (k + 1) : 0x400)({0x40, 0xC3});
        }
        c.at(0x400)({0xBE}).d(CODE + 0x1000)({0xB9}).d(6000)    /* mov esi, f0; mov ecx, 6000 */
                   ({0xFF, 0xD6, 0x83, 0xC6, 0x04, 0x49, 0x75, 0xF8, 0xC3});    /* l: call esi; add esi, 4; dec ecx; jnz l; ret */
        c.at(0x1000);
        for (int i = 0; i < 6000; i++) {
            c({0x40, 0xC3, 0x90, 0x90});                        /* inc eax; ret */
        }
        c.at(0xD000);
        Test &t = T("flush of the translation buffer under live calls", c);
        t.iters = 1;
        t.code_limit = 128u << 10;
    }
}

/* ------------------------------------------------------------ benchmark */

/* What the translator's transitions cost, in host nanoseconds an iteration:
 * the same loop on the processor and translated (cputest --bench). */
static double seconds()
{
#ifdef _WIN32
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

static void bench_one(const char *name, const Code &c, uint32_t n, bool native, bool irq)
{
    State in, out;
    g_rng = 1;
    default_state(in);
    double nat = 0, tr = 1e9;
    if (native) {
        nat = 1e9;
        for (int rep = 0; rep < 3; rep++) {
            fill_mem(1);
            put_code(c);
            const double t0 = seconds();
            g_native(&in);
            const double t = seconds() - t0;
            nat = t < nat ? t : nat;
        }
    }
    for (int rep = 0; rep < 3; rep++) {
        fill_mem(1);
        put_code(c);
        g_bench_irq = irq;
        const double t0 = seconds();
        translated(in, out);
        const double t = seconds() - t0;
        g_bench_irq = false;
        tr = t < tr ? t : tr;
    }
    if (g_stop_op) {
        printf("bench %-34s left with system op %d\n", name, g_stop_op);
    } else if (native) {
        printf("bench %-34s processor %6.2f ns, translated %6.2f ns (x%.1f)\n", name, nat * 1e9 / n, tr * 1e9 / n, tr / nat);
    } else {
        printf("bench %-34s translated %7.1f ns\n", name, tr * 1e9 / n);
    }
}

static void bench()
{
    const uint32_t N = 20000000, S = 1000000;
    bench_one("loop (dec, jnz)", Code({0xB9}).d(N)({0x49, 0x75, 0xFD}), N, true, false);
    bench_one("call and ret", Code({0xB9}).d(N)({0xE8}).rel(0x10)({0x49, 0x75, 0xF8, 0xEB, 0x02}).at(0x10)({0xC3}), N, true, false);
    bench_one("indirect jump", Code({0xB9}).d(N)({0xBE}).d(CODE + 0x0C)({0xFF, 0xE6, 0x49, 0x75, 0xFB}), N, true, false);
    bench_one("indirect call and ret", Code({0xB9}).d(N)({0xBE}).d(CODE + 0x11)({0xFF, 0xD6, 0x49, 0x75, 0xFB, 0xEB, 0x01, 0xC3}), N,
              true, false);
    bench_one("load, add, store", Code({0xB9}).d(N)({0x8B, 0x03, 0x01, 0xC8, 0x89, 0x43, 0x04, 0x49, 0x75, 0xF6}), N, true, false);
    bench_one("fs load", Code({0xB9}).d(N)({0x64, 0xA1}).d(0x10)({0x49, 0x75, 0xF7}), N, false, false);
    bench_one("system op (round trip)", Code({0xB9}).d(S)({0xFA, 0x49, 0x75, 0xFC}), S, false, false);
    bench_one("system op and an interrupt", Code({0xB9}).d(S)({0xFA, 0x49, 0x75, 0xFC}), S, false, true);
}

/* ----------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    std::vector<std::string> only;
    bool do_bench = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) {
            g_verbose = true;
        } else if (!strcmp(argv[i], "--bench")) {
            do_bench = true;
        } else {
            only.push_back(argv[i]);
        }
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    void *mem = VirtualAlloc((void *)(uintptr_t)BASE, TOTAL, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void *mem = mmap((void *)(uintptr_t)BASE, TOTAL, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mem == MAP_FAILED) {
        mem = nullptr;
    }
#endif
    if (mem != (void *)(uintptr_t)BASE) {
        printf("no memory at %08x for the guest\n", BASE);
        return 99;
    }
    if (!build_native()) {
        printf("the way into compatibility mode doesn't assemble\n");
        return 99;
    }
    NcCpuConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sysop = on_sysop;
    cfg.idt_base = 0x80010000u;
    cfg.idt_limit = 0x7FF;
    g_cpu = nc_cpu_create(&cfg, FSBASE);
    if (!g_cpu) {
        printf("no CPU\n");
        return 99;
    }
    if (do_bench) {
        bench();
        return 0;
    }
    define_tests();

    unsigned pass = 0, fail = 0, gaps = 0, gaps_closed = 0;
    for (const Test &t : g_tests) {
        if (!only.empty()) {
            bool wanted = false;
            for (auto &o : only) {
                wanted |= t.name.find(o) != std::string::npos;
            }
            if (!wanted) {
                continue;
            }
        }
        std::string why;
        int at = 0;
        for (int i = 0; i < t.iters && why.empty(); i++) {
            at = i;
            why = run_once(t, 1000 + 7919ull * i);
        }
        if (t.gap) {
            if (why.empty()) {
                printf("GAP CLOSED %s (listed as: %s)\n", t.name.c_str(), t.gap);
                gaps_closed++;
            } else {
                printf("GAP  %s: %s [%s]\n", t.name.c_str(), t.gap, why.c_str());
                gaps++;
            }
        } else if (why.empty()) {
            printf("PASS %s\n", t.name.c_str());
            pass++;
        } else {
            printf("FAIL %s (run %d): %s\n", t.name.c_str(), at, why.c_str());
            fail++;
        }
    }
    printf("%u passed, %u failed, %u known gaps%s\n", pass, fail, gaps,
           gaps_closed ? " (some listed gaps now pass: unlist them)" : "");
    return (int)fail;
}
