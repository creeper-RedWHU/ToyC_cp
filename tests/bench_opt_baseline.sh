#!/usr/bin/env bash
set -u
cd /mnt/d/Toc/branches/perf-loop-opt
for n in p07_loop p08_basic_combined p11_global_const_prop p12_const_expr_chain p10_advanced_matrix p01_const; do
    ./build/compiler -opt < tests/bench/$n.tc > /tmp/${n}_o.s 2>/dev/null
    riscv64-unknown-elf-gcc -march=rv32im -mabi=ilp32 -nostdlib -nostartfiles -T runtime/link.ld runtime/start.S /tmp/${n}_o.s -o /tmp/${n}_o.elf 2>/dev/null
    mi=$(/usr/local/bin/spike --isa=rv32im -g /tmp/${n}_o.elf </dev/null 2>&1 | awk '{ if ($2 ~ /^[0-9]+$/) s += $2 } END { print s }')
    printf '%-24s %14s\n' "$n" "$mi"
done
