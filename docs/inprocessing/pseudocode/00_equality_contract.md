# 0E-P：Operator / Equality Contract 伪代码

版本：v1.0-draft  
日期：2026-11-25  
状态：**已获用户继续指令并完成 0E-I；实现与验收见 `acceptance/00E_equality.md`**  
适用范围：普通无权精确模型计数（exact MC）

## 1. 目标与不做事项

### 1.1 本关目标

1. 精确定义输入 `>=`、`<=`、`=` 到 canonical `>=` 行的映射。
2. 保留一个原始 equality 的两个方向之间的稳定关系，不能再依靠 RoundingSat `constraints` 中的相邻位置猜配。
3. 严格区分原始行 ID、`EqualityGroupID`、`RowVersionID`、RoundingSat 逻辑 `ID`、solver allocator 的 `rs::CRef`、ComponentManager allocator 的 `rs::CRef` 与本地 `ClID`。
4. 定义 equality 两方向的原子发布、检查、未来 commit/undo 和 cache identity 契约。
5. 用原始输入公式的完整模型集合 oracle 验证转换，不能拿转换后的公式与自身比较。

### 1.2 本关明确不做

- 不创建 `core/simplify.h/.cc`，不删除旧空 `PBcounter::simplify()`；这些属于关卡 1。
- 不实现 Basic、BICP、CS、GCD、RIR、RPR、RSR、RUP、SC、LP 或 Equality-specific Simplification。
- 不修改 `count_main()` 的传播、冲突分析、回跳、分解、缓存、分支求和流程。
- 不实现 residual overlay、residual cache 或 row replacement；这里只固定它们以后必须遵守的 equality 契约。
- 不实现变量代换、Gaussian elimination、变量消元或自由变量计数修正。
- 不修复 PMC/WMC/WPMC，也不为它们增加新路径；`-inprocess` 仍只允许普通 MC 且关闭 d-DNNF。
- 不重写 RoundingSat 的 PB 规范化和传播器；复用 `CeArb::addLhs()`、`ConstrExp::invert()`、`Solver::addConstraint()` 和 `postProcess()`。

## 2. 已核实的源码事实

1. `core/pbconstraint.h` 目前只有 `bool equals`；普通行输出时一律写成 `>=`。
2. `core/Instance.cc::load()` 在 0B 后只接受 `>=`、`=`，对 `<=` fail-fast。
3. `core/Main.cc::main_pbmc()` 当前执行：PBInstance 读取 → 重编号 → 负系数改互补文字 → 旧根简化 → 再输出 OPB 字符串 → RoundingSat 重新解析。
4. `roundingsat/parsing.cpp::opb_read()` 对 equality 先调用一次 `addConstraint()`，随后 `input->invert()` 再调用一次；两个返回的逻辑 ID 都被丢弃。
5. `roundingsat/Solver.cpp::addInputConstraint()` 会运行 `postProcess()`、attach、根传播；恒真方向返回 `ID_Undef`，冲突方向返回 `ID_Unsat`，否则返回 attached `Constr::id`。
6. solver `rs::CRef` 是 allocator offset；`garbage_collect()` 会移动对象并更新 solver 自己的 constraints、watch、Reason 和 external CRef。因此 CRef 不是稳定身份。
7. `ComponentManager::init()` 按 solver `constraints` 顺序复制行到自己的 allocator，保留 `Constr::id`，但重新产生 component CRef；该向量下标才是本地 `ClID`。
8. ComponentManager 的 frozen occurrence、ClID 和 legacy cache 在 `count_main()` 开头只初始化一次。0E 不允许在递归中改变这些行。
9. `PBconstraint::Int` 当前是 `long`，parser 使用 `stol`；canonicalization 的取负、RHS 调整与系数求和都必须检查溢出，不能依赖有符号整数回绕。
10. 旧根预处理的部分单行/探测判断使用 `double` 比值。0E-I 不增加新规则，但同一规则的 `0`、`1`、`>1` 判断必须改成等价的精确整数比较，或对不能安全处理的输入 fail-fast；不得让 operator contract 建立在浮点舍入上。

## 3. 规范数学表示

文字 `l` 的取值为 0/1，互补文字记为 `~l = 1-l`。canonical 行统一表示为：

```text
P = sum(a_i * l_i)
a_i > 0
P >= gap
```

### 3.1 有符号系数转正

对任一负系数 `c=-a`，`a>0`：

```text
c*l = -a*l = a*(1-l)-a = a*~l-a
```

所以将 `c*l` 替换为 `a*~l`，并执行 `rhs := rhs+a`，对 `>=`、`<=`、`=` 都保持完全相同的模型集合。零系数删除；重复文字和同一变量的互补文字交给 RoundingSat `addLhs()` 的精确合并逻辑，不能用浮点聚合。

### 3.2 三种 comparator

设正系数化后：

```text
P = sum(a_i*l_i),  A = sum(a_i)
```

| 原关系 | canonical 方向 | 精确依据 | 初始 commit / 后续 undo | 原式 oracle |
|---|---|---|---|---|
| `P >= gap` | 单行 `P >= gap` | 恒等映射 | 单 RowVersion 发布；以后按单行 undo | 直接判定 `P>=gap` |
| `P <= gap` | 单行 `sum(a_i*~l_i) >= A-gap` | `A-P>=A-gap` | canonical 单 RowVersion 发布；以后按单行 undo | 仍直接判定原 `P<=gap` |
| `P = gap` | `EqualityGroup{lower: P>=gap, upper: sum(a_i*~l_i)>=A-gap}` | `P=gap iff P>=gap and P<=gap` | 两方向原子发布；以后一个 GroupUndo 同时恢复两边 | 仍直接判定原 `P=gap` |

实现允许在正系数化之前把原 `<=` 整行乘以 `-1`，再走现有负系数转互补文字流程。这与表中公式完全等价，但所有取负和 RHS 调整必须使用 checked arithmetic。

### 3.3 常量行

规范化、合并重复项后若没有文字：

```text
0 >= gap 且 gap <= 0  -> ROOT_TRUE
0 >= gap 且 gap > 0   -> UNSAT
```

`0 = 0` 的两个方向都为 `ROOT_TRUE`；`0 = k (k!=0)` 至少一个方向为 `UNSAT`。不能沿用当前“空行一律 unsat”的行为。

## 4. 身份与数据契约

名称保持简短，0E-I 只增加必要元数据，不建立通用框架层。

```text
SourceRowID       原输入行的稳定 1-based ID；重编号和等价 canonicalization 不改变它
EqualityGroupID   只给原始 equality 分配；与 SourceRowID 是不同类型/命名空间
RowVersionID      一份不可变 canonical 行内容的身份；初始版本 revision=0
rs::ID            RoundingSat Constr::id；跨 solver GC 稳定
solver rs::CRef   solver ConstraintAllocator 当前 offset；GC 后可能变化
component rs::CRef ComponentManager::pbca_ 的 offset；绝不能交给 solver
ClID              ComponentManager::constraints_ 的 0-based 本地下标
```

建议的最小逻辑结构如下；实际 C++ 名称可保持同样简短：

```text
enum RowSide  { SINGLE, LOWER, UPPER }
enum RowState { ATTACHED, ROOT_TRUE }

RowInfo {
    source: SourceRowID
    group: EqualityGroupID or NO_GROUP
    side: RowSide
    version: RowVersionID
    state: RowState
    solver_id: rs::ID or ID_Undef
    // solver CRef 通过 solver_id 在当前 constraints/allocator 中解析，不能作为稳定键
    canonical_content: immutable exact row
}

ComponentBind {                 // 由 ComponentManager 持有，不能混进 solver binding
    version: RowVersionID
    clid: ClID
    cref: component allocator rs::CRef
}

EqGroup {
    id: EqualityGroupID
    source: SourceRowID
    lower: RowVersionID
    upper: RowVersionID
    revision: 0 initially
}
```

`ROOT_TRUE` 仍必须有方向记录和 immutable canonical content，但没有 solver CRef/ID，也没有 ComponentBind。它不是“方向丢失”。`ATTACHED` 必须同时有唯一 solver logical ID；ComponentManager 初始化完成后还必须有唯一 ComponentBind。

`solverRef(version)` 每次按稳定 `solver_id` 解析当前 CRef，或使用明确随 GC 刷新的索引。禁止把旧 CRef 放入 RowVersion 后永久使用。解析必须得到恰好一个未删除对象并验证 `ca[cr].id == solver_id`。

## 5. 调用前后不变量

### 5.1 调用前

- 只处理普通 MC 输入；变量仍采用 PBInstance/RoundingSat 的 1-based 正负整数文字。
- 输入行尚未进入递归搜索，RoundingSat `decisionLevel()==0`。
- 对 `<=` 的所有算术尚未发生有符号溢出；否则输入已明确拒绝。
- EqualityGroup staging 对当前 source row 为空，不允许重复注册。

### 5.2 成功后

- 每个输入 `>=`/`<=` 对应一个 `SINGLE` canonical `>=` 方向。
- 每个未被旧根预处理完全消去的输入 equality 恰有一个 group、一个 LOWER 记录和一个 UPPER 记录；两个版本不同，即使内容相同或都是 `ROOT_TRUE`。
- 两个 active 方向有不同 solver logical ID、不同当前 solver CRef；ComponentManager 初始化后有不同 component ClID 和 component CRef。
- group 只有在两个方向均完成分类后才对外可见；不存在只发布 LOWER 或只发布 UPPER 的状态。
- `validateGroup()` 在进入 `count_main()` 前通过；任何缺边、重复 side、错误 ID 类型或 binding 不一致都 fail-fast。
- 空 overlay 下 group revision 恒为 0；0E 不产生 undo entry，不改变 legacy cache key，也不改变原有 `>=`/`=` 的计数。

### 5.3 失败后

- `INVALID/OVERFLOW`：打印包含 source row 和 comparator 的明确错误并以非零状态退出；不能继续按 `>=` 猜测。
- `UNSAT`：按根公式无解处理；不进入 `count_main()`，不把已 staging 的半个 group 发布给 component/cache。
- 内部异常或第二方向 attach 异常：整个输入过程失败退出。现有 RoundingSat 没有公式行的通用事务 detach，因此不能假装安全回滚后继续搜索。

## 6. 返回状态

本关不是 Simplifier 规则，不复用未来 `SimpResult`。使用以下窄状态即可：

```text
CanonCode = OK | INVALID | OVERFLOW
AddCode   = ATTACHED | ROOT_TRUE | UNSAT | ERROR
GroupCode = PUBLISHED | UNSAT | ERROR
```

canonicalization 和初始 group 建立是必要输入语义，不受可选简化预算控制，因此没有 `BUDGET` 或 `UNKNOWN`。任何无法精确完成的情况都必须 fail-fast，不能 no-change 后继续误解输入。

### 6.1 预算与 UNKNOWN

本关的工作量与输入项数线性相关，是读取公式不可省略的一部分；不读取完整 comparator 或 equality 的第二方向就不能安全开始搜索。因此 0E 不接受预算截断，也不产生 UNKNOWN。资源耗尽属于明确 ERROR/进程失败，不得把未完成的 group 当作普通行继续。

## 7. 逐步伪代码

### 7.1 解析并规范 comparator

```text
parseLine(tokens, source_id):
    require exactly one comparator in {>=, <=, =}
    parse every coefficient, literal and rhs with checked conversion
    reject literal 0, missing rhs, extra tokens and unsupported comparator

    rel := parsed comparator                 // parser-local，不要求扩充 PBconstraint 公共布局
    row := PBconstraint(terms, rhs, equals=(rel == EQ), constraintId=source_id)

    if rel == LE:
        // raw L<=b  <=>  -L>=-b
        for each coefficient c in row:
            c := checkedNeg(c)
        row.rhs := checkedNeg(row.rhs)
        row.equals := false

    return (OK, row)
```

在 0E-I 通过前，现有 `<=` fail-fast 保持不变。实现必须一次性切换为上述精确路径，不能同时保留“未知 comparator 当 `>=`”的 fallback。

### 7.2 沿用重编号并精确正系数化

```text
canonSigns(row):
    out := empty row preserving source_id and equals
    for (c, l) in row:
        if c == 0: continue
        if c > 0:
            out.add(c, l)
        else:
            a := checkedNeg(c)
            out.add(a, complement(l))
            out.rhs := checkedAdd(out.rhs, a)

    // add/merge 使用 exact integer；不要在 PBInstance 中引入第二套浮点 canonicalizer
    return out
```

若继续保留 `PBconstraint::Int=long`，所有中间计算先在 `mpz_class` 或等价宽类型中完成，确认结果落入 `long` 后再写回；超界返回 `OVERFLOW`。0E-I 不顺带迁移全项目整数类型。

旧根预处理里 `rhs/coef` 的 `0`、`1`、`>1` 判断改用交叉乘法/整数比较表达同一规则，例如 `rhs==0`、`rhs==coef`、`rhs>coef`；不得先转 `double`。

### 7.3 构造 RoundingSat detached 行

```text
makeCe(row):
    ce := cePools.takeArb()
    ce.reset()
    for (a, l) in row:
        ce.addLhs(exact(a), rsLiteral(l))
    ce.addRhs(exact(row.rhs))
    return ce
```

`rsLiteral()` 保持 PBInstance/RoundingSat 的 1-based 正负整数；ComponentManager 的 0-based变量编号不在这里使用。

### 7.4 提交单个方向

```text
addDir(source, group, side, detached_ce):
    staged_content := exactCanonicalCopy(detached_ce)
    before := solver.constraints.size()
    (input_id, processed_id) := solver.addConstraint(detached_ce, FORMULA)

    if processed_id == ID_Unsat:
        return (UNSAT, no RowInfo)

    if processed_id == ID_Undef:
        // postProcess 证明当前根状态下恒真
        return (ROOT_TRUE,
                RowInfo(source, group, side, freshVersion(), ROOT_TRUE,
                        ID_Undef, staged_content))

    require solver.constraints.size() == before + 1
    cr := solver.constraints.back()
    require solver.ca[cr].id == processed_id
    actual_content := solver.ca[cr].toExpanded(...).exactCanonicalCopy()

    return (ATTACHED,
            RowInfo(source, group, side, freshVersion(), ATTACHED,
                    processed_id, actual_content))
```

这里记录的是 `processed_id`，不是 logger 的 `input_id`，也不是 `cr.ofs`。`actual_content` 取自真正 attached 后的行，保证 RowVersion 与 RoundingSat saturation/GCD/cardinality normalization 后的内容一致。

### 7.5 建立普通行

```text
addSingle(row):
    ce := makeCe(row)
    result := addDir(row.source, NO_GROUP, SINGLE, ce)
    if result == UNSAT: return UNSAT
    publish RowInfo
    return OK
```

原始 `<=` 在 7.1 已变为等价 `>=`，所以这里不再区分 comparator，也不在后续简化模块中写第二套 `<=` 逻辑。

### 7.6 建立 EqualityGroup

```text
addEq(row):
    require row.equals
    gid := freshGroupId()
    stage := empty unpublished EqGroup(gid, row.source)

    lower_ce := makeCe(row)                 // L >= b
    upper_ce := lower_ce.clone(cePools)
    upper_ce.invert()                       // -L >= -b，即 L <= b

    // 两个 detached 内容先完整构造；任何 checked arithmetic 失败都尚未发布
    lower := addDir(row.source, gid, LOWER, lower_ce)
    if lower.code == UNSAT:
        return UNSAT

    upper := addDir(row.source, gid, UPPER, upper_ce)
    if upper.code == UNSAT:
        return UNSAT

    stage.lower := lower.version
    stage.upper := upper.version
    require validateStagedGroup(stage, lower.info, upper.info)

    // 唯一可见发布点
    publish lower.info, upper.info and stage together
    return PUBLISHED
```

RoundingSat 每次 `addConstraint()` 都可能做根传播，因此 UPPER 可能在 LOWER 推出的根赋值下成为 `ROOT_TRUE`。这不是丢边：UPPER 的版本和状态仍存在。若第二方向证明根冲突，整个原公式 UNSAT；已 attach 的 LOWER 无需假回滚，因为搜索不会启动，但 group 元数据不得半发布。

### 7.7 导入完整 PBInstance

```text
addRows(pbins):
    clear row/group registry for this instance

    for row in pbins.renumberedClauses in stable order:
        result := row.equals ? addEq(row) : addSingle(row)
        if result == UNSAT: return UNSAT
        if result == ERROR: return ERROR

    // getRenumberedOpb() 当前还会追加旧根预处理推导的 unit；直接导入时不能漏掉
    for (variable, value) in pbins.inferredAssignment sorted by variable:
        lit := value ? variable : -variable
        result := solver.addUnitConstraint(lit, FORMULA)
        if result.processed_id == ID_Unsat: return UNSAT
        // 这些是 level-0 派生 unit，当前 PBInstance 没保留唯一 source proof；不伪造 EqGroup

    require every published group passes validateGroup()
    return OK
```

旧根预处理已经完全消去的 equality 不再有 attached group；其效果由上述根 unit/UNSAT 状态承载。0E 不为缺失的旧 proof 伪造 source dependency。后续递归简化开始后产生的每个推导则必须有正式 Reason，这属于对应规则关卡。

### 7.8 绑定 ComponentManager 的 frozen 行

```text
bindRows():
    // 在 cmpmgr.init() 复制完所有 solver formula rows后执行
    seen_solver_ids := empty set

    for clid in [0, cmpmgr.constraints_.size()):
        component_cr := cmpmgr.constraints_[clid]
        solver_id := cmpmgr.pbca_[component_cr].id
        require solver_id not in seen_solver_ids
        seen_solver_ids.insert(solver_id)

        if RowInfo exists for solver_id:
            require RowInfo.state == ATTACHED
            add ComponentBind(RowInfo.version, clid, component_cr)

    for each RowInfo r:
        if r.state == ATTACHED:
            require r.solver_id is bound exactly once
            require exactly one ComponentBind exists for r.version
            bind := ComponentBind(r.version)
            require cmpmgr.pbca_[bind.cref].id == r.solver_id
        else:
            require r.solver_id == ID_Undef
            require no ComponentBind exists for r.version

    for each EqGroup g:
        require validateGroup(g)
```

学习约束不在初始 RowInfo 表中，继续保持 solver-only。0E 不改变“全局逻辑后果的 learned row 不进入 frozen component graph”的现有策略。

### 7.9 Group 完整性检查

```text
validateGroup(g):
    lower := version(g.lower)
    upper := version(g.upper)

    require lower.group == g.id and upper.group == g.id
    require lower.source == g.source and upper.source == g.source
    require lower.side == LOWER and upper.side == UPPER
    require lower.version != upper.version
    require no third active direction uses g.id

    for r in {lower, upper}:
        if r.state == ATTACHED:
            cr := solverRef(r.version)       // 按 solver_id 解析当前 CRef
            require cr != CRef_Undef
            require solver.ca[cr].id == r.solver_id
            if component manager initialized:
                bind := ComponentBind(r.version)
                require bind exists and cmpmgr.pbca_[bind.cref].id == r.solver_id
        else:
            require r.state == ROOT_TRUE
            require no solver/component binding

    if lower.state == ATTACHED and upper.state == ATTACHED:
        require lower/upper solver IDs and current solver CRefs are distinct
        require lower/upper ComponentBind ClIDs and CRefs are distinct
    return true
```

测试中的“故意漏掉一个方向”通过直接构造不完整 staging/registry 调用 validator 实现，不增加可被正常用户开启的危险 CLI。

## 8. EqualityGroup 的未来事务与 Undo 契约

0E-I 只建立 revision 0，不执行递归 rewrite。但后续规则必须复用下列协议，不能各自发明半组更新：

```text
stageGroup(g, lower_candidate, upper_candidate, proof, level):
    require validateGroup(g)
    require no open transaction for g
    snapshot := {g.revision, g.lower, g.upper, both active states}

    verify proof scope and all dependency versions
    build immutable candidate versions for both sides
    // 未变化的一边也显式携带旧 version，表示参与同一 group transaction
    validate solver attach/Reason pins/overlay and cache staging for both

    if any validation fails:
        discard candidates and staging
        return REJECTED                       // 可见状态完全不变

    push one GroupUndo(snapshot, level)
    atomically publish both current versions/active states
    g.revision++
    mark topology/cache dirty once for the group
    return COMMITTED

undoGroup(entry):
    require current group revision == entry.old_revision + 1
    retire newly published versions without destroying Reason-pinned objects
    atomically restore both old versions/active states
    restore dependency and cache staging
    set revision := entry.old_revision
    require validateGroup(g)
```

“只改 LOWER”在物理内容上允许，但事务中 UPPER 必须以 unchanged version 一起发布和记录。删除整个 equality 必须分别证明两个方向可删；只证明一个方向冗余，只能将该方向标为有证明依赖的 inactive，不能把 group 标为 removed。

## 9. Reason 生命周期

- 0E 的初始 formula 行不原地修改、不删除；现有 RoundingSat watch、slack 和 Reason 语义保持不变。
- `Reason[v]` 保存 solver allocator 的 CRef。RowInfo 的稳定身份是 `solver_id + immutable RowVersionID`，不是 CRef offset。
- GC 后必须用 `solverRef()` 获得当前 CRef；任何缓存的旧 offset 都不可使用。
- 后续替换行时创建新 RowVersion/新 attached 对象。旧对象若仍被任何 `Reason[v]` 或 proof dependency 引用，状态只能进入 `RETIRED_REASON`，保持不可变；确认无引用后才可交给 RoundingSat 合法回收。
- equality 两方向分别检查 Reason pin，但 retirement/publish/undo 按 group 原子执行。不得因为 LOWER 未被引用就销毁仍被 UPPER Reason 引用的 group 状态。
- `ROOT_TRUE` 方向没有 solver Reason；其 canonical version 仍用于 group 完整性和以后 cache identity。

## 10. 临时假设与 rollback

本关没有 BICP/RUP 临时假设，不创建 assumption trail，也不调用递归 rollback。所有 operator canonicalization 在 decision level 0 完成。

未来 probe 必须把同一 group 的两个活动方向都放进隔离公式；probe marker 需要恢复 trail、Reason、watch/slack、overlay、group revision、dependency 和 cache staging。该要求只在此处固定契约，不在 0E 实现 probe。

## 11. Overlay、依赖与 Cache 契约

### 11.1 0E/Legacy 阶段

- ResidualOverlay 尚不存在；所有 group revision 为 0，active attached 行已经分别成为 frozen ClID。
- Legacy cache 仍使用现有变量、ClID 和 gap；因为 0E 没有递归结构改写，两个 equality 方向通过各自 ClID 参与现有分解和 key。
- `ROOT_TRUE` 方向没有 ClID，不进入分解；它的缺席由显式 RowState 解释，而不是被当作 registry 损坏。
- 0E 不改变 cache ownership、father/descendant pollution 或回跳协议。

### 11.2 后续 Residual 模式必须满足

EqualityGroup 的数学 cache identity 序列化：

```text
GROUP_MARKER
canonical(lower current content), lower active state
canonical(upper current content), upper active state
END_GROUP
```

group ID、RowVersionID 和 revision 只负责运行时定位，**不进入数学 key**；相同 residual 公式应得到相同 key。hash 后仍比较完整内容。lookup 期间看见 open/half group transaction 必须拒绝 lookup/store，不能用旧一边加新一边构造 key。

派生版本记录精确 helper/proof dependency。dependency 失活、版本变化或作用域退出时，相关版本必须先撤销或重新证明；不能只更新 cache key 掩盖失效证明。

## 12. 精确性证明

### 12.1 负系数

对每个布尔文字 `l`：`-a*l = a*~l-a`。将 RHS 同时增加 `a` 后，两边真值完全相同。因此逐项处理保持每个完整赋值的真假，不只是保持可满足性或模型数。

### 12.2 `<=` 转 `>=`

对 `P=sum(a_i*l_i)` 与 `A=sum(a_i)`：

```text
P <= gap
<=> A-P >= A-gap
<=> sum(a_i*(1-l_i)) >= A-gap
<=> sum(a_i*~l_i) >= A-gap
```

每一步对每个完整布尔赋值等价。

### 12.3 equality 双方向

```text
P = gap  <=>  (P >= gap) and (P <= gap)
```

再由 12.2 将上界精确变为 canonical `>=`。所以只有两个方向同时存在或被显式证明 `ROOT_TRUE` 时，group 才表示原 equality。只保留下界或上界会严格扩大模型集合。

### 12.4 原子发布

可见状态只在两个方向均完成分类和验证后发布。任何读者要么看到旧完整 group，要么看到新完整 group；不会看到一半。因此传播公式、分解图、proof dependency 与 cache identity 始终对应同一个完整公式版本。

## 13. 正例、反例与完整模型集合

变量顺序均为 `(x,y)`：

| 原式 | canonical 表示 | 完整模型集合 | 计数 |
|---|---|---|---:|
| `x+y>=1` | `x+y>=1` | `{01,10,11}` | 3 |
| `x+y<=1` | `~x+~y>=1` | `{00,01,10}` | 3 |
| `x+y=1` | `{x+y>=1, ~x+~y>=1}` | `{01,10}` | 2 |

有符号与负文字例：

```text
-2*x + 3*~y <= 1
```

精确 canonical 行为 `2*x + 3*y >= 2`，完整模型集合 `{01,10,11}`。不得把原行直接去掉 comparator 后按 `>=` 解释；那会得到不同集合。

对应 equality：

```text
-2*x + 3*~y = 1
```

正系数形式为 `2*~x+3*~y=3`，两个方向为：

```text
2*~x + 3*~y >= 3
2*x  + 3*y  >= 2
```

交集只有 `{10}`。

必须覆盖的反例：

1. 只保留 `x+y>=1` 作为 `x+y=1`：错误增加模型 `11`。
2. 只保留 `~x+~y>=1`：错误增加模型 `00`。
3. `x+y=0`：LOWER 可为 `ROOT_TRUE`，UPPER active；这不是 missing lower。
4. `x+~x=1`：两个方向都可为 `ROOT_TRUE`，仍应保留完整 group 记录，模型 `{0,1}`。
5. `x+y=3`：根 UNSAT；不能先发布 LOWER group 再进入搜索。
6. 两条内容相同的 equality：可以有不同 group/source ID；运行时身份不能错误合并。未来数学 cache key可按完整 residual 内容规范化，但不能混淆 proof ownership。
7. `LONG_MIN` 取负或 `A-gap` 超出 `long`：明确 `OVERFLOW`，不能回绕。

## 14. Oracle、故障测试与验收矩阵

### 14.1 独立 oracle

继续使用 `tests/inprocessing/mc_oracle.py` 直接解析**原始** `>=/<=/=` 文件并枚举所有完整赋值。每个用例同时比较模型集合；PBMC 输出只能比较计数时，测试脚本中的 expected model set 仍由 oracle 固定，不能由 PBMC canonical row生成。

### 14.2 固定测试

1. `x+y>=1 / <=1 / =1` 分别为 3/3/2。
2. 正负系数、正负文字、重复文字、互补文字与零系数。
3. `x+y=0`、`x+y=2`、`x+~x=1`、常量真/假、根 UNSAT。
4. checked arithmetic 边界：可表示最大值通过，取负/求和溢出 fail-fast。
5. 同一公式在 `-no-inprocess` 与 `-inprocess` 下普通 MC 相同；mode 1/2/3 与 d-DNNF 的 `-inprocess` 拒绝保持通过。
6. 固定种子随机生成小型三种 comparator OPB，逐完整赋值与 oracle 对照。

### 14.3 身份与故障注入

1. 每个 nontrivial equality 恰有 LOWER/UPPER 两个不同 RowVersion。
2. ATTACHED 双方向的 solver logical ID、当前 solver CRef、component CRef、ClID 两两不同且映射内容正确。
3. 强制调用 GC 后 `solverRef(version)` 仍解析到相同 logical ID 和等价内容；旧 offset 不被使用。
4. 测试构造缺 LOWER、缺 UPPER、重复 LOWER、side/group 不匹配，`validateGroup()` 必须失败。
5. 模拟第二方向 `ROOT_TRUE`、UNSAT 和 ERROR：前者完整发布，后两者不产生半个可见 group。
6. 模拟 group cache staging 只收到一边：lookup/store 拒绝；legacy 正常路径不受影响。

### 14.4 搜索恢复

在含 equality 的实例上分别覆盖：

- 普通 chronological backtrack；
- `ResolveWithoutLit() -> bjResolve()` 非顺序回跳；
- cache staging/污染清理；
- sibling branch。

每次恢复后 group revision 仍为 0、两方向映射完整、精确计数与 oracle 相同。0E 本身没有递归 group undo，因此任何 revision 变化都是测试失败。

### 14.5 工具验收

- Release build + CTest；
- ASan + UBSan，关闭当前环境不兼容的 LeakSanitizer；
- `git diff --check`；
- 主源码与存在对应文件的 `pbmc-mod/`/`roundingsat-mod/` 持久副本一致。

## 15. 0E-I 预计最小接口与文件

以下只是审核后的预计修改面，本次 0E-P 不执行：

| 文件 | 预计最小修改 |
|---|---|
| `core/pbconstraint.h` | 必要的短 ID/side/state/RowInfo/EqGroup 逻辑元数据；不保存 allocator-local CRef，不建立规则类层次 |
| `core/Instance.h/.cc` | 精确识别三种 comparator；`<=` checked canonicalization；保留 source row；精确替代旧规则中的浮点 0/1 判断 |
| `core/Counter.h/.cc` | 短入口 `addRows()`、`addDir()`、`addEq()`、`solverRef()` 与 group registry；直接复用 RoundingSat add/invert，记录返回 ID |
| `core/Main.cc` | 用可追踪的 `addRows()` 取代 PBMC 内部“输出 OPB 后丢身份地重解析”的路径；其余主流程不动 |
| `core/Component.h`, `core/ComponentManager.cc` | 在现有复制循环中按 solver logical ID 建立独立 ComponentBind（RowVersion→本地 ClID/component CRef）；增加只读校验/accessor |
| `tests/inprocessing/` | 三 comparator 模型集合、随机 oracle、group fault、GC、backjump 与 sanitizer 测试 |
| `pbmc-mod/` | 稳定后同步存在的对应文件 |

RoundingSat 的 `parsing.cpp` 不需要增加 PBMC-specific group 逻辑；`ConstrExp::invert()`、`Solver::addConstraint()`、`Constr::id` 和 propagation 直接复用。若实现发现必须修改 RoundingSat 数学语义或公共 allocator 数据布局，立即暂停 0E-I，先修订本文档重新审核。

## 16. 审核决定（已确认）

以下五项已由用户的继续指令确认，并按本文件实现：

1. `<=` 采用精确 canonicalization，不再长期 fail-fast。
2. PBMC 不再通过匿名 OPB 重解析建立新框架行，而由 `addRows()` 逐方向调用现有 RoundingSat API并记录稳定 logical ID。
3. `ROOT_TRUE` 方向保留版本/side 记录但不强行 attach tautology；只有真正 ATTACHED 的两边才要求两个 solver CRef和两个 ClID。
4. group 的稳定身份不使用 CRef；CRef 必须从 logical ID 解析，避免 GC 后悬空。
5. 0E-I 只建立 revision 0 和完整性检查，不提前实现 overlay/cache/undo；未来所有结构规则必须服从本文的 group transaction。

0E-I 已完成且没有创建简化算法代码；动态证据与停止点见 `../acceptance/00E_equality.md`。
