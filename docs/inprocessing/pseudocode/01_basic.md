# 2A-P：Basic 与逻辑 Residual View 伪代码

版本：v1.0-draft  
日期：2026-11-25  
状态：**等待用户审核；尚未实现 C++**  
适用范围：普通无权精确模型计数（exact MC）

## 1. 本关结论

PBMC 的 Basic 不应重新实现一次 PB 单行传播。真实 RoundingSat 的 `Counting/Watched/Cardinality/Clause` 传播器已经执行同一条精确规则，并通过 `Solver::propagate(lit, CRef)` 为非根赋值保存正式 Reason：

```text
row: sum(a_i * l_i) >= d
T  := 当前为真的系数和
U  := 当前未赋值文字的最大贡献和
gap := d - T
slack := U - gap

gap <= 0       -> 当前行已满足
U < gap        -> 当前节点冲突
a_i > slack    -> 未赋值文字 l_i 必须为真
```

因此 Gate 2A 将 Basic 定义为：

1. 按当前 trail 对当前计数组件中的基础 RowVersion 建立**瞬时、只读** residual view。
2. 精确分类 `SATISFIED/OPEN`，为后续 BICP、CS 和 RIR 固定同一套残余语义。
3. 验证进入 Basic 前的 RoundingSat 传播确实已到不动点。
4. 不删除变量、不提交赋值、不生成第二份 Reason、不修改行或 cache。

正常 Basic 只返回 `UNCHANGED`。如果完整扫描仍发现 `U<gap` 或未知文字 `a_i>slack`，这是“原生传播声称无冲突且已到不动点”与精确全行扫描矛盾，必须以运行时错误 fail-closed；Gate 2A 不绕过原冲突协议建立备用传播器。

## 2. 明确不做

- 不调用 `decide()`、`uncheckedEnqueue()` 或 `propagate()`。
- 不返回 `IMPLIED`、`CONFLICT`、`ROW_CHANGED` 或 `ROW_REMOVED`。
- 不建立传播—简化联合循环；第一次真正需要联合循环的是会产生新赋值的 BICP 提交关卡。
- 不物理删除 fixed variable 的列，不修改 `PBconstraint`、`rs::Constr` 或 ComponentManager 的 frozen 副本。
- 不把 satisfied 行标成 inactive；正式 active-mask/删行属于 RIR。
- 不扫描 learned row 来构造组件拓扑；learned row 继续只参与 RoundingSat 传播与冲突剪枝。
- 不实现 BICP、SB-LP、CS、GCD、RIR、Cache Bypass 或 residual cache。
- 不为 PMC/WMC/WPMC、投影变量、权重或 d-DNNF 增加路径。
- 不引入持久 `ResidualMatrix`、第二套 occurrence、adapter、rule 基类或 proof manager。

## 3. 已核实的源码事实

### 3.1 原生传播已经实现 Basic 的强制规则

- `runPropagationWithoutLearnt(true)` 消费 trail 到 `qhead == trail.size()`，对所有已 attach 的 formula/learned 约束调用原生 watch。
- `Counting::checkForPropagation()` 与 `Watched::checkForPropagation()` 在 slack 为负时返回真实冲突，在 `coef > slack` 时调用 `solver.propagate(lit, cr)`。
- Clause 和 Cardinality 使用各自等价的 watched 规则，同样将当前约束 CRef 作为 Reason。
- `Solver::uncheckedEnqueue()` 在非根层保存 `Reason[var]=cr`；根层赋值是永久 unit，RoundingSat 按原设计将 Reason 置为 `CRef_Undef`。
- `garbage_collect()` 会重写 watches、非空 Reason 和 external CRef；所以 live Reason 可以跨 GC，但不能缓存旧 CRef。

Basic 若再次自行 enqueue，会重复 native UP、增加 trail/Reason 生命周期分支，并让“原生传播后才简化”的协议失去唯一所有者。

### 3.2 可复用的 0E 身份

- `RowInfo` 保存稳定 `RowVersionID`、EqualityGroup/side、`solver_id` 和不可变 canonical content。
- `solverRef(version)` 按稳定 logical ID 解析当前 solver CRef，不能永久保存 allocator offset。
- `ComponentManager::rowBind(version)` 提供该 RowVersion 的 frozen `ClID`；`topComponent()` 已含当前计数组件的 ClID 列表。
- `ROOT_TRUE` 方向没有 solver/component binding，且在所有递归节点永久为真。

### 3.3 不能复用的旧字段

- `ComponentManager::gaps_` 是上一次 `splitComponent()` 期间计算的 `int`，Basic 调用时 `syncAssign()` 尚未执行，它可能过期且精度不足。
- `assigns` 也是供 ComponentManager 使用的 0-based 镜像，此时尚未同步最新 trail。
- Basic 必须直接读取 RoundingSat `Level/Pos` 和 live `rs::Constr`，并使用 `rs::BigVal/BigCoef` 精确计算。

### 3.4 reduceDB 的生命周期缺口

当前 RoundingSat `reduceDB()` 会用 override 删除“根层已满足”的约束，包括 locked `Origin::FORMULA`。冲突分析会保护正在作为 Reason 的约束，但 0E 的 attached RowVersion 仍要求 logical ID 可解析，Basic 也需要从 live 公式行建立 view。

Gate 2A-I 必须采用窄修复：给 `reduceDB()` 增加默认关闭的 `keep_formula` 参数；PBMC 的整个 `-inprocess` 路径在三个 cleanup 调用点传 `true`。这是 0E RowVersion/逐节点 `validateRows(true)` 的共同生命周期要求，不只服务 Basic。独立 RoundingSat 和 `-no-inprocess` 保持原行为，learned database 仍正常清理。不能把已删除公式行的旧 CRef 留给 RowInfo 或 Reason。

## 4. 与 EDPLLSim RV 的关系

EDPLLSim 的 `removeFixedVariables()` 会从当前递归 `LinearConstraint::matrix` 物理删除固定变量列，并同步修改 RHS；该矩阵是组件自己的值域副本。

PBMC 不迁移这种物理删除方式。PBMC 的 solver 约束、watch、Reason、0E RowVersion 和 ComponentManager frozen row 共享身份，直接删列会同时破坏传播与缓存语义。PBMC 的等价做法是每次用 trail 构造逻辑 residual：

```text
true literal    -> 系数从 gap 中扣除
false literal   -> 对 residual 无贡献
unknown literal -> 保留原文字和精确系数
```

这个 view 不拥有原行，也不跨节点保存，因此回跳后无需 undo；下一次调用从恢复后的 trail 重新计算。

## 5. 数学契约

### 5.1 单个 canonical 方向

输入方向已经由 0E 规范为：

```text
sum(a_i * l_i) >= d,  a_i > 0, l_i in {0,1}
```

令当前 trail 固定的真、假、未知文字集合分别为 `T/F/N`：

```text
true_sum = sum(a_i | l_i in T)
gap      = d - true_sum
U        = sum(a_i | l_i in N)
slack    = U - gap
residual = sum(a_i*l_i | l_i in N) >= gap
```

所有运算使用 `rs::BigVal/BigCoef`，不先转 `long`、`int` 或 `double`。

### 5.2 分类证明

- `gap<=0`：已为真的项单独就达到原 degree，任意未知扩展都满足该方向。
- `gap>U`：即使所有未知文字为真也达不到 gap，所以当前 trail 无扩展模型。
- `gap>0 && gap<=U`：行仍 open。
- 对 open 行的未知文字 `l_i`，若 `a_i>slack=U-gap`，令 `l_i=0` 后最大和为 `U-a_i<gap`，所以 `l_i` 必须为真。
- 条件必须是严格 `>`；`a_i==slack` 时令该文字为假仍可能恰好达到 gap，不能传播。

### 5.3 完整模型集合

对固定 trail，删除真/假项得到的 residual 与原方向在未知变量上的满足赋值集合一一相同。Basic 只读取该等价式，不提交任何改写，因此整个公式的完整模型集合和模型数不变，也不产生自由变量补偿因子。

## 6. Equality-aware 契约

EqualityGroup 表示两个 canonical 方向的合取：

```text
LOWER: P >= b
UPPER: A-P >= A-b
```

Basic 对两个 `ATTACHED` 方向分别建立 residual；`ROOT_TRUE` 方向永久跳过但不能从 group metadata 删除。

```text
group satisfied now  <=> 两个方向均为 SATISFIED/ROOT_TRUE
group open           <=> 至少一个 attached 方向为 OPEN
```

该 group 分类只用于统计/检查，不写回 `EqGroup.revision`，不删除整个 equality，也不把一个方向的 Reason 冒充另一方向的证明。任一方向出现 missed conflict/implication 都 fail-closed。

## 7. 最小接口决定

### 7.1 开关

新增短开关：

```text
-basic / -no-basic
```

- 默认关闭，符合所有新规则默认关闭的合同。
- `-basic` 必须与 `-inprocess` 同时使用；单独传 `-basic` 明确报错，不能成为静默 no-op。
- `-inprocess -no-basic` 保持 Gate 1 空框架行为。
- 非 MC 或 d-DNNF 继续由既有 inprocess guard 拒绝。

### 7.2 代码接口

只在现有统一模块增加短方法：

```text
Simplifier<T>::run(counter):
    if !counter.config.basic:
        return UNCHANGED
    return basic(counter)

Simplifier<T>::basic(counter) -> SimpResult
```

Basic 需要读取 PBcounter 的 `rows_`、solver constraints/allocator、Level/Pos 和 `cmpmgr`。Gate 2A-I 采用单个窄 friend：

```text
friend class Simplifier<T_data>
```

不增加一组仅为 Basic 暴露内部容器的 public getter，也不把逻辑搬回 `Counter.cc`。

### 7.3 局部 residual 类型

`Residual` 只放在 `simplify.cc` 内部，不写进公共头文件：

```text
Residual {
    version: RowVersionID
    gap: rs::BigVal
    upper: rs::BigVal       // U
    slack: rs::BigVal
    unknown_terms: transient references or indices
}
```

Gate 2A 不持久保存它。后续结构规则真正需要 overlay 时再设计版本和 undo，不能把瞬时引用塞入 cache 或跨 GC 保存。

## 8. 当前组件行选择

Basic 不全局扫描所有 sibling component：

1. 从 `cmpmgr.topComponent()` 的 ClID 段构造本次局部 `current_clids`。
2. 扫描 `rows_`；跳过 `ROOT_TRUE`。
3. 通过 `cmpmgr.rowBind(version).clid` 判断 attached RowVersion 是否属于当前组件。
4. 扫描 solver `constraints` 一次，按 stable `Constr::id` 建立本次 `solver_id -> current CRef` 映射；禁止对每行 O(全部约束) 调用并缓存旧 CRef。
5. 验证 live `Constr::id == RowInfo.solver_id`，再计算 residual。

ComponentManager 中没有 RowInfo 的根 unit 行可以留在组件 ClID 列表中；它们已经由 native UP 处理，Basic 只扫描有 0E RowVersion 的基础公式方向。learned row 不在 frozen ComponentManager 中，也不进入该 view。

## 9. 逐步伪代码

### 9.1 构造 residual

```text
makeResidual(row, live_constraint, Level, Pos):
    require row.state == ATTACHED
    require live_constraint.id == row.solver_id

    true_sum := BigVal(0)
    U := BigVal(0)
    unknown := empty transient list

    for i in [0, live_constraint.size):
        lit := live_constraint.lit(i)
        coef := exact(live_constraint.coef(i))
        require coef > 0

        if isTrue(Level, lit):
            true_sum += coef
        else if isFalse(Level, lit):
            continue
        else:
            require isUnknown(Pos, lit)
            U += coef
            unknown.push(i)

    gap := exact(live_constraint.degree()) - true_sum
    slack := U - gap
    return Residual(row.version, gap, U, slack, unknown)
```

Debug 构建还应确认同一 live row 不含重复变量、literal 0 或非正系数。不能依赖 `RowInfo.content` 的 `long` 累加来代替 live exact row。

### 9.2 分类并验证传播不动点

```text
checkResidual(row, live_constraint, residual):
    if residual.gap <= 0:
        return SATISFIED

    if residual.upper < residual.gap:
        failClosed("native propagation missed conflict", row.version,
                   row.source, row.side, residual.gap, residual.upper)

    for index in residual.unknown:
        if live_constraint.coef(index) > residual.slack:
            failClosed("native propagation missed implication",
                       row.version, row.source, row.side,
                       live_constraint.lit(index),
                       live_constraint.coef(index), residual.slack)

    return OPEN
```

这里的 fail-closed 是 Release 下的运行时检查，不是只在 Debug 生效的 `assert`。不能把 missed conflict 直接当分支计数 0，也不能把 missed literal 直接写入 trail；否则会掩盖 watch/Reason 协议错误。

### 9.3 `basic()`

```text
basic(counter):
    require counter.config.inprocess
    require counter.config.basic
    require counter.config.mode == MC
    require !counter.config.ddnnf
    require counter.qhead == counter.trail.size
    require no unprocessed split component

    before := debug state digest
    current_clids := clids(counter.cmpmgr.topComponent())
    live := mapStableSolverIDs(counter.constraints, counter.ca)

    local counts := zero
    equality direction counts := zero

    for row in counter.rows_:
        if row.state == ROOT_TRUE:
            continue
        bind := counter.cmpmgr.rowBind(row.version)
        if bind.clid not in current_clids:
            continue

        cr := unique live[row.solver_id]
        view := makeResidual(row, counter.ca[cr], counter.Level, counter.Pos)
        state := checkResidual(row, counter.ca[cr], view)
        update local OPEN/SATISFIED counts

    validate every selected attached RowVersion was scanned exactly once
    debug require state digest unchanged
    add local counts to Basic statistics
    return SimpResult(UNCHANGED)
```

统计只在整次扫描成功后合并，fail-closed 时不留下“部分扫描已成功”的误导数据。

### 9.4 `run()`

```text
run(counter):
    if !counter.config.basic:
        return UNCHANGED

    result := basic(counter)
    require result.status == UNCHANGED
    return result
```

Gate 1 主循环的非 `UNCHANGED` 运行时保护继续保留。本关不改动传播—简化控制流。

### 9.5 formula 生命周期保护

```text
Solver::reduceDB(keep_formula = false):
    for constraint in constraints:
        if keep_formula and constraint.origin == FORMULA:
            continue
        execute existing reason protection and learned cleanup

PBcounter cleanup sites:
    reduceDB(keep_formula = config.inprocess)
```

三个 PBcounter cleanup 点必须一致；遗漏 SAT 子求解中的任一调用都会在之后留下不可解析 RowVersion。RoundingSat 自己的调用继续使用默认 `false`。

## 10. 调用前后不变量

### 10.1 调用前

- 原生传播刚刚无冲突返回，`qhead == trail.size()`。
- 当前 `topComponent` 存在且没有 unprocessed split component。
- 0E row/group 和 component binding 验证通过。
- Basic 使用的 formula constraint 尚未删除，logical ID 可解析到唯一 live CRef。

### 10.2 正常返回后

以下内容逐项不变：

```text
trail / trail_lim / qhead / decisionLevel
Level / Pos / Reason
constraints / allocator / watches / learnedStack
rows_ / groups_ / RowVersion / EqGroup revision
Component / Decision / cache stack
assigns / unitcls / model counts / limlevel
```

唯一变化是 Basic 累计统计和耗时。返回状态必须为 `UNCHANGED`。

### 10.3 回跳

Residual 不跨调用保存，所以回跳不需要 Basic undo。恢复后的下一轮先由原生 watch 恢复 slack 并传播，再从新的 Level/Pos 重算 view。禁止缓存上一个节点的 gap、U、slack 或 unknown term 指针。

## 11. Reason 生命周期

Gate 2A 不创建、替换或销毁 Reason：

- 非根 native propagation 的 Reason 必须是当时传播该文字的 live RoundingSat CRef。
- 根 unit 的 Reason 为 `CRef_Undef` 是 RoundingSat 明确设计，因为根赋值不会在递归回跳中撤销。
- `reduceDB(keep_formula=true)` 保留所有 0E formula RowVersion；正在作为 Reason 的 learned row 仍由原有 `isReason` 检查保护。
- GC 继续由 RoundingSat 更新 Reason 和 watches；Basic 每次按 logical ID 重新解析，不保存 GC 前 CRef。
- 后续结构简化不能因为 Basic 已有 view 就原地修改 Reason 指向的约束。

## 12. Cache 与分解契约

- Basic 没有结构提交、active mask 或 row revision，legacy cache key 保持原样。
- 当前组件选择只用于减少扫描范围，不改变 ComponentManager 的 occurrence、ClID 或 component stack。
- satisfied 行仍由既有 `splitComponent()` 按当前 assignment 判断是否连接变量；Basic 不提前删除它。
- learned constraints 仍只影响传播/冲突，不加入基础分解图，符合已确定策略。
- `syncAssign()` 仍只在 Basic 正常返回后执行。

因此 Gate 2A 可以继续使用 legacy cache；Cache Bypass 仍在变量阶段完成后单独实施。

## 13. 预算与中断

Basic 是当前组件行项数线性的精确检查，不设置可重置预算，也不返回 `BUDGET`。原因是半条行或半个 equality group 的扫描结果不能用作完整 residual 分类。

该功能默认关闭；性能消融通过 `-no-basic` 完成。若将来证明确需增量 dirty-row 扫描，应在结构版本/undo 已存在后单独设计，不能让预算把 missed conflict 当作 `UNCHANGED`。

## 14. 统计

仅在 `-inprocess -basic` 时增加：

```text
c o basic calls       = N
c o basic rows        = R
c o basic open        = O
c o basic satisfied   = S
```

要求 `R == O + S`。Gate 1 的 `simplification changed/conflict/budget` 仍全部为 0。不要增加 forced/conflict “命中”统计，因为正常运行中它们是传播不动点违规，不是规则收益。

## 15. 测试与验收

### 15.1 数学固定例

先用独立完整赋值 oracle 确认：

| 公式 | 模型数 | 原生 UP / Basic 预期 |
|---|---:|---|
| `5x+2y+z>=4` | 4 | native 固定 `x=1`；Basic 随后看到 satisfied |
| `2x+3y>=6` | 0 | 输入/原生传播判冲突；不得进入 Basic 冒充发现 |
| `2x+3y>=0` | 4 | 0E 分类 ROOT_TRUE；Basic 跳过 |
| `2x+3y>=3` | 2 | native 只固定 `y=1`；Basic 随后看到 satisfied |
| `3~x+2y>=3` | 2 | native 只固定 `~x=1`；验证负文字 |
| `x+y>=1` | 3 | root residual OPEN，不能传播 |
| `x+y=1` | 2 | LOWER/UPPER 均扫描，两个方向均 OPEN |

这些例子必须区分“native UP 做出的赋值”和“Basic 只读观察”，不能把已有传播计入 Basic changed。

### 15.2 非根 Reason

专用测试在 `2x+2y+z>=3` 下做决策 `x=0`，运行原生传播：

- `y=1`、`z=1` 由 native UP 推出；
- 两个非根 Reason 均非 `CRef_Undef`，logical ID 指向该 live formula row；
- Basic 调用前后 Reason/CRef/trail 完全不变；
- 强制 GC 后 Reason 被更新且仍能解析同一 logical ID。

### 15.3 fail-closed 故障测试

测试子类只在测试中故意把一个尚未处理的决策越过 `qhead`，模拟 watch 漏传播：

1. 构造剩余 `a_i>slack`，Basic 必须报 missed implication。
2. 构造 `U<gap`，Basic 必须报 missed conflict。
3. 两者都不能继续到 `syncAssign()/splitComponent()` 或输出模型数。

不在生产 CLI 增加 fault-injection 开关。

### 15.4 Equality、组件和 cache

- 两方向 attached、单方向 ROOT_TRUE、双方向 ROOT_TRUE 三类 equality。
- 两个独立组件，确认子组件扫描不读取 sibling RowVersion。
- `-inprocess -no-basic`、`-inprocess -basic` 与 brute-force oracle 计数一致。
- 除 Basic 新统计和耗时外，decisions/conflicts/components/split/cache/backjump 统计一致。
- 强制 cleanup 后 formula logical IDs、ComponentBind 和 Basic 扫描仍有效。
- `-basic` 未配 `-inprocess` 明确拒绝；PMC/WMC/WPMC/d-DNNF guard 继续通过。

### 15.5 构建

Release、ASan+UBSan、`git diff --check` 全部通过；LeakSanitizer 继续按既有环境限制关闭。主源码与 `pbmc-mod`/`roundingsat-mod` 对应文件逐字同步。

## 16. Gate 2A-I 预计修改面

获批后预计修改：

| 文件 | 修改 |
|---|---|
| `core/simplify.h/.cc` | 增加短 `basic()`、只读扫描和最小统计 |
| `core/Counter.h/.cc` | 单个 friend、统计输出、三个 cleanup 传参 |
| `core/Config.h/.cc`、`core/Main.cc` | `basic` 开关及组合 guard |
| `roundingsat/Solver.hpp/.cpp` | `reduceDB(keep_formula=false)` 窄生命周期参数 |
| `CMakeLists.txt`、`tests/inprocessing/` | Basic 单元/集成/故障/Reason/GC 测试 |
| `pbmc-mod/`、`roundingsat-mod/` | 同步存在的对应文件 |
| `docs/inprocessing/acceptance/02A_basic.md` | 独立验收报告 |

不修改 Component/ComponentManager 的公共数据布局，不新增 residual 源码文件。

## 17. 审核决定点

请在允许 Gate 2A-I 前确认：

1. Basic 复用 native UP，只做 residual view 与传播不动点验证，不建立第二套 enqueue。
2. 正常只返回 `UNCHANGED`；missed conflict/implication 作为内部协议错误 fail-closed。
3. 新开关使用短名称 `-basic`，默认关闭且必须搭配 `-inprocess`。
4. 只扫描当前组件的 0E 基础 RowVersion；learned row 继续只参与传播/冲突。
5. PBMC `-inprocess` 时保留 `Origin::FORMULA` 约束，standalone RoundingSat 与 `-no-inprocess` 行为不变。
6. Residual 使用瞬时精确大整数 view，不建立持久 overlay/undo，也不改变 legacy cache。

用户审核通过本文档前，不修改 C++，不开始 Gate 2A-I。
