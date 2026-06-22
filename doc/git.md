# Git 分支管理与多人协作开发规范

本项目为**四人一组**的 ToyC 编译器实践。本文约定分支模型、提交规范与协作流程，
保证 `main` 始终可构建、历史清晰可追溯、四人并行不互相阻塞。

---

## 一、分支模型（GitFlow-lite）

| 分支 | 作用 | 谁能直接提交 | 稳定性 |
|------|------|------------|--------|
| `main` | 稳定发布分支，**始终可构建、可评测** | 无（只接受合并） | 最高 |
| `develop` | 集成分支，汇聚各特性 | 无（只接受合并） | 高 |
| `feat/*` | 特性分支，一人一特性 | 该特性负责人 | 开发中 |
| `fix/*` | 缺陷修复分支 | 修复人 | 开发中 |

```
feat/lexer ─┐
feat/parser ─┼─▶ develop ──(里程碑, --no-ff)──▶ main ──(tag)──▶ 提交评测
feat/codegen ┘
```

- **命名**：`feat/<模块>-<简述>`，如 `feat/regalloc`、`feat/loop-opt`、`fix/param-interf`。
- **粒度**：一个特性分支只做一件事，便于评审与回滚。
- `main`/`develop` **永远不直接 commit**，只通过合并更新。
- 里程碑合并到 `main` 用 `--no-ff` 保留合并节点，并打 tag（如 `v0.1-functional`、
  `v0.2-regalloc`）。

### 四人分工建议（按编译阶段解耦）

| 成员 | 负责模块 | 主要分支 |
|------|---------|---------|
| A | 词法 + 语法 + AST | `feat/frontend` |
| B | 语义分析 + IR 生成 | `feat/sema-ir` |
| C | 代码生成 + 调用约定 | `feat/codegen` |
| D | 优化 + 寄存器分配 + 测试 | `feat/opt`、`feat/tests` |

模块间通过**头文件契约**（`*.h` 的接口）解耦：先约定接口，各自并行实现，
冲突面最小。测试与差分模糊框架（D 负责）尽早搭好，成为所有人的安全网。

---

## 二、提交规范

- **约定式提交**：`<type>(<scope>): <subject>`
  - `type` ∈ `feat`/`fix`/`perf`/`refactor`/`test`/`docs`/`chore`/`build`
  - 例：`perf(codegen): loop rotation + constant strength reduction`
- subject 用祈使句、简明；正文（可选）说明**为什么**与**影响范围**。
- **一次提交一个逻辑改动**，可独立构建、可独立回滚。
- **禁止** `Co-Authored-By` 等尾注；保持作者信息统一。
- 提交身份（本项目约定）：

```sh
git config user.name  "creeper-RedWHU"
git config user.email "zhoujinyao11555@sina.com"
```

---

## 三、日常协作流程（每位成员）

```sh
# 0. 一次性配置身份（见上）

# 1. 从最新 develop 切出特性分支
git checkout develop && git pull --ff-only
git checkout -b feat/my-feature

# 2. 小步提交
git add -p
git commit -m "feat(scope): ..."

# 3. 推送并开启评审（GitHub PR / 合并请求）
git push -u origin feat/my-feature

# 4. 评审通过后合并进 develop（--no-ff 保留特性边界）
git checkout develop && git pull --ff-only
git merge --no-ff feat/my-feature -m "merge: <feature>"
git push origin develop
git branch -d feat/my-feature        # 本地清理
git push origin --delete feat/my-feature  # 远端清理
```

### 保持与 develop 同步（避免大冲突）

```sh
# 在自己的特性分支上，定期合并最新 develop
git checkout feat/my-feature
git merge develop          # 或 git rebase develop（仅限未推送/未共享的提交）
```

> 规则：**已推送/已共享**的分支不要 rebase（会改写历史、坑队友）；
> 只在本地、未共享时用 rebase 整理提交。

---

## 四、里程碑发布到 main

```sh
git checkout main && git pull --ff-only
git merge --no-ff develop -m "release: <里程碑说明>"
git tag -a v0.2-regalloc -m "graph-colouring register allocation"
git push origin main --tags
```

**合并到 main 前的检查清单（CI 或人工）**：
1. 干净 clone + `cmake -S . -B build && cmake --build build` 能产出 `build/compiler`；
2. `tests/run_tests.sh` 与 `tests/run_tests.sh -opt` **全过**；
3. 差分模糊（`tests/fuzz.py`）抽样无失败；
4. `-Wall -Wextra` 无警告。

---

## 五、代码评审（Pull Request）

- 每个 `feat/*` 通过 PR 合入 `develop`，至少一名队友 review。
- 关注点：正确性（尤其边界）、是否破坏既有测试、接口是否清晰、是否有新增测试。
- **不通过测试不合并**；评审意见在 PR 内讨论，改动追加提交。
- 评审标准可参考 `doc/skill/` 下的经验文档。

---

## 六、冲突处理

```sh
git merge develop
# 出现冲突：编辑冲突文件，保留正确逻辑
git add <已解决文件>
git commit          # 完成合并提交
```

- 冲突高发区（如 `CMakeLists.txt` 用了 GLOB、公共头文件）应**提前约定接口**减少碰撞。
- 解决后**务必重新构建并跑测试**再提交。

---

## 七、提交评测

按任务要求，提交时提供仓库地址、访问令牌与分支名，例如：

```
https://<user>:<token>@github.com/<user>/<repo>.git    分支: main
```

确保被提交的分支（通常是 `main`）满足上面"发布检查清单"的全部四项。
