/*
 * xbased native core — the GP's DSP56300 program compiled to x86-64.
 *
 * Each word does what the interpreter (dsp_cpu.c, dsp_emu.c.inc) does with
 * it, bit for bit: its moves and its ALU written out with the fields decoded
 * once, the accumulators in host registers (56 bits in the top of 64, so the
 * host's carry, overflow, sign and zero are the DSP's), the condition codes
 * nobody reads skipped, the DO loops' bodies looping in place. What it
 * doesn't write out (rare words, the stack's registers, anything touching
 * the program memory) runs on the interpreter's own code, and the states it
 * doesn't run in (a REP from the interpreter, scaling modes, interrupts, a
 * DMA starting) go back to the interpreter word by word.
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <functional>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

extern "C" {
#include "dsp_cpu.h"
#include "dsp_dma.h"
#include "dsp_state.h"
#include "dsp_rec.h"
#include "dsp.h"
void nc_log(const char *tag, const char *fmt, ...);
}

#include "native/cpu/x64asm.h"

namespace {

using namespace x64;
typedef ZydisRegister Reg;

#define ZR(n) ZYDIS_REGISTER_##n
const Reg rAX = ZR(RAX), rCX = ZR(RCX), rDX = ZR(RDX), rBX = ZR(RBX), rSP = ZR(RSP), rBP = ZR(RBP),
          rSI = ZR(RSI), rDI = ZR(RDI), r8 = ZR(R8), r9 = ZR(R9), r10 = ZR(R10), r11 = ZR(R11),
          r12 = ZR(R12), r13 = ZR(R13), r14 = ZR(R14), r15 = ZR(R15);
const Reg eAX = ZR(EAX), eCX = ZR(ECX), eDX = ZR(EDX), eSI = ZR(ESI), eDI = ZR(EDI), eBP = ZR(EBP),
          r8d = ZR(R8D), r9d = ZR(R9D), r10d = ZR(R10D), r11d = ZR(R11D), r12d = ZR(R12D),
          r13d = ZR(R13D);
const Reg AL = ZR(AL), CL = ZR(CL), DL = ZR(DL);

#ifdef _WIN32
const Reg ARG0 = rCX, ARG1 = rDX, ARG2 = r8, ARG3 = r9;
const Reg ARG1D = eDX, ARG2D = r8d;
#else
const Reg ARG0 = rDI, ARG1 = rSI, ARG2 = rDX, ARG3 = rCX;
const Reg ARG1D = eSI, ARG2D = eDX;
#endif

Reg r64of(Reg r32)
{
    switch (r32) {
    case ZR(EAX): return rAX;
    case ZR(ECX): return rCX;
    case ZR(EDX): return rDX;
    case ZR(ESI): return rSI;
    case ZR(EDI): return rDI;
    case ZR(EBP): return rBP;
    case ZR(R8D): return r8;
    case ZR(R9D): return r9;
    case ZR(R10D): return r10;
    case ZR(R11D): return r11;
    case ZR(R12D): return r12;
    case ZR(R13D): return r13;
    default: return r32;
    }
}

/* x86 condition nibbles */
enum { CC_O = 0, CC_NO = 1, CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7,
       CC_S = 8, CC_NS = 9, CC_L = 0xC, CC_GE = 0xD, CC_LE = 0xE, CC_G = 0xF };

/* the DSP's condition code bits */
enum { F_C = 1, F_V = 2, F_Z = 4, F_N = 8, F_U = 0x10, F_E = 0x20, F_L = 0x40, F_ALL = 0x3f };

const int SR_LF = 1 << DSP_SR_LF;
const int SR_GUARD = 1 << DSP_SR_T;

int scale_of(uint32_t sr) { return (sr >> DSP_SR_S0) & 3; }
/* the block's key: S1:S0, and SM (4): code with SM's saturation is other code */
int mode_of(const dsp_core_t *c) { return scale_of(c->registers[DSP_REG_SR]) | ((c->sr_ext >> 18) & 4); }

int ROFF(int i) { return (int)(offsetof(dsp_core_t, registers) + 4 * (unsigned)i); }
const int OFF_PC = offsetof(dsp_core_t, pc);
const int OFF_IDLE = offsetof(dsp_core_t, is_idle);
const int OFF_XRAM = offsetof(dsp_core_t, xram);
const int OFF_YRAM = offsetof(dsp_core_t, yram);
const int OFF_MIX = offsetof(dsp_core_t, mixbuffer);

struct Rec;

/* what the compiled code and the dispatcher share (r15) */
struct Ctx {
    Rec *rec;
    uint8_t *patch_site;   /* a jmp's rel32 waiting for its target's code */
    int native_irq;        /* a native stack op raised an interrupt */
    /* emu_ccr_update_e_u_n_z's E, U and N for each scaling mode, by the
     * result's bits 55..45 (v >> 53) */
    uint8_t eun[4][2048];
};

void eun_tables(Ctx *x)
{
    for (int sc = 0; sc < 4; sc++) {
        for (uint32_t i = 0; i < 2048; i++) {
            /* i holds val bits 55..45 (val = the 56-bit accumulator) */
            const uint32_t b55_47 = i >> 2, b55_48 = i >> 3, b55_46 = i >> 1;
            const uint32_t b47 = (i >> 2) & 1, b46 = (i >> 1) & 1, b45 = i & 1, b48 = (i >> 3) & 1;
            uint8_t f = 0;
            if (sc == 3) {
                x->eun[sc][i] = 0;
                continue;
            }
            if (sc == 0) {
                if (b55_47 != 0 && b55_47 != 0x1ff) f |= F_E;
                if (b47 == b46) f |= F_U;
            } else if (sc == 1) {
                if (b55_48 != 0 && b55_48 != 0xff) f |= F_E;
                if (b48 == b47) f |= F_U;
            } else {
                if (b55_46 != 0 && b55_46 != 0x3ff) f |= F_E;
                if (b46 == b45) f |= F_U;
            }
            if (i & 0x400) f |= F_N;
            x->eun[sc][i] = f;
        }
    }
}

enum Kind {
    K_FALLBACK, K_PM, K_NOP, K_MOVE_XY_IMM, K_MOVE_X_LONG, K_JMP, K_JCC, K_JSR, K_RTS,
    K_BITBR, K_REP, K_ALUIMM, K_FB2, K_LOGIC_SR, K_DO_IMM, K_LUA_REL, K_LUA, K_SHIFT_IMM, K_MOVEP,
};

struct Ins {
    uint32_t pc = 0, w = 0, w2 = 0;
    int len = 1;
    Kind kind = K_FALLBACK;
    uint8_t fr = F_ALL, fw = 0, live = F_ALL;
    int touch = 3;            /* the accumulators it uses (bit 0 A, bit 1 B) */
    uint32_t target = 0;      /* static branch target */
    int cc = -1;              /* -1: always */
    bool term = false;
    bool push = false;        /* jsr/bsr */
    /* bit branches */
    bool bset = false, bpp = false;
    int bspace = 0, breg = 0, bbit = 0;
    uint32_t baddr = 0;
    /* moves */
    int space = 0;
    /* rep */
    uint32_t count = 0, iw = 0, iw2 = 0;
    int ilen = 1;
    /* movep: the register (-1: an ea) */
    int preg = -1;
    /* add sub cmp and or with an immediate: 0..4, d, the value, sign */
    int iop = 0, id = 0;
    uint32_t ix = 0;
    bool isext = false;
};

/* ---------------------------------------------------------------------- */
/* the helpers the compiled code calls                                    */

extern "C" {
static uint32_t h_read_x(dsp_core_t *c, uint32_t a) { return dsp56k_read_memory(c, DSP_SPACE_X, a & 0xffffff); }
static uint32_t h_read_y(dsp_core_t *c, uint32_t a) { return dsp56k_read_memory(c, DSP_SPACE_Y, a & 0xffffff); }
static void h_write_y(dsp_core_t *c, uint32_t a, uint32_t v) { dsp56k_write_memory(c, DSP_SPACE_Y, a & 0xffffff, v); }
static uint32_t h_ea_rnn(dsp_core_t *c, uint32_t n)
{
    uint32_t cur = c->registers[DSP_REG_R0 + n];
    dsp56k_rec_update_rn(c, n, c->registers[DSP_REG_N0 + n]);
    uint32_t a = c->registers[DSP_REG_R0 + n];
    c->registers[DSP_REG_R0 + n] = cur;
    return a;
}
}

struct Rec {
    DSPState *dsp = nullptr;
    uint8_t *buf = nullptr, *p = nullptr, *end = nullptr;
    size_t size = 0;
    void *entry[8][DSP_PRAM_SIZE];   /* cold: A and B from memory (mode_of) */
    void *warm[8][DSP_PRAM_SIZE];    /* A and B in r12/r13 already */
    uint8_t la_cand[0x10000];
    uint32_t gen = 0xffffffffu;
    Ctx ctx;
    int64_t (*enter)(dsp_core_t *, void *, int64_t, Ctx *) = nullptr;
    uint8_t *exit_code = nullptr;
    uint8_t *code_base = nullptr;   /* past the trampoline */
    bool broken = false;
    size_t max_block = 64;   /* 1: one instruction a block (the selftest's steps) */
    /* counters for the log (dev) */
    uint64_t n_blocks = 0, n_resets = 0;
    uint64_t st_enter = 0, st_jit = 0, st_interp = 0, st_fallback = 0, st_unclean[8] = {};
    uint32_t st_fb_pc[DSP_PRAM_SIZE];
    uint32_t st_frames = 0;
    uint64_t st_compile_ns = 0, st_frame_compile_ns = 0, st_max_frame_compile_ns = 0, st_deferred = 0;
    /* the compile time of this run (one GP frame in realtime): bounded, see compile() */
    uint64_t run_compile_ns = 0;

    bool init();
    void reset();
    void scan();
    void *compile(uint32_t pc, int mode);
    bool clean() const;
};

extern "C" {
/* the compiled code goes on (after a fallback, a native stack op, a loop's
 * end): the state is one it runs in */
static bool h_resume_ok(Ctx *x)
{
    return x->rec->clean();
}

static bool h_write_x(Ctx *x, uint32_t a, uint32_t v)
{
    dsp_core_t *c = &x->rec->dsp->core;
    dsp56k_write_memory(c, DSP_SPACE_X, a & 0xffffff, v);
    return !x->rec->clean() || c->is_idle;
}

/* the word the interpreter runs */
static void h_fallback(Ctx *x, uint32_t pc)
{
    dsp_core_t *c = &x->rec->dsp->core;
    x->rec->st_fallback++;
    x->rec->st_fb_pc[pc & 0xfff]++;
    c->pc = pc;
    dsp56k_execute_instruction(c);
}

/* a two-word one the interpreter runs: true, on inline */
static bool h_fallback2(Ctx *x, uint32_t pc, uint32_t next)
{
    dsp_core_t *c = &x->rec->dsp->core;
    x->rec->st_fallback++;
    x->rec->st_fb_pc[pc & 0xfff]++;
    c->pc = pc;
    dsp56k_execute_instruction(c);
    return c->pc == next && !c->is_idle && x->rec->clean();
}

static bool h_push(Ctx *x, uint32_t ret)
{
    dsp_core_t *c = &x->rec->dsp->core;
    dsp56k_rec_stack_push(c, ret, c->registers[DSP_REG_SR]);
    if (c->interrupt_counter) {
        x->native_irq = 1;
        return false;
    }
    return true;
}

static void h_rts(Ctx *x)
{
    dsp_core_t *c = &x->rec->dsp->core;
    c->pc = dsp56k_rec_stack_pop_pc(c);
    dsp56k_rec_loop_check(c);
    /* the pop, or the loop's end after it, raised one: the interpreter
     * takes it after this instruction */
    if (c->interrupt_counter) {
        x->native_irq = 1;
    }
}

/* pc moved on: the DO test (its end pops the stack) */
static void h_loop_check(Ctx *x)
{
    dsp_core_t *c = &x->rec->dsp->core;
    dsp56k_rec_loop_check(c);
    if (c->interrupt_counter) {
        x->native_irq = 1;
    }
}

/* the loop's end pops SR's mode bits too: on only in the block's mode */
static bool h_loop_end(Ctx *x, int mode)
{
    dsp_core_t *c = &x->rec->dsp->core;
    dsp56k_rec_loop_end(c);
    if (c->interrupt_counter) {
        x->native_irq = 1;
        return false;
    }
    return x->rec->clean() && mode_of(c) == mode;
}
}

/* the code for pc as things are, compiled now if there's room (no reset
 * while compiled code runs): null, back to the dispatcher */
static void *lookup_code(Rec *r)
{
    dsp_core_t *c = &r->dsp->core;
    if (c->is_idle || c->pc >= DSP_PRAM_SIZE || !r->clean()) return nullptr;
    const int sc = mode_of(c);
    void *e = r->entry[sc][c->pc];
    if (!e && r->end - r->p >= 256 * 1024) e = r->compile(c->pc, sc);
    return e;
}

extern "C" {
static void *h_lookup(Ctx *x)
{
    return lookup_code(x->rec);
}

/* a jump to a target not compiled when it was: compiled, the jump pointed
 * at its warm entry (A and B in their registers), its cold one taken now
 * (they're stored) */
static void *h_link(Ctx *x, uint8_t *site)
{
    void *e = lookup_code(x->rec);
    if (e && site) {
        dsp_core_t *c = &x->rec->dsp->core;
        void *w = x->rec->warm[mode_of(c)][c->pc];
        if (w) patch_rel32(site, w);
    }
    return e;
}

/* DO / DOR #n (emu_do_imm): the loop set up */
static bool h_do(Ctx *x, uint32_t la, uint32_t lc, uint32_t ret)
{
    dsp_core_t *c = &x->rec->dsp->core;
    dsp56k_rec_stack_push(c, c->registers[DSP_REG_LA], c->registers[DSP_REG_LC]);
    c->registers[DSP_REG_LA] = la;
    dsp56k_rec_stack_push(c, ret, c->registers[DSP_REG_SR]);
    c->registers[DSP_REG_SR] |= 1 << DSP_SR_LF;
    c->registers[DSP_REG_LC] = lc;
    if (c->interrupt_counter) {
        x->native_irq = 1;
        return false;
    }
    return true;
}

/* pc on to next (the DO test), then its code */
static void *h_next(Ctx *x, uint32_t next)
{
    dsp_core_t *c = &x->rec->dsp->core;
    c->pc = next;
    dsp56k_rec_loop_check(c);
    if (c->interrupt_counter) {
        x->native_irq = 1;
    }
    return lookup_code(x->rec);
}
}

bool Rec::clean() const
{
    const dsp_core_t *c = &dsp->core;
    const uint32_t sr = c->registers[DSP_REG_SR];
    if ((sr & SR_GUARD) || c->loop_rep || c->pc_on_rep || c->interrupt_state != DSP_INTERRUPT_NONE ||
        c->interrupt_counter || (dsp->dma.control & DMA_CONTROL_RUNNING) || c->pram_gen != gen) {
        return false;
    }
    if ((sr & SR_LF) && !la_cand[c->registers[DSP_REG_LA] & 0xffff]) {
        return false;
    }
    /* an Rn past 16 bits (a lua leaves 24): its moves may reach the
     * peripherals, the interpreter's until an update masks it */
    for (int i = 0; i < 8; i++) {
        if (c->registers[DSP_REG_R0 + i] > 0xffff) return false;
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* decoding                                                               */

enum TKind { T_NONE, T_JMP_IMM, T_JCC_IMM, T_BRA_IMM, T_BRA_LONG, T_BCC_IMM, T_BCC_LONG, T_JSR_IMM,
             T_BSR_IMM, T_BSR_LONG, T_RTS, T_JCLR_REG, T_JSET_REG, T_JCLR_PP, T_JSET_PP, T_BRCLR_REG,
             T_BRSET_REG, T_BRCLR_PP, T_BRSET_PP, T_MOVE_X_IMM, T_MOVE_Y_IMM, T_MOVE_X_LONG, T_REP_IMM,
             T_NOP, T_DO, T_DOR, T_ADD_I, T_ADD_L, T_SUB_I, T_SUB_L, T_CMP_I, T_CMP_L, T_AND_I,
             T_AND_L, T_OR_L, T_ANDI, T_ORI, T_DO_IMM, T_DOR_IMM, T_LUA_REL, T_LUA, T_ASL_I, T_ASR_I,
             T_MOVEP_EA, T_MOVEP_QQ, T_MOVEP_REG, T_JSR_EA, T_JMP_EA };

const struct { const char *tmpl; TKind t; } k_templates[] = {
    {"000011000000aaaaaaaaaaaa", T_JMP_IMM},
    {"00001110CCCCaaaaaaaaaaaa", T_JCC_IMM},
    {"00000101000011aaaa0aaaaa", T_BRA_IMM},
    {"000011010001000011000000", T_BRA_LONG},
    {"00000101CCCC01aaaa0aaaaa", T_BCC_IMM},
    {"00001101000100000100CCCC", T_BCC_LONG},
    {"000011010000aaaaaaaaaaaa", T_JSR_IMM},
    {"00000101000010aaaa0aaaaa", T_BSR_IMM},
    {"000011010001000010000000", T_BSR_LONG},
    {"000000000000000000001100", T_RTS},
    {"0000101011DDDDDD000bbbbb", T_JCLR_REG},
    {"0000101011DDDDDD001bbbbb", T_JSET_REG},
    {"0000101010pppppp1S0bbbbb", T_JCLR_PP},
    {"0000101010pppppp1S1bbbbb", T_JSET_PP},
    {"0000110011DDDDDD100bbbbb", T_BRCLR_REG},
    {"0000110011DDDDDD101bbbbb", T_BRSET_REG},
    {"0000110011pppppp0S0bbbbb", T_BRCLR_PP},
    {"0000110011pppppp0S1bbbbb", T_BRSET_PP},
    {"0000001aaaaaaRRR1a0WDDDD", T_MOVE_X_IMM},
    {"0000001aaaaaaRRR1a1WDDDD", T_MOVE_Y_IMM},
    {"0000101001110RRR1WDDDDDD", T_MOVE_X_LONG},
    {"00000110iiiiiiii1010hhhh", T_REP_IMM},
    {"000000000000000000000000", T_NOP},
    {"0000011001MMMRRR0S000000", T_DO},
    {"0000011000aaaaaa0S000000", T_DO},
    {"00000110iiiiiiii1000hhhh", T_DO_IMM},
    {"0000011011DDDDDD00000000", T_DO},
    {"00000110iiiiiiii1001hhhh", T_DOR_IMM},
    {"0000011011DDDDDD00010000", T_DOR},
    {"0000000101iiiiii1000d000", T_ADD_I},
    {"00000001010000001100d000", T_ADD_L},
    {"0000000101iiiiii1000d100", T_SUB_I},
    {"00000001010000001100d100", T_SUB_L},
    {"0000000101iiiiii1000d101", T_CMP_I},
    {"00000001010000001100d101", T_CMP_L},
    {"0000000101iiiiii1000d110", T_AND_I},
    {"00000001010000001100d110", T_AND_L},
    {"00000001010000001100d010", T_OR_L},
    {"00000000iiiiiiii101110EE", T_ANDI},
    {"0000010000aaaRRRaaaadddd", T_LUA_REL},
    {"0000110000011101SiiiiiiD", T_ASL_I},
    {"0000100sW1MMMRRR1Spppppp", T_MOVEP_EA},
    {"0000101111MMMRRR10000000", T_JSR_EA},
    {"0000101011MMMRRR10000000", T_JMP_EA},
    {"00000111W1MMMRRR0Sqqqqqq", T_MOVEP_QQ},
    {"0000100sW1dddddd00pppppp", T_MOVEP_REG},
    {"0000110000011100SiiiiiiD", T_ASR_I},
    {"00000100010MMRRR000ddddd", T_LUA},
    {"00000000iiiiiiii111110EE", T_ORI},
};

std::vector<TKind> g_tkind;
bool g_step_mode;
bool g_exact_budget;

void init_tkinds()
{
    if (!g_tkind.empty()) {
        return;
    }
    const int n = dsp56k_rec_opcode_count();
    g_tkind.assign(n, T_NONE);
    for (int i = 0; i < n; i++) {
        const char *t = dsp56k_rec_opcode_template(i);
        for (const auto &k : k_templates) {
            if (t && !strcmp(t, k.tmpl)) {
                g_tkind[i] = k.t;
            }
        }
    }
}

TKind tkind_of(uint32_t w)
{
    const int i = dsp56k_rec_opcode_index(w);
    return i < 0 ? T_NONE : g_tkind[i];
}

uint32_t pword(const dsp_core_t *c, uint32_t a)
{
    return a < DSP_PRAM_SIZE ? c->pram[a] : 0;
}

int32_t sext(int bits, uint32_t v)
{
    const int s = 32 - bits;
    return (int32_t)(v << s) >> s;
}

/* ALU ops written out here: the CCR bits each one sets (0xff: not native) */
uint8_t alu_writes(int op)
{
    if (op >= 0x80) {
        return F_E | F_U | F_N | F_Z | F_V;
    }
    if (op >= 0x40) {
        switch (op & 7) {
        case 0: case 4: case 5: return F_E | F_U | F_N | F_Z | F_V | F_C; /* add sub cmp */
        case 1: return 0;                                                 /* tfr */
        case 2: case 3: case 6: return F_N | F_Z | F_V;                   /* or eor and */
        default: return 0xff;
        }
    }
    switch (op) {
    case 0x00: case 0x01: case 0x09: return 0;
    case 0x03: case 0x0b: return F_E | F_U | F_N | F_Z | F_V;
    case 0x05: case 0x0d: case 0x10: case 0x18: case 0x14: case 0x1c:
    case 0x20: case 0x28: case 0x30: case 0x38: case 0x24: case 0x2c: case 0x34: case 0x3c:
        return F_E | F_U | F_N | F_Z | F_V | F_C;
    case 0x11: case 0x19: return F_E | F_U | F_N | F_Z | F_V;
    case 0x13: case 0x1b: return F_E | F_U | F_N | F_Z | F_V;
    case 0x22: case 0x2a: case 0x32: case 0x3a: return F_E | F_U | F_N | F_Z | F_V | F_C;
    case 0x26: case 0x2e: case 0x36: case 0x3e: return F_E | F_U | F_N | F_Z | F_V;
    default: return 0xff;
    }
}

bool is_periph_x(uint32_t a) { return a >= DSP_PERIPH_BASE; }

struct Ins;
bool lazy_capable(const Ins &in);

/* a parallel word compiled here: its moves never reach the peripherals (an
 * absolute X address past them runs on the interpreter) */
bool pm_pure(uint32_t w, uint32_t w2)
{
    if ((w & 0xff000000) != 0) {
        return false;
    }
    /* IFcc, IFcc.U (0x202Cxx, 0x203Cxx): the interpreter's emu_ifcc */
    if ((w & 0xffe000) == 0x202000) {
        return false;
    }
    const int t = (w >> 20) & 0xf;
    int mode = -1, n = 0;
    bool xspace = false;
    if (t == 0) {
        mode = (w >> 11) & 7; n = (w >> 8) & 7; xspace = !((w >> 15) & 1);
    } else if (t == 1) {
        mode = (w >> 11) & 7; n = (w >> 8) & 7; xspace = !((w >> 14) & 1);
    } else if (t == 4 && (w & 0xf40000) == 0x400000) {
        if (w & (1 << 14)) { mode = (w >> 11) & 7; n = (w >> 8) & 7; xspace = true; }
    } else if (t >= 4 && t <= 7) {
        if (w & (1 << 14)) { mode = (w >> 11) & 7; n = (w >> 8) & 7; xspace = !((w >> 19) & 1); }
    }
    /* pm_0 and pm_4x take an "immediate" as an address too; a read of a
     * peripheral has no side effect (h_read_x), a write may start a DMA or
     * end the frame: the interpreter's */
    /* (a write takes the "immediate" of mode 6 as its address too) */
    const bool writes = t == 0 || !((w >> 15) & 1);
    if (mode == 6 && xspace && is_periph_x(w2 & 0xffffff) && writes) {
        return false;
    }
    (void)n;
    return true;
}

int pm_len(uint32_t w)
{
    const int t = (w >> 20) & 0xf;
    int mode = -1;
    if (t == 0 || t == 1) {
        mode = (w >> 11) & 7;
    } else if (t >= 4 && t <= 7) {
        if (w & (1 << 14)) {
            mode = (w >> 11) & 7;
        }
    }
    return mode == 6 ? 2 : 1;
}

Ins decode(const dsp_core_t *c, uint32_t pc, const uint8_t *la_cand)
{
    Ins in;
    in.pc = pc;
    in.w = pword(c, pc);
    in.w2 = pword(c, pc + 1);
    const uint32_t w = in.w;
    if (pc >= DSP_PRAM_SIZE || (w & 0xff000000)) {
        in.kind = K_FALLBACK;
        in.term = true;
        return in;
    }
    if (w >= 0x100000) {
        in.len = pm_len(w);
        if (pc + in.len > DSP_PRAM_SIZE || !pm_pure(w, in.w2)) {
            in.len = 1;
            in.term = true;
            return in; /* fallback */
        }
        const uint8_t aw = alu_writes(w & 0xff);
        in.kind = K_PM;
        if (aw == 0xff) {
            in.fr = F_ALL;
            in.fw = 0;
        } else {
            in.fr = 0;
            in.fw = aw;
        }
        in.touch = 3;
        return in;
    }
    const TKind t = tkind_of(w);
    switch (t) {
    case T_NOP:
        in.kind = K_NOP; in.fr = 0; in.touch = 0;
        return in;
    case T_JMP_IMM:
        in.kind = K_JMP; in.target = w & 0xfff; in.term = true; in.touch = 0;
        return in;
    case T_JCC_IMM:
        in.kind = K_JCC; in.target = w & 0xfff; in.cc = (w >> 12) & 0xf; in.term = true; in.touch = 0;
        return in;
    case T_BRA_IMM: case T_BCC_IMM: case T_BSR_IMM: {
        const uint32_t xxx = (w & 0x1f) + ((w & (0xf << 6)) >> 1);
        in.target = (pc + (uint32_t)sext(9, xxx)) & 0xffffff;
        in.kind = t == T_BSR_IMM ? K_JSR : (t == T_BCC_IMM ? K_JCC : K_JMP);
        if (t == T_BCC_IMM) in.cc = (w >> 12) & 0xf;
        in.push = t == T_BSR_IMM;
        in.term = true; in.touch = 0;
        return in;
    }
    case T_BRA_LONG: case T_BCC_LONG: case T_BSR_LONG:
        if (pc + 1 >= DSP_PRAM_SIZE) break;
        in.len = 2;
        in.target = (pc + in.w2) & 0xffffff;
        in.kind = t == T_BSR_LONG ? K_JSR : (t == T_BCC_LONG ? K_JCC : K_JMP);
        if (t == T_BCC_LONG) in.cc = w & 0xf;
        in.push = t == T_BSR_LONG;
        in.term = true; in.touch = 0;
        return in;
    case T_JSR_IMM:
        in.kind = K_JSR; in.push = true; in.target = w & 0xfff; in.term = true; in.touch = 0;
        return in;
    case T_RTS:
        in.kind = K_RTS; in.term = true; in.touch = 0;
        return in;
    case T_JCLR_REG: case T_JSET_REG: case T_BRCLR_REG: case T_BRSET_REG:
    case T_JCLR_PP: case T_JSET_PP: case T_BRCLR_PP: case T_BRSET_PP: {
        if (pc + 1 >= DSP_PRAM_SIZE) break;
        in.kind = K_BITBR;
        in.len = 2;
        in.bset = t == T_JSET_REG || t == T_BRSET_REG || t == T_JSET_PP || t == T_BRSET_PP;
        in.bpp = t == T_JCLR_PP || t == T_JSET_PP || t == T_BRCLR_PP || t == T_BRSET_PP;
        const bool rel = t == T_BRCLR_REG || t == T_BRSET_REG || t == T_BRCLR_PP || t == T_BRSET_PP;
        in.bbit = w & 0x1f;
        in.target = rel ? ((pc + in.w2) & 0xffffff) : in.w2;
        if (in.bpp) {
            in.bspace = (w >> 6) & 1;
            in.baddr = 0xffffc0 + ((w >> 8) & 0x3f);
            in.touch = 0;
        } else {
            in.breg = (w >> 8) & 0x3f;
            if (in.breg == DSP_REG_A0 || in.breg == DSP_REG_A1 || in.breg == DSP_REG_A2 ||
                in.breg == DSP_REG_B0 || in.breg == DSP_REG_B1 || in.breg == DSP_REG_B2) {
                break; /* an accumulator's part: the interpreter */
            }
            in.touch = in.breg == DSP_REG_A ? 1 : (in.breg == DSP_REG_B ? 2 : 0);
        }
        in.fr = 0;
        in.term = true;
        return in;
    }
    case T_MOVE_X_IMM: case T_MOVE_Y_IMM: {
        const int numreg = w & 0xf;
        in.kind = K_MOVE_XY_IMM;
        in.space = t == T_MOVE_Y_IMM ? DSP_SPACE_Y : DSP_SPACE_X;
        const bool W = (w >> 4) & 1;
        /* a write to X may reach the peripherals (and end the run) */
        in.fr = (!W && in.space == DSP_SPACE_X) ? F_ALL : 0;
        in.touch = 3;
        (void)numreg;
        return in;
    }
    case T_MOVE_X_LONG: {
        if (pc + 1 >= DSP_PRAM_SIZE) break;
        const int numreg = w & 0x3f;
        const bool W = (w >> 6) & 1;
        if (numreg >= 0x28) break; /* SR, SP, the stack...: the interpreter */
        in.kind = K_MOVE_X_LONG;
        in.len = 2;
        in.fr = !W ? F_ALL : 0;
        in.touch = 3;
        return in;
    }
    case T_ADD_I: case T_ADD_L: case T_SUB_I: case T_SUB_L: case T_CMP_I: case T_CMP_L:
    case T_AND_I: case T_AND_L: case T_OR_L: {
        const bool lng = t == T_ADD_L || t == T_SUB_L || t == T_CMP_L || t == T_AND_L || t == T_OR_L;
        if (lng && pc + 1 >= DSP_PRAM_SIZE) break;
        in.kind = K_ALUIMM;
        in.len = lng ? 2 : 1;
        in.ix = lng ? (in.w2 & 0xffffff) : ((w >> 8) & 0x3f);
        in.id = (w >> 3) & 1;
        in.iop = (t == T_ADD_I || t == T_ADD_L) ? 0 : (t == T_SUB_I || t == T_SUB_L) ? 1 :
                 (t == T_CMP_I || t == T_CMP_L) ? 2 : (t == T_AND_I || t == T_AND_L) ? 3 : 4;
        in.isext = in.iop <= 2;
        in.fr = 0;
        in.fw = in.iop <= 2 ? (F_E | F_U | F_N | F_Z | F_V | F_C) : (F_N | F_Z | F_V);
        in.touch = 1 << in.id;
        return in;
    }
    case T_ANDI: case T_ORI: {
        const int ee = w & 3;
        /* setting T (a trace at once) or LF: the interpreter's */
        if (ee == 3 || (t == T_ORI && ee == 0 && (((w >> 8) & 0xff) & 0xa0))) break;
        in.kind = K_LOGIC_SR;
        in.iop = t == T_ORI;
        in.id = ee;
        in.ix = (w >> 8) & 0xff;
        in.fr = F_ALL; /* the CCR's: read */
        in.fw = 0;
        in.touch = 0;
        /* MR: the scaling the rest runs in may change, T or LF too */
        if (ee == 0) in.term = true;
        return in;
    }
    case T_JSR_EA: case T_JMP_EA:
        /* to an absolute address (ea mode 6): a static target; the other
         * modes, the interpreter's */
        if (((w >> 8) & 0x3f) != 0x30 || pc + 1 >= DSP_PRAM_SIZE) break;
        in.kind = t == T_JSR_EA ? K_JSR : K_JMP;
        in.push = t == T_JSR_EA;
        in.len = 2;
        in.target = in.w2 & 0xffffff;
        in.term = true;
        in.touch = 0;
        return in;
    case T_MOVEP_EA: case T_MOVEP_QQ: case T_MOVEP_REG: {
        /* a peripheral and a register or memory (emu_movep_23, _x_qq, _0):
         * a write to one may start a DMA or end the frame, out there */
        const bool W = (w >> 15) & 1;
        in.kind = K_MOVEP;
        in.fr = F_ALL;
        in.touch = 3;
        if (t == T_MOVEP_QQ) {
            in.bspace = DSP_SPACE_X;
            in.baddr = 0xffff80 + (w & 0x3f);
        } else {
            in.bspace = (w >> 16) & 1;
            in.baddr = 0xffffc0 + (w & 0x3f);
        }
        if (t == T_MOVEP_REG) {
            in.preg = (w >> 8) & 0x3f;
            /* SSH (a pop), or a control register written: the interpreter's */
            if (in.preg >= 0x28) break;
            return in;
        }
        in.space = (w >> 6) & 1;
        const int mode = (w >> 11) & 7, n = (w >> 8) & 7;
        if (mode == 6) {
            if (pc + 1 >= DSP_PRAM_SIZE) break;
            in.len = 2;
            /* memory at a peripheral's address: the interpreter's */
            if (n == 0 && in.space == DSP_SPACE_X && is_periph_x(in.w2 & 0xffffff)) break;
        }
        return in;
    }
    case T_LUA_REL:
        /* may leave Rn past 16 bits: out there (all CCR bits up to date) */
        in.kind = K_LUA_REL; in.fr = F_ALL; in.touch = 0;
        return in;
    case T_ASL_I: case T_ASR_I: {
        /* asl/asr #n, S, D (n 1..55: the interpreter's shifts are defined) */
        const uint32_t n = (w >> 1) & 0x3f;
        if (n == 0 || n > 55) break;
        in.kind = K_SHIFT_IMM;
        in.iop = t == T_ASL_I;
        in.ix = n;
        in.id = w & 1;
        in.isext = (w >> 7) & 1; /* the source: B */
        in.fr = 0;
        in.fw = F_E | F_U | F_N | F_Z | F_V | F_C;
        return in;
    }
    case T_LUA:
        /* a data ALU destination: the interpreter's */
        if ((w & 0x1f) < DSP_REG_R0) break;
        in.kind = K_LUA; in.fr = 0; in.touch = 0;
        return in;
    case T_DO_IMM: case T_DOR_IMM: {
        if (pc + 2 > DSP_PRAM_SIZE) break;
        in.kind = K_DO_IMM;
        in.len = 2;
        in.count = ((w >> 8) & 0xff) + ((w & 0xf) << 8);
        in.target = t == T_DO_IMM ? (in.w2 & 0xffff) : ((pc + in.w2) & 0xffff); /* LA */
        if (in.count == 0) {
            /* a count of 0 skips the loop: the interpreter's */
            in.kind = K_FB2;
            in.fr = F_ALL;
            return in;
        }
        in.fr = F_ALL; /* SR goes on the stack */
        in.fw = 0;
        in.touch = 0;
        return in;
    }
    case T_DO: case T_DOR:
        /* the interpreter's, two words; the block goes on into the loop */
        if (pc + 2 > DSP_PRAM_SIZE) break;
        in.kind = K_FB2;
        in.len = 2;
        in.fr = F_ALL;
        in.fw = 0;
        return in;
    case T_REP_IMM: {
        /* a REP that ends a DO loop: the interpreter's way (and in the
         * selftest's steps, one word a step) */
        if (pc + 1 >= DSP_PRAM_SIZE || la_cand[pc] || g_step_mode) break;
        const uint32_t iw = pword(c, pc + 1);
        if (iw < 0x100000 || (iw & 0xff000000)) break;
        const int ilen = pm_len(iw);
        if (pc + 1 + ilen > DSP_PRAM_SIZE) break;
        const uint32_t iw2 = pword(c, pc + 2);
        if (!pm_pure(iw, iw2)) break;
        const uint8_t aw = alu_writes(iw & 0xff);
        in.kind = K_REP;
        in.iw = iw; in.iw2 = iw2; in.ilen = ilen;
        in.len = 1 + ilen;
        uint32_t n = ((w >> 8) & 0xff) + ((w & 0xf) << 8);
        in.count = n ? n : 0x10000;
        in.fr = aw == 0xff ? F_ALL : 0;
        in.fw = aw == 0xff ? 0 : aw;
        in.touch = 3;
        return in;
    }
    default:
        break;
    }
    /* the interpreter's: one word or two (it moves pc itself), the block
     * ends there */
    in.kind = K_FALLBACK;
    in.len = 1;
    in.term = true;
    in.fr = F_ALL;
    in.fw = 0;
    in.touch = 3;
    return in;
}

/* the ops whose CCR is the plain test of their result (C, V aside) */
bool lazy_alu(int op)
{
    if (alu_writes(op) == 0xff || alu_writes(op) == 0) return false;
    if (op >= 0x80) return true;
    if (op >= 0x40) return (op & 7) == 0 || (op & 7) == 4 || (op & 7) == 5;
    return op != 0x13 && op != 0x1b; /* clr sets U and Z in any mode */
}

bool lazy_capable(const Ins &in)
{
    switch (in.kind) {
    case K_PM: return lazy_alu(in.w & 0xff);
    case K_REP: return lazy_alu(in.iw & 0xff);
    case K_ALUIMM: return in.iop <= 2;
    case K_SHIFT_IMM: return true;
    default: return false;
    }
}

/* ---------------------------------------------------------------------- */
/* code generation                                                        */

struct Label {
    uint8_t *pos = nullptr;
    std::vector<uint8_t *> fix;
};

enum { C_NONE, C_CLEAN, C_DIRTY };

struct Gen {
    Rec &r;
    Asm a;
    const dsp_core_t *core;
    uint32_t pc0 = 0;
    int scale = 0;      /* SR's S1:S0 the block runs in */
    int mode = 0;       /* its key: scale, and 4 with SM */
    bool sm = false;    /* SR's SM: the ALU results saturated (sm_sat) */
    int acc[2] = {C_NONE, C_NONE};
    int loop_state[2] = {C_NONE, C_NONE};
    std::vector<std::function<void()>> deferred;
    Label loop_head;
    int n_ins = 0;       /* the block's instructions (its budget) */
    /* a loop's last CCR write, read only past the loop: its result in rbp,
     * C and V in slot 5 (bit 31: pending), the rest worked out on the way
     * out */
    int lazy_k = -1;
    bool lazy_pending = false;
    uint8_t lazy_mask = 0;
    int k = 0;           /* instructions emitted so far */
    bool failed = false;

    Gen(Rec &rr, uint8_t *at, uint8_t *lim) : r(rr), a(at, lim), core(&rr.dsp->core) {}

    /* --- encoding --- */
#define E(m, ...) a.op(ZYDIS_MNEMONIC_##m, {__VA_ARGS__})
    ZydisEncoderOperand RG(int idx) { return M(rBX, ROFF(idx), 4); }
    ZydisEncoderOperand SLOT(int i, int size = 4) { return M(rSP, 32 + 8 * i, size); }

    void bind(Label &l)
    {
        l.pos = a.p;
        for (uint8_t *f : l.fix) {
            patch_rel32(f, l.pos);
        }
        l.fix.clear();
    }
    void jcc(unsigned cc, Label &l)
    {
        uint8_t *at = a.jcc(cc, l.pos ? l.pos : a.p);
        if (!l.pos) l.fix.push_back(at);
    }
    void jmp(Label &l)
    {
        uint8_t *at = a.jmp(l.pos ? l.pos : a.p);
        if (!l.pos) l.fix.push_back(at);
    }
    void callfn(const void *fn)
    {
        E(MOV, R(rAX), I((int64_t)(uintptr_t)fn));
        E(CALL, R(rAX));
    }
    void spill() { E(MOV, SLOT(0), R(r8d)); E(MOV, SLOT(1), R(r9d)); E(MOV, SLOT(2), R(r10d)); E(MOV, SLOT(3), R(r11d)); }
    void unspill() { E(MOV, R(r8d), SLOT(0)); E(MOV, R(r9d), SLOT(1)); E(MOV, R(r10d), SLOT(2)); E(MOV, R(r11d), SLOT(3)); }
    /* restore the temporaries but the one the result goes to */
    void unspill_but(Reg keep)
    {
        if (keep != r8d) E(MOV, R(r8d), SLOT(0));
        if (keep != r9d) E(MOV, R(r9d), SLOT(1));
        if (keep != r10d) E(MOV, R(r10d), SLOT(2));
        if (keep != r11d) E(MOV, R(r11d), SLOT(3));
    }

    /* --- the accumulators: 56 bits at the top of r12 (A) and r13 (B) --- */
    Reg accr(int i) { return i ? r13 : r12; }
    void acc_load(int i)
    {
        const Reg T = accr(i);
        E(MOV, R(i ? r13d : r12d), RG(DSP_REG_A1 + i));
        E(SHL, R(T), I(32));
        E(MOV, R(eAX), RG(DSP_REG_A2 + i));
        E(SHL, R(rAX), I(56));
        E(OR, R(T), R(rAX));
        E(MOV, R(eAX), RG(DSP_REG_A0 + i));
        E(SHL, R(rAX), I(8));
        E(OR, R(T), R(rAX));
    }
    void acc_store(int i)
    {
        const Reg T = accr(i);
        E(MOV, R(rAX), R(T));
        E(SHR, R(rAX), I(8));
        E(AND, R(eAX), I(0xffffff));
        E(MOV, RG(DSP_REG_A0 + i), R(eAX));
        E(MOV, R(rAX), R(T));
        E(SHR, R(rAX), I(32));
        E(AND, R(eAX), I(0xffffff));
        E(MOV, RG(DSP_REG_A1 + i), R(eAX));
        E(MOV, R(rAX), R(T));
        E(SHR, R(rAX), I(56));
        E(MOV, RG(DSP_REG_A2 + i), R(eAX));
    }
    Reg use(int i)
    {
        if (acc[i] == C_NONE) {
            acc_load(i);
            acc[i] = C_CLEAN;
        }
        return accr(i);
    }
    void def(int i) { acc[i] = C_DIRTY; }
    void flush_state(const int st[2])
    {
        for (int i = 0; i < 2; i++) {
            if (st[i] == C_DIRTY) acc_store(i);
        }
    }
    void flush()
    {
        flush_state(acc);
        for (int i = 0; i < 2; i++) {
            if (acc[i] == C_DIRTY) acc[i] = C_CLEAN;
        }
    }
    void drop()
    {
        flush();
        acc[0] = acc[1] = C_NONE;
    }

    /* --- exits --- */
    /* back to the dispatcher with pc set by setpc, after k_done of the
     * block's instructions ran */
    void exit_stub(Label &l, int k_done, std::function<void()> setpc, bool dyn = false)
    {
        int st[2] = {acc[0], acc[1]};
        Label *lp = &l;
        deferred.push_back([this, st, k_done, setpc, lp, dyn]() {
            bind(*lp);
            int s2[2] = {st[0], st[1]};
            flush_state(s2);
            if (n_ins - k_done) E(ADD, R(r14), I(n_ins - k_done));
            setpc();
            if (dyn) exit_dyn();
            else a.jmp(r.exit_code);
        });
    }
    /* pc in memory: on to its code if it's there, else the dispatcher */
    void exit_dyn()
    {
        E(MOV, R(ARG0), R(r15));
        callfn((const void *)h_lookup);
        E(TEST, R(rAX), R(rAX));
        a.jcc(CC_E, r.exit_code);
        E(JMP, R(rAX));
    }
    std::vector<Label *> labels;
    Label &newlabel()
    {
        labels.push_back(new Label());
        return *labels.back();
    }
    ~Gen()
    {
        for (Label *l : labels) delete l;
    }

    /* --- the L bit (sticky) --- */
    void set_L_on(unsigned cc)
    {
        Label &stub = newlabel();
        Label &back = newlabel();
        jcc(cc, stub);
        bind(back);
        Label *s = &stub, *b = &back;
        deferred.push_back([this, s, b]() {
            bind(*s);
            E(OR, M(rBX, ROFF(DSP_REG_SR), 4), I(F_L));
            jmp(*b);
        });
    }

    /* --- addressing --- */
    /* Rn += modifier (the interpreter's emu_update_rn): linear here, the
     * other modes on its code. kind: 0 -Nn, 1 +Nn, 2 -1, 3 +1 */
    void update_rn(int n, int kind, Reg cur = ZYDIS_REGISTER_NONE)
    {
        Label &slow = newlabel();
        Label &done = newlabel();
        E(CMP, M(rBX, ROFF(DSP_REG_M0 + n), 2), I(-1));
        jcc(CC_NE, slow);
        /* (int16)Rn + modifier, 16 bits kept; from cur, Rn as loaded (one
         * 32-bit store after: the next load of Rn forwards from it) */
        if (cur != ZYDIS_REGISTER_NONE) {
            switch (kind) {
            case 2: E(LEA, R(rAX), M(r64of(cur), -1, 8)); break;
            case 3: E(LEA, R(rAX), M(r64of(cur), 1, 8)); break;
            default: E(MOV, R(eAX), R(cur)); break;
            }
        } else {
            E(MOV, R(eAX), RG(DSP_REG_R0 + n));
            if (kind == 2) E(DEC, R(eAX));
            if (kind == 3) E(INC, R(eAX));
        }
        if (kind == 0) E(SUB, R(eAX), RG(DSP_REG_N0 + n));
        if (kind == 1) E(ADD, R(eAX), RG(DSP_REG_N0 + n));
        E(MOVZX, R(eAX), R(ZR(AX)));
        E(MOV, RG(DSP_REG_R0 + n), R(eAX));
        bind(done);
        Label *s = &slow, *d = &done;
        deferred.push_back([this, s, d, n, kind]() {
            bind(*s);
            spill();
            E(MOV, R(ARG0), R(rBX));
            E(MOV, R(ARG1D), I(n));
            if (kind <= 1) {
                E(MOV, R(ARG2D), RG(DSP_REG_N0 + n));
                if (kind == 0) E(NEG, R(ARG2D));
            } else {
                E(MOV, R(ARG2D), I(kind == 2 ? -1 : 1));
            }
            callfn((const void *)dsp56k_rec_update_rn);
            unspill();
            jmp(*d);
        });
    }
    /* emu_calc_ea: the address into dst (a temporary); true: an immediate */
    bool ea(int mode, int n, Reg dst, uint32_t w2, bool *is_static, uint32_t *sa)
    {
        *is_static = false;
        switch (mode) {
        case 0: case 1: case 2: case 3:
            E(MOV, R(dst), RG(DSP_REG_R0 + n));
            update_rn(n, mode, dst);
            return false;
        case 4:
            E(MOV, R(dst), RG(DSP_REG_R0 + n));
            return false;
        case 5: {
            Label &slow = newlabel();
            Label &done = newlabel();
            E(MOVZX, R(eAX), M(rBX, ROFF(DSP_REG_M0 + n), 2));
            E(CMP, R(eAX), I(0xffff));
            jcc(CC_NE, slow);
            E(MOV, R(dst), RG(DSP_REG_R0 + n));
            E(ADD, R(dst), RG(DSP_REG_N0 + n));
            E(AND, R(dst), I(0xffff));
            bind(done);
            Label *s = &slow, *d = &done;
            deferred.push_back([this, s, d, n, dst]() {
                bind(*s);
                spill();
                E(MOV, R(ARG0), R(rBX));
                E(MOV, R(ARG1D), I(n));
                callfn((const void *)h_ea_rnn);
                unspill_but(dst);
                E(MOV, R(dst), R(eAX));
                jmp(*d);
            });
            return false;
        }
        case 6:
            *is_static = true;
            *sa = w2 & 0xffffff;
            E(MOV, R(dst), I((int32_t)*sa));
            return n != 0;
        default: /* 7: -(Rn) */
            update_rn(n, 2);
            E(MOV, R(dst), RG(DSP_REG_R0 + n));
            return false;
        }
    }

    /* --- memory --- */
    /* dst = space:[addr]; addr a temporary, kept */
    void rd(int space, Reg addr, Reg dst)
    {
        Label &slow = newlabel();
        Label &done = newlabel();
        if (space == DSP_SPACE_X) {
            E(CMP, R(addr), I(0xc00));
            jcc(CC_AE, slow);
            E(MOV, R(dst), M(rBX, OFF_XRAM, 4, r64of(addr), 4));
        } else {
            E(CMP, R(addr), I(DSP_YRAM_SIZE));
            jcc(CC_AE, slow);
            E(MOV, R(dst), M(rBX, OFF_YRAM, 4, r64of(addr), 4));
        }
        bind(done);
        Label *s = &slow, *d = &done;
        deferred.push_back([this, s, d, space, addr, dst]() {
            bind(*s);
            if (space == DSP_SPACE_X) {
                /* the mix buffer at X:0x1400 first (the GP's programs read
                 * and write its mixbins all the time), then its alias at 0xc00 */
                Label &call = newlabel();
                Label &hi = newlabel();
                E(LEA, R(rAX), M(r64of(addr), -DSP_MIXBUFFER_BASE, 8));
                E(CMP, R(eAX), I(DSP_MIXBUFFER_SIZE));
                jcc(CC_AE, hi);
                E(MOV, R(dst), M(rBX, OFF_MIX, 4, rAX, 4));
                jmp(*d);
                bind(hi);
                E(LEA, R(rAX), M(r64of(addr), -0xc00, 8));
                E(CMP, R(eAX), I(DSP_MIXBUFFER_SIZE));
                jcc(CC_AE, call);
                E(MOV, R(dst), M(rBX, OFF_MIX, 4, rAX, 4));
                jmp(*d);
                bind(call);
            }
            spill();
            E(MOV, R(ARG0), R(rBX));
            E(MOV, R(ARG1D), R(addr));
            callfn((const void *)(space == DSP_SPACE_X ? h_read_x : h_read_y));
            unspill_but(dst);
            E(MOV, R(dst), R(eAX));
            jmp(*d);
        });
    }
    /* space:[addr] = val; for X, exit_after (non-null): a peripheral write
     * that stops the compiled run ends there */
    void wr(int space, Reg addr, Reg val, Label *exit_after)
    {
        Label &slow = newlabel();
        Label &done = newlabel();
        if (space == DSP_SPACE_X) {
            E(CMP, R(addr), I(0xc00));
            jcc(CC_AE, slow);
            E(MOV, M(rBX, OFF_XRAM, 4, r64of(addr), 4), R(val));
        } else {
            E(CMP, R(addr), I(DSP_YRAM_SIZE));
            jcc(CC_AE, slow);
            E(MOV, M(rBX, OFF_YRAM, 4, r64of(addr), 4), R(val));
        }
        bind(done);
        Label *s = &slow, *d = &done;
        deferred.push_back([this, s, d, space, addr, val, exit_after]() {
            bind(*s);
            if (space == DSP_SPACE_X) {
                /* the mix buffer at X:0x1400 first (the GP's programs read
                 * and write its mixbins all the time), then its alias at 0xc00 */
                Label &call = newlabel();
                Label &hi = newlabel();
                E(LEA, R(rAX), M(r64of(addr), -DSP_MIXBUFFER_BASE, 8));
                E(CMP, R(eAX), I(DSP_MIXBUFFER_SIZE));
                jcc(CC_AE, hi);
                E(MOV, M(rBX, OFF_MIX, 4, rAX, 4), R(val));
                jmp(*d);
                bind(hi);
                E(LEA, R(rAX), M(r64of(addr), -0xc00, 8));
                E(CMP, R(eAX), I(DSP_MIXBUFFER_SIZE));
                jcc(CC_AE, call);
                E(MOV, M(rBX, OFF_MIX, 4, rAX, 4), R(val));
                jmp(*d);
                bind(call);
                spill();
                E(MOV, R(ARG0), R(r15));
                E(MOV, R(ARG1D), R(addr));
                E(MOV, R(ARG2D), R(val));
                callfn((const void *)h_write_x);
                unspill();
                if (exit_after) {
                    E(TEST, R(AL), R(AL));
                    jcc(CC_NE, *exit_after);
                }
                jmp(*d);
            } else {
                spill();
                E(MOV, R(ARG0), R(rBX));
                E(MOV, R(ARG1D), R(addr));
                E(MOV, R(ARG2D), R(val));
                callfn((const void *)h_write_y);
                unspill();
                jmp(*d);
            }
        });
    }
    /* static addresses: their place known now */
    void rd_static(int space, uint32_t addr, Reg dst)
    {
        if (space == DSP_SPACE_X && addr < 0xc00) {
            E(MOV, R(dst), M(rBX, OFF_XRAM + 4 * (int)addr, 4));
        } else if (space == DSP_SPACE_X && addr >= DSP_MIXBUFFER_BASE && addr < DSP_MIXBUFFER_BASE + DSP_MIXBUFFER_SIZE) {
            E(MOV, R(dst), M(rBX, OFF_MIX + 4 * (int)(addr - DSP_MIXBUFFER_BASE), 4));
        } else if (space == DSP_SPACE_X && addr >= 0xc00 && addr < 0xc00 + DSP_MIXBUFFER_SIZE) {
            E(MOV, R(dst), M(rBX, OFF_MIX + 4 * (int)(addr - 0xc00), 4));
        } else if (space == DSP_SPACE_Y && addr < DSP_YRAM_SIZE) {
            E(MOV, R(dst), M(rBX, OFF_YRAM + 4 * (int)addr, 4));
        } else {
            spill();
            E(MOV, R(ARG0), R(rBX));
            E(MOV, R(ARG1D), I((int32_t)addr));
            callfn((const void *)(space == DSP_SPACE_X ? h_read_x : h_read_y));
            unspill_but(dst);
            E(MOV, R(dst), R(eAX));
        }
    }
    void wr_static(int space, uint32_t addr, Reg val)
    {
        if (space == DSP_SPACE_X && addr < 0xc00) {
            E(MOV, M(rBX, OFF_XRAM + 4 * (int)addr, 4), R(val));
        } else if (space == DSP_SPACE_X && addr >= DSP_MIXBUFFER_BASE && addr < DSP_MIXBUFFER_BASE + DSP_MIXBUFFER_SIZE) {
            E(MOV, M(rBX, OFF_MIX + 4 * (int)(addr - DSP_MIXBUFFER_BASE), 4), R(val));
        } else if (space == DSP_SPACE_X && addr >= 0xc00 && addr < 0xc00 + DSP_MIXBUFFER_SIZE) {
            E(MOV, M(rBX, OFF_MIX + 4 * (int)(addr - 0xc00), 4), R(val));
        } else if (space == DSP_SPACE_Y && addr < DSP_YRAM_SIZE) {
            E(MOV, M(rBX, OFF_YRAM + 4 * (int)addr, 4), R(val));
        } else {
            /* not the peripherals (pm_pure): the interpreter's handling of
             * the rest (aliases, past the memories) */
            spill();
            E(MOV, R(ARG1D), I((int32_t)addr));
            E(MOV, R(ARG2D), R(val));
            if (space == DSP_SPACE_X) {
                E(MOV, R(ARG0), R(r15));
                callfn((const void *)h_write_x);
            } else {
                E(MOV, R(ARG0), R(rBX));
                callfn((const void *)h_write_y);
            }
            unspill();
        }
    }

    /* --- registers --- */
    /* emu_pm_read_accu24 (scaling off): dst = the accumulator limited to 24
     * bits; ly (optional): the low word pm_4x moves with it */
    /* emu_s_bit: S (sticky, CCR bit 7) on a read of A or B to a data bus:
     * bits 46/45 (no scaling), 47/46 (down), 45/44 (up) of it differ */
    void s_bit(Reg T)
    {
        if (scale == 3) return;
        const int b = scale == 1 ? 55 : (scale == 2 ? 53 : 54); /* v bit of the higher one */
        Label &skip = newlabel();
        E(MOV, R(rAX), R(T));
        E(MOV, R(rDX), R(rAX));
        E(SHL, R(rDX), I(1));
        E(XOR, R(rAX), R(rDX));
        E(BT, R(rAX), I(b));
        jcc(CC_AE, skip);
        E(OR, M(rBX, ROFF(DSP_REG_SR), 4), I(0x80));
        bind(skip);
    }
    void accu24(int i, Reg dst, Reg ly = ZYDIS_REGISTER_NONE)
    {
        const Reg T = use(i);
        Label &sat = newlabel();
        Label &done = newlabel();
        s_bit(T);
        /* the word the mode reads: A1 (S1:S0 00 or 11), A2:A1 >> 1 (S0),
         * A1:A0 << 1 (S1, its test on A2 and bit 46) */
        const int sh = scale == 1 ? 33 : (scale == 2 ? 31 : 32);
        E(MOV, R(rAX), R(T));
        if (scale == 2) {
            E(MOV, R(rDX), R(T));
            E(SHL, R(rDX), I(1));
            E(MOV, R(rCX), I((int64_t)(1ULL << 55)));
            E(AND, R(rDX), R(rCX));
            E(BTR, R(rAX), I(55));
            E(OR, R(rAX), R(rDX));
            E(SAR, R(rAX), I(54));
        } else {
            E(SAR, R(rAX), I(scale == 1 ? 56 : 55));
        }
        E(ADD, R(rAX), I(1));
        E(CMP, R(rAX), I(1));
        jcc(CC_A, sat);
        E(MOV, R(rAX), R(T));
        E(SHR, R(rAX), I(sh));
        E(AND, R(eAX), I(0xffffff));
        if (ly != ZYDIS_REGISTER_NONE) {
            /* emu_pm_read_accu_lo: the 48-bit read's low word, scaled */
            E(MOV, R(rDX), R(T));
            E(SHR, R(rDX), I(scale == 1 ? 9 : (scale == 2 ? 7 : 8)));
            E(AND, R(eDX), I(0xffffff));
            E(MOV, R(ly), R(eDX));
        }
        E(MOV, R(dst), R(eAX));
        bind(done);
        Label *s = &sat, *d = &done;
        deferred.push_back([this, s, d, T, dst, ly]() {
            bind(*s);
            Label &neg = newlabel();
            E(OR, M(rBX, ROFF(DSP_REG_SR), 4), I(F_L));
            E(TEST, R(T), R(T));
            jcc(CC_S, neg);
            E(MOV, R(dst), I(0x7fffff));
            if (ly != ZYDIS_REGISTER_NONE) E(MOV, R(ly), I(0xffffff));
            jmp(*d);
            bind(neg);
            E(MOV, R(dst), I(0x800000));
            if (ly != ZYDIS_REGISTER_NONE) E(MOV, R(ly), I(0));
            jmp(*d);
        });
    }
    /* A or B = a 24-bit word (A0 = 0, A1 = v, A2 its sign) */
    void acc_from24(int i, Reg v)
    {
        const Reg T = accr(i);
        E(MOV, R(eAX), R(v));
        E(SHL, R(eAX), I(8));
        E(MOVSXD, R(T), R(eAX));
        E(SHL, R(T), I(24));
        def(i);
    }
    bool is_accpart(int reg)
    {
        return reg == DSP_REG_A0 || reg == DSP_REG_A1 || reg == DSP_REG_A2 || reg == DSP_REG_B0 ||
               reg == DSP_REG_B1 || reg == DSP_REG_B2;
    }
    int accpart_of(int reg) { return (reg == DSP_REG_B0 || reg == DSP_REG_B1 || reg == DSP_REG_B2) ? 1 : 0; }
    /* a move's source register (A/B limited) */
    void src_reg(int reg, Reg dst)
    {
        if (reg == DSP_REG_A || reg == DSP_REG_B) {
            accu24(reg & 1, dst);
            return;
        }
        if (is_accpart(reg)) {
            const int i = accpart_of(reg);
            if (acc[i] == C_DIRTY) {
                acc_store(i);
                acc[i] = C_CLEAN;
            }
        }
        E(MOV, R(dst), RG(reg));
    }
    /* a move's destination register; masked: & registers_mask */
    void dst_reg(int reg, Reg v, bool masked)
    {
        if (reg == DSP_REG_A || reg == DSP_REG_B) {
            acc_from24(reg & 1, v);
            return;
        }
        if (is_accpart(reg)) {
            const int i = accpart_of(reg);
            flush();
            acc[i] = C_NONE;
        }
        if (masked) {
            const int m = dsp56k_rec_register_mask(reg);
            if (m == 0) {
                E(MOV, RG(reg), I(0));
                return;
            }
            if (m < 32) {
                /* in place: v is the move's last use */
                E(AND, R(v), I((int32_t)((1u << m) - 1)));
                E(MOV, RG(reg), R(v));
                return;
            }
        }
        E(MOV, RG(reg), R(v));
    }

    /* --- the ALU --- */
    /* rax = a 24-bit register << 8, sign extended */
    void s24(Reg d64, int reg)
    {
        const Reg d32 = d64 == rAX ? eAX : (d64 == rCX ? eCX : (d64 == rDX ? eDX : eDI));
        E(MOV, R(d32), RG(reg));
        E(SHL, R(d32), I(8));
        E(MOVSXD, R(d64), R(d32));
    }
    /* the CCR bits in `want` from the value in T (its 56 bits at the top),
     * added to esi */
    void eunz(Reg T, uint8_t want)
    {
        /* emu_ccr_update_e_u_n_z: S1 and S0 both: all four cleared */
        if (scale == 3) return;
        if ((want & (F_E | F_U | F_N)) == (F_E | F_U | F_N) || (want & (F_E | F_U))) {
            /* E U N from the table, Z from the whole */
            if (want & F_Z) {
                E(TEST, R(T), R(T));
                E(SETZ, R(DL));
                E(MOVZX, R(eDX), R(DL));
                E(LEA, R(rSI), M(rSI, 0, 8, rDX, 4));
            }
            E(MOV, R(rAX), R(T));
            E(SHR, R(rAX), I(53));
            E(MOVZX, R(eAX), M(r15, (int64_t)offsetof(Ctx, eun) + 2048 * scale, 1, rAX, 1));
            if ((want & (F_E | F_U | F_N)) != (F_E | F_U | F_N)) {
                E(AND, R(eAX), I(want & (F_E | F_U | F_N)));
            }
            E(OR, R(eSI), R(eAX));
            return;
        }
        const int e_lo = scale == 1 ? 56 : (scale == 2 ? 54 : 55);
        if (want & (F_Z | F_N)) {
            E(TEST, R(T), R(T));
            if (want & F_Z) {
                E(SETZ, R(AL));
                E(MOVZX, R(eAX), R(AL));
                E(LEA, R(rSI), M(rSI, 0, 8, rAX, 4));
            }
            if (want & F_N) {
                E(SETS, R(AL));
                E(MOVZX, R(eAX), R(AL));
                E(LEA, R(rSI), M(rSI, 0, 8, rAX, 8));
            }
        }
        if (want & F_E) {
            E(MOV, R(rAX), R(T));
            E(SAR, R(rAX), I(e_lo));
            E(ADD, R(rAX), I(1));
            E(CMP, R(rAX), I(1));
            E(SETNBE, R(AL));
            E(MOVZX, R(eAX), R(AL));
            E(SHL, R(eAX), I(5));
            E(OR, R(eSI), R(eAX));
        }
        if (want & F_U) {
            E(MOV, R(rAX), R(T));
            E(SHL, R(rAX), I(63 - e_lo));
            E(MOV, R(rDX), R(rAX));
            E(ADD, R(rDX), R(rDX));
            E(XOR, R(rAX), R(rDX));
            E(SETNS, R(AL));
            E(MOVZX, R(eAX), R(AL));
            E(SHL, R(eAX), I(4));
            E(OR, R(eSI), R(eAX));
        }
    }
    /* the CCR bits of an ALU result: now, or (the loop's last) on the way out */
    void flags_out(Reg res, uint8_t w)
    {
        if (k == lazy_k) {
            E(MOV, R(rBP), R(res));
            E(OR, R(eSI), I((int32_t)0x80000000));
            E(MOV, SLOT(5), R(eSI));
            lazy_pending = true;
            lazy_mask = w;
            return;
        }
        eunz(res, w);
        sr_merge(w);
    }
    void materialize()
    {
        E(MOV, R(eSI), SLOT(5));
        E(AND, R(eSI), I(0x7fffffff));
        eunz(rBP, lazy_mask & (F_E | F_U | F_N | F_Z));
        sr_merge(lazy_mask);
        E(MOV, SLOT(5), I(0));
    }
    /* SR's bits in mask = esi's */
    void sr_merge(uint8_t mask)
    {
        if (!mask) return;
        /* one load and one store: SR's chain from op to op through memory
         * forwards once */
        E(MOV, R(eDX), RG(DSP_REG_SR));
        E(AND, R(eDX), I((int32_t)~(uint32_t)mask));
        E(OR, R(eDX), R(eSI));
        E(MOV, RG(DSP_REG_SR), R(eDX));
    }
    /* dsp_rnd56 (of: the rounding add's overflow, SETO, when given) */
    void rnd(Reg T, Reg of = ZYDIS_REGISTER_NONE)
    {
        const Reg T32 = T == r12 ? r12d : r13d;
        if (scale & 1) {
            /* S0: + 1 in A1's bit 0, A1's two low bits off on a tie */
            Label &skip = newlabel();
            E(MOV, R(eAX), I(1));
            E(SHL, R(rAX), I(32));
            E(ADD, R(T), R(rAX));
            if (of != ZYDIS_REGISTER_NONE) E(SETO, R(of));
            E(TEST, R(T32), R(T32));
            jcc(CC_NE, skip);
            E(BT, R(T), I(32));
            jcc(CC_B, skip);
            E(BTR, R(T), I(33));
            bind(skip);
            E(BTR, R(T), I(32));
            E(SHR, R(T), I(32));
            E(SHL, R(T), I(32));
            return;
        }
        if (scale == 2) {
            /* S1: + 1 << 22, A0 keeps its bit 23 unless the rest is 0 */
            Label &keep2 = newlabel();
            E(MOV, R(eAX), I(1 << 30));
            E(ADD, R(T), R(rAX));
            if (of != ZYDIS_REGISTER_NONE) E(SETO, R(of));
            E(MOV, R(eAX), R(T32));
            E(AND, R(eAX), I((int32_t)0x80000000));
            E(TEST, R(T32), I(0x7fffff00));
            jcc(CC_NE, keep2);
            E(XOR, R(eAX), R(eAX));
            bind(keep2);
            E(SHR, R(T), I(32));
            E(SHL, R(T), I(32));
            E(OR, R(T), R(rAX));
            return;
        }
        Label &keep = newlabel();
        E(MOV, R(eAX), I((int32_t)0x80000000));
        E(ADD, R(T), R(rAX));
        if (of != ZYDIS_REGISTER_NONE) E(SETO, R(of));
        E(TEST, R(T32), R(T32));
        jcc(CC_NE, keep);
        E(BTR, R(T), I(32));
        bind(keep);
        E(SHR, R(T), I(32));
        E(SHL, R(T), I(32));
    }
    /* SM (emu_sm_sat3): T saturated to 48 bits when its bits 55 48 47 differ
     * (host 63 56 55), V (if live) and L then. Most results have bits 55-47
     * all the same: one test, the rest out of line */
    void sm_sat(Reg T, bool rounded, uint8_t w)
    {
        if (!sm) return;
        Label &slow = newlabel();
        Label &back = newlabel();
        E(MOV, R(rAX), R(T));
        E(SAR, R(rAX), I(55));
        E(ADD, R(rAX), I(1));
        E(CMP, R(rAX), I(1));
        jcc(CC_A, slow);
        bind(back);
        Label *s = &slow, *b = &back;
        deferred.push_back([this, s, b, T, rounded, w]() {
            bind(*s);
            Label &neg = newlabel();
            Label &set = newlabel();
            E(MOV, R(rAX), R(T));
            E(SHR, R(rAX), I(55));
            E(MOV, R(eDX), R(eAX));
            E(SHR, R(eDX), I(6));
            E(AND, R(eDX), I(4));
            E(AND, R(eAX), I(3));
            E(OR, R(eAX), R(eDX));
            E(TEST, R(eAX), R(eAX));
            jcc(CC_E, *b);
            E(CMP, R(eAX), I(7));
            jcc(CC_E, *b);
            E(BT, R(T), I(63));
            jcc(CC_B, neg);
            E(MOV, R(T), I((int64_t)(rounded ? 0x007fffff00000000ULL : 0x007fffffffffff00ULL)));
            jmp(set);
            bind(neg);
            E(MOV, R(T), I((int64_t)0xff80000000000000ULL));
            bind(set);
            if (w & F_V) E(OR, R(eSI), I(F_V));
            E(OR, M(rBX, ROFF(DSP_REG_SR), 4), I(F_L));
            jmp(*b);
        });
    }
    /* rcx = the 56-bit source of add/sub/cmp: the other accumulator, X, Y,
     * or a 24-bit register */
    void alu_src(int op, int d)
    {
        if (op < 0x20) {
            E(MOV, R(rCX), R(use(d ^ 1)));
        } else if (op < 0x40) {
            const int hi = (op >= 0x30) ? DSP_REG_Y1 : DSP_REG_X1;
            const int lo = (op >= 0x30) ? DSP_REG_Y0 : DSP_REG_X0;
            E(MOV, R(eCX), RG(hi));
            E(SHL, R(rCX), I(40));
            E(SAR, R(rCX), I(8));
            E(MOV, R(eDX), RG(lo));
            E(SHL, R(rDX), I(8));
            E(OR, R(rCX), R(rDX));
        } else {
            static const int src[4] = {DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1};
            E(MOV, R(eCX), RG(src[(op >> 4) & 3]));
            E(SHL, R(eCX), I(8));
            E(MOVSXD, R(rCX), R(eCX));
            E(SHL, R(rCX), I(24));
        }
    }
    /* add (sub, cmp): host flags are the DSP's */
    void alu_addsub(int op, int d, int what /* 0 add 1 sub 2 cmp */, uint8_t live)
    {
        const uint8_t w = (F_E | F_U | F_N | F_Z | F_V | F_C) & live;
        alu_src(op, d);
        const Reg T = use(d);
        Reg res = T;
        if (what == 2) {
            E(MOV, R(rDI), R(T));
            res = rDI;
        }
        if (w) E(XOR, R(eSI), R(eSI)); /* esi only for flags computed */
        if (what == 0) E(ADD, R(res), R(rCX));
        else E(SUB, R(res), R(rCX));
        if (w & F_C) E(SETB, R(ZR(SIL)));
        if (w & F_V) {
            E(SETO, R(AL));
            E(MOVZX, R(eAX), R(AL));
            E(LEA, R(rSI), M(rSI, 0, 8, rAX, 2));
        }
        set_L_on(CC_O);
        sm_sat(res, false, w);
        flags_out(res, w);
        if (what != 2) def(d);
    }
    void alu_native(int op, uint8_t live)
    {
        const int d = (op >> 3) & 1;
        if (op >= 0x80) {
            static const int pairs[8][2] = {{DSP_REG_X0, DSP_REG_X0}, {DSP_REG_Y0, DSP_REG_Y0},
                                            {DSP_REG_X1, DSP_REG_X0}, {DSP_REG_Y1, DSP_REG_Y0},
                                            {DSP_REG_X0, DSP_REG_Y1}, {DSP_REG_Y0, DSP_REG_X0},
                                            {DSP_REG_X1, DSP_REG_Y0}, {DSP_REG_Y1, DSP_REG_X1}};
            const int pr = (op >> 4) & 7, kk = op & 7;
            const bool minus = kk & 4, round = kk & 1, mac = kk & 2;
            const uint8_t w = (F_E | F_U | F_N | F_Z | F_V) & live;
            if (mac) use(d);
            s24(rAX, pairs[pr][0]);
            if (pairs[pr][1] == pairs[pr][0]) {
                E(MOV, R(rCX), R(rAX));
            } else {
                s24(rCX, pairs[pr][1]);
            }
            E(IMUL, R(rAX), R(rCX));
            E(SAR, R(rAX), I(7));
            if (minus) E(NEG, R(rAX));
            if (w) E(XOR, R(eSI), R(eSI)); /* esi only for flags computed */
            const Reg T = accr(d);
            if (mac && round) {
                /* macr: V and L of D + P + r as one sum, the add's overflow
                 * XOR the rounding's (emu_macr) */
                E(ADD, R(T), R(rAX));
                E(SETO, R(DL));
                rnd(T, CL);
                E(XOR, R(DL), R(CL));
                E(MOVZX, R(eDX), R(DL));
                if (w & F_V) E(LEA, R(rSI), M(rSI, 0, 8, rDX, 2));
                E(TEST, R(eDX), R(eDX));
                set_L_on(CC_NE);
            } else if (mac) {
                E(ADD, R(T), R(rAX));
                if (w & F_V) {
                    E(SETO, R(DL));
                    E(MOVZX, R(eDX), R(DL));
                    E(LEA, R(rSI), M(rSI, 0, 8, rDX, 2));
                }
                set_L_on(CC_O);
            } else {
                E(MOV, R(T), R(rAX));
                if (round) rnd(T);
            }
            sm_sat(T, round, w);
            def(d);
            flags_out(T, w);
            return;
        }
        if (op >= 0x40) {
            switch (op & 7) {
            case 0: alu_addsub(op, d, 0, live); return;
            case 4: alu_addsub(op, d, 1, live); return;
            case 5: alu_addsub(op, d, 2, live); return;
            case 1: { /* tfr reg24 */
                static const int src[4] = {DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1};
                const Reg T = accr(d);
                E(MOV, R(eAX), RG(src[(op >> 4) & 3]));
                E(SHL, R(eAX), I(8));
                E(MOVSXD, R(T), R(eAX));
                E(SHL, R(T), I(24));
                def(d);
                return;
            }
            default: { /* or eor and: A1 with a 24-bit register */
                static const int src[4] = {DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1};
                const uint8_t w = (F_N | F_Z | F_V) & live;
                const Reg T = use(d);
                E(MOV, R(eCX), RG(src[(op >> 4) & 3]));
                E(SHL, R(rCX), I(32));
                if ((op & 7) == 2) {
                    E(OR, R(T), R(rCX));
                } else if ((op & 7) == 3) {
                    E(XOR, R(T), R(rCX));
                } else {
                    E(MOV, R(rDX), I((int64_t)0xff000000ffffffffULL));
                    E(OR, R(rCX), R(rDX));
                    E(AND, R(T), R(rCX));
                }
                def(d);
                if (w) {
                    if (w) E(XOR, R(eSI), R(eSI)); /* esi only for flags computed */
                    E(MOV, R(rAX), R(T));
                    E(SHL, R(rAX), I(8));
                    E(SHR, R(rAX), I(40));
                    E(TEST, R(eAX), R(eAX));
                    if (w & F_Z) {
                        E(SETZ, R(DL));
                        E(MOVZX, R(eDX), R(DL));
                        E(LEA, R(rSI), M(rSI, 0, 8, rDX, 4));
                    }
                    if (w & F_N) {
                        E(SHR, R(eAX), I(23));
                        E(LEA, R(rSI), M(rSI, 0, 8, rAX, 8));
                    }
                    sr_merge(w);
                }
                return;
            }
            }
        }
        switch (op) {
        case 0x00:
            return;
        case 0x01: case 0x09: /* tfr acc */
            E(MOV, R(accr(d)), R(use(d ^ 1)));
            def(d);
            return;
        case 0x03: case 0x0b: { /* tst */
            const uint8_t w = (F_E | F_U | F_N | F_Z | F_V) & live;
            if (!w) return;
            const Reg T = use(d);
            if (w) E(XOR, R(eSI), R(eSI)); /* esi only for flags computed */
            flags_out(T, w);
            return;
        }
        case 0x05: case 0x0d: alu_addsub(op, d, 2, live); return;
        case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: alu_addsub(op, d, 0, live); return;
        case 0x14: case 0x1c: case 0x24: case 0x2c: case 0x34: case 0x3c: alu_addsub(op, d, 1, live); return;
        case 0x11: case 0x19: { /* rnd: V and L of the rounding add (emu_rnd_a) */
            const uint8_t w = (F_E | F_U | F_N | F_Z | F_V) & live;
            const Reg T = use(d);
            if (w) E(XOR, R(eSI), R(eSI)); /* esi only for flags computed */
            rnd(T, CL);
            def(d);
            E(MOVZX, R(eCX), R(CL));
            if (w & F_V) E(LEA, R(rSI), M(rSI, 0, 8, rCX, 2));
            E(TEST, R(eCX), R(eCX));
            set_L_on(CC_NE);
            sm_sat(T, true, w);
            flags_out(T, w);
            return;
        }
        case 0x13: case 0x1b: { /* clr */
            const uint8_t w = (F_E | F_U | F_N | F_Z | F_V) & live;
            const Reg T = accr(d);
            E(XOR, R(T == r12 ? r12d : r13d), R(T == r12 ? r12d : r13d));
            def(d);
            if (w) {
                E(MOV, R(eSI), I((int32_t)(w & (F_U | F_Z))));
                sr_merge(w);
            }
            return;
        }
        case 0x32: case 0x3a: { /* asl */
            const uint8_t w = (F_E | F_U | F_N | F_Z | F_V | F_C) & live;
            const Reg T = use(d);
            if (w) E(XOR, R(eSI), R(eSI)); /* esi only for flags computed */
            E(SHL, R(T), I(1));
            if (w & F_C) E(SETB, R(ZR(SIL)));
            if (w & F_V) {
                E(SETO, R(AL));
                E(MOVZX, R(eAX), R(AL));
                E(LEA, R(rSI), M(rSI, 0, 8, rAX, 2));
            }
            set_L_on(CC_O); /* the manual's: L as V */
            sm_sat(T, false, w);
            def(d);
            flags_out(T, w);
            return;
        }
        case 0x22: case 0x2a: { /* asr: arithmetic (dsp_asr56) */
            const uint8_t w = (F_E | F_U | F_N | F_Z | F_V | F_C) & live;
            const Reg T = use(d);
            if (w) E(XOR, R(eSI), R(eSI)); /* esi only for flags computed */
            E(BT, R(T), I(8));
            if (w & F_C) E(SETB, R(ZR(SIL)));
            E(SAR, R(T), I(9));
            E(SHL, R(T), I(8));
            sm_sat(T, false, w);
            def(d);
            flags_out(T, w);
            return;
        }
        case 0x26: case 0x2e: case 0x36: case 0x3e: { /* abs, neg */
            const uint8_t w = (F_E | F_U | F_N | F_Z | F_V) & live;
            const Reg T = use(d);
            if (w) E(XOR, R(eSI), R(eSI)); /* esi only for flags computed */
            E(MOV, R(rAX), I((int64_t)0x8000000000000000ULL));
            E(CMP, R(T), R(rAX));
            E(SETZ, R(DL));
            E(MOVZX, R(eDX), R(DL));
            E(MOV, R(rAX), R(T));
            E(NEG, R(rAX));
            if (op == 0x36 || op == 0x3e) {
                E(MOV, R(T), R(rAX));
            } else {
                E(TEST, R(T), R(T));
                E(CMOVS, R(T), R(rAX));
            }
            def(d);
            if (w & F_V) E(LEA, R(rSI), M(rSI, 0, 8, rDX, 2));
            E(TEST, R(eDX), R(eDX));
            set_L_on(CC_NE);
            sm_sat(T, false, w);
            flags_out(T, w);
            return;
        }
        }
    }
    /* the ALU part of a parallel word: native, or the interpreter's function */
    void alu(int op, uint8_t live)
    {
        if (alu_writes(op) != 0xff) {
            alu_native(op, live);
            return;
        }
        spill();
        drop();
        E(MOV, R(ARG0), R(rBX));
        callfn(dsp56k_rec_alu_fn(op));
        if (sm) {
            E(MOV, R(ARG0), R(rBX));
            E(MOV, R(ARG1D), I(op));
            callfn((const void *)dsp56k_rec_alu_sm);
        }
        unspill();
    }

    /* --- parallel words (emu_pm_*): sources, the ALU, destinations --- */
    void pm(uint32_t w, uint32_t w2, uint8_t live)
    {
        const int t = (w >> 20) & 0xf;
        const int op = w & 0xff;
        bool st;
        uint32_t sa;
        if (t == 0) {
            const int memspace = (w >> 15) & 1, numreg = (w >> 16) & 1;
            ea((w >> 11) & 7, (w >> 8) & 7, r10d, w2, &st, &sa);
            accu24(numreg, r8d);
            E(MOV, R(r9d), RG(DSP_REG_X0 + (memspace << 1)));
            alu(op, live);
            if (st) wr_static(memspace, sa, r8d);
            else wr(memspace, r10d, r8d, nullptr);
            acc_from24(numreg, r9d);
            return;
        }
        if (t == 1) {
            const bool imm = ea((w >> 11) & 7, (w >> 8) & 7, r10d, w2, &st, &sa);
            const int memspace = (w >> 14) & 1;
            static const int xr[4] = {DSP_REG_X0, DSP_REG_X1, DSP_REG_A, DSP_REG_B};
            static const int yr[4] = {DSP_REG_Y0, DSP_REG_Y1, DSP_REG_A, DSP_REG_B};
            const int numreg1 = memspace ? yr[(w >> 16) & 3] : xr[(w >> 18) & 3];
            const bool write1 = (w >> 15) & 1;
            if (write1) {
                if (imm) E(MOV, R(r8d), R(r10d));
                else if (st) rd_static(memspace, sa, r8d);
                else rd(memspace, r10d, r8d);
            } else {
                src_reg(numreg1, r8d);
            }
            const int s2 = memspace ? ((w >> 19) & 1) : ((w >> 17) & 1);
            accu24(s2, r9d);
            alu(op, live);
            if (write1) {
                if (numreg1 == DSP_REG_A || numreg1 == DSP_REG_B) {
                    acc_from24(numreg1 & 1, r8d);
                }
                /* the interpreter stores it in registers[numreg1] too */
                E(MOV, RG(numreg1), R(r8d));
            } else {
                if (st) wr_static(memspace, sa, r8d);
                else wr(memspace, r10d, r8d, nullptr);
            }
            const int d2 = memspace ? (DSP_REG_X0 + ((w >> 18) & 1)) : (DSP_REG_Y0 + ((w >> 16) & 1));
            E(MOV, RG(d2), R(r9d));
            return;
        }
        if (t == 2 || t == 3) {
            if (t == 2 && (w & 0xffff00) == 0x200000) {
                alu(op, live);
                return;
            }
            if (t == 2 && (w & 0xffe000) == 0x204000) {
                const int e = (w >> 8) & 0x1f;
                ea((e >> 3) & 7, e & 7, r10d, w2, &st, &sa);
                alu(op, live);
                return;
            }
            if (t == 2 && (w & 0xfc0000) == 0x200000) {
                const int srcreg = (w >> 13) & 0x1f, dstreg = (w >> 8) & 0x1f;
                src_reg(srcreg, r8d);
                alu(op, live);
                dst_reg(dstreg, r8d, true);
                return;
            }
            /* pm_3: #xx,D */
            alu(op, live);
            const int dstreg = (w >> 16) & 0x1f;
            uint32_t v = (w >> 8) & 0xff;
            if (dstreg >= DSP_REG_X0 && dstreg <= DSP_REG_B && !(dstreg >= DSP_REG_A0 && dstreg <= DSP_REG_B1)) {
                v <<= 16;
            }
            if (dstreg == DSP_REG_A || dstreg == DSP_REG_B) {
                E(MOV, R(r8d), I((int32_t)v));
                acc_from24(dstreg & 1, r8d);
            } else {
                E(MOV, R(r8d), I((int32_t)v));
                dst_reg(dstreg, r8d, true);
            }
            return;
        }
        if (t == 4 && (w & 0xf40000) == 0x400000) {
            /* pm_4x: l:ea or l:aa */
            const int value = (w >> 8) & 0x3f;
            if (w & (1 << 14)) {
                ea((w >> 11) & 7, (w >> 8) & 7, r10d, w2, &st, &sa);
            } else {
                st = true;
                sa = value;
                E(MOV, R(r10d), I(value));
            }
            const int numreg = ((w >> 16) & 3) | ((w >> 17) & 4);
            const bool wrd = (w >> 15) & 1;
            if (wrd) {
                if (st) { rd_static(DSP_SPACE_X, sa, r8d); rd_static(DSP_SPACE_Y, sa, r9d); }
                else { rd(DSP_SPACE_X, r10d, r8d); rd(DSP_SPACE_Y, r10d, r9d); }
            } else {
                switch (numreg) {
                case 0: case 1: {
                    const Reg T = use(numreg);
                    s_bit(T);
                    E(MOV, R(rAX), R(T)); E(SHR, R(rAX), I(32)); E(AND, R(eAX), I(0xffffff)); E(MOV, R(r8d), R(eAX));
                    E(MOV, R(rAX), R(T)); E(SHR, R(rAX), I(8)); E(AND, R(eAX), I(0xffffff)); E(MOV, R(r9d), R(eAX));
                    break;
                }
                case 2: E(MOV, R(r8d), RG(DSP_REG_X1)); E(MOV, R(r9d), RG(DSP_REG_X0)); break;
                case 3: E(MOV, R(r8d), RG(DSP_REG_Y1)); E(MOV, R(r9d), RG(DSP_REG_Y0)); break;
                case 4: case 5: accu24(numreg & 1, r8d, r9d); break;
                case 6: accu24(0, r8d); accu24(1, r9d); break;
                default: accu24(1, r8d); accu24(0, r9d); break;
                }
            }
            alu(op, live);
            if (wrd) {
                switch (numreg) {
                case 0: case 1: {
                    const Reg T = use(numreg);
                    E(SHR, R(T), I(56)); E(SHL, R(T), I(56));
                    E(MOV, R(eAX), R(r8d)); E(SHL, R(rAX), I(32)); E(OR, R(T), R(rAX));
                    E(MOV, R(eAX), R(r9d)); E(SHL, R(rAX), I(8)); E(OR, R(T), R(rAX));
                    def(numreg);
                    break;
                }
                case 2: E(MOV, RG(DSP_REG_X1), R(r8d)); E(MOV, RG(DSP_REG_X0), R(r9d)); break;
                case 3: E(MOV, RG(DSP_REG_Y1), R(r8d)); E(MOV, RG(DSP_REG_Y0), R(r9d)); break;
                case 4: case 5: {
                    const Reg T = accr(numreg & 1);
                    E(MOV, R(eAX), R(r8d)); E(SHL, R(eAX), I(8)); E(MOVSXD, R(T), R(eAX)); E(SHL, R(T), I(24));
                    E(MOV, R(eAX), R(r9d)); E(SHL, R(rAX), I(8)); E(OR, R(T), R(rAX));
                    def(numreg & 1);
                    break;
                }
                case 6: acc_from24(0, r8d); acc_from24(1, r9d); break;
                default: acc_from24(1, r8d); acc_from24(0, r9d); break;
                }
            } else {
                if (st) { wr_static(DSP_SPACE_X, sa, r8d); wr_static(DSP_SPACE_Y, sa, r9d); }
                else { wr(DSP_SPACE_X, r10d, r8d, nullptr); wr(DSP_SPACE_Y, r10d, r9d, nullptr); }
            }
            return;
        }
        if (t >= 4 && t <= 7) {
            /* pm_5: x:ea/aa or y:ea/aa with one register */
            bool imm = false;
            const int value = (w >> 8) & 0x3f;
            if (w & (1 << 14)) {
                imm = ea((w >> 11) & 7, (w >> 8) & 7, r10d, w2, &st, &sa);
            } else {
                st = true;
                sa = value;
            }
            const int memspace = (w >> 19) & 1;
            const int numreg = ((w >> 16) & 7) | ((w >> 17) & (3 << 3));
            const bool wrd = (w >> 15) & 1;
            if (wrd) {
                if (imm) E(MOV, R(r8d), I((int32_t)sa));
                else if (st) rd_static(memspace, sa, r8d);
                else rd(memspace, r10d, r8d);
            } else {
                src_reg(numreg, r8d);
            }
            alu(op, live);
            if (wrd) {
                dst_reg(numreg, r8d, true);
            } else {
                if (st) wr_static(memspace, sa, r8d);
                else wr(memspace, r10d, r8d, nullptr);
            }
            return;
        }
        /* pm_8: x:ea and y:ea */
        int ea1 = (w >> 8) & 0x1f;
        if ((ea1 >> 3) == 0) ea1 |= 1 << 5;
        int ea2 = ((w >> 13) & 3) | ((w >> 17) & (3 << 3));
        if ((ea1 & (1 << 2)) == 0) ea2 |= 1 << 2;
        if ((ea2 >> 3) == 0) ea2 |= 1 << 5;
        ea((ea1 >> 3) & 7, ea1 & 7, r10d, w2, &st, &sa);
        ea((ea2 >> 3) & 7, ea2 & 7, r11d, w2, &st, &sa);
        static const int xr[4] = {DSP_REG_X0, DSP_REG_X1, DSP_REG_A, DSP_REG_B};
        static const int yr[4] = {DSP_REG_Y0, DSP_REG_Y1, DSP_REG_A, DSP_REG_B};
        const int numreg1 = xr[(w >> 18) & 3], numreg2 = yr[(w >> 16) & 3];
        const bool w1 = (w >> 15) & 1, w2b = (w >> 22) & 1;
        if (w1) rd(DSP_SPACE_X, r10d, r8d);
        else src_reg(numreg1, r8d);
        if (w2b) rd(DSP_SPACE_Y, r11d, r9d);
        else src_reg(numreg2, r9d);
        alu(op, live);
        if (w1) {
            if (numreg1 == DSP_REG_A || numreg1 == DSP_REG_B) acc_from24(numreg1 & 1, r8d);
            else E(MOV, RG(numreg1), R(r8d));
        } else {
            wr(DSP_SPACE_X, r10d, r8d, nullptr);
        }
        if (w2b) {
            if (numreg2 == DSP_REG_A || numreg2 == DSP_REG_B) acc_from24(numreg2 & 1, r9d);
            else E(MOV, RG(numreg2), R(r9d));
        } else {
            wr(DSP_SPACE_Y, r11d, r9d, nullptr);
        }
    }

    /* dsp_write_reg for the registers a move reaches here */
    void write_reg(int numreg, Reg v)
    {
        if (numreg == DSP_REG_A || numreg == DSP_REG_B) {
            /* A0 = 0, A1 = v as it is, A2 its sign */
            const int i = numreg & 1;
            flush();
            acc[i] = C_NONE;
            E(MOV, RG(DSP_REG_A0 + i), I(0));
            E(MOV, RG(DSP_REG_A1 + i), R(v));
            E(MOV, R(eAX), R(v));
            E(SHL, R(eAX), I(8));
            E(SAR, R(eAX), I(31));
            E(AND, R(eAX), I(0xff));
            E(MOV, RG(DSP_REG_A2 + i), R(eAX));
            return;
        }
        dst_reg(numreg, v, true);
    }

    /* --- block exits and transfers --- */
    /* the DO test where pc moves to `to` (dsp_postexecute_update_pc), then
     * on to it */
    void transfer(uint32_t to)
    {
        const uint32_t la = (to - 1) & 0xffffff;
        if (to >= 1 && la <= 0xffff && r.la_cand[la]) {
            Label &plain = newlabel();
            Label &end = newlabel();
            E(TEST, M(rBX, ROFF(DSP_REG_SR), 4), I(SR_LF));
            jcc(CC_E, plain);
            E(CMP, RG(DSP_REG_LA), I((int32_t)la));
            jcc(CC_NE, plain);
            E(MOV, R(eAX), RG(DSP_REG_LC));
            E(DEC, R(eAX));
            E(AND, R(eAX), I(0xffff));
            E(MOV, RG(DSP_REG_LC), R(eAX));
            jcc(CC_E, end);
            /* again: pc = SSH */
            Label &other = newlabel();
            E(MOV, R(eAX), RG(DSP_REG_SSH));
            E(CMP, R(eAX), I((int32_t)pc0));
            jcc(CC_NE, other);
            back_edge();
            bind(other);
            {
                E(MOV, M(rBX, OFF_PC, 4), R(eAX));
                if (lazy_pending) materialize();
                int st[2] = {acc[0], acc[1]};
                flush_state(st);
                exit_dyn();
            }
            bind(end);
            {
                Label &bad = newlabel();
                if (lazy_pending) materialize();
                const bool was = lazy_pending;
                lazy_pending = false;
                E(MOV, R(ARG0), R(r15));
                E(MOV, R(ARG1D), I(mode));
                callfn((const void *)h_loop_end);
                E(TEST, R(AL), R(AL));
                jcc(CC_E, bad);
                transfer_plain(to);
                bind(bad);
                int st[2] = {acc[0], acc[1]};
                flush_state(st);
                E(MOV, M(rBX, OFF_PC, 4), I((int32_t)to));
                a.jmp(r.exit_code);
                lazy_pending = was;
            }
            bind(plain);
        }
        transfer_plain(to);
    }
    /* both accumulators in their registers, for a warm entry */
    void warm_up()
    {
        for (int i = 0; i < 2; i++) {
            if (acc[i] == C_NONE) acc_load(i);
        }
    }
    void back_edge()
    {
        warm_up();
        jmp(loop_head);
    }
    void transfer_plain(uint32_t to)
    {
        if (to == pc0) {
            back_edge();
            return;
        }
        if (lazy_pending) materialize();
        if (to < DSP_PRAM_SIZE) {
            /* on in the registers to the target's warm entry */
            warm_up();
            if (r.warm[mode][to]) {
                a.jmp(r.warm[mode][to]);
                return;
            }
        } else {
            int st[2] = {acc[0], acc[1]};
            flush_state(st);
        }
        /* a jump to a stub that compiles the target and points the jump
         * at it */
        uint8_t *site = a.jmp(a.p);
        uint8_t *stub = a.p;
        patch_rel32(site, stub);
        if (to < DSP_PRAM_SIZE) {
            acc_store(0);
            acc_store(1);
        }
        E(MOV, M(rBX, OFF_PC, 4), I((int32_t)to));
        if (to < DSP_PRAM_SIZE) {
            E(MOV, R(ARG0), R(r15));
            E(MOV, R(ARG1), I((int64_t)(uintptr_t)site));
            callfn((const void *)h_link);
            E(TEST, R(rAX), R(rAX));
            a.jcc(CC_E, r.exit_code);
            E(JMP, R(rAX));
        } else {
            a.jmp(r.exit_code);
        }
    }
    /* the condition cc of SR true: jump to l */
    void jcc_dsp(int cc, Label &l)
    {
        E(MOV, R(eAX), RG(DSP_REG_SR));
        const bool neg = cc >= 8;
        switch (cc & 7) {
        case 0: E(TEST, R(eAX), I(F_C)); jcc(neg ? CC_NE : CC_E, l); return;
        case 2: E(TEST, R(eAX), I(F_Z)); jcc(neg ? CC_NE : CC_E, l); return;
        case 3: E(TEST, R(eAX), I(F_N)); jcc(neg ? CC_NE : CC_E, l); return;
        case 5: E(TEST, R(eAX), I(F_E)); jcc(neg ? CC_NE : CC_E, l); return;
        case 6: E(TEST, R(eAX), I(F_L)); jcc(neg ? CC_NE : CC_E, l); return;
        case 1: /* GE: N ^ V == 0 */
            E(MOV, R(eCX), R(eAX)); E(SHR, R(eCX), I(2)); E(XOR, R(eCX), R(eAX));
            E(TEST, R(eCX), I(2)); jcc(neg ? CC_NE : CC_E, l); return;
        case 7: /* GT: Z | (N ^ V) == 0 */
            E(MOV, R(eCX), R(eAX)); E(SHR, R(eCX), I(2)); E(XOR, R(eCX), R(eAX));
            E(MOV, R(eDX), R(eAX)); E(SHR, R(eDX), I(1)); E(OR, R(eCX), R(eDX));
            E(TEST, R(eCX), I(2)); jcc(neg ? CC_NE : CC_E, l); return;
        default: /* 4 NN: (Z | (~U & ~E)) == 0 */
            E(MOV, R(eCX), R(eAX)); E(NOT, R(eCX)); E(MOV, R(eDX), R(eCX)); E(SHR, R(eDX), I(1));
            E(AND, R(eCX), R(eDX)); E(SHR, R(eCX), I(2)); E(OR, R(eCX), R(eAX));
            E(TEST, R(eCX), I(4)); jcc(neg ? CC_NE : CC_E, l); return;
        }
    }

    /* --- one instruction --- */
    void ins(Ins &in, std::vector<Ins> &v, size_t idx)
    {
        const uint32_t next = in.pc + in.len;
        switch (in.kind) {
        case K_NOP:
            return;
        case K_PM:
            pm(in.w, in.w2, in.live);
            return;
        case K_REP: {
            Label &loop = newlabel();
            E(MOV, R(eAX), RG(DSP_REG_LC));
            E(MOV, RG(DSP_REG_LCSAVE), R(eAX));
            if (in.count > 1) E(SUB, R(r14), I((int32_t)(in.count - 1)));
            E(MOV, SLOT(4, 8), I((int32_t)in.count));
            /* the accumulators stay in their registers round the loop,
             * taken as written (a pass may have) */
            for (int i = 0; i < 2; i++) {
                use(i);
                acc[i] = C_DIRTY;
            }
            bind(loop);
            pm(in.iw, in.iw2, in.live);
            for (int i = 0; i < 2; i++) {
                if (acc[i] == C_NONE) acc_load(i);
                acc[i] = C_DIRTY;
            }
            E(DEC, SLOT(4, 8));
            jcc(CC_NE, loop);
            E(MOV, R(eAX), RG(DSP_REG_LCSAVE));
            E(MOV, RG(DSP_REG_LC), R(eAX));
            return;
        }
        case K_MOVE_XY_IMM: {
            const uint32_t w = in.w;
            const uint32_t xxx = (((w >> 11) & 0x3f) << 1) + ((w >> 6) & 1);
            const bool W = (w >> 4) & 1;
            const int n = (w >> 8) & 7, numreg = w & 0xf;
            E(MOV, R(r10d), RG(DSP_REG_R0 + n));
            E(ADD, R(r10d), I(sext(7, xxx)));
            E(AND, R(r10d), I(0xffffff));
            if (!W) {
                src_reg(numreg, r8d);
                Label &ex = newlabel();
                wr(in.space, r10d, r8d, in.space == DSP_SPACE_X ? &ex : nullptr);
                if (in.space == DSP_SPACE_X) after_exit(ex, k + 1, next);
            } else {
                rd(in.space, r10d, r8d);
                write_reg(numreg, r8d);
            }
            return;
        }
        case K_MOVE_X_LONG: {
            const uint32_t w = in.w;
            const bool W = (w >> 6) & 1;
            const int n = (w >> 8) & 7, numreg = w & 0x3f;
            E(MOV, R(r10d), RG(DSP_REG_R0 + n));
            E(ADD, R(r10d), I((int32_t)(in.w2 & 0xffffff)));
            E(AND, R(r10d), I(0xffffff));
            if (!W) {
                src_reg(numreg, r8d);
                Label &ex = newlabel();
                wr(DSP_SPACE_X, r10d, r8d, &ex);
                after_exit(ex, k + 1, next);
            } else {
                rd(DSP_SPACE_X, r10d, r8d);
                write_reg(numreg, r8d);
            }
            return;
        }
        case K_ALUIMM: {
            const int d = in.id;
            if (in.iop <= 2) {
                const uint8_t w = (F_E | F_U | F_N | F_Z | F_V | F_C) & in.live;
                const Reg T = use(d);
                if (in.isext) {
                    E(MOV, R(eCX), I((int32_t)(in.ix << 8)));
                    E(MOVSXD, R(rCX), R(eCX));
                    E(SHL, R(rCX), I(24));
                } else {
                    E(MOV, R(eCX), I((int32_t)in.ix));
                    E(SHL, R(rCX), I(32));
                }
                Reg res = T;
                if (in.iop == 2) {
                    E(MOV, R(rDI), R(T));
                    res = rDI;
                }
                if (w) E(XOR, R(eSI), R(eSI)); /* esi only for flags computed */
                if (in.iop == 0) E(ADD, R(res), R(rCX));
                else E(SUB, R(res), R(rCX));
                if (w & F_C) E(SETB, R(ZR(SIL)));
                if (w & F_V) {
                    E(SETO, R(AL));
                    E(MOVZX, R(eAX), R(AL));
                    E(LEA, R(rSI), M(rSI, 0, 8, rAX, 2));
                }
                set_L_on(CC_O);
                sm_sat(res, false, w);
                flags_out(res, w);
                if (in.iop != 2) def(d);
            } else {
                /* A1 &= x or |= x */
                const uint8_t w = (F_N | F_Z | F_V) & in.live;
                const Reg T = use(d);
                E(MOV, R(eCX), I((int32_t)in.ix));
                E(SHL, R(rCX), I(32));
                if (in.iop == 4) {
                    E(OR, R(T), R(rCX));
                } else {
                    E(MOV, R(rDX), I((int64_t)0xff000000ffffffffULL));
                    E(OR, R(rCX), R(rDX));
                    E(AND, R(T), R(rCX));
                }
                def(d);
                if (w) {
                    if (w) E(XOR, R(eSI), R(eSI)); /* esi only for flags computed */
                    E(MOV, R(rAX), R(T));
                    E(SHL, R(rAX), I(8));
                    E(SHR, R(rAX), I(40));
                    E(TEST, R(eAX), R(eAX));
                    if (w & F_Z) {
                        E(SETZ, R(DL));
                        E(MOVZX, R(eDX), R(DL));
                        E(LEA, R(rSI), M(rSI, 0, 8, rDX, 4));
                    }
                    if (w & F_N) {
                        E(SHR, R(eAX), I(23));
                        E(LEA, R(rSI), M(rSI, 0, 8, rAX, 8));
                    }
                    sr_merge(w);
                }
            }
            return;
        }
        case K_FB2: {
            drop();
            Label &out = newlabel();
            E(MOV, R(ARG0), R(r15));
            E(MOV, R(ARG1D), I((int32_t)in.pc));
            E(MOV, R(ARG2D), I((int32_t)next));
            callfn((const void *)h_fallback2);
            E(TEST, R(AL), R(AL));
            jcc(CC_E, out);
            exit_stub(out, k + 1, []() {}, true);
            return;
        }
        case K_MOVEP: {
            const uint32_t w = in.w;
            const bool W = (w >> 15) & 1;
            const int mode = (w >> 11) & 7, n = (w >> 8) & 7;
            bool st = false, imm = false;
            uint32_t sa = 0;
            if (in.preg < 0) imm = ea(mode, n, r10d, in.w2, &st, &sa);
            if (W) {
                /* the source, then the peripheral written */
                if (in.preg >= 0) src_reg(in.preg, r8d);
                else if (imm) E(MOV, R(r8d), R(r10d));
                else if (st) rd_static(in.space, sa, r8d);
                else rd(in.space, r10d, r8d);
                spill();
                E(MOV, R(ARG1D), I((int32_t)in.baddr));
                E(MOV, R(ARG2D), R(r8d));
                if (in.bspace == DSP_SPACE_X) {
                    E(MOV, R(ARG0), R(r15));
                    callfn((const void *)h_write_x);
                    unspill();
                    Label &ex = newlabel();
                    E(TEST, R(AL), R(AL));
                    jcc(CC_NE, ex);
                    after_exit(ex, k + 1, next);
                } else {
                    E(MOV, R(ARG0), R(rBX));
                    callfn((const void *)h_write_y);
                    unspill();
                }
            } else {
                /* the peripheral read, then the destination */
                spill();
                E(MOV, R(ARG0), R(rBX));
                E(MOV, R(ARG1D), I((int32_t)in.baddr));
                callfn((const void *)(in.bspace == DSP_SPACE_X ? h_read_x : h_read_y));
                unspill_but(r8d);
                E(MOV, R(r8d), R(eAX));
                if (in.preg >= 0) {
                    write_reg(in.preg, r8d);
                } else if (st) {
                    wr_static(in.space, sa, r8d);
                } else {
                    wr(in.space, r10d, r8d, nullptr);
                }
            }
            return;
        }
        case K_SHIFT_IMM: {
            const int n = (int)in.ix, d = in.id, src = in.isext ? 1 : 0;
            const uint8_t w = (F_E | F_U | F_N | F_Z | F_V | F_C) & in.live;
            const Reg S = use(src);
            const Reg T = accr(d);
            if (w) E(XOR, R(eSI), R(eSI)); /* esi only for flags computed */
            if (in.iop) {
                /* dsp_asl56: C the last bit out, V if bit 55 changes at any
                 * time of the shift (bits 55..55-n not all the same), L as V */
                E(MOV, R(rAX), R(S));
                if (w & F_C) {
                    E(BT, R(rAX), I(64 - n));
                    E(SETB, R(ZR(SIL)));
                }
                E(MOV, R(rDX), R(rAX));
                E(SAR, R(rDX), I(63 - n));
                E(ADD, R(rDX), I(1));
                E(CMP, R(rDX), I(1));
                E(SETNBE, R(DL));
                E(MOVZX, R(eDX), R(DL));
                if (w & F_V) E(LEA, R(rSI), M(rSI, 0, 8, rDX, 2));
                E(TEST, R(eDX), R(eDX));
                set_L_on(CC_NE);
                E(SHL, R(rAX), I(n));
                E(MOV, R(T), R(rAX));
            } else {
                /* dsp_asr56: arithmetic, C the last bit out */
                E(MOV, R(rAX), R(S));
                if (w & F_C) {
                    E(BT, R(rAX), I(n + 7));
                    E(SETB, R(ZR(SIL)));
                }
                E(SAR, R(rAX), I(8 + n));
                E(SHL, R(rAX), I(8));
                E(MOV, R(T), R(rAX));
            }
            sm_sat(T, false, w);
            def(d);
            flags_out(T, w);
            return;
        }
        case K_LUA_REL: {
            const uint32_t w = in.w;
            const uint32_t aa = ((w >> 4) & 0xf) + (((w >> 11) & 7) << 4);
            const int n = (w >> 8) & 7, d = w & 7;
            E(MOV, R(eAX), RG(DSP_REG_R0 + n));
            E(ADD, R(eAX), I(sext(7, aa)));
            E(AND, R(eAX), I(0xffffff));
            E(MOV, RG(((w >> 3) & 1) ? DSP_REG_N0 + d : DSP_REG_R0 + d), R(eAX));
            if (!((w >> 3) & 1)) {
                Label &ex = newlabel();
                E(CMP, R(eAX), I(0xffff));
                jcc(CC_A, ex);
                after_exit(ex, k + 1, next);
            }
            return;
        }
        case K_LUA: {
            /* emu_lua: Rn as the ea's update leaves it, Rn kept */
            const uint32_t w = in.w;
            const int e = (w >> 8) & 0x1f, n = (w >> 8) & 7, d = w & 7;
            E(MOV, R(r8d), RG(DSP_REG_R0 + n));
            bool st;
            uint32_t sa;
            ea((e >> 3) & 7, e & 7, r10d, 0, &st, &sa);
            E(MOV, R(r9d), RG(DSP_REG_R0 + n));
            E(MOV, RG(DSP_REG_R0 + n), R(r8d));
            E(MOV, RG(((w >> 3) & 1) ? DSP_REG_N0 + d : DSP_REG_R0 + d), R(r9d));
            return;
        }
        case K_DO_IMM: {
            Label &bad = newlabel();
            E(MOV, R(ARG0), R(r15));
            E(MOV, R(ARG1D), I((int32_t)in.target));
            E(MOV, R(ARG2D), I((int32_t)in.count));
            E(MOV, R(ARG3), I((int32_t)next));
            callfn((const void *)h_do);
            E(TEST, R(AL), R(AL));
            jcc(CC_E, bad);
            exit_stub(bad, k + 1, [this, next]() { E(MOV, M(rBX, OFF_PC, 4), I((int32_t)next)); });
            return;
        }
        case K_LOGIC_SR: {
            const uint32_t v = in.ix;
            if (in.id == 2) {
                if (in.iop) E(OR, RG(DSP_REG_OMR), I((int32_t)v));
                else E(AND, RG(DSP_REG_OMR), I((int32_t)v));
            } else if (in.id == 1) {
                if (in.iop) E(OR, RG(DSP_REG_SR), I((int32_t)v));
                else E(AND, RG(DSP_REG_SR), I((int32_t)(0xff00 | v)));
            } else {
                if (in.iop) E(OR, RG(DSP_REG_SR), I((int32_t)(v << 8)));
                else E(AND, RG(DSP_REG_SR), I((int32_t)((v << 8) | 0xff)));
            }
            if (in.term) {
                /* MR: the next one in the mode now set */
                int st[2] = {acc[0], acc[1]};
                flush_state(st);
                E(MOV, R(ARG0), R(r15));
                E(MOV, R(ARG1D), I((int32_t)next));
                callfn((const void *)h_next);
                E(TEST, R(rAX), R(rAX));
                a.jcc(CC_E, r.exit_code);
                E(JMP, R(rAX));
            }
            return;
        }
        case K_FALLBACK:
            /* it moves pc itself (and runs the DO test): out at its pc */
            drop();
            E(MOV, R(ARG0), R(r15));
            E(MOV, R(ARG1D), I((int32_t)in.pc));
            callfn((const void *)h_fallback);
            exit_dyn();
            return;
        case K_JMP:
            flush_pending_k();
            transfer(in.target);
            return;
        case K_JSR: {
            Label &bad = newlabel();
            E(MOV, R(ARG0), R(r15));
            E(MOV, R(ARG1D), I((int32_t)next));
            callfn((const void *)h_push);
            E(TEST, R(AL), R(AL));
            jcc(CC_E, bad);
            transfer(in.target);
            bind(bad);
            {
                int st[2] = {acc[0], acc[1]};
                flush_state(st);
                E(MOV, M(rBX, OFF_PC, 4), I((int32_t)in.target));
                a.jmp(r.exit_code);
            }
            return;
        }
        case K_RTS: {
            int st[2] = {acc[0], acc[1]};
            flush_state(st);
            E(MOV, R(ARG0), R(r15));
            callfn((const void *)h_rts);
            exit_dyn();
            return;
        }
        case K_JCC: {
            Label &taken = newlabel();
            jcc_dsp(in.cc, taken);
            transfer(next);
            bind(taken);
            transfer(in.target);
            return;
        }
        case K_BITBR: {
            if (in.bpp) {
                spill();
                E(MOV, R(ARG0), R(rBX));
                E(MOV, R(ARG1D), I((int32_t)in.baddr));
                callfn((const void *)(in.bspace ? h_read_y : h_read_x));
                E(MOV, R(r8d), R(eAX));
            } else {
                src_reg(in.breg, r8d);
            }
            Label &taken = newlabel();
            E(BT, R(r8d), I(in.bbit));
            jcc(in.bset ? CC_B : CC_AE, taken);
            transfer(next);
            bind(taken);
            transfer(in.target);
            return;
        }
        }
        (void)v;
        (void)idx;
    }
    void flush_pending_k() {}
    /* a peripheral write that stops the run: pc on, the DO test, out */
    void after_exit(Label &ex, int k_done, uint32_t next)
    {
        int st[2] = {acc[0], acc[1]};
        Label *e = &ex;
        deferred.push_back([this, st, k_done, next, e]() {
            bind(*e);
            int s2[2] = {st[0], st[1]};
            flush_state(s2);
            if (n_ins - k_done) E(ADD, R(r14), I(n_ins - k_done));
            E(MOV, M(rBX, OFF_PC, 4), I((int32_t)next));
            E(MOV, R(ARG0), R(r15));
            callfn((const void *)h_loop_check);
            a.jmp(r.exit_code);
        });
    }
#undef E
};

/* ---------------------------------------------------------------------- */

void *alloc_exec(size_t size)
{
#ifdef _WIN32
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

void free_exec(void *p, size_t size)
{
#ifdef _WIN32
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

bool Rec::init()
{
    init_tkinds();
    size = 8u << 20;
    buf = (uint8_t *)alloc_exec(size);
    if (!buf) {
        return false;
    }
    end = buf + size;
    ctx.rec = this;
    ctx.patch_site = nullptr;
    ctx.native_irq = 0;
    eun_tables(&ctx);
    /* the trampoline: enter(core, code, budget, ctx) -> budget left */
    Asm a(buf, end);
#define E(m, ...) a.op(ZYDIS_MNEMONIC_##m, {__VA_ARGS__})
    enter = (int64_t (*)(dsp_core_t *, void *, int64_t, Ctx *))(void *)a.p;
    E(PUSH, R(rBX)); E(PUSH, R(rBP)); E(PUSH, R(rSI)); E(PUSH, R(rDI));
    E(PUSH, R(r12)); E(PUSH, R(r13)); E(PUSH, R(r14)); E(PUSH, R(r15));
    E(SUB, R(rSP), I(88));
    E(MOV, R(rBX), R(ARG0));
    E(MOV, R(r14), R(ARG2));
    E(MOV, R(r15), R(ARG3));
    E(MOV, M(rSP, 32 + 8 * 5, 8), I(0));
    E(JMP, R(ARG1));
    exit_code = a.p;
    E(MOV, R(rAX), R(r14));
    E(ADD, R(rSP), I(88));
    E(POP, R(r15)); E(POP, R(r14)); E(POP, R(r13)); E(POP, R(r12));
    E(POP, R(rDI)); E(POP, R(rSI)); E(POP, R(rBP)); E(POP, R(rBX));
    E(RET);
#undef E
    if (a.failed) {
        return false;
    }
    code_base = a.p + 16;
    reset();
    return true;
}

void Rec::reset()
{
    p = code_base;
    memset(entry, 0, sizeof(entry));
    memset(warm, 0, sizeof(warm));
    gen = dsp->core.pram_gen;
    scan();
    n_resets++;
}

/* the LA values the program's DO loops set */
void Rec::scan()
{
    memset(la_cand, 0, sizeof(la_cand));
    const dsp_core_t *c = &dsp->core;
    for (uint32_t pc = 0; pc + 1 < DSP_PRAM_SIZE; pc++) {
        const uint32_t w = c->pram[pc];
        if (w >= 0x100000 || (w & 0xff000000)) continue;
        const TKind t = tkind_of(w);
        if (t == T_DO || t == T_DO_IMM) la_cand[c->pram[pc + 1] & 0xffff] = 1;
        if (t == T_DOR || t == T_DOR_IMM) la_cand[(pc + c->pram[pc + 1]) & 0xffff] = 1;
    }
}

struct CompileTimer {
    Rec *r;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~CompileTimer()
    {
        const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - t0).count();
        r->st_compile_ns += ns;
        r->st_frame_compile_ns += ns;
        r->run_compile_ns += ns;
    }
};

/* a GP frame compiles for this long at most (XBASED_DSP_COMPILE_CAP_US, 0
 * off): a new program's ~600 blocks took 11 ms of one 0.67 ms frame (Black);
 * past it the interpreter runs those words (exact) and the next frames
 * compile the rest */
static uint64_t compile_cap_ns()
{
    static int64_t cap = -1;
    if (cap < 0) {
        const char *e = getenv("XBASED_DSP_COMPILE_CAP_US");
        cap = (e && *e ? strtoll(e, nullptr, 10) : 250) * 1000;
        if (cap < 0) cap = 0;
    }
    return (uint64_t)cap;
}

void *Rec::compile(uint32_t pc0, int mode)
{
    if (pc0 >= DSP_PRAM_SIZE) return nullptr;
    if (entry[mode][pc0]) return entry[mode][pc0];
    if (dsp->is_gp && !g_step_mode && !g_exact_budget && compile_cap_ns() &&
        run_compile_ns >= compile_cap_ns()) {
        st_deferred++;
        return nullptr;
    }
    CompileTimer timer{this};
    if (end - p < 64 * 1024) {
        reset();
    }
    const dsp_core_t *c = &dsp->core;

    /* the block: on to a branch, a DO end, or 64 instructions */
    std::vector<Ins> v;
    uint32_t pc = pc0;
    bool ends_at_la = false;
    for (;;) {
        Ins in = decode(c, pc, la_cand);
        v.push_back(in);
        if (in.term) break;
        const uint32_t next = pc + in.len;
        if (next - 1 <= 0xffff && la_cand[next - 1]) { ends_at_la = true; break; }
        if (v.size() >= max_block || next >= DSP_PRAM_SIZE) break;
        pc = next;
    }
    /* condition codes: the ones read before written */
    uint8_t live = F_ALL;
    for (size_t i = v.size(); i-- > 0;) {
        v[i].live = live;
        live = (uint8_t)((live & ~v[i].fw) | v[i].fr);
    }
    bool backedge = ends_at_la;
    for (const Ins &in : v) {
        if (in.term && in.target == pc0 && in.kind != K_RTS && in.kind != K_FALLBACK) backedge = true;
    }

    Gen g(*this, p, end);
    g.pc0 = pc0;
    g.scale = mode & 3;
    g.mode = mode;
    g.sm = (mode & 4) != 0;
    g.n_ins = (int)v.size();
    uint8_t *start = g.a.p;
    /* cold: A and B loaded; warm (from another block's code): already in
     * r12/r13, maybe newer than memory */
    for (int i = 0; i < 2; i++) {
        g.acc_load(i);
        g.acc[i] = C_DIRTY;
        g.loop_state[i] = C_DIRTY;
    }
    /* a loop's last CCR write left for the way out (lazy) */
    if (backedge) {
        int last = -1;
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i].fw) last = (int)i;
        }
        bool ok = last >= 0 && lazy_capable(v[last]);
        for (size_t i = last + 1; ok && i < v.size(); i++) {
            if (v[i].fr) ok = false;
        }
        if (ok) {
            /* the flags the next pass reads before it writes them */
            uint8_t lb = 0;
            for (size_t i = v.size(); i-- > 0;) {
                lb = (uint8_t)((lb & ~v[i].fw) | v[i].fr);
            }
            if (!(lb & v[last].fw)) g.lazy_k = last;
        }
    }
    uint8_t *warm_at = g.a.p;
    g.bind(g.loop_head);
    {
        Label &stub = g.newlabel();
        g.a.op(ZYDIS_MNEMONIC_SUB, {R(r14), I(g.n_ins)});
        g.jcc(CC_L, stub);
        Gen *gp = &g;
        g.exit_stub(stub, 0, [gp, pc0]() {
            if (gp->lazy_k >= 0) {
                /* a pass's CCR pending from the one before */
                Label &skip = gp->newlabel();
                gp->a.op(ZYDIS_MNEMONIC_MOV, {R(eSI), gp->SLOT(5)});
                gp->a.op(ZYDIS_MNEMONIC_TEST, {R(eSI), R(eSI)});
                gp->jcc(CC_NS, skip);
                gp->materialize();
                gp->bind(skip);
            }
            gp->a.op(ZYDIS_MNEMONIC_MOV, {M(rBX, OFF_PC, 4), I((int32_t)pc0)});
        });
    }
    for (size_t i = 0; i < v.size(); i++) {
        g.k = (int)i;
        g.ins(v[i], v, i);
    }
    const Ins &last = v.back();
    if (!last.term) {
        g.transfer(last.pc + last.len);
    }
    for (size_t i = 0; i < g.deferred.size(); i++) {
        g.deferred[i]();
    }
    if (g.a.failed || g.failed) {
        return nullptr;
    }
    p = g.a.p;
    entry[mode][pc0] = start;
    warm[mode][pc0] = warm_at;
    n_blocks++;
    return start;
}

Rec *rec_of(DSPState *dsp)
{
    Rec *r = (Rec *)dsp->rec;
    if (r) return r->broken ? nullptr : r;
    r = new Rec();
    r->dsp = dsp;
    if (!r->init()) {
        nc_log("xbased-apu", "dsp: no executable memory for the DSP recompiler, the interpreter runs it");
        r->broken = true;
        dsp->rec = r;
        return nullptr;
    }
    dsp->rec = r;
    return r;
}

} // namespace

extern "C" bool dsp_rec_run(DSPState *dsp, int cycles)
{
    Rec *r = rec_of(dsp);
    if (!r) return false;
    dsp_core_t *c = &dsp->core;
    dsp->save_cycles += cycles;
    if (dsp->save_cycles <= 0) return true;
    r->run_compile_ns = 0;
    int dma_timer = 0;
    while (dsp->save_cycles > 0) {
        if (c->pram_gen != r->gen) r->reset();
        if (r->clean() && c->pc < DSP_PRAM_SIZE) {
            void *code = r->compile(c->pc, mode_of(c));
            if (code) {
                /* a block's worth past the cycles: the last one runs here,
                 * not word by word on the interpreter */
                const int64_t budget = dsp->save_cycles / 2 + ((g_step_mode || g_exact_budget) ? 1 : 64);
                r->ctx.patch_site = nullptr;
                r->ctx.native_irq = 0;
                const int64_t left = r->enter(c, code, budget, &r->ctx);
                int64_t ran = budget - left;
                if (ran < 0) ran = 0;
                r->st_enter++;
                r->st_jit += ran;
                dsp->save_cycles -= (int)(2 * ran);
                c->cycle_count += (uint32_t)ran;
                if (r->ctx.native_irq) {
                    dsp56k_rec_post_interrupts(c);
                }
                if (c->is_idle) break;
                if (ran > 0) continue;
            }
        }
        /* the interpreter, one word (dsp_run) */
        {
            const uint32_t sr = c->registers[DSP_REG_SR];
            r->st_interp++;
            r->st_unclean[(sr & SR_GUARD) ? 0 : (c->loop_rep || c->pc_on_rep) ? 1 :
                          (c->interrupt_state || c->interrupt_counter) ? 2 :
                          (dsp->dma.control & DMA_CONTROL_RUNNING) ? 3 :
                          ((sr & SR_LF) && !r->la_cand[c->registers[DSP_REG_LA] & 0xffff]) ? 4 :
                          c->pc >= DSP_PRAM_SIZE ? 5 : 6]++;
        }
        dsp56k_execute_instruction(c);
        dsp->save_cycles -= c->instr_cycle;
        c->cycle_count++;
        if (dsp->dma.control & DMA_CONTROL_RUNNING) {
            dma_timer++;
        }
        if (dma_timer > 2) {
            dma_timer = 0;
            dsp->dma.control &= ~DMA_CONTROL_RUNNING;
            dsp->dma.control |= DMA_CONTROL_STOPPED;
        }
        if (c->is_idle) break;
    }
    return true;
}

/* dev (XBASED_APU_STATS): where the GP's instructions ran, once a second */
extern "C" void dsp_rec_frame_stats(DSPState *dsp)
{
    Rec *r = (Rec *)dsp->rec;
    if (!r || r->broken) return;
    if (r->st_frame_compile_ns > r->st_max_frame_compile_ns) r->st_max_frame_compile_ns = r->st_frame_compile_ns;
    r->st_frame_compile_ns = 0;
    if (++r->st_frames < 1500) return;
    r->st_frames = 0;
    nc_log("xbased-apu", "dsp rec: compiled %llu blocks in all, %.2f ms this second, %.2f ms its worst frame, "
           "%llu left for later frames", (unsigned long long)r->n_blocks, r->st_compile_ns / 1e6,
           r->st_max_frame_compile_ns / 1e6, (unsigned long long)r->st_deferred);
    r->st_compile_ns = 0;
    r->st_max_frame_compile_ns = 0;
    r->st_deferred = 0;
    char top[256];
    size_t o = 0;
    for (int k = 0; k < 6; k++) {
        int best = -1;
        for (int i = 0; i < DSP_PRAM_SIZE; i++) {
            if (r->st_fb_pc[i] && (best < 0 || r->st_fb_pc[i] > r->st_fb_pc[best])) best = i;
        }
        if (best < 0) break;
        o += snprintf(top + o, sizeof(top) - o, " %04x:%06x*%u", best, dsp->core.pram[best], r->st_fb_pc[best]);
        r->st_fb_pc[best] = 0;
    }
    top[o] = 0;
    nc_log("xbased-apu", "dsp rec: enters=%llu jit=%llu interp=%llu (scale/T %llu rep %llu irq %llu dma %llu la %llu pc %llu other %llu) fallbacks=%llu blocks=%llu resets=%llu top:%s",
           (unsigned long long)r->st_enter, (unsigned long long)r->st_jit, (unsigned long long)r->st_interp,
           (unsigned long long)r->st_unclean[0], (unsigned long long)r->st_unclean[1], (unsigned long long)r->st_unclean[2],
           (unsigned long long)r->st_unclean[3], (unsigned long long)r->st_unclean[4], (unsigned long long)r->st_unclean[5],
           (unsigned long long)r->st_unclean[6], (unsigned long long)r->st_fallback, (unsigned long long)r->n_blocks,
           (unsigned long long)r->n_resets, top);
    r->st_enter = r->st_jit = r->st_interp = r->st_fallback = 0;
    memset(r->st_unclean, 0, sizeof(r->st_unclean));
    memset(r->st_fb_pc, 0, sizeof(r->st_fb_pc));
}

extern "C" bool dsp_rec_available(DSPState *dsp)
{
    return rec_of(dsp) != nullptr;
}

extern "C" void dsp_rec_invalidate(DSPState *dsp)
{
    dsp->core.pram_gen++;
}

extern "C" void dsp_rec_destroy(DSPState *dsp)
{
    Rec *r = (Rec *)dsp->rec;
    if (!r) return;
    if (r->buf) free_exec(r->buf, r->size);
    delete r;
    dsp->rec = nullptr;
}

/* ---------------------------------------------------------------------- */
/* the fuzzer: random programs, interpreter and compiled, compared        */

namespace {

struct Rng {
    uint64_t s;
    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return (uint32_t)(s >> 11);
    }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

uint32_t g_fz_hash;

uint32_t fz_read_periph(dsp_core_t *core, uint32_t address)
{
    (void)core;
    return (address * 2654435761u) & 0xffffff;
}

void fz_write_periph(dsp_core_t *core, uint32_t address, uint32_t value)
{
    g_fz_hash = (g_fz_hash ^ address) * 16777619u;
    g_fz_hash = (g_fz_hash ^ value) * 16777619u;
    if (address == 0xffffc4 && (value & 1)) {
        core->is_idle = true;
    }
}

void fz_scratch(void *, uint8_t *ptr, uint32_t, size_t len, bool dir)
{
    if (!dir) memset(ptr, 0, len);
}

void fz_fifo(void *, uint8_t *ptr, unsigned int, size_t len, bool dir)
{
    if (!dir) memset(ptr, 0, len);
}

/* an instruction word for the fuzzer */
uint32_t fz_word(Rng &g, uint32_t pc, uint32_t len, uint32_t *w2, int *nw)
{
    *nw = 1;
    *w2 = g.below(4) ? g.below(0x2000) : (g.next() & 0xffffff);
    /* mostly forward: fewer endless loops */
    const uint32_t tgt = g.below(8) ? pc + 1 + g.below(len + 8 - pc) : g.below(len + 8);
    switch (g.below(32)) {
    default: { /* a parallel word */
        uint32_t w = 0x100000 | (g.next() & 0xefffff);
        if ((g.next() & 3) == 0) w = (w & 0xffff00) | (0x80 + g.below(0x80));
        *nw = pm_len(w);
        /* now and then a peripheral's address (reads compiled, writes the
         * interpreter's) */
        if (*nw == 2 && g.below(4) == 0) *w2 = 0xffffc0 + g.below(64);
        else if (*nw == 2 && is_periph_x(*w2)) *w2 &= 0xffff;
        return w;
    }
    case 0: return 0x000000; /* nop */
    case 1: return 0x0c0000 | (tgt & 0xfff); /* jmp */
    case 2: return 0x0e0000 | (g.below(16) << 12) | (tgt & 0xfff); /* jcc */
    case 3: { /* bcc / bra short */
        const int32_t off = (int32_t)tgt - (int32_t)pc;
        if (off < -256 || off > 255) return 0;
        const uint32_t x = (uint32_t)off & 0x1ff;
        const uint32_t enc = (x & 0x1f) | ((x & 0x1e0) << 1);
        return g.below(2) ? (0x050c00 | enc) : (0x050400 | (g.below(16) << 12) | enc);
    }
    case 4: { /* jclr/jset/brclr/brset on a register */
        static const int regs[] = {4, 5, 6, 7, 14, 15, 16, 17, 18, 19, 24, 25};
        const int reg = regs[g.below(12)];
        *nw = 2;
        const uint32_t b = g.below(24);
        switch (g.below(4)) {
        case 0: *w2 = tgt; return 0x0ac000 | (reg << 8) | 0x00 | b;
        case 1: *w2 = tgt; return 0x0ac000 | (reg << 8) | 0x20 | b;
        case 2: *w2 = (tgt - pc) & 0xffffff; return 0x0cc000 | (reg << 8) | 0x80 | b;
        default: *w2 = (tgt - pc) & 0xffffff; return 0x0cc000 | (reg << 8) | 0xa0 | b;
        }
    }
    case 5: { /* move X/Y:(Rn + xxx) */
        const uint32_t xxx = g.below(128);
        return 0x020080 | ((xxx >> 1) << 11) | ((xxx & 1) << 6) | (g.below(8) << 8) |
               (g.below(2) << 5) | (g.below(2) << 4) | g.below(16);
    }
    case 6: { /* rep #n then a parallel word */
        return 0x0600a0 | (g.below(6) << 8);
    }
    case 7: { /* do / dor #n, expr (n 0: not run) */
        *nw = 2;
        const uint32_t n = g.below(5);
        if (g.below(2)) {
            *w2 = 2 + g.below(6);
            return 0x060090 | (n << 8);
        }
        *w2 = (pc + 2 + g.below(6)) & 0xffff;
        return 0x060080 | (n << 8);
    }
    case 8: { /* jsr / rts */
        if (g.below(2)) return 0x00000c;
        return 0x0d0000 | (tgt & 0xfff);
    }
    case 9: { /* movec to a register (the interpreter's) */
        return 0x0500a0 | (g.below(256) << 8) | g.below(32);
    }
    case 15: { /* movep: ea / register <-> X:pp or Y:pp, ea <-> X:qq */
        const uint32_t W = g.below(2), pp = g.below(64);
        switch (g.below(3)) {
        case 0: {
            const uint32_t mm = g.below(2) ? (4 << 3) | g.below(8) : (g.below(4) << 3) | g.below(8);
            return 0x080080 | (g.below(2) << 16) | (W << 15) | 0x4000 | (mm << 8) | (g.below(2) << 6) | pp;
        }
        case 1: {
            const uint32_t mm = (g.below(4) << 3) | g.below(8);
            return 0x070000 | (W << 15) | 0x4000 | (mm << 8) | (g.below(2) << 6) | pp;
        }
        default: {
            static const int regs[] = {4, 5, 6, 7, 14, 15, 16, 17, 24, 25};
            return 0x084000 | (g.below(2) << 16) | (W << 15) | (regs[g.below(10)] << 8) | pp;
        }
        }
    }
    case 16: /* IFcc / IFcc.U with an ALU op (the interpreter's) */
        return 0x202000 | (g.below(2) << 12) | (g.below(16) << 8) | g.below(256);
    case 17: { /* the instructions the second audit added (the interpreter's) */
        const uint32_t d = g.below(2), r3 = 2 + g.below(6), q3 = 2 + g.below(6);
        switch (g.below(16)) {
        case 0: return 0x014082 | (g.below(64) << 8) | (d << 3) | g.below(2);  /* or/eor #xx */
        case 1: *nw = 2; *w2 = g.next() & 0xffffff; return 0x0140cb & ~8u | (d << 3); /* eor #xxxx */
        case 2: return 0x0c1e80 | (g.below(2) << 6) | (g.below(32) << 1) | d;   /* lsl/lsr #ii */
        case 3: return 0x0c1e10 | (g.below(2) << 5) | (r3 << 1) | d;            /* lsl/lsr S */
        case 4: return 0x0c1e40 | (g.below(2) << 5) | (g.below(2) << 4) | (r3 << 1) | d; /* asl/asr S1,S2 */
        case 5: return 0x0c1e00 | (g.below(2) << 1) | d;                        /* clb */
        case 6: return 0x0c1e20 | (r3 << 1) | d;                                /* normf */
        case 7: return 0x0c1b80 | (r3 << 1) | d;                                /* merge */
        case 8: return 0x0c1b00 | (q3 << 4) | (r3 << 1) | d;                    /* insert S1,S2 */
        case 9: return 0x0c1a00 | (g.below(2) << 7) | (g.below(2) << 4) | (r3 << 1) | d; /* extract(u) S1 */
        case 10: return 0x0100c0 | (g.below(32) << 8) | g.below(64);           /* mpy/mac(r) S,#n */
        case 11: *nw = 2; *w2 = g.next() & 0xffffff;                            /* maci/macri/mpyri */
            return 0x0141c0 | (g.below(16) << 2) | (1 + g.below(3));
        case 12: return 0x012680 | (g.below(2) << 8) | g.below(128);            /* mac/mpy su,uu */
        case 13: { const uint32_t ss = g.below(3); /* dmac ss su uu */
            return 0x012480 | ((ss ? 1u : 0u) << 8) | ((ss == 2 ? 1u : 0u) << 6) | g.below(64); }
        case 14: return 0x020000 | (g.below(16) << 12) | (g.below(2) ? 0x800 | (g.below(8) << 8) | g.below(8) /* tcc */
                                                                   : ((8 + g.below(8)) << 3));
        default: *nw = 2; *w2 = g.below(0x40);                                  /* move Y:(Rn+xxxx) */
            return 0x0b7080 | (g.below(8) << 8) | (g.below(2) << 6) | (4 + g.below(12));
        }
    }
    case 14: /* cmpu S1, S2 */
        return 0x0c1ff0 | (g.below(8) << 1) | g.below(2);
    case 13: { /* lua (Rn + aa), lua ea, asl/asr #n */
        switch (g.below(3)) {
        case 0: return 0x040000 | (g.below(8) << 11) | (g.below(8) << 8) | (g.below(16) << 4) | g.below(16);
        case 1: return 0x044000 | (g.below(4) << 11) | (g.below(8) << 8) | g.below(32);
        default: return (g.below(2) ? 0x0c1d00 : 0x0c1c00) | (g.below(2) << 7) | (g.below(64) << 1) | g.below(2);
        }
    }
    case 12: { /* andi / ori on MR (the scaling bits), CCR, OMR */
        const uint32_t ee = g.below(3);
        uint32_t v = g.next() & 0xff;
        if (ee == 0 && g.below(4)) v = g.below(2) ? (v & 0x0c) : (v | 0xa3);
        return (g.below(2) ? 0x0000f8 : 0x0000b8) | (v << 8) | ee;
    }
    case 10: case 11: { /* add sub cmp and or with an immediate */
        static const uint32_t ops[] = {0, 4, 5, 6, 2};
        const uint32_t op = ops[g.below(5)], d = g.below(2);
        if (g.below(2)) {
            *nw = 2;
            *w2 = g.next() & 0xffffff;
            return 0x0140c0 | (d << 3) | op;
        }
        return 0x014080 | (g.below(64) << 8) | (d << 3) | op;
    }
    }
}

bool fz_equal(const dsp_core_t *a, const dsp_core_t *b, char *why, size_t n)
{
    if (a->pc != b->pc) { snprintf(why, n, "pc %x %x", a->pc, b->pc); return false; }
    for (int i = 0; i < DSP_REG_MAX; i++) {
        if (a->registers[i] != b->registers[i]) {
            snprintf(why, n, "reg %02x: %06x %06x", i, a->registers[i], b->registers[i]);
            return false;
        }
    }
    if (memcmp(a->stack, b->stack, sizeof(a->stack))) { snprintf(why, n, "stack"); return false; }
    if (a->sr_ext != b->sr_ext) { snprintf(why, n, "sr_ext %06x %06x", a->sr_ext, b->sr_ext); return false; }
    for (int i = 0; i < DSP_XRAM_SIZE; i++)
        if (a->xram[i] != b->xram[i]) { snprintf(why, n, "x:%04x %06x %06x", i, a->xram[i], b->xram[i]); return false; }
    for (int i = 0; i < DSP_YRAM_SIZE; i++)
        if (a->yram[i] != b->yram[i]) { snprintf(why, n, "y:%04x %06x %06x", i, a->yram[i], b->yram[i]); return false; }
    for (int i = 0; i < DSP_MIXBUFFER_SIZE; i++)
        if (a->mixbuffer[i] != b->mixbuffer[i]) { snprintf(why, n, "mix:%04x", i); return false; }
    if (a->loop_rep != b->loop_rep || a->pc_on_rep != b->pc_on_rep) { snprintf(why, n, "rep"); return false; }
    if (a->is_idle != b->is_idle) { snprintf(why, n, "idle"); return false; }
    if (a->interrupt_state != b->interrupt_state || a->interrupt_counter != b->interrupt_counter ||
        a->interrupt_pipeline_count != b->interrupt_pipeline_count ||
        memcmp(a->interrupt_is_pending, b->interrupt_is_pending, sizeof(a->interrupt_is_pending))) {
        snprintf(why, n, "interrupts (state %d %d, pending %d %d)", a->interrupt_state, b->interrupt_state,
                 a->interrupt_counter, b->interrupt_counter);
        return false;
    }
    return true;
}

/* the program again a word at a time on both: the first one they differ on */
void step_compare(DSPState *ref, DSPState *jit, const dsp_core_t &init)
{
    Rec *r = rec_of(jit);
    ref->core = init;
    jit->core = init;
    jit->core.pram_gen = init.pram_gen + 7;
    ref->save_cycles = jit->save_cycles = 0;
    r->max_block = 1;
    g_step_mode = true;
    r->reset();
    for (int step = 0; step < 200000 && !ref->core.is_idle; step++) {
        const dsp_core_t before = ref->core;
        ref->save_cycles = 0;
        dsp_run_interp(ref, 1);
        for (int i = 4; i < 0x10; i++) {
            const uint32_t lim = (i == DSP_REG_A2 || i == DSP_REG_B2) ? 0xff : 0xffffff;
            if (ref->core.registers[i] > lim && before.registers[i] <= lim) {
                nc_log("xbased-apu", "dsp rec selftest:  step %d at p:%04x %06x: the interpreter leaves reg %02x = %x",
                       step, before.pc, before.pram[before.pc & 0xfff], i, ref->core.registers[i]);
            }
        }
        jit->save_cycles = 0;
        dsp_rec_run(jit, 1);
        char why[96];
        if (!fz_equal(&ref->core, &jit->core, why, sizeof(why))) {
            nc_log("xbased-apu", "dsp rec selftest:  step %d at p:%04x %06x %06x (sr %04x a %02x:%06x:%06x b %02x:%06x:%06x"
                   " ssh %04x la %04x lc %04x sp %02x): %s",
                   step, before.pc, before.pram[before.pc & 0xfff], before.pram[(before.pc + 1) & 0xfff],
                   before.registers[DSP_REG_SR], before.registers[DSP_REG_A2], before.registers[DSP_REG_A1],
                   before.registers[DSP_REG_A0], before.registers[DSP_REG_B2], before.registers[DSP_REG_B1],
                   before.registers[DSP_REG_B0], before.registers[DSP_REG_SSH], before.registers[DSP_REG_LA],
                   before.registers[DSP_REG_LC], before.registers[DSP_REG_SP], why);
            break;
        }
    }
    r->max_block = 64;
    g_step_mode = false;
    r->reset();

    /* compiled blocks a run at a time (blocks of up to mb words, one a
     * run), the interpreter the same number of words: the first run they
     * differ after */
    for (int mb = 2; mb <= 64; mb *= 2) {
    r->max_block = mb;
    g_exact_budget = true;
    r->reset();
    ref->core = init;
    jit->core = init;
    jit->core.pram_gen = init.pram_gen + 9 + mb;
    bool found = false;
    for (int run = 0; run < 20000 && !ref->core.is_idle && !jit->core.is_idle; run++) {
        const dsp_core_t before = jit->core;
        const uint32_t c0 = jit->core.cycle_count;
        jit->save_cycles = 0;
        dsp_rec_run(jit, 2 * (mb - 1));
        const uint32_t n = jit->core.cycle_count - c0;
        for (uint32_t i = 0; i < n && !ref->core.is_idle; i++) {
            dsp56k_execute_instruction(&ref->core);
            ref->core.cycle_count++;
        }
        char why[96];
        if (!fz_equal(&ref->core, &jit->core, why, sizeof(why))) {
            nc_log("xbased-apu", "dsp rec selftest:  blocks of %d: run %d from p:%04x (%u words, sr %04x la %04x lc %04x"
                   " sp %02x ssh %04x): %s",
                   mb, run, before.pc, n, before.registers[DSP_REG_SR], before.registers[DSP_REG_LA],
                   before.registers[DSP_REG_LC], before.registers[DSP_REG_SP], before.registers[DSP_REG_SSH], why);
            found = true;
            break;
        }
    }
    if (found) break;
    }
    g_exact_budget = false;
    r->max_block = 64;
    r->reset();
}

} // namespace

/* the manual's examples (appendix A, its bit pictures as numbers) and what
 * its text says of the instructions fixed or added against it: one
 * instruction on each core, both must give the manual's result.
 * in: register (or 0x1000 | an X address) and value, 0 ends;
 * out: register (0x2000: pc), value, mask */
struct MTest {
    const char *what;
    uint32_t w, w2;
    uint32_t in[6][2];
    uint32_t out[7][3];
};

enum : uint32_t {
    mX0 = DSP_REG_X0, mX1 = DSP_REG_X1, mY0 = DSP_REG_Y0, mY1 = DSP_REG_Y1,
    mA0 = DSP_REG_A0, mB0 = DSP_REG_B0, mA2 = DSP_REG_A2, mB2 = DSP_REG_B2,
    mA1 = DSP_REG_A1, mB1 = DSP_REG_B1, mR0 = DSP_REG_R0, mR1 = DSP_REG_R0 + 1,
    mR2 = DSP_REG_R0 + 2, mSR = DSP_REG_SR, mPC = 0x2000, mX = 0x1000, mEXT = 0x3000, SM = 0x100000,
    fC = 1, fV = 2, fZ = 4, fN = 8, fU = 0x10, fE = 0x20, fL = 0x40,
    M24 = 0xffffff, M8 = 0xff,
};

const MTest k_mtests[] = {
    /* A-6.23 CLB B,A: 5 leading ones, 9 - 5 = 4 */
    {"clb b,a (example)", 0x0c1e02, 0, {{mB2, 0xf8}, {mB1, 0xc54a91}},
     {{mA2, 0, M8}, {mA1, 4, M24}, {mA0, 0, M24}, {mSR, 0, fN | fZ | fV}}},
    /* A-6.88 example: CLB A,B with A $20:000000:000000 gives 7 */
    {"clb a,b (normf example)", 0x0c1e01, 0, {{mA2, 0x20}},
     {{mB2, 0, M8}, {mB1, 7, M24}, {mB0, 0, M24}}},
    /* A-6.88 NORMF B1,A: 7 shifts right, A $00:400000:000000 */
    {"normf b1,a (example)", 0x0c1e26, 0, {{mA2, 0x20}, {mB1, 7}},
     {{mA2, 0, M8}, {mA1, 0x400000, M24}, {mA0, 0, M24}, {mSR, 0, fV}}},
    /* NORMF with a negative count: ASL */
    {"normf x0,a (-2: asl 2)", 0x0c1e28, 0, {{mA1, 0x100000}, {mX0, 0xfffffe}},
     {{mA2, 0, M8}, {mA1, 0x400000, M24}, {mSR, 0, fV}}},
    /* A-6.55 LSL #7,A: C the last bit out (0) */
    {"lsl #7,a (example)", 0x0c1e8e, 0, {{mA1, 0x98ca91}, {mSR, fC}},
     {{mA1, 0x654880, M24}, {mSR, 0, fC | fV}}},
    /* LSL #0: C cleared, N Z of D */
    {"lsl #0,a", 0x0c1e80, 0, {{mSR, fC}},
     {{mA1, 0, M24}, {mSR, fZ, fC | fZ | fN | fV}}},
    /* A-6.56 LSR X0,B: shift 3, C 1 */
    {"lsr x0,b (example)", 0x0c1e39, 0, {{mB1, 0xf07c1f}, {mX0, 3}},
     {{mB1, 0x1e0f83, M24}, {mSR, fC, fC | fN | fV}}},
    {"lsr #4,a", 0x0c1ec8, 0, {{mA1, 0x000018}},
     {{mA1, 1, M24}, {mSR, fC, fC | fN | fZ}}},
    /* A-6.65 MERGE X0,B */
    {"merge x0,b (example)", 0x0c1b89, 0, {{mX0, 0x000aa2}, {mB1, 0x000883}},
     {{mB1, 0xaa2883, M24}, {mSR, fN, fN | fZ | fV}}},
    /* A-6.39 EXTRACT B1,A,A: offset 11 width 5, 10101 sign-extended */
    {"extract b1,a,a (example)", 0x0c1a06, 0, {{mB1, 0x00500b}, {mA0, 0x00a800}},
     {{mA2, 0xff, M8}, {mA1, 0xffffff, M24}, {mA0, 0xfffff5, M24}, {mSR, 0, fV | fC}}},
    /* A-6.40 EXTRACTU B1,A,A: offset 11 width 7, 1110101 */
    {"extractu b1,a,a (example)", 0x0c1a86, 0, {{mB1, 0x00700b}, {mA0, 0x03a800}},
     {{mA2, 0, M8}, {mA1, 0, M24}, {mA0, 0x75, M24}}},
    /* A-6.45 INSERT B1,X0,A: offset 10 width 5, X0's 10010 */
    {"insert b1,x0,a (example)", 0x0c1b46, 0, {{mB1, 0x00500a}, {mX0, 0x000012}},
     {{mA2, 0, M8}, {mA1, 0, M24}, {mA0, 0x004800, M24}}},
    /* A-6.101 ROL through C */
    {"rol a (c in)", 0x200037, 0, {{mA1, 0x800000}, {mSR, fC}},
     {{mA1, 0x000001, M24}, {mSR, fC, fC | fN | fZ | fV}}},
    /* A-6.102 ROR through C */
    {"ror a (c out)", 0x200027, 0, {{mA1, 0x000001}},
     {{mA1, 0, M24}, {mSR, fC | fZ, fC | fN | fZ | fV}}},
    {"ror a (c in)", 0x200027, 0, {{mA1, 0x000002}, {mSR, fC}},
     {{mA1, 0x800001, M24}, {mSR, fN, fC | fN | fZ | fV}}},
    /* A-6.64 MAXM: |B| - |A| <= 0 moves A to B, C cleared */
    {"maxm a,b (moved)", 0x200015, 0, {{mA2, 0xff}, {mA1, 0xffffff}, {mB0, 0x800000}},
     {{mB2, 0xff, M8}, {mB1, 0xffffff, M24}, {mB0, 0, M24}, {mSR, 0, fC}}},
    {"maxm a,b (kept)", 0x200015, 0, {{mA1, 0x000001}, {mB1, 0x000002}},
     {{mB1, 2, M24}, {mSR, fC, fC}}},
    /* A-6.100 RND: V (and L) by the standard definition */
    {"rnd a (overflow)", 0x200011, 0, {{mA2, 0x7f}, {mA1, 0xffffff}, {mA0, 0x800000}},
     {{mA2, 0x80, M8}, {mA1, 0, M24}, {mA0, 0, M24}, {mSR, fV | fL | fN, fV | fL | fN}}},
    {"rnd a (no overflow clears V)", 0x200011, 0, {{mA1, 0x123456}, {mA0, 0x400000}, {mSR, fV}},
     {{mA1, 0x123456, M24}, {mA0, 0, M24}, {mSR, 0, fV}}},
    /* A-6.61 MACR: V of D + P + r */
    {"macr x0,x0,a (round overflows)", 0x200083, 0, {{mA2, 0x7f}, {mA1, 0xffffff}, {mA0, 0x800000}},
     {{mA2, 0x80, M8}, {mA1, 0, M24}, {mSR, fV | fL, fV | fL}}},
    /* A-6.41 IFcc: the op, the CCR kept; A-6.42 IFcc.U: the CCR updated */
    {"ifeq add x0,a (true)", 0x202a40, 0, {{mX0, 0x100000}, {mSR, fZ}},
     {{mA1, 0x100000, M24}, {mA2, 0, M8}, {mSR, fZ, 0xff}}},
    {"ifeq.u add x0,a (true)", 0x203a40, 0, {{mX0, 0x100000}, {mSR, fZ}},
     {{mA1, 0x100000, M24}, {mSR, fU, 0xff}}},
    {"ifne add x0,a (false)", 0x202240, 0, {{mX0, 0x100000}, {mSR, fZ}},
     {{mA1, 0, M24}, {mSR, fZ, 0xff}}},
    /* A-6.110 Tcc S2,D2 alone: R1 to R2, A untouched */
    {"tcc r1,r2", 0x020902, 0, {{mR1, 0x123}, {mA1, 5}, {DSP_REG_B1, 0x77}},
     {{mR2, 0x123, 0xffff}, {mA1, 5, M24}}},
    /* A-6.80 MPY S,#n: Y1 * 2^-3 */
    {"mpy y1,#3,a", 0x0103c0, 0, {{mY1, 0x400000}},
     {{mA2, 0, M8}, {mA1, 0x080000, M24}, {mA0, 0, M24}}},
    /* A-6.58 MAC S,#n */
    {"mac x0,#1,b", 0x0101da, 0, {{mX0, 0x200000}, {mB1, 0x100000}},
     {{mB2, 0, M8}, {mB1, 0x200000, M24}, {mB0, 0, M24}}},
    /* A-6.32 DMACss: D >> 24 plus the product */
    {"dmacss x0,x0,a", 0x012480, 0, {{mA1, 0x000001}},
     {{mA2, 0, M8}, {mA1, 0, M24}, {mA0, 1, M24}}},
    /* A-6.47 JCLR #20 (bit numbers past 15 decoded) */
    {"jclr #20,x0 (clear: jumps)", 0x0ac414, 0x40, {{mX0, 0x0fffff}}, {{mPC, 0x40, 0xffff}}},
    {"jclr #20,x0 (set: goes on)", 0x0ac414, 0x40, {{mX0, 0x100000}}, {{mPC, 2, 0xffff}}},
    /* A-6.71 MOVE X:(R0+xxxx),A (it decoded as a JCLR) */
    {"move x:(r0+5),a", 0x0a70ce, 5, {{mR0, 0x10}, {mX | 0x15, 0x123456}},
     {{mA2, 0, M8}, {mA1, 0x123456, M24}, {mA0, 0, M24}, {mPC, 2, 0xffff}}},
    /* A-6.87 NORM: from SR's E U Z (here ASL, R0 - 1), C unchanged */
    {"norm r0,a (c kept)", 0x01d815, 0, {{mA1, 0x200000}, {mR0, 0x1000}, {mSR, fC | fU}},
     {{mA1, 0x400000, M24}, {mR0, 0x0fff, 0xffff}, {mSR, fC, fC | fV}}},
    /* A-6.12 BCLR #3,SR: N cleared, C not the bit tested */
    {"bclr #3,sr (c kept)", 0x0af943, 0, {{mSR, fC | fN}}, {{mSR, fC, fC | fN}}},
    /* MOVEC X0,SR: S (bit 7) is a flag of the DSP56300's SR, kept */
    {"movec x0,sr (s kept)", 0x04c4b9, 0, {{mX0, 0x000081}}, {{mSR, 0x81, 0xff}}},
    /* the family manual 3.3.3, Table 3-1: SM saturates the result to 48 bits
     * by bits 55 48 47, V and L set */
    {"sm: add x0,a saturates", 0x200040, 0, {{mEXT, SM}, {mA1, 0x600000}, {mX0, 0x400000}},
     {{mA2, 0, M8}, {mA1, 0x7fffff, M24}, {mA0, 0xffffff, M24}, {mSR, fV | fL, fV | fL | fE | fN}}},
    {"sm: add x0,a negative", 0x200040, 0, {{mEXT, SM}, {mA2, 0xff}, {mA1, 0xa00000}, {mX0, 0xc00000}},
     {{mA2, 0xff, M8}, {mA1, 0x800000, M24}, {mA0, 0, M24}, {mSR, fV | fL | fN, fV | fL | fN | fE}}},
    {"sm off: add x0,a doesn't", 0x200040, 0, {{mA1, 0x600000}, {mX0, 0x400000}},
     {{mA2, 0, M8}, {mA1, 0xa00000, M24}, {mSR, fE, fV | fE}}},
    {"sm: bits 55 48 47 alike stay", 0x200040, 0, {{mEXT, SM}, {mA2, 0x7e}}, {{mA2, 0x7e, M8}, {mSR, 0, fV}}},
    {"sm: macr rounds the constant", 0x200083, 0, {{mEXT, SM}, {mA1, 0x700000}, {mX0, 0x400000}},
     {{mA2, 0, M8}, {mA1, 0x7fffff, M24}, {mA0, 0, M24}, {mSR, fV | fL, fV | fL}}},
    {"sm: tfr a,b doesn't", 0x200009, 0, {{mEXT, SM}, {mA1, 0xa00000}}, {{mB2, 0, M8}, {mB1, 0xa00000, M24}}},
    {"sm: cmp x0,a sets V", 0x200045, 0, {{mEXT, SM}, {mA1, 0x700000}, {mX0, 0x900000}},
     {{mA1, 0x700000, M24}, {mSR, fV | fL, fV | fL | fN}}},
    {"movec #$10,sr: sm off", 0x0510b9, 0, {{mEXT, SM}}, {{mEXT, 0, 0xffffff}}},
    /* A-6.90 OR #xx, A-6.38 EOR #xxxxxx */
    {"or #$21,a", 0x016182, 0, {{mA1, 0x800000}},
     {{mA1, 0x800021, M24}, {mSR, fN, fN | fZ | fV}}},
    {"eor #$ffffff,b", 0x0140cb, 0xffffff, {{mB1, 0x0f0f0f}},
     {{mB1, 0xf0f0f0, M24}, {mSR, fN, fN | fZ | fV}}},
};

/* -1: no JIT; else the tests that fail (logged) */
int manual_tests(DSPState *ref, DSPState *jit)
{
    Rec *r = rec_of(jit);
    int bad = 0;
    for (const MTest &t : k_mtests) {
        dsp56k_reset_cpu(&ref->core);
        dsp_core_t init = ref->core;
        init.read_peripheral = fz_read_periph;
        init.write_peripheral = fz_write_periph;
        init.is_gp = true;
        for (uint32_t i = 0; i < DSP_PRAM_SIZE; i++) init.pram[i] = 0x0a8420; /* bset #0,x:$ffffc4 */
        init.pram[0] = t.w;
        init.pram[1] = t.w2;
        memset(init.pram_opcache, 0, sizeof(init.pram_opcache));
        memset(init.xram, 0, sizeof(init.xram));
        memset(init.yram, 0, sizeof(init.yram));
        for (const auto &p : t.in) {
            if (!p[0]) break;
            if (p[0] == mEXT) init.sr_ext = p[1];
            else if (p[0] & mX) init.xram[p[0] & 0xfff] = p[1];
            else init.registers[p[0]] = p[1];
        }
        init.is_idle = false;
        init.pram_gen = jit->core.pram_gen + 1;
        for (int side = 0; side < 2; side++) {
            DSPState *s = side ? jit : ref;
            s->core = init;
            s->save_cycles = 0;
            s->dma.control = 0;
            if (side) {
                r->max_block = 1;
                g_step_mode = true;
                r->reset();
                dsp_rec_run(jit, 1);
                r->max_block = 64;
                g_step_mode = false;
                r->reset();
            } else {
                dsp_run_interp(ref, 1);
            }
            for (const auto &o : t.out) {
                if (!o[0]) break;
                const uint32_t got = o[0] == mPC ? s->core.pc :
                                     o[0] == mEXT ? s->core.sr_ext : s->core.registers[o[0]];
                if ((got & o[2]) != o[1]) {
                    nc_log("xbased-apu", "dsp manual test %s: %s gives reg %02x = %06x (mask %06x), the manual %06x",
                           t.what, side ? "the recompiler" : "the interpreter", o[0], got & o[2], o[2], o[1]);
                    bad++;
                }
            }
        }
    }
    nc_log("xbased-apu", "dsp manual tests: %d, %d checks failed",
           (int)(sizeof(k_mtests) / sizeof(k_mtests[0])), bad);
    return bad;
}

extern "C" int dsp_rec_selftest(int programs, uint32_t seed)
{
    DSPState *ref = dsp_init(nullptr, fz_scratch, fz_fifo);
    DSPState *jit = dsp_init(nullptr, fz_scratch, fz_fifo);
    if (!rec_of(jit)) {
        dsp_destroy(ref);
        dsp_destroy(jit);
        return -1;
    }
    manual_tests(ref, jit);
    Rng g{0x9e3779b97f4a7c15ull ^ seed};
    int bad = 0, ran = 0, capped = 0;
    for (int prog = 0; prog < programs; prog++) {
        dsp_core_t init;
        dsp56k_reset_cpu(&ref->core);
        init = ref->core;
        init.read_peripheral = fz_read_periph;
        init.write_peripheral = fz_write_periph;
        init.is_gp = true;
        const uint32_t len = 8 + g.below(48);
        for (uint32_t i = 0; i < DSP_PRAM_SIZE; i++) init.pram[i] = 0x0a8420; /* bset #0,x:$ffffc4 */
        for (uint32_t pc = 0; pc < len;) {
            uint32_t w2;
            int nw;
            const uint32_t w = fz_word(g, pc, len, &w2, &nw);
            init.pram[pc] = w;
            if (nw == 2 && pc + 1 < len) init.pram[pc + 1] = w2;
            pc += nw;
        }
        memset(init.pram_opcache, 0, sizeof(init.pram_opcache));
        for (int i = 4; i < 0x10; i++) init.registers[i] = g.next() & 0xffffff;
        init.registers[DSP_REG_A2] &= 0xff;
        init.registers[DSP_REG_B2] &= 0xff;
        if (g.below(4) == 0) init.registers[DSP_REG_A2] = g.below(2) ? 0 : 0xff;
        if (g.below(4) == 0) init.registers[DSP_REG_B2] = g.below(2) ? 0 : 0xff;
        for (int i = 0; i < 8; i++) {
            init.registers[DSP_REG_R0 + i] = g.below(3) ? g.below(0x900) :
                                             (g.below(2) ? 0xbf0 + g.below(0x420) : (g.next() & 0xffff));
            init.registers[DSP_REG_N0 + i] = g.below(2) ? g.below(8) : (g.next() & 0xffff);
            const uint32_t m = g.below(8);
            init.registers[DSP_REG_M0 + i] = m < 5 ? 0xffff : (m == 5 ? g.below(64) : (m == 6 ? 0 : (g.next() & 0xffff)));
        }
        init.registers[DSP_REG_SR] = g.next() & 0x7f;
        init.sr_ext = g.below(2) ? 0x100000 : 0;
        if (g.below(2)) init.registers[DSP_REG_SR] |= g.below(4) << DSP_SR_S0;
        for (int i = 0; i < DSP_XRAM_SIZE; i++) init.xram[i] = g.next() & 0xffffff;
        for (int i = 0; i < DSP_YRAM_SIZE; i++) init.yram[i] = g.next() & 0xffffff;
        for (int i = 0; i < DSP_MIXBUFFER_SIZE; i++) init.mixbuffer[i] = g.next() & 0xffffff;
        init.is_idle = false;
        init.pram_gen = jit->core.pram_gen + 1;

        ref->core = init;
        jit->core = init;
        ref->dma.control = jit->dma.control = 0;
        ref->save_cycles = jit->save_cycles = 0;
        g_fz_hash = 0;
        dsp_run_interp(ref, 40000);
        const uint32_t h_ref = g_fz_hash;
        if (!ref->core.is_idle) {
            capped++;
            continue;
        }
        g_fz_hash = 0;
        dsp_rec_run(jit, 40000);
        const uint32_t h_jit = g_fz_hash;
        ran++;
        char why[96];
        bool same = fz_equal(&ref->core, &jit->core, why, sizeof(why));
        if (same && h_ref != h_jit) {
            snprintf(why, sizeof(why), "peripheral writes");
            same = false;
        }
        if (!same) {
            if (bad < 12) {
                step_compare(ref, jit, init);
                nc_log("xbased-apu", "dsp rec selftest: program %d differs: %s", prog, why);
                char line[400];
                size_t o = 0;
                for (uint32_t pc = 0; pc < len && o < sizeof(line) - 8; pc++) {
                    o += snprintf(line + o, sizeof(line) - o, " %06x", init.pram[pc]);
                }
                nc_log("xbased-apu", "dsp rec selftest:  P:%s", line);
                nc_log("xbased-apu", "dsp rec selftest:  sr %04x m0-7 %04x %04x %04x %04x %04x %04x %04x %04x",
                       init.registers[DSP_REG_SR], init.registers[DSP_REG_M0], init.registers[DSP_REG_M1],
                       init.registers[DSP_REG_M2], init.registers[DSP_REG_M3], init.registers[DSP_REG_M4],
                       init.registers[DSP_REG_M5], init.registers[DSP_REG_M6], init.registers[DSP_REG_M7]);
            }
            bad++;
        }
    }
    nc_log("xbased-apu", "dsp rec selftest: %d programs compared, %d differ (%d past the cap)", ran, bad, capped);
    dsp_rec_destroy(jit);
    dsp_destroy(ref);
    dsp_destroy(jit);
    return bad;
}
