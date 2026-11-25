# 关卡 0E 验收报告：Operator / Equality Contract

日期：2026-11-25  
结论：**PASS（仅普通无权精确模型计数 MC）**

## 1. 范围与停止点

本关实现并验证 `>=`、`<=`、`=` 的精确输入语义，以及 equality 两个 canonical 方向的稳定身份和 ComponentManager 绑定。没有创建 `simplify.h/.cc`，没有实现 BICP、CS、GCD、SC/SC-LP 或其他简化规则，也没有改变 `count_main()` 的传播、冲突分析、分解、缓存查询和分支状态机顺序。

本结论不覆盖 PMC、WMC、WPMC 或 d-DNNF；`-inprocess` 仍只允许普通 MC 且关闭 d-DNNF。

## 2. 实现结果

1. `PBInstance::load()` 现在只接受明确的 `>=`、`<=`、`=`。`L <= b` 通过 checked arithmetic 转成 `-L >= -b`，再沿用既有的负系数转互补文字流程；零系数项被删除，空行按精确常量语义分类。
2. 负系数取正、RHS 调整、赋值代入和 probe RHS 更新使用 `mpz_class` 中间值检查 `long` 边界；无法表示的 `<=` 取反明确报 overflow，绝不按错误 comparator 继续。
3. 旧根单位推理与二元 probe 的 `rhs/coef` 判断已由 `double` 比值改成精确整数比较；空 probe 行也按常量约束处理。没有增加新推理规则。
4. 删除了 `Main.cc` 中“PBInstance 输出 OPB 字符串，再由 RoundingSat 重新解析”的导入回路。`PBcounter::addRows()` 直接用 `CeArb::addLhs/addRhs` 导入 canonical 行，并保留 `addConstraint()` 返回的 processed logical ID。
5. 每个 surviving equality 建立一个 `EqGroup` 和两个不同的 `RowVersion`。LOWER 使用原 canonical 行，UPPER 由 RoundingSat 原生 `invert()` 生成。两个方向先 staging、完整验证后一次发布；第二方向冲突时公式按根 UNSAT 结束，不发布半个 group。
6. `RowInfo` 明确区分 `ATTACHED` 与 `ROOT_TRUE`。恒真方向仍保留版本、side 和不可变行内容，但没有伪造 solver ID/CRef；attached 行只持久保存跨 GC 稳定的 logical ID，当前 solver CRef 每次通过 ID 解析。
7. `ComponentManager` 按 logical ID 为 attached `RowVersion` 建立独立的 `{ClID, component CRef}` 绑定。进入计数前再次校验 solver binding、component binding、group 双边、side、source、唯一 ID，以及两个 active equality 方向不共享 binding。
8. 根 UNSAT 现在作为精确计数 0 正常输出，不再以“解析错误”退出。输入预处理推导出的根单位按变量顺序提交到 RoundingSat 正式根赋值状态。

## 3. 关键身份不变量

```text
SourceRowID / EqualityGroupID / RowVersionID
                 │
                 ├── solver logical ID ──按需解析──> 当前 solver CRef
                 │
                 └── ComponentBind ───────────────> Component ClID + component CRef
```

- solver CRef 和 component CRef 属于不同 allocator，不能互换，也不作为稳定 identity。
- `ROOT_TRUE` 方向没有 solver/component binding，但不会从 equality group 消失。
- `ATTACHED` 方向必须在 solver 和 ComponentManager 各解析到恰好一个对象，且两处对象的 logical ID 一致。
- equality group 恰有 LOWER/UPPER 两行；两个版本不同，两个都 attached 时 logical ID、ClID 和 component CRef 也不同。

## 4. 动态测试

独立 brute-force oracle 直接判定原始 OPB 公式，不复用 PBMC canonicalization。固定用例覆盖：

| 用例 | 覆盖点 | 精确模型数 |
|---|---|---:|
| `less_equal.opb` | 正系数 `<=` | 3 |
| `signed_less_equal.opb` | `-2x + 3~y <= 1` | 3 |
| `equality_one.opb` | equality 两方向均 attached | 2 |
| `signed_equality.opb` | `-2x + 3~y = 1` | 1 |
| `equality_one_root_true.opb` | 一个 attached、一个 `ROOT_TRUE` | 1 |
| `equality_complement.opb` | `x + ~x = 1`，两个方向均 `ROOT_TRUE` | 2 |
| `equality_two_root_true.opb` | 两个方向均 `ROOT_TRUE` | 4 |
| `equality_unsat.opb` | equality 第一方向根冲突 | 0 |
| `constant_true.opb` | 空常量真行 | 4 |
| `constant_false.opb` | 空常量假行 | 0 |
| `overflow_less_equal.opb` | `LONG_MIN` 取反 overflow | 明确拒绝 |

运行时 equality 统计分别确认：

```text
equality_one.opb:           groups 1, attached 2, root-true 0
equality_one_root_true.opb: groups 1, attached 1, root-true 1
equality_two_root_true.opb: groups 1, attached 0, root-true 2
```

此外，固定种子生成 36 个非平凡小型混合 `>=/<=/=` 实例，包含正负/零系数、正负文字、重复变量和互补文字；PBMC 普通 MC 结果全部与完整赋值枚举一致。Gate 0B 的 24 个随机用例、mode guard 和真实 backjump 路径也继续通过。

`equality_metadata_test.cc` 还对缺边、错误 side、`ROOT_TRUE` 错配 solver ID、两个 active 方向复用 logical ID，以及多余 group 行逐项验证 fail-closed。`equality_gc_test.cc` 在 allocator 前部制造 padding、记录两个方向的旧 CRef、强制执行 RoundingSat GC，并确认两个 offset 实际移动后仍可由 RowVersion → logical ID 解析并通过内容/身份校验。

## 5. 构建与 sanitizer

Release：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

结果：4/4 tests passed（metadata fault、强制 GC、Gate 0B、Gate 0E）。

ASan + UBSan：

```bash
cmake -S . -B /tmp/pbmc-gate0e-sanitize \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build /tmp/pbmc-gate0e-sanitize -j2
cmake -E env \
  ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir /tmp/pbmc-gate0e-sanitize --output-on-failure
```

结果：4/4 tests passed；没有 ASan/UBSan 错误。按 0B 约定，当前容器关闭 LeakSanitizer，因此该结果不代表单独的内存泄漏审计。

## 6. 下一关

Gate 0E 到此停止。下一步是 Gate 1 的独立伪代码文档与用户审核；在审核前不创建/接入真正的递归简化算法。
