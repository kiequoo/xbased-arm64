/*
 * xbased native core — a faulting x86-64 instruction run against a buffer
 * (x64emu.h).
 *
 * The instruction is encoded again with its memory operand RIP-relative to
 * the buffer, in a per-thread page beside the registers it runs with, and
 * run there: whatever it computes (an ALU operation's flags, a sign
 * extension, an SSE move) is the CPU's own.
 */
#include "native/cpu/x64emu.h"
#include "native/cpu/x64asm.h"
#include "native/core/nc.h"

#ifdef _WIN32
#include <windows.h>
#else
#include "nc_winapi.h"
#endif

#include <cstddef>
#include <cstring>

using namespace x64;

namespace {

struct alignas(4096) Page {
    uint8_t code[1024];
    alignas(16) uint8_t data[16];
    uint64_t gpr[16];
    uint64_t rflags;
    uint64_t host_rsp;
    alignas(16) uint8_t guest_fx[512];
    alignas(16) uint8_t host_fx[512];
};

struct Decoded {
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    unsigned mem;       /* the memory operand's index */
    uint64_t rip;
    bool valid;
};

thread_local Page *t_page;
thread_local Decoded t_dec;

const ZydisRegister k_gpr[16] = {ZYDIS_REGISTER_RAX, ZYDIS_REGISTER_RCX, ZYDIS_REGISTER_RDX, ZYDIS_REGISTER_RBX,
                                 ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_RBP, ZYDIS_REGISTER_RSI, ZYDIS_REGISTER_RDI,
                                 ZYDIS_REGISTER_R8,  ZYDIS_REGISTER_R9,  ZYDIS_REGISTER_R10, ZYDIS_REGISTER_R11,
                                 ZYDIS_REGISTER_R12, ZYDIS_REGISTER_R13, ZYDIS_REGISTER_R14, ZYDIS_REGISTER_R15};

uint64_t reg_value(const CONTEXT *c, ZydisRegister r, bool *ok)
{
    if (r == ZYDIS_REGISTER_NONE) {
        return 0;
    }
    if (r == ZYDIS_REGISTER_RIP || r == ZYDIS_REGISTER_EIP) {
        *ok = false; /* not an emulated access (RIP-relative: host data) */
        return 0;
    }
    const ZydisRegister big = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
    const DWORD64 *v = &c->Rax;
    for (int i = 0; i < 16; i++) {
        if (k_gpr[i] == big) {
            const uint64_t x = v[i];
            return ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, r) == 32 ? (uint32_t)x : x;
        }
    }
    *ok = false;
    return 0;
}

bool names_rsp(const ZydisDecodedOperand &o)
{
    auto rsp = [](ZydisRegister r) {
        return r != ZYDIS_REGISTER_NONE &&
               ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r) == ZYDIS_REGISTER_RSP;
    };
    return (o.type == ZYDIS_OPERAND_TYPE_REGISTER && rsp(o.reg.value));
}

} // namespace

extern "C" bool nc_x64_access(const void *ctxp, uint64_t fault_addr, NcX64Access *a)
{
    const CONTEXT *c = static_cast<const CONTEXT *>(ctxp);
    Decoded &d = t_dec;
    d.valid = false;
    static thread_local ZydisDecoder dec;
    static thread_local bool init;
    if (!init) {
        ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        init = true;
    }
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, (const void *)c->Rip, ZYDIS_MAX_INSTRUCTION_LENGTH, &d.in, d.ops))) {
        return false;
    }
    int mem = -1;
    for (unsigned i = 0; i < d.in.operand_count; i++) {
        const ZydisDecodedOperand &o = d.ops[i];
        if (names_rsp(o)) {
            return false; /* pushes, pops, calls: not something to replay */
        }
        if (o.type == ZYDIS_OPERAND_TYPE_MEMORY && o.mem.type == ZYDIS_MEMOP_TYPE_MEM) {
            if (mem >= 0 || o.visibility != ZYDIS_OPERAND_VISIBILITY_EXPLICIT) {
                return false; /* string instructions and other two-operand forms */
            }
            mem = (int)i;
        }
    }
    if (mem < 0 || d.in.mnemonic == ZYDIS_MNEMONIC_LEA) {
        return false;
    }
    const ZydisDecodedOperand &o = d.ops[mem];
    bool ok = true;
    uint64_t ea = reg_value(c, o.mem.base, &ok) + reg_value(c, o.mem.index, &ok) * (o.mem.scale ? o.mem.scale : 1) +
                  (uint64_t)(o.mem.disp.has_displacement ? o.mem.disp.value : 0);
    if (!ok || (o.mem.segment != ZYDIS_REGISTER_DS && o.mem.segment != ZYDIS_REGISTER_SS &&
                o.mem.segment != ZYDIS_REGISTER_ES && o.mem.segment != ZYDIS_REGISTER_CS &&
                o.mem.segment != ZYDIS_REGISTER_NONE)) {
        return false;
    }
    if (d.in.address_width == 32) {
        ea = (uint32_t)ea;
    }
    const unsigned bytes = o.size / 8;
    if (!bytes || bytes > 16 || fault_addr < ea || fault_addr - ea >= bytes) {
        return false;
    }
    a->ea = ea;
    a->bytes = bytes;
    a->read = (o.actions & ZYDIS_OPERAND_ACTION_MASK_READ) != 0;
    a->write = (o.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0;
    a->len = d.in.length;
    d.mem = (unsigned)mem;
    d.rip = c->Rip;
    d.valid = true;
    return true;
}

extern "C" bool nc_x64_run(void *ctxp, uint8_t data[16])
{
    CONTEXT *c = static_cast<CONTEXT *>(ctxp);
    Decoded &d = t_dec;
    if (!d.valid || d.rip != c->Rip) {
        return false;
    }
    if (!t_page) {
        t_page = (Page *)VirtualAlloc(nullptr, sizeof(Page), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!t_page) {
            return false;
        }
    }
    Page *p = t_page;

    ZydisEncoderRequest req;
    if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(&d.in, d.ops, d.in.operand_count_visible, &req))) {
        return false;
    }
    req.address_size_hint = ZYDIS_ADDRESS_SIZE_HINT_NONE;
    req.prefixes &= ~(ZYDIS_ATTRIB_HAS_SEGMENT_CS | ZYDIS_ATTRIB_HAS_SEGMENT_SS | ZYDIS_ATTRIB_HAS_SEGMENT_DS |
                      ZYDIS_ATTRIB_HAS_SEGMENT_ES | ZYDIS_ATTRIB_HAS_SEGMENT_FS | ZYDIS_ATTRIB_HAS_SEGMENT_GS |
                      ZYDIS_ATTRIB_HAS_ADDRESSSIZE);
    ZydisEncoderOperand &m = req.operands[d.mem];
    m.mem.base = ZYDIS_REGISTER_RIP;
    m.mem.index = ZYDIS_REGISTER_NONE;
    m.mem.scale = 0;
    m.mem.displacement = (int64_t)(uintptr_t)p->data;

    static const ZydisRegister callee[8] = {ZYDIS_REGISTER_RBX, ZYDIS_REGISTER_RBP, ZYDIS_REGISTER_RDI,
                                            ZYDIS_REGISTER_RSI, ZYDIS_REGISTER_R12, ZYDIS_REGISTER_R13,
                                            ZYDIS_REGISTER_R14, ZYDIS_REGISTER_R15};
    Asm a(p->code, p->code + sizeof(p->code));
    for (ZydisRegister r : callee) {
        a.op(ZYDIS_MNEMONIC_PUSH, {R(r)});
    }
    a.op(ZYDIS_MNEMONIC_MOV, {A(&p->host_rsp, 8), R(ZYDIS_REGISTER_RSP)});
    a.op(ZYDIS_MNEMONIC_FXSAVE, {A(p->host_fx, 512)});
    a.op(ZYDIS_MNEMONIC_FXRSTOR, {A(p->guest_fx, 512)});
    a.op(ZYDIS_MNEMONIC_PUSH, {A(&p->rflags, 8)});
    a.op(ZYDIS_MNEMONIC_POPFQ, {});
    for (int i = 0; i < 16; i++) {
        if (i != 4) {
            a.op(ZYDIS_MNEMONIC_MOV, {R(k_gpr[i]), A(&p->gpr[i], 8)});
        }
    }
    if (!a.encode(req)) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        if (i != 4) {
            a.op(ZYDIS_MNEMONIC_MOV, {A(&p->gpr[i], 8), R(k_gpr[i])});
        }
    }
    a.op(ZYDIS_MNEMONIC_PUSHFQ, {});
    a.op(ZYDIS_MNEMONIC_POP, {A(&p->rflags, 8)});
    a.op(ZYDIS_MNEMONIC_FXSAVE, {A(p->guest_fx, 512)});
    a.op(ZYDIS_MNEMONIC_FXRSTOR, {A(p->host_fx, 512)});
    a.op(ZYDIS_MNEMONIC_MOV, {R(ZYDIS_REGISTER_RSP), A(&p->host_rsp, 8)});
    for (int i = 7; i >= 0; i--) {
        a.op(ZYDIS_MNEMONIC_POP, {R(callee[i])});
    }
    a.op(ZYDIS_MNEMONIC_RET, {});
    if (a.failed) {
        return false;
    }

    memcpy(p->data, data, 16);
    const DWORD64 *v = &c->Rax;
    for (int i = 0; i < 16; i++) {
        p->gpr[i] = v[i];
    }
    p->rflags = (c->EFlags & 0x00000CD5u) | 0x202; /* the arithmetic flags and DF */
    memcpy(p->guest_fx, &c->FltSave, 512);
    FlushInstructionCache(GetCurrentProcess(), p->code, sizeof(p->code));
    reinterpret_cast<void (*)()>(p->code)();
    DWORD64 *w = &c->Rax;
    for (int i = 0; i < 16; i++) {
        if (i != 4) {
            w[i] = p->gpr[i];
        }
    }
    c->EFlags = (c->EFlags & ~0x00000CD5u) | ((DWORD)p->rflags & 0x00000CD5u);
    memcpy(&c->FltSave, p->guest_fx, 512);
    c->MxCsr = c->FltSave.MxCsr;
    memcpy(data, p->data, 16);
    c->Rip += d.in.length;
    d.valid = false;
    return true;
}

extern "C" bool nc_x64_string(void *ctxp, const NcX64Mem *mem)
{
    CONTEXT *c = static_cast<CONTEXT *>(ctxp);
    const uint8_t *p = (const uint8_t *)c->Rip;
    bool rep = false, a32 = false, o16 = false, rexw = false;
    unsigned n = 0;
    for (;; n++) {
        const uint8_t b = p[n];
        if (b == 0xF3 || b == 0xF2) {
            rep = true;
        } else if (b == 0x67) {
            a32 = true;
        } else if (b == 0x66) {
            o16 = true;
        } else if (b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36) {
        } else if ((b & 0xF0) == 0x40) {
            rexw = (b & 8) != 0;
        } else {
            break;
        }
        if (n > 8) {
            return false;
        }
    }
    const uint8_t op = p[n];
    if (op != 0xA4 && op != 0xA5 && op != 0xAA && op != 0xAB && op != 0xAC && op != 0xAD) {
        return false;
    }
    const unsigned size = !(op & 1) ? 1 : rexw ? 8 : o16 ? 2 : 4;
    const bool down = (c->EFlags & 0x400) != 0;
    const uint64_t amask = a32 ? 0xFFFFFFFFull : ~0ull;
    uint64_t si = c->Rsi & amask, di = c->Rdi & amask, cx = c->Rcx & amask;
    uint64_t count = rep ? cx : 1;
    const int64_t step = down ? -(int64_t)size : (int64_t)size;
    const uint64_t vmask = size == 8 ? ~0ull : (1ull << (8 * size)) - 1;
    for (; count; count--) {
        switch (op) {
        case 0xA4:
        case 0xA5:
            mem->wr(mem->opaque, di, mem->rd(mem->opaque, si, size), size);
            si = (si + step) & amask;
            di = (di + step) & amask;
            break;
        case 0xAA:
        case 0xAB:
            mem->wr(mem->opaque, di, c->Rax & vmask, size);
            di = (di + step) & amask;
            break;
        default: {
            const uint64_t v = mem->rd(mem->opaque, si, size);
            if (size == 4) {
                c->Rax = (uint32_t)v;
            } else {
                c->Rax = (c->Rax & ~vmask) | (v & vmask);
            }
            si = (si + step) & amask;
            break;
        }
        }
        if (rep) {
            cx = (cx - 1) & amask;
        }
    }
    auto put = [&](DWORD64 &r, uint64_t v) { r = a32 ? (uint32_t)v : v; };
    if (op <= 0xA5 || op >= 0xAC) {
        put(c->Rsi, si);
    }
    if (op <= 0xAB) {
        put(c->Rdi, di);
    }
    if (rep) {
        put(c->Rcx, cx);
    }
    c->Rip += n + 1;
    return true;
}
