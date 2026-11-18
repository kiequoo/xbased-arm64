/*
 * xbased native core � setjmp/longjmp for the fault scopes (jmp.h), Windows
 * x64: the callee-saved registers of its ABI (rbx rbp rdi rsi r12-r15,
 * xmm6-xmm15), rsp and the return address, MXCSR and the x87 control word.
 * No unwinding: the jump leaves the vectored exception handler's frames
 * behind as the signal handler's were on Android.
 * Layout: GPRs at 0x00..0x38, rsp 0x40, rip 0x48, xmm6-15 0x50..0xE0,
 * mxcsr 0xF0, fcw 0xF4.
 */
#include "native/core/jmp.h"

/* System V passes the arguments in rdi and esi; the code reads Windows' rcx and edx */
#ifdef _WIN32
#define ARGS_TO_WIN ""
#else
#define ARGS_TO_WIN "movq %rdi, %rcx\n" "movl %esi, %edx\n"
#endif

__attribute__((naked, returns_twice)) int nc_setjmp(nc_jmp_buf env)
{
    __asm__ volatile(ARGS_TO_WIN
                     "movq %rbx, 0x00(%rcx)\n"
                     "movq %rbp, 0x08(%rcx)\n"
                     "movq %rdi, 0x10(%rcx)\n"
                     "movq %rsi, 0x18(%rcx)\n"
                     "movq %r12, 0x20(%rcx)\n"
                     "movq %r13, 0x28(%rcx)\n"
                     "movq %r14, 0x30(%rcx)\n"
                     "movq %r15, 0x38(%rcx)\n"
                     "leaq 8(%rsp), %rdx\n"
                     "movq %rdx, 0x40(%rcx)\n"
                     "movq (%rsp), %rdx\n"
                     "movq %rdx, 0x48(%rcx)\n"
                     "movdqu %xmm6, 0x50(%rcx)\n"
                     "movdqu %xmm7, 0x60(%rcx)\n"
                     "movdqu %xmm8, 0x70(%rcx)\n"
                     "movdqu %xmm9, 0x80(%rcx)\n"
                     "movdqu %xmm10, 0x90(%rcx)\n"
                     "movdqu %xmm11, 0xa0(%rcx)\n"
                     "movdqu %xmm12, 0xb0(%rcx)\n"
                     "movdqu %xmm13, 0xc0(%rcx)\n"
                     "movdqu %xmm14, 0xd0(%rcx)\n"
                     "movdqu %xmm15, 0xe0(%rcx)\n"
                     "stmxcsr 0xf0(%rcx)\n"
                     "fnstcw 0xf4(%rcx)\n"
                     "xorl %eax, %eax\n"
                     "ret\n");
}

__attribute__((naked, noreturn)) void nc_longjmp(nc_jmp_buf env, int val)
{
    __asm__ volatile(ARGS_TO_WIN
                     "movl %edx, %eax\n"
                     "testl %eax, %eax\n"
                     "jnz 1f\n"
                     "incl %eax\n" /* 0 comes back as 1 */
                     "1:\n"
                     "movq 0x00(%rcx), %rbx\n"
                     "movq 0x08(%rcx), %rbp\n"
                     "movq 0x10(%rcx), %rdi\n"
                     "movq 0x18(%rcx), %rsi\n"
                     "movq 0x20(%rcx), %r12\n"
                     "movq 0x28(%rcx), %r13\n"
                     "movq 0x30(%rcx), %r14\n"
                     "movq 0x38(%rcx), %r15\n"
                     "movdqu 0x50(%rcx), %xmm6\n"
                     "movdqu 0x60(%rcx), %xmm7\n"
                     "movdqu 0x70(%rcx), %xmm8\n"
                     "movdqu 0x80(%rcx), %xmm9\n"
                     "movdqu 0x90(%rcx), %xmm10\n"
                     "movdqu 0xa0(%rcx), %xmm11\n"
                     "movdqu 0xb0(%rcx), %xmm12\n"
                     "movdqu 0xc0(%rcx), %xmm13\n"
                     "movdqu 0xd0(%rcx), %xmm14\n"
                     "movdqu 0xe0(%rcx), %xmm15\n"
                     "ldmxcsr 0xf0(%rcx)\n"
                     "fnclex\n"
                     "fldcw 0xf4(%rcx)\n"
                     "movq 0x40(%rcx), %rsp\n"
                     "jmpq *0x48(%rcx)\n");
}
