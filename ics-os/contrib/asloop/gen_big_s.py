#!/usr/bin/env python3
# Generate a large valid x86-64 .s file for the in-OS `as` tight loop
# (contrib/asloop). The in-OS `as` (GNU binutils 2.23, DEFAULT_ARCH=x86_64)
# defaults to AT&T syntax -- exactly what GCC emits for x86-64 and what the
# strict self-host cert feeds it (see contrib/bintest/mini.s: `mov $..,%rax`,
# `leaq ..(%rip),%rbx`). We therefore emit AT&T, not Intel: an Intel-syntax
# file (`mov rax,rdx`) is rejected line-by-line by the default AT&T `as`
# (deterministic baseline failure, NOT the corruption under test).
#
# The content is deterministic (seeded by the line index) so every run reads
# identical bytes. Instructions are plain register-to-register / immediate /
# stack-relative forms that are unambiguously valid in 64-bit AT&T mode; no
# shift-by-register (which would require %cl) is used.
import sys

def main():
    target = int(sys.argv[1]) if len(sys.argv) > 1 else 4 * 1024 * 1024
    regs = ["%rax","%rbx","%rcx","%rdx","%rsi","%rdi","%r8","%r9",
            "%r10","%r11","%r12","%r13","%r14","%r15"]
    nr = len(regs)
    # 2-operand AT&T (dst,src) register ops; all valid with 64-bit registers.
    ops2 = ["movq","addq","subq","xorq","andq","orq","cmpq","testq","imulq","subq"]
    out = sys.stdout.buffer
    out.write(b".code64\n.text\n.globl _start\n.type _start,@function\n_start:\n")
    size = 0
    i = 0
    while size < target:
        r1 = regs[i % nr]
        r2 = regs[(i * 7 + 3) % nr]
        imm = (i * 2654435761) % 0x7FFFFFFF
        l1 = "    %s %s,%s\n" % (ops2[i % 10], r1, r2)
        l2 = "    addq $%d,%s\n" % (imm % 0x7FFFFFFF, r2)
        l3 = "    leaq -%d(%%rsp),%s\n" % ((imm % 256) + 1, r1)
        data = (l1 + l2 + l3).encode()
        out.write(data)
        size += len(data)
        i += 1
    out.write(b"    ret\n")
    out.flush()

if __name__ == "__main__":
    main()
