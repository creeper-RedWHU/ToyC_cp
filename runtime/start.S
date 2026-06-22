# Minimal bare-metal startup for running ToyC programs under the Spike RISC-V
# ISA simulator.  This file is ONLY used for *local* testing; it is never part
# of the compiler's stdout output (the judge links its own runtime/crt0).
#
# Protocol: call main(), then signal program exit to Spike via the HTIF
# `tohost` channel with payload `(exit_code << 1) | 1`.

.section .text.init,"ax",@progbits
.globl _start
_start:
    la   sp, _stack_top
    call main
    slli a0, a0, 1
    ori  a0, a0, 1
    la   t0, tohost
    sw   a0, 0(t0)
    sw   zero, 4(t0)
1:  j    1b

.section .tohost,"aw",@progbits
.align 6
.globl tohost
tohost:   .dword 0
.align 6
.globl fromhost
fromhost: .dword 0

.section .bss
.align 4
.skip 1048576
.globl _stack_top
_stack_top:
