# 关卡 0B 验收报告：普通 MC 基线

日期：2026-11-25  
结论：**PASS（仅普通无权精确模型计数 MC）**

## 1. 范围

本关只修复递归简化框架开始前的可测试基线，没有实现或接入任何简化规则，也没有改变 `count_main()` 的传播、冲突分析、分解、缓存或分支状态机。

本结论不覆盖 PMC、WMC、WPMC 或 d-DNNF。`-inprocess` 当前只是新框架的模式边界开关；关卡 1 前不会触发简化算法。

## 2. 已完成项

1. 新增独立 Python brute-force OPB oracle，枚举完整布尔赋值并直接判定原始 `>=`、`<=`、`=` 约束；oracle 不调用 PBMC、RoundingSat 或 PBMC 的 canonicalization。
2. 增加永久普通 MC 小例，覆盖 `>=`、`=`、负系数、冲突、两个独立组件和 backjump；另以 `<=` 小例固定应有模型数。
3. 完整初始化 `ConfigCounter` 的存活字段；`rmvsatcl_threshold` 从错误的 `bool` 修正为 `int`。
4. 增加 `-inprocess` 启动边界：只允许 `-mode=0 -no-ddnnf`；mode 1/2/3 和 d-DNNF 均明确报错并以非零状态退出。
5. 0B 当时前端仅接受 `>=` 和 `=`，`<=` 明确 fail-fast；该临时限制已由后续 0E 的精确 canonicalization 取代。
6. 暴露带 literal、无 literal 与 fallback backjump 统计，以及普通 chronological backtrack 统计。固定用例动态覆盖 `ResolveWithoutLit() -> bjResolve()` 和普通 `backtrack()`，并验证开关 backjump 不改变精确计数。
7. ASan/UBSan 运行时发现 RoundingSat 的 `ConstraintAllocator` 以 32 位 word 紧邻分配对象，不能保证下一对象的自然对齐。最小修复为每次分配补齐到 `std::max_align_t`，并同步修正 wasted accounting 与 garbage collection 的步长；没有修改约束内容、Reason 或求解语义。
8. 稳定变更已同步到现有 `pbmc-mod/` 与 `roundingsat-mod/` 对应持久副本。

## 3. 变更文件

主代码与持久副本：

- `core/Config.h`, `core/Config.cc`, `core/Main.cc`, `core/Instance.cc`
- `core/Counter.h`, `core/Counter.cc`
- `roundingsat/SolverStructs.hpp`, `roundingsat/Solver.cpp`
- 上述存在对应文件的 `pbmc-mod/`、`roundingsat-mod/` 副本
- `CMakeLists.txt`

测试：

- `tests/inprocessing/mc_oracle.py`
- `tests/inprocessing/run_gate_0b.py`
- `tests/inprocessing/cases/*.opb`

## 4. Oracle 与动态覆盖

永久用例的精确计数：

| 用例 | 语义 | 模型数 |
|---|---|---:|
| `or_ge.opb` | `x1 + x2 >= 1` | 3 |
| `equality_one.opb` | `x1 + x2 = 1` | 2 |
| `conflict_unique.opb` | 含冲突搜索的三行系统 | 1 |
| `two_components.opb` | 两个独立组件 | 9 |
| `negative_coefficient.opb` | 负系数 | 3 |
| `backjump_without_lit.opb` | 无 literal 回跳路径 | 19 |
| `less_equal.opb` | `x1 + x2 <= 1`；0B 时仅 oracle/fail-fast，0E 后 PBMC 已支持 | 3 |

每个已支持的永久用例都在 `-no-inprocess` 与 `-inprocess` 下和 oracle 比较。此外以固定种子生成 24 个非平凡小型 `>=`/`=` OPB 实例，包含正负系数和正负文字；PBMC 的普通 MC 结果全部与完整枚举一致。

固定 backjump 用例在 `-vs=0 -bjthd=0.99` 下观测到：

```text
backjumps = 1 (with-lit 0, without-lit 1, fallback 0)
chronological backs = 7
```

因此该用例实际进入 `ResolveWithoutLit()`、调用 `bjResolve()`，也执行了普通 `backtrack()`；`-no-bj` 仍返回 19。

## 5. 构建与测试结果

Release：

```bash
cmake -S . -B /tmp/pbmc-gate0b-release \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build /tmp/pbmc-gate0b-release -j2
ctest --test-dir /tmp/pbmc-gate0b-release --output-on-failure
```

结果：`gate_0b_exact_mc` 通过，1/1 tests passed。

ASan + UBSan：

```bash
cmake -S . -B /tmp/pbmc-gate0b-sanitize \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build /tmp/pbmc-gate0b-sanitize -j2
cmake -E env \
  ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir /tmp/pbmc-gate0b-sanitize --output-on-failure
```

结果：同一完整 Gate 0B 测试通过，1/1 tests passed；没有 ASan/UBSan 错误。当前容器的 LeakSanitizer 与 ptrace 环境不兼容，因此仍按源码审计约定关闭 leak detection；这不构成内存泄漏结论。

编译器仍报告仓库已有的初始化顺序和未使用局部变量警告；它们不属于 0B 的 sanitizer 错误，也未在本关扩大修复范围。

## 6. 停止点

关卡 0B 已完成。本段是当时的历史停止点；后续 0E-P/0E-I 已完成，详见 `00E_equality.md`。Gate 1 前仍不实现 `simplify.h/.cc` 或任何 CS/BICP/SC 等简化规则。
