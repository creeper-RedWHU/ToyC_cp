#!/usr/bin/env bash
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
SPIKE="${SPIKE:-/usr/local/bin/spike}"
RT=runtime
LINK="-march=rv32im -mabi=ilp32 -nostdlib -nostartfiles -T $RT/link.ld"

icount() {
    $SPIKE --isa=rv32im -g "$1" </dev/null 2>&1 \
        | awk '{ if ($2 ~ /^[0-9]+$/) s += $2 } END { print s }'
}

printf "%-22s %14s %14s %8s\n" "bench" "ours(insn)" "gcc(insn)" "ratio"
for name in p03_copy p08_basic_combined; do
    tc="tests/bench/$name.tc"
    ./build/compiler -opt < "$tc" > /tmp/$name.mine.s 2>/dev/null
    riscv64-unknown-elf-gcc $LINK "$RT/start.S" /tmp/$name.mine.s -o /tmp/$name.mine.elf 2>/dev/null \
        || { printf "%-22s %14s\n" "$name" "MINE_LINK_FAIL"; continue; }
    riscv64-unknown-elf-gcc $LINK -O2 "$RT/start.S" -x c "$tc" -o /tmp/$name.gcc.elf 2>/dev/null \
        || { printf "%-22s %14s\n" "$name" "GCC_LINK_FAIL"; continue; }
    mi=$(icount /tmp/$name.mine.elf)
    gi=$(icount /tmp/$name.gcc.elf)
    ratio=$(awk "BEGIN{ if ($mi>0) printf \"%.3f\", $gi/$mi; else print \"NA\" }")
    printf "%-22s %14s %14s %8s\n" "$name" "$mi" "$gi" "$ratio"
done
