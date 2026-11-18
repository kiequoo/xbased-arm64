/*
 * xbased native core — a small x86-64 assembler on Zydis's encoder, for the
 * Windows CPU (cpu_x86.cpp) and the host instruction emulator (x64emu.cpp).
 *
 * Operands are Zydis encoder operands; a memory operand based on RIP takes
 * the absolute address it reaches as its displacement (the code's address
 * is known as it is written). Jumps that get patched later are written by
 * hand as rel32 forms.
 */
#ifndef NATIVE_CPU_X64ASM_H
#define NATIVE_CPU_X64ASM_H

#include <Zydis/Zydis.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace x64 {

inline ZydisEncoderOperand R(ZydisRegister r)
{
    ZydisEncoderOperand o;
    memset(&o, 0, sizeof(o));
    o.type = ZYDIS_OPERAND_TYPE_REGISTER;
    o.reg.value = r;
    return o;
}

inline ZydisEncoderOperand I(int64_t v)
{
    ZydisEncoderOperand o;
    memset(&o, 0, sizeof(o));
    o.type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
    o.imm.s = v;
    return o;
}

/* [base + index * scale + disp], size bytes */
inline ZydisEncoderOperand M(ZydisRegister base, int64_t disp, uint16_t size,
                             ZydisRegister index = ZYDIS_REGISTER_NONE, uint8_t scale = 0)
{
    ZydisEncoderOperand o;
    memset(&o, 0, sizeof(o));
    o.type = ZYDIS_OPERAND_TYPE_MEMORY;
    o.mem.base = base;
    o.mem.index = index;
    o.mem.scale = index == ZYDIS_REGISTER_NONE ? 0 : scale;
    o.mem.displacement = disp;
    o.mem.size = size;
    return o;
}

/* [rip-relative to the absolute address a] */
inline ZydisEncoderOperand A(const void *a, uint16_t size)
{
    return M(ZYDIS_REGISTER_RIP, (int64_t)(uintptr_t)a, size);
}

struct Asm {
    uint8_t *p = nullptr, *end = nullptr;
    bool failed = false;

    Asm() = default;
    Asm(uint8_t *at, uint8_t *limit) : p(at), end(limit) {}

    bool room(size_t n) const { return p && p + n <= end; }

    void byte(uint8_t b)
    {
        if (room(1)) {
            *p++ = b;
        } else {
            failed = true;
        }
    }
    void bytes(const uint8_t *b, size_t n)
    {
        if (room(n)) {
            memcpy(p, b, n);
            p += n;
        } else {
            failed = true;
        }
    }
    void bytes(std::initializer_list<uint8_t> b)
    {
        for (uint8_t v : b) {
            byte(v);
        }
    }
    void u32(uint32_t v)
    {
        if (room(4)) {
            memcpy(p, &v, 4);
            p += 4;
        } else {
            failed = true;
        }
    }
    void u64(uint64_t v)
    {
        if (room(8)) {
            memcpy(p, &v, 8);
            p += 8;
        } else {
            failed = true;
        }
    }

    /* A request as it is: 64-bit mode, the hints the caller set. */
    bool encode(ZydisEncoderRequest &req)
    {
        if (!room(ZYDIS_MAX_INSTRUCTION_LENGTH)) {
            failed = true;
            return false;
        }
        ZyanUSize len = ZYDIS_MAX_INSTRUCTION_LENGTH;
        if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstructionAbsolute(&req, p, &len, (ZyanU64)(uintptr_t)p))) {
            return false;
        }
        p += len;
        return true;
    }

    /* mnemonic ops...; addr32: 32-bit address size (the guest's addresses) */
    bool op(ZydisMnemonic m, std::initializer_list<ZydisEncoderOperand> ops, bool addr32 = false)
    {
        ZydisEncoderRequest req;
        memset(&req, 0, sizeof(req));
        req.machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
        req.mnemonic = m;
        req.address_size_hint = addr32 ? ZYDIS_ADDRESS_SIZE_HINT_32 : ZYDIS_ADDRESS_SIZE_HINT_NONE;
        for (const auto &o : ops) {
            req.operands[req.operand_count++] = o;
            /* LEA's memory operand is sized as the address (Zydis wants it) */
            ZydisEncoderOperand &e = req.operands[req.operand_count - 1];
            if (m == ZYDIS_MNEMONIC_LEA && e.type == ZYDIS_OPERAND_TYPE_MEMORY && !e.mem.size) {
                e.mem.size = addr32 ? 4 : 8;
            }
        }
        if (!encode(req)) {
            failed = true;
            fprintf(stderr, "xbased-native: x64asm: %s (%u operands) doesn't encode\n", ZydisMnemonicGetString(m),
                    (unsigned)req.operand_count);
            return false;
        }
        return true;
    }

    /* jmp rel32 to target (patchable: the rel32 is the last 4 bytes) */
    uint8_t *jmp(const void *target)
    {
        byte(0xE9);
        uint8_t *at = p;
        u32((uint32_t)((intptr_t)target - (intptr_t)(p + 4)));
        return at;
    }
    /* jcc rel32 (cc: the condition's low nibble) */
    uint8_t *jcc(unsigned cc, const void *target)
    {
        byte(0x0F);
        byte((uint8_t)(0x80 | (cc & 15)));
        uint8_t *at = p;
        u32((uint32_t)((intptr_t)target - (intptr_t)(p + 4)));
        return at;
    }
    uint8_t *call(const void *target)
    {
        byte(0xE8);
        uint8_t *at = p;
        u32((uint32_t)((intptr_t)target - (intptr_t)(p + 4)));
        return at;
    }
};

/* Points a rel32 written by Asm::jmp/jcc/call at target. */
inline void patch_rel32(uint8_t *at, const void *target)
{
    const uint32_t rel = (uint32_t)((intptr_t)target - (intptr_t)(at + 4));
    memcpy(at, &rel, 4);
}

inline bool fits_rel32(const void *from, const void *to)
{
    const intptr_t d = (intptr_t)to - (intptr_t)from;
    return d >= INT32_MIN + 64 && d <= INT32_MAX - 64;
}

} // namespace x64

#endif
