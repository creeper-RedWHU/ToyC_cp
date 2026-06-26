#!/usr/bin/env bash
set -u
cd /mnt/d/Toc/branches/develop
RT=runtime
SPIKE=/usr/local/bin/spike
LINK="-march=rv32im -mabi=ilp32 -nostdlib -nostartfiles -T $RT/link.ld"
icount() { $SPIKE --isa=rv32im -g "$1" </dev/null 2>&1 | awk '{ if ($2 ~ /^[0-9]+$/) s += $2 } END { print s }'; }
printf '%-22s %14s %14s %8s\n' 'bench' 'ours(insn)' 'gcc(insn)' 'ratio'
for name in p01_const p02_dead_code p03_copy p04_common_subexpr p05_algebra p06_tail_recursion p07_loop p08_basic_combined p09_advanced_graph p10_advanced_matrix p11_global_const_prop p12_const_expr_chain; do
  tc=tests/bench/$name.tc
  ./build/compiler -opt < "$tc" > /tmp/$name.base.s 2>/dev/null
  riscv64-unknown-elf-gcc $LINK $RT/start.S /tmp/$name.base.s -o /tmp/$name.base.elf 2>/dev/null || { printf '%-22s %s\n' "$name" BASE_LINK_FAIL; continue; }
  riscv64-unknown-elf-gcc $LINK -O2 $RT/start.S -x c "$tc" -o /tmp/$name.gcc.elf 2>/dev/null || { printf '%-22s %s\n' "$name" GCC_LINK_FAIL; continue; }
  mi=$(icount /tmp/$name.base.elf)
  gi=$(icount /tmp/$name.gcc.elf)
  ratio=$(awk "BEGIN{ if ($mi>0) printf \"%.3f\", $gi/$mi; else print \"NA\" }")
  printf '%-22s %14s %14s %8s\n' "$name" "$mi" "$gi" "$ratio"
done
