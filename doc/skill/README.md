# doc/skill — 可复用经验沉淀

本目录收录在实现 ToyC→RISC-V32 编译器过程中沉淀下来的、**可迁移到其它编译器/系统
项目**的工程经验。每篇聚焦一个主题，给出"结论 + 为什么 + 怎么做"。

| 文档 | 主题 |
|------|------|
| [compiler-build-lessons.md](compiler-build-lessons.md) | 实现一个 C 子集编译器的整体经验与踩坑 |
| [riscv32-codegen.md](riscv32-codegen.md) | RV32 代码生成：调用约定、栈帧、寄存器使用约定 |
| [register-allocation.md](register-allocation.md) | 图着色寄存器分配的关键点与一个隐蔽 bug |
| [differential-testing-with-spike.md](differential-testing-with-spike.md) | 用 Spike + 原生参照做编译器差分模糊测试 |
| [perf-measurement-pitfalls.md](perf-measurement-pitfalls.md) | 性能度量陷阱：功能级模拟器 ≠ 真实耗时 |
