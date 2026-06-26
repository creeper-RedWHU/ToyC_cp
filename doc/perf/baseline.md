# 性能基准 Baseline（develop 分支）

测量环境：WSL2 Ubuntu 26.04 + 源码编译的 spike（riscv-isa-sim）+ riscv64-unknown-elf-gcc 14.2。
编译器构建：`cmake -DCMAKE_BUILD_TYPE=Release`，编译器以 `-opt` 模式生成汇编。

## 指标说明

本地 spike 是**功能级模拟器**，墙钟时间在毫秒级波动大、不可复现，**不能**直接预测评测平台
（真实时钟周期）的得分。因此 baseline 采用 **动态指令总数**（`spike -g` histogram 求和）作为
平台无关、零噪声、可复现的核心指标。

- `ours(insn)`：本编译器 `-opt` 生成代码在 spike 上执行的动态指令总数
- `gcc(insn)`：`gcc -O2` 生成代码的动态指令总数
- `ratio = gcc / ours`：越接近或超过 1.0 越好（1.0 = 与 gcc -O2 持平）

## Baseline 数据（2026-06-26，修复 copyProp/LICM 之后）

| bench | ours(insn) | gcc(insn) | ratio | 主要瓶颈 |
|-------|-----------:|----------:|------:|---------|
| bench_matrix          |     130,001 |    55,001 | 0.423 | 内联+LICM（小函数 elem 每次调用开销）|
| p06_tail_recursion    |  88,130,001 | 40,045,001 | 0.454 | 尾递归未转循环，每层调用栈开销 |
| bench_sort            |  32,255,001 | 15,755,001 | 0.488 | 循环体与分支布局 |
| p04_common_subexpr    |  64,005,001 | 32,005,001 | 0.500 | CSE 仅块内，循环体内重复子表达式未消除 |
| fib                   | 313,735,001 | 160,480,001 | 0.512 | 递归调用约定开销，无内联/展开 |
| p06 …                 |            |           |       |        |
| bench_call_intensive  | 101,205,001 | 60,805,001 | 0.601 | 函数调用开销，内联门槛保守 |
| helper_loop           | 324,005,001 | 216,005,001 | 0.667 | 同上，循环内小函数调用 |
| p02_dead_code         |  24,005,001 | 16,005,001 | 0.667 | 死代码跨块未充分消除 |
| p05_algebra           |  30,005,001 | 22,005,001 | 0.733 | 代数化简后仍有冗余 mv |
| p01_const             |  36,005,001 | 27,005,001 | 0.750 | 常量折叠后循环体仍偏大 |
| p08_basic_combined    |  24,005,001 | 18,005,001 | 0.750 | 综合 |
| p11_global_const_prop |  63,005,001 | 48,005,001 | 0.762 | 全局变量反复 load，无全局常量传播 |
| divmod                | 136,005,001 | 104,005,001 | 0.765 | 除法/取模序列 |
| p09_advanced_graph    |  83,805,001 | 64,130,001 | 0.765 | 递归调用 |
| p10_advanced_matrix   |   6,550,001 |  5,025,001 | 0.767 | 内联+LICM |
| p07_loop              |  52,020,001 | 40,010,001 | 0.769 | 内层循环不变量提升不充分 |
| bench_reg_pressure    |  90,005,001 | 72,005,001 | 0.800 | 寄存器分配/溢出 |
| p03_copy              |  26,005,001 | 22,005,001 | 0.846 | 复制链 |
| nested                | 224,035,001 | 192,025,001 | 0.857 | 嵌套循环 |
| arith                 | 180,005,001 | 160,005,001 | 0.889 | 算术强度削减 |
| p12_const_expr_chain  |  39,005,001 | 36,005,001 | 0.923 | 常量链 |

## 功能正确性（前提门槛）

- 默认模式：59/59 PASS
- `-opt` 模式：59/59 PASS
- 优化一致性对比（run_opt_compare.sh）：59 OK / 0 OPT_BUG

## 复现方法

```sh
wsl -d Ubuntu -- bash -lc 'cd /mnt/d/Toc/branches/<wt> && \
  cmake --build build -j$(nproc) && \
  SPIKE=/usr/local/bin/spike bash tests/bench_icount.sh'
```
