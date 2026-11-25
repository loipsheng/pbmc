# 关卡 1 验收报告：空简化框架与原生传播接入

日期：2026-11-25  
结论：**PASS（仅普通无权精确模型计数 MC）**

## 1. 范围与停止点

本关只建立递归简化的空框架、明确状态、最小统计和正确调用位置，没有实现任何数学简化。`Simplifier::run()` 唯一返回 `UNCHANGED`；没有新增赋值、Reason、假设 trail、overlay、undo、proof dependency 或新 cache key。

调用顺序现在是：

```text
runPropagationWithoutLearnt(true)
    ├── conflict -> 原有 analyze/learn/backjump/backtrack -> 下一轮
    └── no conflict
          -> simplifyNode()               // Gate 1: UNCHANGED only
          -> syncAssign()
          -> splitComponent()
          -> legacy cache / branch / backtrack
```

Gate 1 到此停止。下一步只能先创建和审核 Gate 2A Basic 的独立伪代码，不能直接实现 Basic、BICP 或 Cache Bypass。

## 2. 实现结果

1. 新建唯一共享模块 `core/simplify.h/.cc`，定义短接口 `SimpStatus`、`SimpResult`、`SimpStats` 和 `Simplifier::run()`。实现同时显式实例化现有 `mpz_class` 与 `mpfr::mpreal` 模板，但运行时新框架仍只允许普通 MC。
2. `PBcounter::simplifyNode()` 检查 `inprocess + MC + !ddnnf`，记录调用和耗时，Debug 下验证 0E row/EqualityGroup/Component binding，再调用空 `Simplifier::run()`。
3. 主循环只在原生传播无冲突后调用 `simplifyNode()`，位置早于 `syncAssign()` 和 `splitComponent()`。Gate 1 对任何非 `UNCHANGED` 状态执行 Release 下也生效的 `logic_error`，不会继续分解或输出错误计数。
4. 删除位置过晚且恒为 `true` 的旧 `PBcounter::simplify()`、`on_simp`、`simp_dbs/simplify_time`，以及只服务旧 hook 的 `ComponentManager::fixed_level_/checkfixedDL()`。
5. 删除 `-rmvsatcl/-rmvsatclthd` 和 `ConfigCounter::remove_sat_cls/rmvsatcl_threshold`，不保留 deprecated alias。旧参数现在由 strict parser 作为未知参数非零拒绝。
6. 新统计仅在 `-inprocess` 时输出 calls、changed、conflict、budget、time 与 `cache = legacy`。空框架的 changed/conflict/budget 必须为 0。
7. 保留根预处理 `PBInstance::simplifyRenumberedConstraint()` 和 Glucose `Solver::simplify()`，没有改动 RoundingSat 传播、冲突分析、Reason、分支计数或 cache 数据结构。
8. 主源码对应变更已同步到 `pbmc-mod/`；由于 `Config.h` 本关首次成为覆盖文件，已补入 `pbmc-mod/core/Config.h`。

## 3. 修改文件

```text
CMakeLists.txt
core/simplify.h
core/simplify.cc
core/Counter.h
core/Counter.cc
core/Config.h
core/Config.cc
core/Component.h
utils/Options.cc
tests/inprocessing/simplify_result_test.cc
tests/inprocessing/run_gate_1.py
pbmc-mod/core/simplify.h
pbmc-mod/core/simplify.cc
pbmc-mod/core/Counter.h
pbmc-mod/core/Counter.cc
pbmc-mod/core/Config.h
pbmc-mod/core/Config.cc
pbmc-mod/core/Component.h
pbmc-mod/utils/Options.cc
docs/inprocessing/pseudocode/01_skeleton.md
paper/PBMC_Recursive_Simplification_Codex_Implementation_Guide_v1.1_CN.md
```

## 4. 动态测试

Gate 1 专项对以下实例逐一运行 `-no-inprocess` 与 `-inprocess`：

```text
or_ge.opb
less_equal.opb
equality_one.opb
signed_equality.opb
two_components.opb
conflict_unique.opb
backjump_without_lit.opb
```

每个实例均满足：

```text
disabled count == enabled count == brute-force oracle
disabled stable search stats == enabled stable search stats
enabled calls > 0
enabled changed == conflict == budget == 0
enabled cache == legacy
```

差分统计覆盖 equality groups、conflicts、decisions、components、split、cache lookup/hit/reduce、SAT calls/starts、backjumps 和 chronological backs。`-rmvsatcl`、`-no-rmvsatcl`、`-rmvsatclthd=2` 均确认以 `Unknown flag` 非零退出。

同时完整复跑 Gate 0B/0E：24 个既有随机 MC 用例、36 个混合 `>=/<=/=` 随机用例、mode/d-DNNF guard、真实 backjump、equality metadata 故障和强制 allocator GC 全部通过。

## 5. 构建与 sanitizer

Release：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

结果：**6/6 tests passed**。

ASan + UBSan：

```bash
cmake -S . -B /tmp/pbmc-gate1-sanitize \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build /tmp/pbmc-gate1-sanitize -j2
cmake -E env \
  ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir /tmp/pbmc-gate1-sanitize --output-on-failure
```

结果：**6/6 tests passed**，没有 ASan/UBSan 错误。沿用 0B/0E 约定关闭 LeakSanitizer，因此不把该结果声明为独立内存泄漏审计。

## 6. 静态审计

- `git diff --check` 通过。
- 主源码与 `pbmc-mod` 的 Counter、Component、Config、Options、simplify 文件逐字一致。
- PBMC 的旧 `simplify/on_simp/rmvsatcl/checkfixedDL/fixed_level_` 引用全部消失；测试文件只保留旧参数拒绝用例。
- `PBInstance::simplifyRenumberedConstraint()` 与 Glucose `Solver::simplify()` 仍存在。
- Release 配置确认 `CMAKE_BUILD_TYPE=Release`、`BUILD_TESTING=ON`。

## 7. 已知边界

- 当前没有任何简化规则，只有 `UNCHANGED`；尚未形成传播—简化联合不动点。
- 当前仍使用 legacy component cache，没有 Cache Bypass 或 residual cache。
- 没有新增 Reason、结构版本、undo 或 proof 协议；后续规则需要时必须先在各自伪代码中精确设计。
- 新框架仍拒绝 PMC/WMC/WPMC 与 d-DNNF；本关没有删除这些旧模式的既有代码。
- 构建仍显示既有的成员初始化顺序和未使用局部变量警告；本关没有扩大范围清理它们。
