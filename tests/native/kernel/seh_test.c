/*
 * Host test of the kernel's exception dispatch for what the processor raises
 * in the title's code (native/kernel/seh.c, nc_seh_fault): the record and the
 * context a handler is given, the walk down the registration chain, and what
 * each disposition leads to. No guest code runs: the test stands in for the
 * handlers, returning to the dispatcher as they would.
 *
 *   clang -O1 -g -DNC_DESKTOP=1 -I<repo> tests/native/kernel/seh_test.c native/kernel/seh.c -o seh_test
 */
#include "native/kernel/kernel.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the guest CPU's frame, as far as seh.c reads and writes it */
struct NcFrame {
    uint32_t r[8], eip, eflags;
};
uint32_t nc_frame_gpr(const NcFrame *f, int reg) { return f->r[reg]; }
void nc_frame_set_gpr(NcFrame *f, int reg, uint32_t v) { f->r[reg] = v; }
uint32_t nc_frame_eip(const NcFrame *f) { return f->eip; }
void nc_frame_set_eip(NcFrame *f, uint32_t eip) { f->eip = eip; }
uint32_t nc_frame_eflags(NcFrame *f) { return f->eflags; }
void nc_frame_set_eflags(NcFrame *f, uint32_t e) { f->eflags = e; }
uint16_t nc_frame_cs(const NcFrame *f) { (void)f; return 8; }

static int g_stops;
void nc_kernel_request_stop(NcKernel *k, NcFrame *f, const char *fmt, ...)
{
    (void)k;
    (void)f;
    (void)fmt;
    g_stops++;
}
void nc_kernel_frame_chain(NcKernel *k, NcFrame *f, char *buf, size_t len)
{
    (void)k;
    (void)f;
    if (len) {
        buf[0] = 0;
    }
}
void nc_kernel_impl(NcKernel *k, const char *name, NcKFn fn) { (void)k; (void)name; (void)fn; }
void nc_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }

static int g_fail;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                       \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* seh.c's layouts (EXCEPTION_RECORD, the Xbox's CONTEXT, a dispatch level) */
enum {
    ER_CODE = 0x00, ER_ADDRESS = 0x0C, ER_NPARAMS = 0x10, ER_INFO = 0x14,
    CX_EAX = 0x21C, CX_EBP = 0x220, CX_EIP = 0x224, CX_ESP = 0x230,
    LV_REG = 24,
};
#define CHAIN_END 0xFFFFFFFFu

/* 128 KiB of guest memory: the KPCR at 0x1000, the stack from 0x8000 to 0x10000 */
enum {
    MEM = 0x20000, KPCR = 0x1000, STACK_LIMIT = 0x8000, STACK_BASE = 0x10000,
    R1 = 0xFF00, R2 = 0xFE00, H1 = 0x4000, H2 = 0x3000, ESP0 = 0xFD00, EIP0 = 0x2000,
    SEH_RETURN = 0x5000, SEH_NESTED = 0x5010, SEH_COLLIDED = 0x5020,
};
static uint8_t g_mem[MEM];
static uint32_t g_pte[MEM / NC_PAGE];
static NcVm g_vm;
static NcKernel g_k;
static NcFrame g_f;

static uint32_t rd(uint32_t va) { return nc_vm_rd32(&g_vm, va); }
static void wr(uint32_t va, uint32_t v) { nc_vm_wr32(&g_vm, va, v); }

/* The machine before a fault: a thread at EIP0 with ESP0, its chain R2 -> R1
 * (two: both; one: R1 alone; none: empty). */
static void setup(int handlers)
{
    memset(g_mem, 0, sizeof(g_mem));
    for (unsigned i = 0; i < MEM / NC_PAGE; i++) {
        g_pte[i] = (i * NC_PAGE) | NC_PTE_VALID;
    }
    memset(&g_vm, 0, sizeof(g_vm));
    g_vm.base = g_mem;
    g_vm.pte = g_pte;
    memset(&g_k, 0, sizeof(g_k));
    g_k.vm = &g_vm;
    g_k.kpcr_va = KPCR;
    g_k.seh_return_va = SEH_RETURN;
    g_k.seh_nested_va = SEH_NESTED;
    g_k.seh_collided_va = SEH_COLLIDED;
    g_k.current = &g_k.threads[0];
    g_k.threads[0].id = 1;
    wr(KPCR + 4, STACK_BASE);
    wr(KPCR + 8, STACK_LIMIT);
    wr(R1, CHAIN_END);
    wr(R1 + 4, H1);
    wr(R2, R1);
    wr(R2 + 4, H2);
    wr(KPCR, handlers == 2 ? R2 : handlers == 1 ? R1 : CHAIN_END);
    for (int i = 0; i < 8; i++) {
        g_f.r[i] = 0x11110000u + (uint32_t)i;
    }
    g_f.r[NC_ESP] = ESP0;
    g_f.eip = EIP0;
    g_f.eflags = 0x246;
    g_stops = 0;
}

/* A handler's return to the dispatcher: its disposition in EAX, the stack
 * as its RET leaves it. */
static void handler_returns(uint32_t base, uint32_t disposition)
{
    g_f.r[NC_EAX] = disposition;
    g_f.r[NC_ESP] = base + 4;
    nc_seh_stub(&g_k, &g_f, NC_KORDINAL_SEH_RETURN);
}

static void test_continue(void)
{
    setup(1);
    CHECK(nc_seh_fault(&g_k, &g_f, NC_STATUS_ACCESS_VIOLATION, EIP0, 2, 1, 0xDEAD0000u));
    const uint32_t base = g_f.r[NC_ESP];
    CHECK(g_f.eip == H1);                       /* the handler runs... */
    CHECK(base < ESP0 && base >= STACK_LIMIT);  /* ...below the faulting code's stack */
    CHECK(rd(base) == SEH_RETURN);
    const uint32_t rec = rd(base + 4), ctx = rd(base + 12);
    CHECK(rd(base + 8) == R1);                  /* the establisher frame */
    CHECK(rd(rec + ER_CODE) == NC_STATUS_ACCESS_VIOLATION && rd(rec + ER_ADDRESS) == EIP0);
    CHECK(rd(rec + ER_NPARAMS) == 2 && rd(rec + ER_INFO) == 1 && rd(rec + ER_INFO + 4) == 0xDEAD0000u);
    CHECK(rd(ctx + CX_EIP) == EIP0 && rd(ctx + CX_ESP) == ESP0);
    CHECK(rd(ctx + CX_EAX) == 0x11110000u + NC_EAX && rd(ctx + CX_EBP) == 0x11110000u + NC_EBP);
    CHECK(rd(KPCR) == base + LV_REG && rd(base + LV_REG + 4) == SEH_NESTED);    /* the kernel's own frame around the call */
    /* the handler mends the context (steps over the instruction) and continues */
    wr(ctx + CX_EIP, EIP0 + 2);
    wr(ctx + CX_EAX, 0x1234);
    handler_returns(base, 0);
    CHECK(g_f.eip == EIP0 + 2 && g_f.r[NC_ESP] == ESP0 && g_f.r[NC_EAX] == 0x1234);
    CHECK(rd(KPCR) == R1 && g_k.threads[0].nseh == 0 && g_stops == 0);
    printf("%s a handler continues the faulting code\n", g_fail ? "FAIL" : "PASS");
}

static void test_search(void)
{
    const int before = g_fail;
    setup(2);
    CHECK(nc_seh_fault(&g_k, &g_f, NC_STATUS_INTEGER_DIVIDE_BY_ZERO, EIP0, 0, 0, 0));
    uint32_t base = g_f.r[NC_ESP];
    CHECK(g_f.eip == H2 && rd(base + 8) == R2);         /* the innermost first */
    handler_returns(base, 1);                            /* not its exception: the search goes on */
    base = g_f.r[NC_ESP];
    CHECK(g_f.eip == H1 && rd(base + 8) == R1 && g_stops == 0);
    handler_returns(base, 0);
    CHECK(g_f.eip == EIP0 && g_f.r[NC_ESP] == ESP0 && rd(KPCR) == R2 && g_k.threads[0].nseh == 0);
    printf("%s the search goes down the chain\n", g_fail == before ? "PASS" : "FAIL");
}

static void test_unhandled(void)
{
    const int before = g_fail;
    setup(1);
    CHECK(nc_seh_fault(&g_k, &g_f, NC_STATUS_ILLEGAL_INSTRUCTION, EIP0, 0, 0, 0));
    handler_returns(g_f.r[NC_ESP], 1);
    CHECK(g_stops == 1);                                 /* no handler took it: the stop it always was */
    printf("%s nobody takes it: the title stops\n", g_fail == before ? "PASS" : "FAIL");
}

static void test_refused(void)
{
    const int before = g_fail;
    /* no handler registered: nothing touched, the caller reports the fault */
    setup(0);
    const NcFrame f0 = g_f;
    CHECK(!nc_seh_fault(&g_k, &g_f, NC_STATUS_ACCESS_VIOLATION, EIP0, 2, 0, 0));
    CHECK(!memcmp(&f0, &g_f, sizeof(f0)) && g_k.threads[0].nseh == 0 && g_stops == 0);
    /* no stack under the faulting code for the dispatch */
    setup(1);
    g_f.r[NC_ESP] = STACK_LIMIT + 0x100;
    g_pte[STACK_LIMIT / NC_PAGE - 1] = 0;
    CHECK(!nc_seh_fault(&g_k, &g_f, NC_STATUS_ACCESS_VIOLATION, EIP0, 2, 0, 0));
    CHECK(g_f.eip == EIP0 && g_k.threads[0].nseh == 0);
    printf("%s no handler or no stack: refused, nothing changed\n", g_fail == before ? "PASS" : "FAIL");
}

static void test_trap(void)
{
    const int before = g_fail;
    /* INTO's trap: the record names the instruction, the context goes on after it */
    setup(1);
    CHECK(nc_seh_fault(&g_k, &g_f, NC_STATUS_INTEGER_OVERFLOW, EIP0 + 1, 0, 0, 0));
    const uint32_t base = g_f.r[NC_ESP], rec = rd(base + 4), ctx = rd(base + 12);
    CHECK(rd(rec + ER_ADDRESS) == EIP0 && rd(ctx + CX_EIP) == EIP0 + 1);
    handler_returns(base, 0);
    CHECK(g_f.eip == EIP0 + 1);
    printf("%s a trap continues after its instruction\n", g_fail == before ? "PASS" : "FAIL");
}

int main(void)
{
    test_continue();
    test_search();
    test_unhandled();
    test_refused();
    test_trap();
    printf(g_fail ? "%d checks failed\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
