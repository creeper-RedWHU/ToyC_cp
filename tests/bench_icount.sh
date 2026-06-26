#!/usr/bin/env bash
# 用 spike histogram (-g) 测每个 benchmark 的动态指令总数。
# 平台无关、零噪声指标：指令越少越好。对比本编译器 -opt 与 gcc -O2。
set -u
# 自动定位脚本所在仓库根（tests/ 的上一级），保证在任意 worktree 中都测本分支的编译器
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
RT=runtime
SPIKE="${SPIKE:-/usr/local/bin/spike}"
LINK="-march=rv32im -mabi=ilp32 -nostdlib -nostartfiles -T $RT/link.ld"

# 求 histogram 指令总数：spike -g 输出 "PC 次数"，对第二列求和
icount() {
    $SPIKE --isa=rv32im -g "$1" </dev/null 2>&1 \
        | awk '{ if ($2 ~ /^[0-9]+$/) s += $2 } END { print s }'
}

printf "%-22s %14s %14s %8s\n" "bench" "ours(insn)" "gcc(insn)" "ratio"
for tc in tests/bench/*.tc; do
    name=$(basename "$tc" .tc)
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
