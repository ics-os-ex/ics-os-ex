#ifndef ICSOS_CPU_CONTEXT_H
#define ICSOS_CPU_CONTEXT_H

#include "../types.h"

/* Software-saved CPU state for context switching (x86_64). */
typedef struct __attribute__((packed)) _cpu_context {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 rip, cs, rflags, rsp, ss;
    u64 cr3;
    u64 retcanary;
} cpu_context;

/* FXSAVE area must be 16-byte aligned; pad in PCB. */
typedef struct __attribute__((aligned(16))) _fpu_state {
    u8 fx[512];
} fpu_state;

void context_switch(cpu_context *old, cpu_context *newctx, volatile int *release_on_cpu);
void context_load(cpu_context *ctx);
void ctx_load_check(cpu_context *ctx);
void fpu_save(fpu_state *s);
void fpu_restore(fpu_state *s);

#endif
