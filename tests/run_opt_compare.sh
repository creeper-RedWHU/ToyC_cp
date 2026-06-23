#!/usr/bin/env bash
# ToyC 优化正确性对比脚本
#
# 对每个功能测试用例同时运行"无 -opt"和"-opt"两个版本，
# 比较两者的退出码是否一致。如果不一致，说明优化引入了 bug（OPT_BUG）。
#
# 该脚本依赖 run_tests.sh 的相同环境变量，需要 build/compiler、spike、riscv64-unknown-elf-gcc。
#
# Usage:
#   tests/run_opt_compare.sh                 # 测试 tests/cases/ 下所有 *.tc
#   tests/run_opt_compare.sh [case_glob]     # 只测匹配的用例
#   tests/run_opt_compare.sh inline_*.tc    # 只测内联相关用例
#
# 输出格式：
#   OK       <name>  (both=N)           — 两种模式结果相同
#   OPT_BUG  <name>  plain=N opt=M      — 优化改变了结果（BUG！）
#   COMP_ERR <name>  [plain|opt]        — 编译失败（两者之一）
#   TIMEOUT  <name>  [plain|opt]        — Spike 超时
#   SKIP     <name>                     — 编译跳过（host 编译失败且无 expect pin）

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPILER="${COMPILER:-$ROOT/build/compiler}"
CASES_DIR="$ROOT/tests/cases"
TMP="$ROOT/tests/tmp/opt_compare"
RT="$ROOT/runtime"

RVGCC="${RVGCC:-riscv64-unknown-elf-gcc}"
SPIKE="${SPIKE:-spike}"
HOSTCC="${HOSTCC:-cc}"
RVFLAGS="-march=rv32im -mabi=ilp32 -O0 -nostdlib -nostartfiles -T $RT/link.ld"
SPIKE_ISA="rv32im"
TIMEOUT_CMD="${TIMEOUT:-timeout 10}"

GLOB="${1:-*.tc}"

mkdir -p "$TMP"

if [ ! -x "$COMPILER" ]; then
    echo "ERROR: compiler not found at $COMPILER (set COMPILER= or build first)"
    exit 2
fi

ok=0; opt_bugs=0; comp_errs=0; timeouts=0; skips=0
opt_bug_list=""
shopt -s nullglob

for tc in "$CASES_DIR"/$GLOB; do
    name="$(basename "$tc" .tc)"
    s_plain="$TMP/${name}_plain.s"
    s_opt="$TMP/${name}_opt.s"
    elf_plain="$TMP/${name}_plain.elf"
    elf_opt="$TMP/${name}_opt.elf"

    # ── 步骤 1：无 -opt 编译 ────────────────────────────────────────────
    if ! "$COMPILER" < "$tc" > "$s_plain" 2>"$TMP/${name}_plain.cerr"; then
        echo "COMP_ERR  $name  plain"
        comp_errs=$((comp_errs + 1))
        continue
    fi
    if ! "$RVGCC" $RVFLAGS "$RT/start.S" "$s_plain" -o "$elf_plain" \
            2>"$TMP/${name}_plain.lerr"; then
        echo "COMP_ERR  $name  plain (link)"
        comp_errs=$((comp_errs + 1))
        continue
    fi
    $TIMEOUT_CMD "$SPIKE" --isa=$SPIKE_ISA "$elf_plain" >/dev/null 2>&1
    plain_exit=$?
    if [ "$plain_exit" = "124" ]; then
        echo "TIMEOUT   $name  plain"
        timeouts=$((timeouts + 1))
        continue
    fi
    plain=$(( (plain_exit % 256 + 256) % 256 ))

    # ── 步骤 2：-opt 编译 ───────────────────────────────────────────────
    if ! "$COMPILER" -opt < "$tc" > "$s_opt" 2>"$TMP/${name}_opt.cerr"; then
        echo "COMP_ERR  $name  opt"
        comp_errs=$((comp_errs + 1))
        continue
    fi
    if ! "$RVGCC" $RVFLAGS "$RT/start.S" "$s_opt" -o "$elf_opt" \
            2>"$TMP/${name}_opt.lerr"; then
        echo "COMP_ERR  $name  opt (link)"
        comp_errs=$((comp_errs + 1))
        continue
    fi
    $TIMEOUT_CMD "$SPIKE" --isa=$SPIKE_ISA "$elf_opt" >/dev/null 2>&1
    opt_exit=$?
    if [ "$opt_exit" = "124" ]; then
        echo "TIMEOUT   $name  opt"
        timeouts=$((timeouts + 1))
        continue
    fi
    opt=$(( (opt_exit % 256 + 256) % 256 ))

    # ── 步骤 3：对比结果 ────────────────────────────────────────────────
    if [ "$plain" = "$opt" ]; then
        echo "OK        $name  (both=$plain)"
        ok=$((ok + 1))
    else
        echo "OPT_BUG   $name  plain=$plain opt=$opt"
        opt_bugs=$((opt_bugs + 1))
        opt_bug_list="$opt_bug_list $name"
    fi
done

echo ""
echo "══════════════════════════════════════════════════════"
echo " 优化正确性对比结果"
echo "══════════════════════════════════════════════════════"
echo " OK       : $ok"
echo " OPT_BUG  : $opt_bugs"
echo " COMP_ERR : $comp_errs"
echo " TIMEOUT  : $timeouts"
echo " SKIP     : $skips"
if [ -n "$opt_bug_list" ]; then
    echo ""
    echo " *** OPT_BUG 用例（-opt 改变了结果）: ***"
    for name in $opt_bug_list; do
        echo "   - $name"
    done
fi
echo "══════════════════════════════════════════════════════"
[ "$opt_bugs" -eq 0 ] && [ "$comp_errs" -eq 0 ]
