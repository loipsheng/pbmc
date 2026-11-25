# PBMC 递归简化 Prompt 0 源码审计

> 审计日期：2026-11-24；BICP/Equality/Cardinality 与 EDPLLSim component cache 补充审计：2026-11-25
>
> 审计基线：`819fd7e`（`pbmc修改准备`）
>
> 审计范围：指导文档、BICP/Equality-aware/Cardinality 接入、Pairwise Multi-row SC 方案、计数主循环、RoundingSat 传播/学习/Reason 生命周期、分量分解与缓存、普通 MC（其他模式仅保留历史基线记录）、EDPLLSim component cache/CS/SB-LP/RIR-LP/GLPK、构建与 sanitizer 基线
>
> 本次修改范围：只修订文档；没有修改简化算法、根预处理器或任何 C++ 源码。

## 1. 结论

Prompt 0 的源码审计与文档修订可以完成，但当前基线不能被标记为“可以立即进入简化算法编码”。在关卡 1 前至少要先处理或明确隔离以下问题：

1. 旧 `PBcounter::simplify()` 不是“没有调用”，而是**有条件调用、函数体为空、调用点位于 split/cache 之后**。后续应删除旧声明、定义、旧调用及专属死状态，再在正确位置建立简短的新入口；本次没有执行删除。
2. 新简化入口必须位于 `runPropagationWithoutLearnt(true)` 成功之后、`syncAssign()` 和 `splitComponent()` 之前。冲突学习完成后应通过现有回跳协议进入下一轮传播，不能在冲突约束仍处于处理中时直接改写公式。
3. 当前不存在安全的运行时 cache-off。`-cs` 是容量参数且最小为 1 MB；`ComponentManager.cc` 还在源码中硬定义 `CACHE`。把 `-cs=0`、清桶或让 `createEntryFor()` 返回空值都不是安全 bypass。
4. 用户已明确新框架只实现普通无权精确 MC，即全部变量的完整满足赋值数。PMC/WMC/WPMC 的既有结果保留为历史证据，但不修复、不进入新框架和验收门槛；启用新框架时必须明确拒绝 `-mode=1/2/3` 与 d-DNNF。以后删除 projected/weighted 代码另立任务。
5. sanitizer 基线发现 `ConfigCounter` 拷贝读取非法 `bool`。直接原因是该结构存在未初始化字段；另有 `rmvsatcl_threshold` 用 `bool` 接收整数阈值的类型错误。它们都属于现有基线问题。
6. 当前 stdout 将 backjump 统计注释掉，受执行环境 ptrace 限制也未能用 gdb 动态命中 `bjResolve()`。回跳协议已逐函数审计，但“至少一次非顺序回跳”的运行验收仍需在后续增加可测试统计后补齐。
7. 仍作为 `Reason` 的 attached constraint 不能原地改写或销毁。现有 `reduceDB()` 会跳过被 `Reason` 引用的约束，GC 会重映射 watches 与 `Reason`；新版本管理必须保留这些性质，并为结构改写增加不可变旧版本生命周期。
8. `ComponentManager` 在搜索开始时一次性复制基础约束；搜索期 attach 的 learned constraint 不进入其分量图。该策略只对基础公式的全局逻辑后果安全；局部简化行或基础行替换必须进入 residual view、分量拓扑与 cache identity。
9. `PBMC_Pairwise_MultiRow_SC_Codex_Guide_v1.0_CN.md` 的核心定理可行：若目标余项在一条独立辅助行下有精确无条件下界 $L$，则可把一个目标系数改为 `min(a,k-L)`。公共文字界和精确有理差值界均成立；T1/T2/T7 的计数与缩紧值已用完整枚举核对。首版应准确称为“两行 Multi-row SC”，一次只提交一个系数；多系数批量反例成立。
10. 真正联合多条辅助行的 SC-LP 也可沿用同一定理：GLPK/MIP 只负责在 `F\{target}` 下提出余项下界候选，最终必须精确检查 `G ∧ (R<=L-1)` 不可满足。指南 T4 的三辅助行 `8→1` 应归入 SC-LP，不应宣称两行规则必然得到。
11. 用户要求的代码组织可行：简化规则集中在 `core/simplify.h/.cc`，采用一个轻量 `Simplifier` 和短方法；`Counter`、`ComponentManager/Cache`、Config/CMake 只保留必要接入。当前 `PBcounter<T_data>` 在 `Counter.cc` 显式实例化，关卡 1 需用非模板边界或在 `simplify.cc` 明确实例化，不能把大量实现塞回 `Counter.h`。
12. AP 与 FLP 应合并成唯一 **BICP** 模块。一次 `x=0/x=1` 探测共享 failed-literal 与公共蕴含结果；临时 trail、Reason、watch/slack、overlay、EqualityGroup 和预算必须完整恢复。公共蕴含要求两个方向都完整，单侧确认冲突则足以推出相反文字；UNKNOWN 不得当 SAT/UNSAT。
13. Equality-aware 目前不是现成功能：RoundingSat parser 对 `=` 确实用 `invert()` 加入第二个方向，但递归层没有稳定 equality sibling/group identity；PBMC 前端普通行默认按 `>=` 处理，没有可靠的 `<=` 语义。所有规则之前必须建立 `<=` canonicalization 与 EqualityGroup 事务，否则 `<=` 应 fail-fast。
14. RoundingSat cardinality detection 应保留但不作为 PBMC 新算法。`postProcess()` 已对 detached row 调用等价保持的 `simplifyToCardinality(true)`，`toConstr()` 才可能选择专用 `Cardinality` propagator。PBMC 只需薄封装和递归改写后的再次调用；由于内容、类型、watches、Reason 和 cache identity 可能变化，仍需独立接入验收。
15. 每项技术必须先交付独立 pseudocode 文档并停止，用户审核后下一关才可编码。伪代码与 C++ 不得在同一技术关卡同时开始。
16. 用户新增顺序是可行且更安全的：变量阶段完成后先做 Cache Bypass，再参考 EDPLLSim 完成 residual component cache，全部验收后才进入 CS/SC。EDPLLSim 的版本化字节键、完整判等、probe-only lookup、insert-time intern、模板引用计数和插入年龄淘汰可迁移；其稠密 `Ax<=b`/整数 ranges/局部列号与递归复制式 solver 流程不能照搬。PBMC 必须保留现有 Component/Decision 栈和 father/descendant pollution 协议，并让 EqualityGroup 两方向进入同一个 residual identity。

因此：**Prompt 0 文档交付完成；关卡 1 尚未获准开始，且有基线阻断项。**

## 2. 实际阅读材料

### 2.1 已完整阅读

- `paper/PBMC_Recursive_Simplification_Codex_Implementation_Guide_v1.0_CN.md`
- `paper/PBMC_Pairwise_MultiRow_SC_Codex_Guide_v1.0_CN.md`（805 行，已完整阅读）
- `PBMC_CURRENT_ARCHITECTURE_CN.md`
- `PBMC_CODE.md`
- `PBMC_SIMPLIFICATION.md`
- `PBMC_FILE_AND_DIRECTORY_GUIDE_CN.md`
- `AAAI_EDPLLSim/EDPLLSim_Cache_AdaptiveK_to_PBMC_Guide.md`
- `AAAI_EDPLLSim/src/cache.h`（269 行，已完整阅读）
- `AAAI_EDPLLSim/src/cache.cc`（1023 行，已完整阅读）
- `AAAI_EDPLLSim/src/solver.cc`（91 行，已完整阅读）
- `AAAI_EDPLLSim/src/linear_constraint.cc`（1365 行，已完整阅读）
- `AAAI_EDPLLSim/src/linear_constraint.h`
- `AAAI_EDPLLSim/Makefile`、`build_utils.sh`、`README.md`
- `AAAI_EDPLLSim/tests/rir_lp/rir_lp_test.cc`
- `README.md`
- 下文列出的真实 C++ 源码、构建文件和对应 `pbmc-mod/` 副本

### 2.2 v1.0 所列但仓库中不存在

- `PBMC_CODE_ARCHITECTURE_AND_EXTENSION_GUIDE_CN(1).md`
- `PBMC_SIMPLIFICATION_TECHNIQUES_INTEGRATION_GUIDE_CN(1).md`
- `PBMC_Final_Simplification_Framework_v3.0_CN.md`
- `EDPLLSim_to_PBMC_Simplification_Guide(1).md`

本次没有猜测这些缺失文件的内容。v1.1 已改为引用仓库中真实存在的材料。EDPLLSim 文档的缓存思想只作为迁移原则；其中“当前工作区没有 PBMC 源码”等前提不适用于本仓库。

## 3. 真实源码锚点

行号针对审计基线 `819fd7e`；后续修改后应以函数名重新定位。

| 对象 | 真实位置 | 审计事实 |
|---|---|---|
| 前端调用链 | `core/Main.cc:204-257` | `load → renumberVar → makeRenumberedCoeffPositive → simplifyRenumberedConstraint → getRenumberedOpb → RoundingSat parsing`；根简化硬编码开启 |
| 计数入口 | `core/Counter.cc:374-391` | `countModels()` 调用 `import()` 后进入 `count_main()` |
| 主循环 | `core/Counter.cc:394-560` | 每轮原生传播、冲突处理或 split/cache、状态恢复、分支 |
| 旧空函数 | `core/Counter.cc:149-155` | `simplify()` 只返回 `true`；注释“not called”与真实调用不一致 |
| 旧调用 | `core/Counter.cc:521-528` | 位于 `splitComponent()` 及组件/SAT 处理之后，仅特殊 level 条件下执行 |
| 旧声明/状态 | `core/Counter.h:105,185` | `simplify()` 声明和 `on_simp` 成员 |
| 旧配置 | `core/Config.cc:22-23,78-79`; `core/Config.h:21-22` | `-rmvsatcl`/阈值只服务旧空入口；阈值字段错误声明为 `bool` |
| 传播 | `core/Counter.cc:325-369` | `runPropagationWithoutLearnt()` 不调用 `processLearnedStack()` |
| 冲突分析 | `core/Counter.cc:427-463,563-638` | `analyzePBMC2()` 使用模拟 level/position 计算回跳约束 |
| 回跳/学习 | `core/Counter.cc:642-707` | `bjResolve()` 同步 solver、ComponentManager 和 cache pollution；`ResolveWithoutLit()` 仅在选择性回跳获准时 attach learned |
| 顺序回溯 | `core/Counter.cc:710-770` | 完成分支求和、组件弹栈、cache 写回和兄弟分支切换 |
| 非投影 SAT | `core/Counter.cc:775-1240` | `solveSATFixedOrder()` 有独立传播、learnedStack 与恢复协议；首版节点简化不得插入其内部 |
| RS 赋值 | `roundingsat/Solver.cpp:137-180` | `uncheckedEnqueue/decide/propagate/backjumpTo` 维护 trail、Reason、Pos、Level |
| learned attach | `roundingsat/Solver.cpp:470-514`; `core/Counter.cc:681-705` | learned row 转为 solver `Constr`、初始化 watches 并加入 solver constraints；发生在 ComponentManager 初始化以后 |
| Reason 回收安全 | `roundingsat/Solver.cpp:626-632,656-720` | `reduceDB()` 跳过仍作为 Reason 的约束；GC 重映射 watch、Reason 和 external `CRef` |
| RS 规范化 | `roundingsat/ConstrExp.cpp:402-484,655-678,800-861` | 已有 remove/saturate、GCD、cardinality；只能在 detached `CeSuper/ConstrExp` 上按契约复用 |
| Cardinality attach | `roundingsat/ConstrExp.cpp:65-95,800-872`; `Solver.cpp:467-514`; `Constr.cpp:165-278` | `postProcess()` 先做等价 cardinality 化；`toConstr()` 按 `propCard` 选择专用对象并初始化不同 watches，禁止 attached 原地换型 |
| Equality parser | `roundingsat/parsing.cpp:49-119` | `=` 先加一个方向，再 `invert()` 加第二方向；两行未保留 PBMC 递归层可直接使用的 sibling/group identity |
| PBMC comparator | `core/Instance.cc:500-540`; `core/pbconstraint.h:19-58` | 只有 `equals` 布尔；非 `=` comparator 未严格校验并最终输出 `>=`，不能声称原生精确支持 `<=` |
| 分量初始化 | `core/ComponentManager.cc:84-146` | 一次复制原约束、建立 frozen occurrence、本地 ClID 和 gap 存储 |
| split/cache | `core/ComponentManager.cc:178-275` | cache lookup 位于 `splitComponent()` 内；孤立变量因子也在此处理 |
| residual gap | `core/ComponentManager.cc:278-363` | 基于 frozen 系数和当前 assigns 计算；满足行被忽略；仅 key 构造时做 gap saturation |
| cache key | `core/Component.h:814-951` | packed key 含全局变量 ID、本地 ClID、gap；hash 后仍比较完整 packed 内容 |
| cache 实现 | `core/ComponentCache.cc:63-236` | 固定 bucket 表、entry base、淘汰、父子 ownership；无 runtime bypass |
| cache 编译开关 | `core/ComponentManager.cc:14,81` | 源文件内硬定义 `CACHE`，不是受 CMake/CLI 控制的可靠功能开关 |
| EDPLLSim key builder | `AAAI_EDPLLSim/src/cache.cc:24-338` | canonical logical component；lookup 只 probe，insert 才 intern/acquire；版本化 bytes + ULEB128 + exact bigint |
| EDPLLSim template pool | `AAAI_EDPLLSim/src/cache.cc:375-673` | full-content interning、live-key refcount、零引用批量 reclaim、ID 仅在安全后复用 |
| EDPLLSim cache lifecycle | `AAAI_EDPLLSim/src/cache.cc:693-1023`; `linear_constraint.cc:1162-1279` | hash 后 full bytes compare；插入年龄且 hit 不续期；entry 删除释放模板；每实例 clear/configure |
| EDPLLSim count/cache order | `AAAI_EDPLLSim/src/solver.cc:29-89` | simplify 后分离自由变量因子；lookup/store 的 residual count 不含外部 factor |
| 模式配置 | `core/Config.cc:15-24,62-91` | 仓库现有 `-mode=0..3`；新简化框架仅允许 mode 0 普通 MC 且关闭 d-DNNF |
| 权重导入 | `core/Counter.cc:1336-1347` | 只复制前 `npvars` 的权重，符合当前 WPMC 对非投影变量只判断可扩展性的设计 |
| 文字转换 | `core/Instance.cc:32-36`; `core/Counter.cc:161-207` | 输入权重与递归 trail 的 Glucose literal 转换极性不一致，已由小例复现 |
| 构建 | `CMakeLists.txt:1-41` | C++17；源码显式列举；默认 `RS_LOWER_PREC=ON`，定义 `LOW_PREC` |

`CRef` 不能跨层混用：RoundingSat allocator 的 `rs::CRef` 是 allocator offset，`Constr::id` 是逻辑 ID，而 ComponentManager 有自己的 allocator 和局部 `ClID`。后续 overlay/cache identity 必须显式区分三者。

## 4. 三条真实流程

### 4.1 无冲突计数流程

```text
count_main loop
  legacy unitcls → decide
  runPropagationWithoutLearnt(true)
  no conflict
  [new] simplifyNode()：变量 → 系数 → 约束
        若产生正式赋值/结构变化，回到原生传播
  syncAssign()
  splitComponent(assigns, legacy lit_weight argument)
    search active components from frozen occurrence
    account isolated variables
    build temporary cache entries
    lookup legacy cache
  solve non-projection-only components by solveSATFixedOrder()
  if a projection component remains: choose variable and decide
  otherwise backtrack()
    first/second branch sum
    independent components multiply through Decision::increaseModels()
    completed component value stored in cache
```

新节点简化的正确插入点是 `no conflict` 与 `syncAssign()` 之间。当前旧 `simplify()` 调用在上述 split/SAT 状态处理之后，过晚且作用域不稳定。

### 4.2 冲突学习与回跳流程

```text
propagation conflict
  analyzePBMC2(confl, learned, backtrack_level)
  ├─ analysis failure → bjResolve(level) → continue
  └─ learned CeSuper
       heuristicWeakening()
       postProcess()
       ResolveWithoutLit(level, learned)
       ├─ selective backjump allowed:
       │    attach learned → solver/component/cache synchronized backjump
       └─ not allowed:
            mark current branch zero → ordinary backtrack
  next loop performs native propagation
  only after that propagation is conflict-free may node simplification run
```

不能假设主循环会自动消费 `learnedStack`。`runPropagationWithoutLearnt()` 明确跳过它；SAT 子求解才使用带 limit-level 的 learnedStack 路径。

### 4.3 传播—简化联合循环与冲突边界

普通 MC 中，新简化推出的固定文字贡献因子 1，因此本计划不新增投影/权重 `trail_accounted` 或 ledger。正式蕴含必须通过 RoundingSat 的传播接口进入 live trail，重新执行原生传播，并在分解前由现有 `syncAssign()` 同步。

固定协议为：

```text
native propagation
  ├─ real conflict → 原 conflict analysis / learn / backjump
  │                   → restored node 重新 propagation
  └─ no conflict → simplifyNode
       ├─ IMPLIED / ROW_CHANGED / ROW_REMOVED → commit + undo/revision → propagation
       ├─ real CONFLICT with legal proof → 原 conflict path
       └─ UNCHANGED / BUDGET → split/cache/branch
```

BICP/RUP 的临时假设冲突不是当前真实节点冲突，不能直接学习、回跳或把真实节点计数置 0。若简化严格证明当前节点无模型但不能提供原冲突分析可用的证明，只能令当前分支贡献 0 后安全顺序回溯。

### 4.4 Reason 生命周期与约束版本

`Reason[v]` 是 solver allocator 的 `CRef`，不是稳定的跨层 ID。现有 `reduceDB()` 逐文字检查 Reason 并跳过仍被引用的 constraint；`garbage_collect()` 会移动约束并重映射 `constraints`、watches、`Reason` 和 external 引用。这解释了为什么简化不能原地修改或释放 Reason 指向的对象。当前 `removeLastAssignment()` 没有清空被弹出变量的 `Reason[v]`，所以旧引用可能保守地延迟回收；在证明可安全清空前不得擅改这项 RoundingSat 公共语义。

后续 committed rewrite 至少要实现 `ACTIVE → RETIRED_REASON → RECLAIMABLE`：新内容创建新版本；旧版本在 solver `Reason` 仍引用期间保持不可变；回跳撤销活动版本并按已验证接口处理相应依赖；只有无实际 Reason/证明依赖后才交给 solver 合法回收。母版/Template ID 必须与精确 row version、scope 和当前 solver 引用区分。

### 4.5 学习约束与分量图

`cmpmgr.init()` 只在 `count_main()` 开头调用一次，并把当时的 solver constraints 复制到自己的 allocator/occurrence/ClID。`ResolveWithoutLit()` 在之后 attach 的 learned constraint 只进入 RoundingSat solver，不进入冻结分量图。

保留该行为的条件是 learned row 为基础公式的全局逻辑后果，因而只用于传播/剪枝而不改变模型集合。局部 probe 结论、scope-dependent row 和基础 residual replacement 不能仅贴上 learned 标签后从分量图/cache identity 中省略；它们需要 guard/undo 或 residual overlay。

## 5. Cache 审计

### 5.1 当前没有 cache-off

- `-cs` 的 `IntRange` 是 `[1, INT32_MAX]`，含义是非严格 MB 容量，不是开关。
- `-cs=1` 也不能视为安全 bypass；固定 900001 buckets 本身已占内存，初次淘汰还可能在无可删 score 时访问空集合。
- `ComponentManager.cc` 自行 `#define CACHE`；简单取消宏还会改变 component boundary 逻辑，而许多后续调用没有同样的条件保护。
- legacy temporary entry 同时承担 packed key、component stack ID、father/descendant pollution ownership。仅关闭 hash bucket 或返回空 ID 会让后续 `cacheModelCountOf()`、`eraseComponentStackID()`、`removeCachePollutions()` 等路径失效。

### 5.2 后续最小实现方向

不新增 `CacheBypassAdapter`。在现有 `ConfigComponentManager`、`ComponentManager` 和 `ComponentCache` 内加入一个小的运行模式，例如：

```cpp
enum class CacheMode { Legacy, Bypass, Residual };
```

Bypass 必须从 `cmpmgr.init()` 前按**整次计数运行**选择，不能在搜索中途清桶或切换。它应：

- 保留 Component/Decision 栈、分量边界、分支求和和分量求积；
- 对 root entry、create/request/link、store/erase、pollution cleanup 的 cache-ID 路径成套 guard；
- 保证真正的 hash lookup/hit/insert 均为零；
- 不以 `nullptr` 或伪造 ID 让现有未 guard 的代码继续运行；
- 先在没有结构改写时与 Legacy 做计数差分。

是否保留轻量 ephemeral ownership entry，或在 Bypass 下完全移除 descendant 依赖，应在关卡 1 通过现有回跳路径的单元测试决定；不能只改一个函数。

### 5.3 合法模式矩阵

| 简化状态 | Legacy | Bypass | Residual cache |
|---|---:|---:|---:|
| 新模块关闭、公式冻结 | 当前基线 | 2D 后作为差分 | 2E 后三路差分 |
| 只读 residual/view-only | 可用，不得改变 split 语义 | 2D 后可用 | 2E 后可用，不发布候选 row |
| 仅产生有 reason 的 trail 赋值 | 原理上可用，须回归 | 2D 后可用 | 2E 后可用 |
| 动态改系数/支持集/active rows | 禁止 | 无复用 correctness oracle | 2E 后的生产模式 |
| residual-aware identity 验收后 | 只作冻结公式对照 | 继续作无缓存 oracle | 正式启用 |

### 5.4 EDPLLSim component cache 的实际迁移边界

`AAAI_EDPLLSim/src/cache.h/.cc` 的真实实现不是单个哈希表，而是四层协议：

1. `ComponentNormalizer` 构造数学规范 residual component；
2. `RowTemplatePool` 按完整 canonical row 内容做 interning，lookup 只 probe，insert 才 acquire；
3. component key 写入格式版本、delta-coded TemplateID、变量域和 exact RHS，hash 后仍比较完整 byte vector；
4. cache entry 按插入序列老化，命中不续期，删除 entry 时 release 模板引用，每个 instance 显式 clear/configure。

PBMC 可直接迁移第 2–4 层的算法形状，但第 1 层必须按 PBMC 重写。原因是 EDPLLSim 的 key builder面向稠密整数域 `Ax<=b`、变量区间和局部列号，而 PBMC 是布尔 literal、规范 `>=` residual gap、全局变量 ID，并且 `=` 由两个方向组成同一 EqualityGroup。PBMC 还必须把模板引用责任接入现有 `CachedComponent` 的活动 stack ID、father/descendant pollution、hit 临时 entry、backjump 和 eviction，不能用 EDPLLSim 的静态 `LinearConstraint::component_cache` 替代现有 manager。

计划固定为：变量 Basic/BICP 完成 → 2D Bypass → `03_component_cache.md` → ReferenceKey oracle → packed/template key → ownership/eviction/cutover → CS。`ReferenceKey` 只做测试 oracle；生产使用一套 `TemplateID + exact gap` key。现有 PBMC 把 gap 保存为 `int` 并用 `double/log` 计算 packed 位宽，这不能作为动态 residual identity；2E 必须为 `rs::BigVal` 提供无损编码，并在 `LOW_PREC` 与 GMP build 下测试。

## 6. 数据结构与复用边界

为符合 PBMC/EDPLLSim 现有风格和用户最终决定，v1.1 不再预设十余个基础类、抽象规则接口或规则专属文件。

- 所有简化技术统一放在 `core/simplify.h/.cc`：一个轻量 `Simplifier`，入口 `PBcounter::simplifyNode()`/`Simplifier::run()`，短方法为 `basic/bicp/cs/gcd/normalizeCardinality/multiRowSC/rir/rpr/rsr/rup/equality/sbLP/scLP/rirLP`；不引入 `ISimplificationRule` 虚继承层。
- `SimpResult` 不得退化成 `bool`；首版状态为 `UNCHANGED/IMPLIED/ROW_CHANGED/ROW_REMOVED/CONFLICT/BUDGET`，只在实际需要时增加 reason/row payload。当前确定规则不做变量消元，也不为普通 MC 引入 `CountFactor`、投影/权重 ledger 或额外 trail accounting。
- RoundingSat 已有 `CeSuper/ConstrExp`、`saturate()`、`divideByGCD()` 和 `simplifyToCardinality()`；在 detached row 上复用，不能复制第二套大数表示。
- `postProcess()` 的 unit removal 只针对 solver 的 level-0 unit，不等于当前递归层的 residual variable elimination。
- ComponentManager 的 `occlists_` 在初始化时基于原公式建立。因为当前框架不新增非零项，它可先作为 rewrite 后 occurrence 的保守超集，通过 active row/removed term 过滤；首版不复制第二套 `var→rows`。
- 初始关卡不改变 `PBconstraint`、attached `Constr`、`Component`、`Decision` 的公共布局。只有经测试证明必要时才加最小 accessor/mode guard。
- `core/simplify.cc` 必须显式加入顶层 `CMakeLists.txt`，并与 `core/simplify.h` 在稳定后同步到 `pbmc-mod/`；本次没有新增 C++ 源文件。
- 简化实现前必须先建立最小 `EqualityGroup` 元数据：一个原 equality 的两个 canonical 方向各自保留 RowVersion/CRef，但共用 group revision、undo marker 和 cache publish。该元数据是必要接入，不授权重写现有通用约束容器。
- 每项技术的 `docs/inprocessing/pseudocode/NN_name.md` 是代码前置物；文档关和实现关分开验收。Cardinality 的文档只描述复用 RoundingSat 的接入、换型和恢复，不复制算法。

### 6.1 EDPLLSim 简化与 GLPK 的可迁移边界

用户指定的 `AAAI_EDPLLSim/src/linear_constraint.cc` 已完整阅读。真实实现结论如下：

1. `coefficientStrengtheningSingle()` 是 EDPLLSim 的单行系数强化：对一般整数域的每条 `Ax<=b`，利用该行自身和变量区间逐个缩小系数并同步 RHS；它不读取辅助行。对 Boolean PB 规范式 `sum(a_i*l_i)>=gap`，对应操作是 RoundingSat 已有 saturation `a_i=min(a_i,gap)`。按用户术语，PBMC 将它命名为 **CS**，实现为 `Simplifier::cs()`；Multi-row SC/SC-LP 则保留给读取其他约束的规则。
2. EDPLLSim 的 `LP()` 实际把变量设为 `GLP_IV`，先运行 `glp_simplex()` 再运行 `glp_intopt()`；它是整数优化/MIP，而不是只解连续 LP relaxation。
3. SB-LP 对每个变量分别最大化、最小化，缩紧一般整数 domain；PBMC 适配后只针对未赋值 Boolean 变量产生 `x=0/1` 候选，最终必须由 RoundingSat 对相反假设做 exact conflict 验证并生成合法 Reason。
4. RIR-LP 暂时放开目标行，在其余系统下优化目标 LHS。PBMC 对 `L>=degree` 的删除候选必须再精确验证 `F\{C}∧¬C` 不可满足；GLPK 不能直接授权删行或返回节点计数 0。
5. EDPLLSim 已对 GLPK double 加 fail-closed guard：range、系数、RHS 和最大可能 LHS 必须能在 `2^53` 连续整数范围精确表示，否则不简化；还统一检查 simplex/MIP status。PBMC 应复用这些 guard，但由于 GLPK 仍是浮点求解器，最终提交仍需要 exact checker。
6. `tests/rir_lp/rir_lp_test.cc` 覆盖 infeasible relaxation、常量冲突、冗余行、不可表示整数、`2^53` LHS、`3e9`、随机 brute-force 和 K 路径，可改写为 Boolean OPB 回归测试。
7. EDPLLSim 将相关方法集中在 `linear_constraint.h/.cc`，这支持 PBMC 使用统一 `simplify.h/.cc` 的组织方式；应借鉴集中实现与短方法，而不是照搬其一般整数域数据结构。

### 6.2 Pairwise Multi-row SC 与 SC-LP 审计

对目标 `C:a*l+R>=k`，只要不使用目标行自身且能证明当前保留前提 `G |= R>=L`，`0<=L<k`，就可令 `a'=min(a,k-L)`。这是完整系统 `G∧C` 的等价改写，不是目标单行的孤立等价。若 `L>=k`，则 `G` 已蕴含目标整行，应转为删行候选。

- 公共同极性文字界通过“辅助私有项最大贡献 + 公共项最小系数比”给出安全下界；支持集可以不同。
- 通用差值界对精确有理 `lambda=p/q` 展开 `qR-pH`，用布尔仿射式 `c+sum(d_v*x_v)` 的精确盒域最小值 `c+sum(min(0,d_v))`；负文字常数必须一起展开，全程不得用浮点。
- 多个候选下界可以取最大值，不能相加；首版只使用一条 helper，候选受 top-Q/pair/nnz/bigint 预算约束。
- 一次只提交一个系数并重算。`5x+5y+5z>=6; x+y+z>=2` 证明“分别可缩”不代表可同时批量缩；三个 5 同改为 1 会把模型数从 4 改为 0。
- helper 是派生版本的 proof dependency；helper 失活/删行前必须撤销或重证。无条件版本不会把正系数降为 0，因此通常不直接改变支持图。
- SC-LP 使用全部活动辅助行最小化 `R`，但 GLPK 只产生候选。精确 checker 可把 `R<=L-1` 编成 `sum(a_i*~l_i)>=U_R-L+1`，在排除目标行的隔离 PB context 中证明冲突；失败/UNKNOWN/超预算均 no-change。

已用完整布尔枚举复核：T1 为 4 个模型、T2 为 13 个、T4 为 5 个、T7 为 6 个；批量反例原系统为 4 个模型，三个系数同时改为 1 后为 0。该枚举只核对示例，不代替实现中的 proof 与随机模型集合差分。

### 6.3 BICP、Equality 与 Cardinality 决策

1. **BICP**：公开算法入口只保留 `bicp()`。内部 `probeBranch(l)` 是私有步骤；同一次双向结果同时产生 failed-literal 与 common-implication 候选。临时 Reason 不可越过 rollback，正式推导需合并证明或 exact recheck。
2. **Equality-aware**：`>=` 保持；`<=` 精确转换为 `>=`；`=` 映射为两个 canonical `>=` 方向并保留 group identity。所有规则必须覆盖三者。删除整个 equality 需在排除两个方向后分别证明；任一方向 commit/undo 失败都整组回滚。
3. **Equality-specific**：是后续独立增强，只研究联合两个方向比逐行规则多得到的简化。若伪代码不能展示新增效果则不实现，不用重复 pass 凑模块。
4. **Cardinality**：保留 `simplifyToCardinality(true)`，不重写判定也不称为 PBMC 新技术；但递归新 row version 的 attach/type/watch/Reason/cache 接入单独验收。
5. **伪代码关**：每项先提交单独文档，至少含三种 operator、证明、状态、rollback、Reason、EqualityGroup、cache/overlay、预算与 oracle；获批前禁止改 C++。

依赖方面，EDPLLSim 仓库包含 `utils/glpk-4.65.tar.gz`，`build_utils.sh` 以 `configure/make/sudo make install/ldconfig` 安装，Makefile 通过 `-lglpk` 链接。当前机器已安装 GLPK 4.65，`glpk.h` 和 `libglpk.so` 均存在。PBMC 后续应使用默认关闭的 CMake option + `find_path/find_library`，不能在 CMake 中自动解压、联网或执行 `sudo`。

参考实现验证：在 `AAAI_EDPLLSim/` 执行 `make test-rir-lp`，结果为 `RIR-LP regression tests passed`。这只证明 EDPLLSim 当前回归测试通过，不等于其 GLPK 结果可以不经 exact checker 直接用于 PBMC。

## 7. 基线构建与实测

### 7.1 Release 构建

```text
make release
结果：PASS，bin/pbmc 构建完成
```

### 7.2 小例

所有输入均通过 `/dev/fd` 传入，没有向仓库加入临时 fixture。运行时使用 `-no-td` 隔离树分解启发式。

| 用例 | 模式 | 独立期望 | PBMC 结果 | 状态 |
|---|---|---:|---:|---|
| `x1∨x2` | MC | 3 | 3 | PASS |
| `(x1∨x2)∧(~x1∨x2)∧(x1∨~x2)` | MC | 1；应有冲突 | 1；conflicts=1 | PASS |
| `(x1∨x2)∧(x3∨x4)` | MC | 9；两个分量 | 9；Components=3 | PASS |
| `x1+x2=1` | MC | 2 | 2 | PASS |
| `x1∨x2`，投影 `{x1}` | PMC | 2 | 2 | PASS |
| `x1∨x2`，权重 `x1=.2,~x1=.8,x2=.3,~x2=.7` | WMC | .44 | .94 | **FAIL** |
| 同上，自然数权重 `2,8,3,7` | WMC `-natw` | 44 | 94 | **FAIL** |
| 唯一模型 `x1=x2=1`，投影 `{x1}`，权重 `.2/.8` | WPMC | .2 | .8 | **FAIL** |
| unit `x1>=1`，权重 `.2/.8` | WMC | .2 | 1 | **FAIL** |

最后四行是 Prompt 0 的历史诊断，失败与源码转换一致，不是数值容差。根据用户后续决定，它们不再进入实现或验收矩阵，也不安排修复；这里只保留审计证据。

仓库默认 knapsack 冒烟实例在 `-bj` 与 `-no-bj` 下均得到 MC=5028，但没有冲突，不能作为 backjump 动态覆盖证明。

### 7.3 ASan/UBSan

独立构建命令：

```text
cmake -S . -B /tmp/pbmc-audit-asan-20261124 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build /tmp/pbmc-audit-asan-20261124 -j2
```

构建 PASS。小例在关闭当前环境不兼容的 LeakSanitizer 后可以完成，但 UBSan 间歇报告：

```text
core/Config.h:14: runtime error: load of value 248,
which is not a valid value for type 'bool'
ConfigCounter copy → PBcounter constructor (core/Counter.cc:35)
```

`ConfigCounter::doPreprocss` 没有在 `Configuration::Configuration()` 中初始化，结构拷贝会读取不确定值。`rmvsatcl_threshold` 还被声明为 `bool`，但从整数选项赋值。LeakSanitizer 在当前 ptrace/容器环境本身无法工作，此点不计为 PBMC 泄漏结论。

### 7.4 关键小例复现命令

```bash
# MC：期望并得到 3
./bin/pbmc -no-td -mode=0 \
  <(printf '%s\n' '* #variable= 2 #constraint= 1' '*' \
  '+1 x1 +1 x2 >= 1 ;')

# MC conflict：期望并得到 1，conflicts=1
./bin/pbmc -no-td -mode=0 \
  <(printf '%s\n' '* #variable= 2 #constraint= 3' '*' \
  '+1 x1 +1 x2 >= 1 ;' '+1 ~x1 +1 x2 >= 1 ;' \
  '+1 x1 +1 ~x2 >= 1 ;')

```

PMC/weighted 复现输入已由 Prompt 0 结果表记录，不纳入后续自动测试；新框架只测试对 mode 1/2/3 的显式拒绝。

## 8. 关卡 1 前阻断项

### 必须先解决

1. 建立只覆盖普通 MC 的 brute-force oracle 和永久小例，并增加 PMC/WMC/WPMC/d-DNNF 新框架启动拒绝测试。
2. 初始化 `ConfigCounter` 的存活字段，使 UBSan 基线干净；旧 `rmvsatcl` 阈值字段随旧空入口删除，若为兼容保留才改成正确类型。
3. 为 backjump 增加可测试统计或专用测试，动态覆盖 `ResolveWithoutLit()`、`bjResolve()`、普通 `backtrack()` 和 SAT 子求解恢复。
4. 在变量阶段结束后、任何 CS/SC 或 committed structural rewrite 前，依次完成 Reason 不可变版本生命周期、整次运行的 Cache Bypass 和 EDPLLSim 风格 residual component cache（identity、full compare、模板 refcount、ownership、淘汰）。
5. 在任何简化规则前完成 operator/Equality contract：修复或明确规范 `<=`，为 RoundingSat 展开的 `=` 两方向建立稳定 group identity，并用完整模型集合测试。
6. 为下一项技术先完成对应独立 pseudocode 文档；没有用户审核通过不得编码。

### 可以延后

- residual cache 的参数调优、局部变量重命名和跨实例持久化；identity、模板 interning、基础淘汰与 ownership 不能延后；
- LP 三项和其精确证书；
- Adaptive K；
- projected/weighted 代码删除（单独清理，不能与简化算法实现混合）；
- 根预处理器既有算法问题、VCIS/TD 消融、其他无关重构。

## 9. 下一关的最小修改计划（尚未执行）

下一阶段按 0E → 关卡 1 进行，不实现任何数学简化规则：

1. 先只写并审核 `docs/inprocessing/pseudocode/00_equality_contract.md`；随后建立 `<=` canonicalization/拒绝协议和 `=` 两方向的 EqualityGroup 身份，不能让现有前端静默误解 comparator。
2. 删除旧 `PBcounter::simplify()` 声明、空定义、过晚调用和 `on_simp`；不要删除 `core/Solver.cc` 中不同用途的 Glucose `simplify()`。
3. 审查后删除只服务旧空入口的 `-rmvsatcl`/阈值字段，或短期保留为明确 deprecated CLI；不复用其名字承载新框架。
4. 新建且只新建 `core/simplify.h/.cc` 作为简化规则实现，在 `Counter` 中加入简短的 `simplifyNode()` 接入和小型 `SimpResult`；新模块默认关闭，准确插入无冲突传播之后、`syncAssign()/splitComponent()` 之前；状态不能只用 `bool`。
5. 建立 propagation—simplification 联合循环；正式蕴含进入 solver trail 后重新传播。普通 MC 不新增投影/权重 `trail_accounted` 或 ledger。
6. 启动时只允许 mode 0 普通 MC 且关闭 d-DNNF；其他组合明确报错。空骨架必须与旧版 MC decisions/components/cache stats 一致。
7. Basic/BICP 变量阶段完成后，Cache Bypass 直接在现有 ComponentManager/ComponentCache 中成套实现，不新增 adapter 层；整次 count run 固定模式。
8. Bypass 通过后先写并审核 `docs/inprocessing/pseudocode/03_component_cache.md`，随后分阶段迁移 EDPLLSim 的 packed key/template pool/lifecycle 内核并适配 PBMC ownership；Residual↔Bypass 全套验收通过后才开始 CS。
9. 不创建规则专属文件。各项按短方法逐关加入 `Simplifier`：`basic/bicp/cs/gcd/normalizeCardinality/multiRowSC/rir/rpr/rsr/rup/equality`，后期再加入 `sbLP/scLP/rirLP`。每项先交付独立 pseudocode 并停止；获批后才编码。两行 Multi-row SC 一次只提交一个系数；SC-LP 在 residual cache 与 exact checker 完成后单独验收。

用户审核本报告和 v1.1 指南前，不开始上述任何源码修改。
