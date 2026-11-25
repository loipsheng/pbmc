# 1-P：空简化框架与原生传播接入伪代码

版本：v1.0  
日期：2026-11-25  
状态：**已审核；Gate 1-I 已按本文实现并通过验收**  
适用范围：普通无权精确模型计数（exact MC）

## 1. 本关目标

Gate 1 只建立递归简化的空入口和正确调用位置，不执行任何数学简化：

1. 删除当前搜索中位置错误、函数体为空的旧 `PBcounter::simplify()` 及其专属死状态。
2. 新建唯一的 `core/simplify.h/.cc`，提供短入口 `PBcounter::simplifyNode()` 和 `Simplifier::run()`。
3. 新入口只在原生传播无冲突之后、`syncAssign()` 与组件分解之前调用。
4. 结果使用明确状态，不能继续使用 `bool`。
5. 空实现只能返回 `UNCHANGED`，不得改变 trail、Reason、约束、EqualityGroup、ComponentManager 或 cache。
6. `-no-inprocess` 与 `-inprocess` 的普通 MC 计数和搜索统计除新增空框架统计外保持一致。

本关完成后仍没有 Basic、BICP、CS、GCD、RIR、RPR、RSR、RUP、SC、LP 或 Equality-specific Simplification。

## 2. 明确不做

- 不修改根层 `PBInstance::simplifyRenumberedConstraint()`；它与旧递归空入口不是同一个函数。
- 不触碰 `core/Solver.cc` 中 Glucose 体系的 `simplify()`。
- 不改变 `runPropagationWithoutLearnt()`、冲突分析、选择性学习、回跳、顺序回溯、分支求和或分量乘积。
- 不提交新赋值，不调用 `decide()`、`uncheckedEnqueue()` 或 `propagate()`。
- 不 attach、替换、删除或原地修改任何 RoundingSat 约束。
- 不建立 residual overlay、undo、dirty row、proof dependency 或新 occurrence。
- 不实现 Cache Bypass，也不修改 legacy component cache key、lookup、store、ownership 或淘汰。
- 不为 MC 增加权重、投影、自由变量补偿或 `CountFactor`。
- 不为未来规则预建抽象规则层、adapter、logger、proof manager 或规则专属文件。

## 3. 0E 后的真实源码事实

### 3.1 当前主循环

`core/Counter.cc::count_main()` 当前顺序是：

```text
legacy unitcls -> decide
runPropagationWithoutLearnt(true)
if conflict:
    analyzePBMC2 / ResolveWithoutLit / bjResolve / backtrack
else:
    syncAssign()
    cmpmgr.splitComponent()
    cache lookup / SAT-only component / backtrack

if a projection component remains:
    old late simplify() hook
    reduceDB()
    choose branch variable
```

旧 hook 位于 split/cache/SAT-component 处理之后，且只在
`on_simp && decisionLevel() <= limlevel && cmpmgr.checkfixedDL()` 时调用。函数体始终 `return true`，所以没有数学效果，但 `checkfixedDL()` 会修改自己的计数状态。

### 3.2 旧入口的完整死状态

源码搜索确认以下内容只服务旧空入口：

| 内容 | 当前位置 | Gate 1-I 处理 |
|---|---|---|
| `PBcounter::simplify()` | `Counter.h/.cc` | 删除声明、空定义和过晚调用 |
| `PBcounter::on_simp` | `Counter.h` 与构造函数 | 删除 |
| `-rmvsatcl/-rmvsatclthd` | `Config.cc` | 删除，不复用为新框架开关 |
| `remove_sat_cls/rmvsatcl_threshold` | `ConfigCounter` | 删除 |
| `rmvsatcl*` help 隐藏项 | `utils/Options.cc` | 删除 |
| `simp_dbs/simplify_time` | `Counter.h/.cc` 的旧统计 | 删除，由新窄统计替代 |
| `ComponentManager::checkfixedDL()` | `Component.h` | 删除 |
| `ComponentManager::fixed_level_` | `Component.h` | 删除 |

`-inprocess/-no-inprocess` 已在 0B 建立并由 `ConfigCounter::inprocess` 保存；它是新框架唯一总开关，继续沿用。

### 3.3 传播与 learned row

- `runPropagationWithoutLearnt()` 只传播已 attach 约束，不消费 `learnedStack`。
- 真实传播冲突必须先走现有 `analyzePBMC2()`；分析失败或选择性回跳后的下一轮重新传播。
- `ResolveWithoutLit()` 只有在允许回跳时才 attach learned row；不允许时当前分支计数置 0 并顺序回溯。
- 因此简化入口不能放在 conflict 分支，也不能在本关新造第二套冲突学习路径。

### 3.4 Equality 与 ComponentManager

- 0E 已建立 `rows_`、`groups_`、solver logical ID 和 ComponentBind。
- `cmpmgr.init()` 在 `count_main()` 循环前完成；Gate 1 没有结构改写，因此继续使用 frozen rows 和 legacy cache 是安全的。
- `validateRows(false/true)` 已在初始化前后检查 solver/component binding。
- 空框架不得重解析 comparator，也不得重新展开 equality。

## 4. 最小公共接口

### 4.1 状态

保持用户确定的明确状态，不用 `bool`：

```text
enum class SimpStatus : uint8_t {
    UNCHANGED,
    IMPLIED,
    ROW_CHANGED,
    ROW_REMOVED,
    CONFLICT,
    BUDGET
}

struct SimpResult {
    SimpStatus status = UNCHANGED
}
```

Gate 1 不增加 reason、row、undo 或 proof payload。后续某条规则真正需要相应数据时，再为 `SimpResult` 增加最小字段。

### 4.2 统计

```text
struct SimpStats {
    uint64 calls = 0
    uint64 changed = 0
    uint64 conflicts = 0
    uint64 budgets = 0
    double seconds = 0
}
```

`changed` 统一统计未来的 `IMPLIED/ROW_CHANGED/ROW_REMOVED`，不为尚未实现的规则建立更多计数器。Gate 1 中只有 `calls/seconds` 可以非零。

### 4.3 类与入口

```text
template<class T_data> class PBcounter

template<class T_data>
class Simplifier {
public:
    static SimpResult run(PBcounter<T_data>& counter)
}

PBcounter<T_data>::simplifyNode() -> SimpResult
```

- `simplify.h` 只 forward-declare `PBcounter`，不包含 `Counter.h`，避免循环 include。
- `Counter.h` 包含 `simplify.h`，并保存一个 `SimpStats`。
- `simplify.cc` 包含两者并显式实例化 `mpz_class` 与现有 `mpfr::mpreal` 模板，保证当前构建仍完整；新框架运行时只允许 MC。
- Gate 1 不增加 `friend`，因为空 `run()` 不需要访问 solver 内部状态。Basic 真正需要状态时再选择窄 accessor 或必要的 friend。
- 不在 PBcounter 中保存一个无状态 `Simplifier` 对象；避免为后续预算/undo 过早固定生命周期。

## 5. 空实现伪代码

### 5.1 `Simplifier::run()`

```text
run(counter):
    // Gate 1 唯一合法实现
    return SimpResult(UNCHANGED)
```

它不能读取或修改：

```text
trail, trail_lim, qhead, Level, Pos, Reason
constraints, ca, watches, learnedStack
rows_, groups_, RowVersion, EqGroup revision
assigns, unitcls, cmpmgr stacks, cache entries
npmodels, Decision branch models, limlevel
```

### 5.2 `PBcounter::simplifyNode()`

```text
simplifyNode():
    require config.inprocess
    require config.mode == MC
    require !config.ddnnf

    stats.calls++
    start := cpuTime()

    debug-only: validateRows(component_bound=true)
    result := Simplifier<T_data>::run(*this)

    stats.seconds += cpuTime() - start
    update changed/conflicts/budgets according to result.status
    return result
```

0E 的完整 binding 检查仍在 `count_main()` 初始化时无条件执行；逐节点 `validateRows(true)` 仅用于 Debug，避免 Release 每个节点线性扫描整个公式。

## 6. 主循环接入

Gate 1-I 只把空入口放到唯一正确位置：真实传播无冲突之后、`syncAssign()` 之前。

```text
count_main loop:
    apply legacy unitcls
    confl := runPropagationWithoutLearnt(true)

    if confl:
        execute existing conflict analysis / learning / backjump / backtrack
        continue or finish exactly as before

    // only the no-conflict path reaches here
    if config.inprocess:
        result := simplifyNode()
        if result.status != UNCHANGED:
            throw logic_error("Gate 1 empty simplifier returned a non-empty status")
        // Gate 1 fail-closed: no implemented rule can justify a mutation
        // or a conflict, so this is a release-build runtime check.

    syncAssign()
    splitComponent()
    continue existing cache/SAT/branch/backtrack path
```

本关不提前实现传播—简化不动点。Gate 2 开始出现正式蕴含时，才把
`IMPLIED/ROW_CHANGED/ROW_REMOVED` 接到“重新原生传播”；出现可分析冲突时才为 `CONFLICT` 增加合法 proof payload 和现有冲突路径适配。

### 6.1 为什么不是在 learned conflict 后直接调用

冲突发生后，当前节点已经不再是可简化的无冲突状态。正确流程是：

```text
conflict -> analyze/learn/backjump or branch-zero/backtrack
         -> restored state begins next outer iteration
         -> native propagation
         -> only if no conflict, empty simplifyNode
```

这样不会在已冲突节点重复简化，也不会跳过 learned row 生效后的传播。

## 7. 状态、Reason 与回滚契约

Gate 1 的前后状态必须逐项相同：

```text
decisionLevel, trail size/content, trail_lim, qhead
Level/Pos/Reason
constraint logical IDs/current CRefs and allocator content
watch lists and propagated slack state
rows_/groups_ and component bindings
Component/Decision/cache stack and all model counts
limlevel, last_suc/last_bklevel/last_cr/last_lit
```

因此：

- 不创建假设 trail，也没有临时 Reason。
- 不需要 undo entry；若空调用后任何上述状态变化，测试失败。
- GC/Reason 生命周期继续完全由现有 RoundingSat 管理。
- 后续 BICP 的完整假设回滚不能复用 Gate 1 的空实现冒充完成。

## 8. MC、Equality 与 cache 边界

### 8.1 只允许 exact MC

启动边界继续由 `Main.cc` 执行：

```text
if inprocess and (mode != MC or ddnnf): reject with nonzero exit
```

Gate 1 不开发 PMC/WMC/WPMC 路径，也不删除这些旧代码。`-no-inprocess` 时旧模式保持现状。

### 8.2 Equality-aware

空框架没有规则，所以不会改变 equality 模型集合。它只消费 0E 已建立的 row/group registry，并在 Debug 中验证：

- LOWER/UPPER 未丢失；
- `ROOT_TRUE` 没有伪 binding；
- attached 方向仍能解析 solver/component binding；
- revision 仍为 0。

### 8.3 cache 模式

Gate 1 的 cache mode 固定为：

```text
LEGACY
```

空入口不生成 cache key、不清空 cache、不改变 lookup/store 顺序。Cache Bypass 仍属于独立的 2D 关卡。

## 9. 配置清理

Gate 1-I 采用彻底删除旧选项，而不是兼容保留：

```text
delete -rmvsatcl / -no-rmvsatcl
delete -rmvsatclthd=<n>
keep   -inprocess / -no-inprocess
```

删除后传入旧选项应由现有 strict option parser 报 `Unknown flag` 并非零退出。这样不会让“删除满足子句”的旧名字误导为新递归简化总开关。

## 10. 统计输出

仅当 `config.inprocess` 时，在普通统计中输出：

```text
c o simplification calls    = N
c o simplification changed  = 0
c o simplification conflict = 0
c o simplification budget   = 0
c o simplification time     = T
c o simplification cache    = legacy
```

Gate 1 的 `changed/conflict/budget` 非零属于错误。不要输出规则级命中、dirty rows、proof 数或预算细分。

## 11. 文件修改面

Gate 1-I 获批后预计只修改：

| 文件 | 修改 |
|---|---|
| `core/simplify.h/.cc` | 新建最小状态、统计与空 `run()` |
| `core/Counter.h/.cc` | 删除旧入口/状态；增加 `simplifyNode()`；移动到传播后调用；输出窄统计 |
| `core/Config.h/.cc` | 删除旧 `rmvsatcl` 配置，保留 `inprocess` |
| `core/Component.h` | 删除仅服务旧 hook 的 `fixed_level_` 与 `checkfixedDL()` |
| `utils/Options.cc` | 删除旧选项的隐藏名单项 |
| `CMakeLists.txt` | 显式加入 `core/simplify.cc` |
| `tests/inprocessing/` | Gate 1 空框架、旧 flag 拒绝和状态/统计差分测试 |
| `pbmc-mod/` | 稳定后同步所有存在的对应文件和新 `simplify.h/.cc` |
| `docs/inprocessing/acceptance/01_skeleton.md` | 实现后的独立验收报告 |

不修改 `roundingsat/`、`ComponentManager.cc`、cache 数据结构或 0E identity 布局。

## 12. 测试与验收

### 12.1 编译与静态检查

1. Release 构建。
2. ASan + UBSan，继续关闭当前环境不兼容的 LeakSanitizer。
3. `git diff --check`。
4. `rg` 确认旧 `PBcounter::simplify/on_simp/rmvsatcl/checkfixedDL/fixed_level_` 在主源码和持久副本中全部消失。
5. `rg` 确认根 `PBInstance::simplifyRenumberedConstraint()` 和 Glucose `Solver::simplify()` 仍存在且未改。

### 12.2 普通 MC oracle

对 Gate 0B/0E 全部固定例和随机例运行：

```text
-no-inprocess -> exact count A
-inprocess    -> exact count B
oracle        -> exact count O
require A == B == O
```

继续覆盖 `>=/<=/=`、正负文字、`ROOT_TRUE` equality、根 UNSAT、两个组件和 backjump。

### 12.3 搜索统计差分

对确定性固定用例比较下列旧统计，排除耗时字段：

```text
conflicts, decisions, components, split
cache lookup/hit/reduce
SAT calls/starts
backjumps(with/without/fallback), chronological backs
```

`-no-inprocess` 与空 `-inprocess` 必须一致。开启时另要求：

```text
simp.calls > 0
simp.changed == simp.conflicts == simp.budgets == 0
cache mode == legacy
```

根层直接 UNSAT 可以不进入 `count_main()`，因此允许 calls 为 0。

### 12.4 故障与模式测试

1. `-inprocess -mode=1/2/3` 和 `-inprocess -ddnnf` 继续拒绝。
2. `-rmvsatcl`、`-no-rmvsatcl`、`-rmvsatclthd=2` 均作为未知参数拒绝。
3. 测试专用 fault injection 让空 `run()` 返回非 `UNCHANGED`，主循环必须 fail-closed，不能继续分解或给出计数；若为此需要生产 test hook，则改用直接单元测试 `SimpResult`，不在生产代码保留 hook。
4. equality metadata 与强制 GC 测试继续通过。

### 12.5 恢复路径

固定 backjump 用例继续实际覆盖：

- 普通 chronological backtrack；
- `ResolveWithoutLit() -> bjResolve()`；
- `-no-bj`；
- sibling branch 和 cache pollution cleanup。

空框架不能改变这些路径的次数或精确计数。

## 13. 实现顺序

获批后的 Gate 1-I 按以下小步执行：

1. 新建 `simplify.h/.cc` 与编译目标，空 `run()` 单元编译通过。
2. 增加 `simplifyNode()` 和窄统计，但尚不接主循环。
3. 删除旧 `simplify()`、旧配置、旧统计及 `fixed_level_` 死状态。
4. 在传播无冲突后接入空入口，并以运行时 fail-closed 检查保证只返回 `UNCHANGED`。
5. 扩展 Gate 0 测试做 on/off 计数与搜索统计差分。
6. 运行 Release、ASan/UBSan，修复本关引入的问题。
7. 同步 `pbmc-mod/`，提交 `acceptance/01_skeleton.md` 后停止。

## 14. 审核决定点

请在允许 Gate 1-I 前确认：

1. 旧 `rmvsatcl` 两个选项彻底删除，不保留 deprecated alias。
2. `fixed_level_/checkfixedDL()` 随旧 hook 删除，因为源码确认没有其他调用者。
3. Gate 1 只允许 `UNCHANGED`；其他状态虽定义但在对应规则和 proof 协议完成前 fail-closed。
4. 空框架使用现有 legacy cache，不把 Cache Bypass 提前合入。
5. Gate 1 不增加 `friend`、overlay、undo 或通用 proof 层；Basic 需要时再增加最小接口。

本文五项决定已由用户确认；实现结果见 `docs/inprocessing/acceptance/01_skeleton.md`。
