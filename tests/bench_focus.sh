#!/usr/bin/env bash
# Measure optimized icount for a focused set of benchmarks.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
RT=runtime
SPIKE="${SPIKE:-/usr/local/bin/spike}"
LINK="-march=rv32im -mabi=ilp32 -nostdlib -nostartfiles -T $RT/link.ld"

icount() {
    $SPIKE --isa=rv32im -g "$1" </dev/null 2>&1 \
        | awk '{ if ($2 ~ /^[0-9]+$/) s += $2 } END { print s }'
}

names="p01_const p02_dead_code p03_copy p04_common_subexpr p05_algebra p06_tail_recursion p07_loop p08_basic_combined p09_advanced_graph p10_advanced_matrix p11_global_const_prop p12_const_expr_chain arith divmod"

printf "%-24s %14s %14s %8s\n" "bench" "ours(insn)" "gcc(insn)" "ratio"
for n in $names; do
    tc="tests/bench/$n.tc"
    [ -f "$tc" ] || { printf "%-24s %14s\n" "$n" "NO_TC"; continue; }
    ./build/compiler -opt < "$tc" > /tmp/$n.mine.s 2>/dev/null
    riscv64-unknown-elf-gcc $LINK "$RT/start.S" /tmp/$n.mine.s -o /tmp/$n.mine.elf 2>/dev/null \
        || { printf "%-24s %14s\n" "$n" "MINE_LINK_FAIL"; continue; }
    riscv64-unknown-elf-gcc $LINK -O2 "$RT/start.S" -x c "$tc" -o /tmp/$n.gcc.elf 2>/dev/null \
        || { printf "%-24s %14s\n" "$n" "GCC_LINK_FAIL"; continue; }
    mi=$(icount /tmp/$n.mine.elf)
    gi=$(icount /tmp/$n.gcc.elf)
    ratio=$(awk "BEGIN{ if ($mi>0) printf \"%.3f\", $gi/$mi; else print \"NA\" }")
    printf "%-24s %14s %14s %8s\n" "$n" "$mi" "$gi" "$ratio"
done
