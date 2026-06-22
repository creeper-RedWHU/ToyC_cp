# ToyC Compiler

A compiler for the **ToyC** language (a simplified C subset) that translates a
single source program read from `stdin` into **RISC-V 32-bit (RV32IM)** assembly
written to `stdout`.

```
compiler [-opt] < input.tc > output.s
```

`-opt` enables the optimizing pipeline (it is also safe to run without it).

## Build

The project uses CMake and produces a single executable named `compiler` at the
build directory root.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# -> build/compiler
```

A plain `Makefile` wrapper is also provided:

```sh
make            # configures + builds via CMake into ./build
```

## Pipeline

```
source ─▶ Lexer ─▶ Parser ─▶ AST ─▶ Sema ─▶ IR (TAC + CFG) ─▶ Opt ─▶ RISC-V32 codegen ─▶ asm
```

| Stage     | Files                          | Responsibility |
|-----------|--------------------------------|----------------|
| Lexer     | `src/lexer/*`                  | Tokenize; skip whitespace & comments |
| Parser    | `src/parser/*`, `src/ast/*`    | Recursive descent → AST |
| Sema      | `src/sema/*`                   | Scopes, name resolution, const-eval, return-path & loop checks |
| IR        | `src/ir/*`                     | Three-address code, basic blocks, CFG |
| Opt       | `src/opt/*`                    | Const folding/prop, CSE, DCE, register allocation, peephole |
| Codegen   | `src/codegen/*`                | RV32IM emission, calling convention, stack frames |

## Testing

Local testing runs generated code on the [Spike](https://github.com/riscv-software-src/riscv-isa-sim)
RISC-V simulator and compares the program exit code against the value obtained by
compiling the same source natively (ToyC being a C subset). See
`tests/run_tests.sh` and `runtime/` (a bare-metal startup used *only* for local
testing — it is never emitted by the compiler).

```sh
tests/run_tests.sh          # functional run
tests/run_tests.sh -opt     # with optimizations
```

Requirements for local testing: a RISC-V GCC (`riscv64-unknown-elf-gcc`,
used only as assembler/linker via `-march=rv32im`) and `spike`.

## Documentation

| 文档 | 内容 |
|------|------|
| [doc/report/实践报告.md](doc/report/实践报告.md) | 实践报告（架构、各阶段、优化、测试、性能） |
| [doc/git.md](doc/git.md) | 分支管理与四人协作开发规范 |
| [doc/skill/](doc/skill/) | 可复用工程经验（编译器/RV32 代码生成/寄存器分配/差分测试/性能度量） |
| [doc/任务要求.md](doc/任务要求.md) | 原始任务要求 |
