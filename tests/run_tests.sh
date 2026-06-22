#!/usr/bin/env bash
# ToyC compiler test harness.
#
# For every *.tc case under tests/cases/ this script:
#   1. derives the EXPECTED exit code by compiling the case as C with the host
#      compiler and running it natively (ToyC is a strict C subset);
#   2. runs our compiler to produce RISC-V32 assembly;
#   3. assembles+links it with the bare-metal runtime and runs it on Spike;
#   4. compares the two exit codes (low 8 bits).
#
# A case may pin its own expected value with a leading comment:  // expect: 42
#
# Usage:  tests/run_tests.sh [-opt] [case_glob]
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPILER="${COMPILER:-$ROOT/build/compiler}"
CASES_DIR="$ROOT/tests/cases"
TMP="$ROOT/tests/tmp"
RT="$ROOT/runtime"

RVGCC="${RVGCC:-riscv64-unknown-elf-gcc}"
SPIKE="${SPIKE:-spike}"
HOSTCC="${HOSTCC:-cc}"
RVFLAGS="-march=rv32im -mabi=ilp32 -O0 -nostdlib -nostartfiles -T $RT/link.ld"
SPIKE_ISA="rv32im"

OPTFLAG=""
GLOB="*.tc"
for a in "$@"; do
  if [ "$a" = "-opt" ]; then OPTFLAG="-opt"; else GLOB="$a"; fi
done

mkdir -p "$TMP"
if [ ! -x "$COMPILER" ]; then
  echo "compiler not found at $COMPILER (set COMPILER= or build first)"; exit 2
fi

pass=0; fail=0; failed_list=""
shopt -s nullglob
for tc in "$CASES_DIR"/$GLOB; do
  name="$(basename "$tc" .tc)"
  s="$TMP/$name.s"; elf="$TMP/$name.elf"; nat="$TMP/$name.native"

  # Expected value: explicit pin or native run.
  exp="$(grep -oE '//[[:space:]]*expect:[[:space:]]*-?[0-9]+' "$tc" | head -1 | grep -oE '\-?[0-9]+$')"
  if [ -z "$exp" ]; then
    if ! "$HOSTCC" -x c -w -o "$nat" "$tc" >/dev/null 2>&1; then
      echo "SKIP  $name (host compile failed; pin with // expect:)"; continue
    fi
    "$nat"; exp=$?
  fi
  exp=$(( (exp % 256 + 256) % 256 ))

  # Our compiler -> assembly.
  if ! "$COMPILER" $OPTFLAG < "$tc" > "$s" 2>"$TMP/$name.cerr"; then
    echo "FAIL  $name (compiler error)"; fail=$((fail+1)); failed_list="$failed_list $name"; continue
  fi
  # Assemble + link + run.
  if ! "$RVGCC" $RVFLAGS "$RT/start.S" "$s" -o "$elf" 2>"$TMP/$name.lerr"; then
    echo "FAIL  $name (assemble/link error -> $TMP/$name.lerr)"; fail=$((fail+1)); failed_list="$failed_list $name"; continue
  fi
  "$SPIKE" --isa=$SPIKE_ISA "$elf" >/dev/null 2>&1; got=$?
  got=$(( (got % 256 + 256) % 256 ))

  if [ "$got" = "$exp" ]; then
    pass=$((pass+1)); # echo "ok    $name ($got)"
  else
    echo "FAIL  $name  expected=$exp got=$got"; fail=$((fail+1)); failed_list="$failed_list $name"
  fi
done

echo "------------------------------------------"
echo "PASS=$pass FAIL=$fail   ${OPTFLAG:+(opt)}"
[ -n "$failed_list" ] && echo "failed:$failed_list"
[ "$fail" -eq 0 ]
