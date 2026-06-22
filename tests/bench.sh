#!/usr/bin/env bash
# Performance comparison against the gcc -O2 baseline (the judge's reference).
# Compiles each benchmark with our compiler (-opt) and with gcc -O2, runs both
# on Spike, and reports wall-clock time and the perf ratio (gcc / ours, capped
# at 1.0 = the judge's per-test perf score).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPILER="${COMPILER:-$ROOT/build/compiler}"
RT="$ROOT/runtime"; BENCH="$ROOT/tests/bench"; TMP="$ROOT/tests/tmp"; mkdir -p "$TMP"
RVGCC="${RVGCC:-riscv64-unknown-elf-gcc}"; SPIKE="${SPIKE:-spike}"
LINK="-march=rv32im -mabi=ilp32 -nostdlib -nostartfiles -T $RT/link.ld"

timeit() { # echo best of 2 spike runs (seconds)
  local elf="$1" best=99 t
  for _ in 1 2; do
    local s=$(python3 -c 'import time;print(time.time())')
    "$SPIKE" --isa=rv32im "$elf" >/dev/null 2>&1
    local e=$(python3 -c 'import time;print(time.time())')
    t=$(python3 -c "print($e-$s)")
    awk "BEGIN{exit !($t<$best)}" && best=$t
  done
  echo "$best"
}

printf "%-12s %10s %10s %8s\n" "bench" "ours(s)" "gcc-O2(s)" "score"
total=0; n=0
for tc in "$BENCH"/*.tc; do
  name=$(basename "$tc" .tc)
  "$COMPILER" -opt < "$tc" > "$TMP/$name.mine.s" 2>/dev/null
  "$RVGCC" $LINK "$RT/start.S" "$TMP/$name.mine.s" -o "$TMP/$name.mine.elf" 2>/dev/null || { echo "$name: link FAIL"; continue; }
  "$RVGCC" $LINK -O2 "$RT/start.S" -x c "$tc" -o "$TMP/$name.gcc.elf" 2>/dev/null || { echo "$name: gcc FAIL"; continue; }
  mine=$(timeit "$TMP/$name.mine.elf")
  gcc=$(timeit "$TMP/$name.gcc.elf")
  score=$(python3 -c "m=$mine;g=$gcc;print('%.3f'%min(1.0,g/m if m>0 else 1))")
  printf "%-12s %10.3f %10.3f %8s\n" "$name" "$mine" "$gcc" "$score"
  total=$(python3 -c "print($total+$score)"); n=$((n+1))
done
[ "$n" -gt 0 ] && printf "%-12s %30s %8s\n" "AVG" "" "$(python3 -c "print('%.3f'%($total/$n))")"
