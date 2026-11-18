/*
 * xbased native core — one faulting x86-64 instruction run against a
 * buffer instead of its memory operand (Windows): the device registers'
 * accesses (core/mmio.c) and the dirty tracking's emulated stores
 * (hle/dirty.c), from translated code and host code alike.
 *
 * nc_x64_access decodes the instruction at the CONTEXT's RIP: the host
 * address and size of its memory operand, whether it reads and writes it.
 * nc_x64_run then runs it with that operand replaced by data (filled from
 * the device or RAM beforehand when it reads; to be stored afterwards when
 * it writes), the registers, flags and x87/SSE state taken from and put
 * back in the CONTEXT, and RIP moved past it.
 */
#ifndef NATIVE_CPU_X64EMU_H
#define NATIVE_CPU_X64EMU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NcX64Access {
    uint64_t ea;        /* the memory operand's host address */
    unsigned bytes;     /* its size (1 to 16) */
    bool read, write;
    unsigned len;       /* the instruction's */
} NcX64Access;

/* ctx: a CONTEXT *. fault_addr must lie in the operand. */
bool nc_x64_access(const void *ctx, uint64_t fault_addr, NcX64Access *a);
/* The instruction nc_x64_access last decoded on this thread. */
bool nc_x64_run(void *ctx, uint8_t data[16]);

/* A string instruction (MOVS, STOS, LODS; REP or not) at the CONTEXT's RIP
 * that touches fault_addr: run element by element through rd and wr (host
 * addresses), RSI/RDI/RCX and RIP updated as the CPU would. False if it
 * isn't one. */
typedef struct NcX64Mem {
    uint64_t (*rd)(void *opaque, uint64_t addr, unsigned bytes);
    void (*wr)(void *opaque, uint64_t addr, uint64_t v, unsigned bytes);
    void *opaque;
} NcX64Mem;
bool nc_x64_string(void *ctx, const NcX64Mem *mem);

#ifdef __cplusplus
}
#endif

#endif
