# PBMC 递归简化模块：Codex 分阶段编码、验收与缓存迁移实施指南（v1.0）

> **用途**：把本文件与 PBMC 源代码仓库一并交给 Codex。Codex **先审查仓库、修订本指南，提交审计报告**；之后按照关卡，一次只实现一项技术。**每项技术都要独立开关、独立测试、提供可复现实验结果；未验收不继续下一关。**
>
> **基线说明**：下文的具体文件、函数和代码行为来自已提供的 PBMC 源码分析指南，所述版本为 `08eb389`。本文件**没有直接访问或编译真实 PBMC 仓库**，因此凡与实际仓库有出入，须以 Codex 对真实代码的检查为准，修改“源码事实记录”后才能编码。所有新接口均为**设计提案**，不是声称仓库现有的 API。
>
> **用户已经确定的设计**：暂不改根预处理器；简化技术放在搜索递归状态机中；每项独立 `.h/.cc`；顺序为**变量简化 → 系数简化 → 约束简化**；系数规范化使用**版本 A：CS → GCD → Cardinality Detection**；**先完成并验证各技术，后添加自适应 K**；动态改写系数/失活约束期间可临时关闭组件缓存，但必须做完整、明确的 bypass，不能带着不安全的旧键使用缓存。

---

## 0. Codex 的执行合同（必须逐条执行）

1. **先审计，后编码**。首次任务只能阅读本指南、两份代码分析指南和真实代码，输出差异清单、源码定位、风险及本指南的修订版；不得立刻实现多项简化技术。
2. **不修改旧 PBInstance 预处理算法**。保留既有 `simplifyRenumberedConstraint()`、RoundingSat 输入解析和 `postProcess()` 的原有数学功能；允许为了连接新递归模块做必要的、单独测试的最小接口调整。不要顺手修 VCIS、TD、缓存淘汰和其他不相关算法。
3. **每个算法规则独立 `.h` 和 `.cc`**，共享基础数据类型、证明、调度与探测引擎；不要复制 RoundingSat 的 propagation/SAT solver 代码。头文件和实现文件的真实路径在审计后确定。
4. **一次只做一项规则的一个可验收版本**。一次变更含单项规则、开关、单元测试、计数差分测试、最小性能统计和文档；失败回滚或修复该项，不跳到下一项。
5. **默认所有新规则关闭**。旧版本与“仅挂载空模块”的运行结果、状态机与计数需保持可比。每个规则单独开关；另设 `--inprocess` 总开关，开关命名以仓库配置风格审计结果为准。
6. **当前先不实现 K 自适应**。保持三个阶段的固定先后顺序；非 LP/LP 增强分别通过明确开关与固定预算测量。后续独立阶段才引入 K 选择，不改变数学规则。
7. **所有新增递归改写必须守住精确计数语义**。既要验证 MC，也要验证 PMC、WMC、WPMC（实际构建支持哪些就全部覆盖；不支持的模式应在审计中列出并使相应规则明确拒用）。
8. **禁用不可靠的浮点推断**。涉及系数、gap、逻辑蕴含的判断使用精确整数/有理运算；LP 结果在无可核验的精确证书前只能做候选，不能据此删行、固定变量或替换系数。
9. **不直接原地修改已 attach 的 `rs::Constr`**。任何真正的残余系数/文字改写通过隔离的 immutable template + 可回溯 overlay、或经过证明且重建受影响对象的方案完成；必须同步 solver、ComponentManager、reason、缓存和 undo。
10. **源码事实优先**。若文档中引用的类、函数、行号、调用顺序与真实仓库不同，先修改文档并标注“审计修订”，不可假装已验证。
11. **未通过关卡，禁止进入下一关**。每关提交：变更文件、数学正确性说明、测试日志、计数对照、性能统计、已知限制、当前开关/缓存模式以及是否通过。由用户确认后继续下一项。
12. **禁止一次性重写求解器**。维护可回退、可 bisect 的提交；每次改动独立、可关闭，保留旧行为基线。

### 0.1 资料阅读顺序（Codex 第一次必须通读）

1. `PBMC_Recursive_Simplification_Codex_Implementation_Guide_v1.0_CN.md`（本文件）；
2. `PBMC_CODE_ARCHITECTURE_AND_EXTENSION_GUIDE_CN(1).md`；
3. `PBMC_SIMPLIFICATION_TECHNIQUES_INTEGRATION_GUIDE_CN(1).md`；
4. `PBMC_Final_Simplification_Framework_v3.0_CN.md`（数学推导、38 项筛选、目标流程）；
5. `EDPLLSim_to_PBMC_Simplification_Guide(1).md`（八种技术的原式和 PB 特化）；
6. 如提供：`EDPLLSim_Cache_AdaptiveK_to_PBMC_Guide.md`、EDPLLSim 最新缓存源码/说明，用于**迁移原则而非复制字段**；
7. PBMC 真实代码：入口、RoundingSat、ComponentManager、Component/Cache、WMC/PMC/d-DNNF 和已有测试。

若文件在 Codex 工作区不存在，应在审计报告中列明缺失，不猜测其内容。新技术的证明与实现以真实代码约束为准，不沿用前文中未经证明的示例变换。

### 0.2 交付物结构（本指南与代码共同维护）

在仓库中建立或更新 `docs/inprocessing/`，建议包含：

```text
docs/inprocessing/
  00_code_audit.md                 # 真实源码定位与本指南差异
  01_invariants_and_semantics.md   # MC/PMC/WMC/WPMC、理由和状态契约
  02_stage_plan.md                 # 关卡与实际配置
  03_rule_register.md              # 独立规则、开关、测试、审计记录
  04_cache_migration.md            # 关闭/恢复 cache 与 key v2 证明
  05_test_matrix.md                # oracle、回跳、压力、性能
  06_experiments.md                # 结果和消融表
  acceptance/                      # 每关一份验收报告
```

文件路径为建议；以真实构建系统兼容为先。Codex 首次只补齐审计及计划，不大量创建空源码文件。

---

## 1. 核心技术决策与明确的不做事项

### 1.1 本轮做什么

- 以 **PBMC 计数搜索中的节点简化**为第一目标，集成到实际 `count_main()` 状态机，而不是只在根部运行。
- 三个功能阶段：**I 变量简化**（UP/逻辑 RV/BasicCheck、AP、FLP，后续 SB-LP）；**II 系数简化**（CS、GCD、Cardinality、Pairwise Multi-row SC，后续 SC-LP）；**III 约束简化**（RIR、RPR、Generalized RSR、RUP，后续 RIR-LP）。
- 统一事件队列、dirty-row、revision、证明/原因、undo、预算、统计和计数记账。
- 分层实现：先只读或 assignment-only；然后安全的约束失活；最后实际的系数/结构重写。**单项算法数学正确 ≠ 它已被安全接入求解器。**
- 动态结构改写期间先用**完全关闭 cache**进行正确性开发，之后实现 residual-aware cache 并重新开启；不允许将“cache off”当成分解/回跳正确性证明。

### 1.2 当前明确不做

- 本轮**不替换、不扩展根预处理器**（未来另写独立计划）；不调整用户现有根 preprocessing 开关默认行为。
- 第一批**不做**一般变量消元、等价/互补变量代换、Lifting 新增非零项、约束跨作用域无证明重写、对称性剪枝、BCE/纯文字 SAT-only 简化。
- 第一批**不做**真正多条辅助行联合的 Multi-row SC、条件式目标变量赋值下界优化、Full Backbone、完整 ADD 构造。
- 不把 RoundingSat 冲突学习里的 weakening/divide-round 当成可以替换原约束的等价 SC。
- 自适应 K、自动选择 LP/Non-LP、动态预算调参、Cache age/renew 优化和可选的新分支启发式全部延后到数学规则 + 缓存正确性稳定后。

### 1.3 约束与计数语义

统一逻辑形式：对一个规范 PB 不等式，

$$
C:\sum_i a_i\ell_i\ge \mathrm{gap},\qquad
 a_i\in\mathbb Z_{>0},\quad\ell_i\in\{x_i,\bar x_i\}.
$$

令 $T$ 为当前已经为真的文字贡献，则 `gap=degree-T`；令 $U=\sum_{\text{unassigned}}a_i$，则 `slack=U-gap`（RoundingSat 也可以按“尚未为假”的系数和减 degree 增量维护同一数值）。

- `gap <= 0`：当前约束已满足；
- `gap > U` / `slack < 0`：当前赋值冲突；
- `a_i > slack`：未赋值文字 $\ell_i$ 必须为真（PB 原生 UP）；
- RV 是**把已固定文字在残余视图中消去并调整 gap**，不是再次推断赋值；
- Gap 不是 Slack；缓存中的 gap saturation 不是 CS；
- 未展开的 `=` 约束禁止直接使用 `>=` 的 CS。原 PBMC parser 可能把一个等式拆成两个内部不等式，按实际代码确认。

**四种计数模式**：MC 统计全部完整模型；WMC 累加完整模型的权重；PMC 统计可扩展的投影赋值；当前 PBMC 的 WPMC 只对原投影文字计权、非投影变量只参与是否存在扩展。所有模型集合不变的约束强化/删行通常对四种模式都安全；任何变量消除、等价映射、自由变量因子需额外处理。

---

## 2. 第 0 关：源码审计与本指南修订（**禁止编码新规则**）

> **目标**：把所有“按指南推测”变成“经真实代码确认”。将审计结果写入本文件附录或项目文档，产出可执行的阶段表后，请用户批准开始关卡 1。

### 2.1 必须核对的源码路径和调用关系

以下仅为上传指南基于 `08eb389` 的定位，Codex 必须通过 `rg` 和实际函数调用链核对：

| 核对对象 | 指南给出的锚点 | 必须查明的问题 |
|---|---|---|
| 主入口 | `core/Main.cc::main_pbmc` | PBInstance、RoundingSat、VCIS/TD、countModels 的顺序；旧根预处理不要动 |
| 根输入规则 | `core/Instance.cc::simplifyRenumberedConstraint` | 原有二项 probing 与修正异常；本阶段只记录，不改 |
| 主循环 | `core/Counter.cc::count_main` | 实际每轮传播、同步、split/cache、branch、冲突的确切位置 |
| 旧空入口 | `PBcounter::simplify()` | 是否仍是空壳；是否受 `checkfixedDL()` 限制 |
| RoundingSat 传播 | `runPropagationWithoutLearnt()` | `learnedStack` 不自动处理的事实是否仍成立；reason 处理 |
| 赋值与 reason | `roundingsat/Solver.hpp`, `Solver.cpp` | `trail`, `qhead`, `Reason`, `decide`, `propagate`, `backjumpTo` 的契约 |
| 约束改写 | `ConstrExp::postProcess()` | 是否已有 `saturate → divideByGCD → simplifyToCardinality`，是否可复用独立对象 |
| 已 attach 约束 | `Constr`, `CRef`, allocators | watch/slack/reason 生命周期；禁止直接写 coefficients |
| 分解与缓存 | `ComponentManager::init`, `splitComponent`, `searchComponent` | 初始冻结副本、occlists、ClID、gap、缓存查询是否在 split 内 |
| 缓存键 | `PackedComponent`, `ComponentCache` | VarIDs + ClIDs + gap 的编码、精确比较、gap saturation |
| 两类计数路径 | `count_main()`, `solveSATFixedOrder()` | MC/PMC 与纯非投影 SAT 子求解作用域和记账 |
| 回溯与回跳 | `cancelCurDL`, `backtrack`, `bjResolve`, `solveSATFixedOrder` | 四种以上恢复路径、`limlevel` 及 cache pollution |
| 权重和 d-DNNF | `lit_weight`, `gweight`, `models_[2]`, `hasmodel_`, `ddnnf` | level-0 和新 enqueue 后的 exactly-once accounting |
| 测试/构建 | `CMake`, `Makefile`, CI, flags | 精确 CLI、已有测试、编译选项、默认模式、ASan 方案 |

必须记录 PBInstance / RoundingSat `rs::Lit`（1-based 正负整数）/ Component（0-based）转换；solver `CRef`、solver logical ID、ComponentManager `CRef`、本地 `ClID` **不能混为一个 ID**。同时核查 `PBconstraint::Int=long`、`stol`、当前 `gaps_` 位宽和涉及 `double` 的精确性风险。

### 2.2 必须画出三条真实流程

1. **无冲突计数**：进入节点 → 传播 → `syncAssign` → 分量创建及查询 cache → 投影变量分支 / 非投影 SAT → 两分支和 / 分量积 → 缓存写回。
2. **冲突及选择性回跳**：传递冲突约束 → `analyzePBMC2` → `ResolveWithoutLit` / `bjResolve` → trail/Decision/Component/cache pollution 同步。
3. **新增赋值记账**：`bpos`/`accounted_trail_head` 与 WMC/WPMC 固定投影文字权重、d-DNNF node 如何写入，是否会因新的 `continue` 漏记。

### 2.3 Cache Off 可行性审计（先回答，后实施）

禁止把设置 `-cs=0` 或只清空 hash bucket **直接当作**“安全地禁用缓存”。Codex 需要查明：

- 哪些逻辑在 `splitComponent()` 内**创建临时 CachedComponent/ID**，即使未命中也承担父子计数/污染管理职责？
- 关闭缓存时是否仍需要保留**组件栈和临时工作条目**，只是跳过 hash lookup/store？
- 非顺序回跳依赖哪些 descendant 链、`numComps`、owner/generation？
- 是否支持 cache off 基线？若没有，先增加**CacheBypass 模式**并验证；禁止简单 `return nullptr` 破坏现有状态机。
- Cache off 后是否还能对独立组件正确执行乘法、对互斥分支正确求和？

输出一张 `cache mode × simplification structural mode` 的合法性矩阵，并在源码加显式防线：不安全的组合在启动时拒绝或强制到经过测试的 Bypass，而不是静默启用旧 key。

### 2.4 本关验收门槛

- 形成 `00_code_audit.md`、本指南源码锚点修订、编译和测试命令清单。
- 至少跑通原版无改动构建、两个布尔约束最小例、可分解例、冲突例、一次 backjump 例；保存日志。
- 明确所有四种模式在当前仓库的可用性、选项名称与验证方式。
- 给出**必须先修的阻塞问题**与可以延后的非阻塞问题；禁止一次修十个旧 bug。
- 用户确认审计与修订方案后，进入第 1 关。

---

## 3. 目标架构：规则各自 `.h/.cc`，共享基础设施

### 3.1 推荐目录布局（Codex 根据真实仓库调整）

```text
core/simplify/
  SimplificationConfig.h / .cc         # 独立开关、预算，暂无 Adaptive K
  SimplificationTypes.h                 # RuleResult, ExactTerm, Proof, DirtyReason
  SimplificationContext.h / .cc        # 当前 scope、变量/投影/权重、ID 映射
  SimplificationScheduler.h / .cc      # 三阶段调度、事件/预算、统计、联合稳定
  SimplificationUndo.h / .cc           # level/branch marker 与 delta rollback
  ResidualFormulaView.h / .cc          # frozen base + reversible overlay
  RowActivityBounds.h / .cc            # 共享单行/双行活动界计算、精确整数
  ProofReasonBridge.h / .cc            # PB reason、conflict、attach 的安全接口
  ProbeEngine.h / .cc                  # 共享假设、传播、完整回滚；规则不在此重写
  CacheBypassAdapter.h / .cc           # 完全跳过旧 hash 命中/写入，保留计数栈
  rules/
    BasicCheckRule.h / .cc
    AssumeProbingRule.h / .cc
    FailedLiteralRule.h / .cc
    CoefficientSaturationRule.h / .cc
    GcdReductionRule.h / .cc
    CardinalityDetectionRule.h / .cc
    PairwiseMultiRowSCRule.h / .cc
    IndividualRowRule.h / .cc
    ParallelRowRule.h / .cc
    SubsetRowRule.h / .cc
    RupRedundancyRule.h / .cc
    SbLpRule.h / .cc                    # 延后且默认关闭
    ScLpRule.h / .cc                    # 延后且默认关闭
    RirLpRule.h / .cc                   # 延后且默认关闭
```

**UP 和 RV 不重新实现为第二套独立求解器**：原生 UP 由 RoundingSat 执行，RV 由 trail + residual view 映射完成。允许 `PropagationBridge.h/.cc`、`ResidualAssignmentView.h/.cc` 作为封装（依照真实代码结构确定）。每个真正新增/可单独消融的规则依旧拥有自己的 `.h/.cc`。

### 3.2 规则接口（仅概念契约）

```cpp
struct RuleResult {
    bool progress = false;             // 只表示已验证并提交的数学进展
    bool topology_changed = false;
    bool coefficient_changed = false;
    bool budget_exhausted = false;
    TerminalStatus terminal = Continue;
    std::vector<CheckedImplication> implied;
    std::vector<CheckedDerivedConstraint> entailed;
    std::vector<RowDelta> deltas;
    std::optional<CheckedConflict> conflict;
    CountFactor factor;                 // 默认为 1，带 scope/owner
    RuleWorkCounters work;
};

class ISimplificationRule {
public:
    virtual RuleResult apply(SimplifyContext&, WorkBudget&) = 0;
};
```

`progress` / delta / implication 可同时出现，不能只用一个互斥 enum 丢掉内容；只有终止状态互斥。**规则只提出数学后果，统一 adapter 事务校验并提交**。提交顺序包括证明校验、scope 检查、overlay 更新/attach、enqueue、propagate、trail accounting、拓扑/缓存身份刷新。规则未得证明时不得生成假 reason。

### 3.3 三个阶段的固定执行顺序（目前无 K）

```text
EnterCurrentCountingScope()
  ↓
Native UP → residual RV → BasicCheck   [必须完成]
  ↓
I. Variable Simplification
  ├─ AssumeProbing (AP)
  └─ FailedLiteral (FLP)               [共享 ProbeEngine；独立开关]
  ↓  若产生正式赋值，原生 UP 到稳定
II. Coefficient Simplification
  ├─ CS
  ├─ GCD
  ├─ Cardinality Detection           [用户选择 Version A]
  └─ Pairwise Multi-row SC            [候选限制 / 预算 / 有效证明]
  ↓  只对改动行再 NormalizeA；有合法新后果才调用 native UP
III. Constraint Simplification
  ├─ RIR / BasicCheck status
  ├─ RPR
  ├─ Generalized RSR
  └─ RUP-based redundancy             [候选限制 / 预算]
  ↓
如有真实新赋值/改写/删行，回到 I；否则停止可选规则
  ↓
Finalize trail accounting + residual identity
  ↓
Component decomposition → cache lookup (若安全启用) → branch/count
```

`BasicCheck` 可提前检查恒真/冲突（必要维护），Stage III 的 RIR 负责正式管理冗余行的失活或删行，避免两个模块重复提交同一次删除。Stage I 完成 mandatory propagation，Stage II/III 出现新赋值后直接回到 I。重复规范化只处理受影响的行。

### 3.4 版本 A：NormalizeA 的定义

```text
NormalizeA(row):
    verify row operator (>=; equality first expanded or equality-specific)
    check 0 >= gap / gap > U / empty row
    CoefficientSaturation(row): a_i <- min(a_i, gap)  [gap>0]
    GcdReduction(row): g=gcd(a_i), a_i/=g, gap=ceil(gap/g)
    CardinalityDetection(row): iff certified equivalent, convert coefficients to 1
    check 0 >= gap / gap > U / newly implied literals
```

若 `CardinalityDetection` 已由 RoundingSat 原实现准确覆盖，应直接封装调用，不复制另一套未验证算法。确认其实际行为（例如是否有更强的 cardinality 变换），将独立规范化规则用于“派生 residual view”，不要误将 view 改写自动视为 solver `Constr` 已改写。

**注意**：CS → GCD 对固定的规范不等式一次即可达到二者不动点；当 Multi-row SC 或赋值实际改变行时，再对变化行调用 NormalizeA。不是在 CS/GCD 之间无上限循环。

### 3.5 固定预算、禁用 K

建议配置为 `--inprocess`, `--inprocess-rules=...`, `--inprocess-budget=...`, `--inprocess-cache=legacy|bypass|residual-v2`, `--inprocess-dry-run` 等，但首次审计须按真实 CLI 风格重命名。三阶段默认执行顺序固定；规则各有预算。预算耗尽仅跳过可选简化，**绝不跳过原生传播、必要回溯、exact accounting 或证明校验**。

---

## 4. 关卡 1：建立空简化模块与原生传播接入

**规则**：仅建立基础设施，不新增数学变换，不动旧根预处理。

**开发任务**：

1. 保留 `PBcounter::count_main()` 的冲突/回跳/分支核心逻辑；找到“传播完成且无冲突、`splitComponent()`/cache lookup 之前”的真实插入点。
2. 将旧空 `simplify()` 替换或绕开为 `SimplificationScheduler::runNode()`；删除不合适的条件调度只在此单独测试；默认 `--inprocess=off` 行为不变。
3. 使用 `RuleResult`、`SimplifyContext`、CurrentComponentScope、UndoMarker、RuleStats 等契约；空调度不制造赋值、不生成临时缓存 key。
4. 单独完成 `trail_refresh_head` 与 `accounted_trail_head` 设计，保证简化后新增 trail 会被 WMC/WPMC 和 d-DNNF **恰好一次**记账；level 0 已在 trail 中的固定投影文字同样有测试。
5. 实现 `CacheBypassAdapter` 的最小版本（可以在第 3 关前结束，但审计必须先定义）。保证此处没有破坏 component/Decision 栈和求和、乘法、回跳清理。
6. 建立调试日志：`node_id, level, component_id, rule, before/after trail_size, outcome, work, cache_mode`。

**本关测试**：所有规则关闭时，新旧 PBMC 在同测例上的 MC/PMC/WMC/WPMC 数值相同；原生传播不减少；decision/component/cache 行为无无法解释的差异；valgrind/ASan 基础测试不过不进入下一关。

**验收**：产生独立提交、`acceptance/01_skeleton.md`，附编译命令、最小模型对照、已知边界。用户批准后开始变量简化。

---

## 5. 关卡 2：变量简化——逐个实现

### 5.1 关卡 2A：BasicCheckRule（独立 `.h/.cc`）

- 读取当前有效 residual row 的 `gap`、未赋值贡献 `U`、`slack=U-gap`；`gap<=0` 标记当前已满足，`U<gap` 返回 conflict，`a_i>slack` 识别可能 forced literal。
- 首选已有 RoundingSat watched propagation，不要在 BasicCheck 中第二次直接写 `assigns`。若需提交新蕴含，必须有合法 PB reason；不可把 `decide()` 当作传播。
- 支持负文字与 equality 两方向，且必须去除同变量互补和重复的影响。BasicCheck 自身不做正式 structural delete，交给 RIR Rule 的 overlay 阶段。
- 将 fixed 文字代入**只读残余视图**；将 RV 作为表示功能维护，而不是实现另一套 physical matrix column delete。

**测试**：`5x+2y+z>=4` 强制 x=1；`2x+3y>=6` 冲突；`2x+3y>=0` 恒真；`2x+3y>=3` 仅强制 y=1；负文字 `3~x+2y>=3` 强制 ~x=1（需检查示例是否有其他强制项）。基线无重复固定/权重；回跳后恢复。

**验收门槛**：规则独开时 oracle 一致、所有强制文字有合法推导、未改变原始结构 key；然后再做 AP。

### 5.2 关卡 2B：ProbeEngine + AssumeProbingRule（独立 `.h/.cc`）

**与现有 PBMC 不同**：现有前端 probing（指南指出只看二项 PB 约束，并存在 `double` 与同变量重复项风险）不是完整的双分支传播式 probing；新模块在当前合法作用域内分别临时假设 `x=0` 和 `x=1`，执行 native propagation 到不动点，记录各分支**经证明**的赋值及冲突、状态是否耗尽预算。

- `ProbeEngine` 只负责一次假设、传播和完整 rollback，**AP 与 FLP 共享结果**，避免重复试探；不得复用没有完成状态恢复的 checkpoint helper。
- `AssumeProbingRule` 只处理双侧均被完整传播且有公共蕴含时，提交该蕴含；当某侧预算耗尽，标记 UNKNOWN，不假定该侧 SAT/UNSAT。
- 探测副作用必须恢复 `trail`, `trail_lim`, `qhead`, watches/slack、probe-level Reason、components、overlay、accounting、d-DNNF 临时状态；可以采用独立轻量临时 solver，但要先测试与 live solver 公式同步。
- 提交的非根蕴含需要全局合法 reason 或充分的决策前缀 guard，不能仅靠 AP 两侧局部结果直接向 `Reason[v]` 写伪造 `CRef`。

**示例**：`x+y>=1`, `~x+y+z>=2`, `~y+t>=1`；两侧均推出 `y=1,t=1`，新 AP 可以提取公共蕴含。验证不发生 trail 泄漏、不会对 y/t 重复计权。

**验收门槛**：正反 probe 状态逐字段恢复、UNKNOWN 安全、所有 4 种计数模式正确、可复现收益统计；再做 FLP。

### 5.3 关卡 2C：FailedLiteralRule（独立 `.h/.cc`）

- 从同一个 `ProbeEngine` 获取两个方向传播结果。
- 假设 `l` 产生**经确认的冲突** → 当前上下文推 `~l`；两方向都冲突 → 当前上下文无解；没有冲突则不据此固定变量。
- 不允许把“没有发现冲突”当 SAT；未完成的探测为 UNKNOWN。
- 非根冲突必须遵守 PBMC `limlevel`、选择性回跳和 counting scopes；若无法生成可用于非顺序回跳的冲突证明，只能采用已证实合法的局部顺序回溯策略，不能裸调用标准 CDCL 的 `backjumpTo`。
- 两项规则各自可单独开关，ProbeEngine 保持唯一。

**最小例**：`x+y>=1`, `x+~y>=1`；令 `x=0` 后推出 `y=1` 与 `~y=1`，所以固定 `x=1`。再测两方向都冲突、无冲突、UNKNOWN、深层 backjump/组件边界。

**验收门槛**：通过全部 probing rollback、reason、cache legacy（只产生赋值时）和四模式对照后，变量阶段暂时冻结。

### 5.4 本阶段 SB-LP 暂不做

把 `SbLpRule.h/.cc` 加入**后期 LP 专项关卡**；现在不要把旧 EDPLLSim 使用的 GLPK double 直接迁移到 PBMC 中用于固定布尔变量。必须先解决 exact certificate 及安全回退。

---

## 6. 关卡 3：系数简化——先只读，再实际结构替换

> **重要工程顺序**：从这一关开始明确引入两种模式：
> 1. `view-only / dry-run`：从 frozen row 和 trail 构造规范 residual view，**不修改**已经 attach 的 `Constr`，原 cache key 仍基于冻结公式；视图仅供规则自身筛选/统计，不能让 ComponentManager 在未证明对齐前以为 solver 已改写。
> 2. `committed-rewrite`：真正改变当前 residual 约束的系数、degree、支持集，需要事务式 overlay、对应的传播/证明契约与回溯；**此时必须使用 CacheBypass**，直到后续 residual-aware cache 关卡验收通过。
>
> 单纯设置 `cache=off` **不能**解决 solver 与 ComponentManager 不一致；每条实际改写先做完整的“公式表示一致性”测试，才允许计数主循环使用。

### 6.1 关卡 3A：CoefficientSaturationRule（独立 `.h/.cc`）

- 仅对规范正系数 `>=` 残余行执行 `a_i <- min(a_i,gap)`；gap≤0 交由 BasicCheck。**不要对未展开的 equality 直接饱和**。
- 已有 `ConstrExp::saturate()` 优先复用，不再维护手写第二套与它行为不一致的数学实现；区分 `coefficient saturation` 与 cache 的 `gap saturation`。
- 测试 `8x+3y+2z>=5 ↔ 5x+3y+2z>=5`、负文字 `8~x+3y+2z>=5`、已赋值后 residual gap 减少、大整数溢出、`gap>sum` 冲突。
- 首次以 view-only 正确性为验收，然后在下一次**独立变更**中启用 committed residual rewrite（须先通过 CacheBypass + overlay 基础关卡）。

**验收**：数学等价验证、与 RoundingSat 原 `saturate()` 比对、cache Bypass 真正生效、实际 rewrite 前后计数一致。

### 6.2 关卡 3B：GcdReductionRule（独立 `.h/.cc`）

- `g=gcd(a_i)>0`；新系数 `a_i/g`，新 gap `ceil(gap/g)`；对正负 gap、极大整数、空行、互补文字严格测试。GCD 使用**规范化后当前整数表示**，不要混用一般 `<=` 形式需要的 floor。
- 单行测试 `8x+4y+2z>=5 ↔ 4x+2y+z>=3`，再测另一方案 `CS → GCD`；项目正式顺序仍选用户版本 A。
- 区分 `GCD for actual residual rewrite` 与 `GCD solely for key canonicalization`，两条路径不能偷偷互改母版。

**验收**：单项可开关；序列化前后的 exact 数值一致；同一行 repeated call 幂等；commit 路径的 cache/undo/propagation 契约通过。

### 6.3 关卡 3C：CardinalityDetectionRule（独立 `.h/.cc`）

- 优先封装 RoundingSat 已有 `simplifyToCardinality(true)`；具体条件以审计后的真实实现为准。
- 数学充分条件示例：按系数升序，若最大的 `q-1` 项总和 `< gap` 且最小的 `q` 项之和 `>= gap`，则等价于 `sum(l_i)>=q`（空和为 0）。
- 例如 `3x+3y+2z>=5 ↔ x+y+z>=2`。同时验证**非 cardinality** 反例，避免误检。
- 新行如果成为 Clause/Cardinality，必须检查 solver 使用何种 `Constr` 具体类型及 watch/slack 初始化，不能只是改一个 type tag。

**验收**：比较每个完整赋值真值及四模式计数，保持负文字含义，不破坏 reason、rollback、ComponentManager 状态。

### 6.4 关卡 3D：PairwiseMultiRowSCRule（独立 `.h/.cc`，非 LP 第一版）

数学目标：

$$
C_i:a_j\ell_j+R_{ij}\ge gap_i,\quad R_{ij}=\sum_{t\ne j}a_t\ell_t.
$$

利用**不包含目标行 $C_i$ 的仍有效辅助约束集合**（首版仅一条 $C_h$）证明 $R_{ij}\ge L_{ij}$，其中 $L_{ij}$ 为在当前作用域内有效的**整数下界**。若 $0\le L_{ij}<gap_i$，则可用：

$$
a'_j=\min(a_j,\,gap_i-L_{ij}),\qquad gap'_i=gap_i.
$$

若 $L_{ij}\ge gap_i$，目标行已被其他保留约束蕴含，应交由冗余行规则删除，而不是把目标行无证明地“简化为空”。**辅助行必须保持有效，防止后续删除产生循环依赖**。公式对当前作用域的布尔满足赋值保持等价；如使用局部上下文作为依据，退出该作用域必须 undo。

**通用双行活动界**：将负文字统一写成 `$1-x$` 后，对于任意固定非负有理比例 $\lambda$，

$$
R_{ij}=\lambda L_h+(R_{ij}-\lambda L_h)
\ \Rightarrow\ 
R_{ij}\ge \lambda gap_h+\min_{x\in\{0,1\}^n}(R_{ij}-\lambda L_h).
$$

右侧差值的最小值按独立变量取端点求得，因变量域包含辅助行之外的全部 Boolean cube，所以它是**安全的下界**；再对整数值的 $R_{ij}$ 向上取整，可得到有效整数界。比例先尝试 $\lambda=1$、公共同极性系数比等少量候选，不用未证实的浮点比较。也可以用公共文字求界，非公共辅助文字按最有利取值消除。

**候选筛选与开销**：
- `occ` 只找与目标余项 support 相交的活动辅助行；不要求两个 support 完全一致。
- 若辅助独有项已经足以满足自身，则该辅助行可能无增益；快速过滤但不可误报为已证明无增益（保守跳过仅损失性能）。
- 每目标行最多 Q 条辅助行，每对最多 R 个比例，每次扫描有 nnz/时间预算；记录尝试数、成功缩紧数、删除 nnz、proof 成本。
- 首版不允许新增非零项，不实现一般 Lifting；跨多行联合/条件式按目标变量赋值的强化放到后续研究。
- 一条辅助行变化应使相关**未变化**的目标行重新获得候选机会；逆依赖或保守按共同 support 入队，不只看两条都 dirty 的行。

**测试**：
```
C1: 8x+3y+2z>=5
C2: y+z>=1
=> C1: 3x+3y+2z>=5   (C2 保留)
```
在整系统下验证恰有 4 个完整模型；再测不匹配 support `C1:8x+4y+3z+2w>=6`, `C2:2y+2z+t>=3`，对 x 的系数可缩到 3；无公共支撑不得虚假缩紧；同一行作为自己证明前提必须拒绝。

**验收**：视图版与 commit 版分别测；至少证明对所有受测试完整赋值，`F_before` 与 `F_after` 等价；100% rollback、cache Bypass、主计数四模式、分量拓扑对齐后才可进入约束阶段。

### 6.5 不启用 SC-LP

留给后期 LP 关卡；命名为 Gurobi §5.4 多行系数缩紧的 LP 实现候选（简称 SC-LP 是本研究暂定的工作名）。不能将任何没有精确认证的 LP 下界向上取整后直接替换系数。

---

## 7. 关卡 4：约束简化——逐个实现

### 7.1 关卡 4A：IndividualRowRule（RIR；独立 `.h/.cc`）

- 将 BasicCheck 提供的 `gap<=0` 恒真、`gap>U` 冲突状态转成**当前作用域可撤销的结果**。BasicCheck 不重复正式失活；RIR 只在一次正确验证后提交 active-mask 的变化或冲突返回。
- 已满足行本来就被 `searchComponent()` 忽略，因此先验证本规则是否仅做已有功能的增量维护；不必为了“技术数”制造重复扫描。
- 禁止永久删除一个只在当前分支已满足的约束；undo 后必须恢复。

**验收**：满足约束不再连接分量、回跳后恢复；BasicCheck 和 RIR 不重复计算收益或因子。

### 7.2 关卡 4B：ParallelRowRule（RPR；统一实现在 `simplify.h/.cc`）

- 将 canonical 后的相同 LHS（相同 literal/系数，不仅 support 相同）约束分组，保留最大 RHS 的 `>=` 约束，删除较弱行。正比例缩放要经过精确整数归一化。
- `same LHS`, `different RHS` 简单例：`2x+2y>=2` 与 `x+y>=1` 规范化后重复；`x+y>=2` 蕴含 `x+y>=1`，应保留前者。
- 对互补方向，不得把不一致等式当作普通平行冗余删除；检查约束方向和 canonical literal polarity。
- hash 只筛选 bucket；**完整行内容+degree 精确比较**后才能删除。任何写入 overlay 的删除要通过“当前其余保留公式蕴含目标行”验证。

**验收**：行重排/哈希碰撞/取反文字/多 RHS/等式展开测试；无 false delete；活动行数、residual cache identity 与回跳恢复正确。RPR 的正比例行具有相同 literal support，保留行仍连接相同变量，因此 **RPR 本身不应被要求增加分量数**；删除不同 support 的桥接冗余行并产生新分量属于下一关 RSR 的验收范围。

### 7.3 关卡 4C：SubsetRowRule（Generalized RSR；独立 `.h/.cc`）

- 先实现传统 support subset 规则的精确 PB 充分条件，再实现“一般系数/极性/不同 RHS 的双行差值活动界”扩展；每一层单独验收。
- 对 `C_i:L_i>=k_i`（保留）与 `C_j:L_j>=k_j`（拟删），若能证明

$$
\min_{x\in\{0,1\}^n}(L_j-L_i)\ge k_j-k_i,
$$

则 `C_i` 蕴含 `C_j`，可删 `C_j`；允许有常数项和正负文字，按逐变量有符号系数计算最小值。带正比例的版本另作为增强选项，不与基础版同时改。
- 示例 `3x+2y>=3` 蕴含 `4x+3y+z>=3`；反例 `3x+y>=3` 与 `x+3y>=3` **互不蕴含**，不得删。
- 在某当前公式中按顺序删除时每次证明基于“尚保留的其余约束”；禁止两条互相蕴含的行在同一批次都因对方存在而被删。
- 不把“RSR 调用成功”自动等同于新增 decomposition；实际测新连通分量的变化。

**验收**：精确 entailment oracle、反向极性、不同 RHS、固定候选对/文字预算、提交前重算、undo、四模式、缓存 Bypass。不同 support 的一般蕴含不能用“同 LHS hash”作必要过滤条件；后续若增加候选索引，必须以完整全对扫描作正确性 oracle。

### 7.4 关卡 4D：RupRedundancyRule（独立 `.h/.cc`）

目标是证明目标约束被**其余保留约束**蕴含。对 `C:sum a_i*l_i>=k`，`U=sum a_i`，它的精确布尔否定为

$$
\neg C:\sum_i a_i\bar\ell_i\ge U-k+1.
$$

- 构造**临时独立的 propagation/proof context**：从活动集合去除目标 `C`，加入 `¬C`，执行 RUP 的 UP 闭包；冲突则允许删除；没有冲突或预算耗尽只表示**RUP 未证明**，不能判定该行非冗余。
- 禁止借助目标行本身、由目标行派生且失去独立证明的 learned constraints 或两条同时删除的行形成循环论证。使用干净的当前保留公式/证明依赖集合。
- 递归局部冗余只在当前 scope 有效，提供 active-mask、undo 和完整 proof；原 solver 中仍可能保留原行作 propagation，必须检查跨组件语义和冲突分析。
- 样例：`x+y>=1`, `x+~y>=1` 蕴含 `x>=1`；删除目标行 `x>=1` 后用 `~x>=1` 进行 RUP，传播导出 `y` 与 `~y` 冲突。
- 后续可探索 PBCounter DeleteConstraint 的 ADD 假路径，但与 RUP 是不同实现，不属于本关。

**验收**：有证明删除、无证明不删、目标行依赖污染、含负文字/等式/当前分支、deep undo/非顺序回跳、MC/PMC/WMC/WPMC 全通过。

### 7.5 SB-LP、SC-LP、RIR-LP 尚不入主干

LP 三项将在结构简化与 cache 改造后独立关卡各自实现。不将它们捆绑到同一个 PR 中，也不预设它们优于 AP/FLP、Multi-row SC、RUP。

---

## 8. 必须实现的增量调度、版本和工作量预算

### 8.1 三个不同概念

- `changed`：**已经提交**的新赋值、等价行改写、有效删行/失活所产生的数学进展；只读 cache normalization 不是 committed rewrite。
- `dirty`：旧活动界、gap、签名、候选检测可能失效，需要重新检查；dirty 不保证会有新化简。
- `revision`：一次具体状态的身份；约束内容、有效作用域或影响该规则的前提变化后递增/生成新身份；回溯不能复用同一个 revision 解释不同的行（防 ABA）。

### 8.2 为什么多行规则不能只处理变化的目标行

假设辅助行 `C_h` 变了但目标行 `C_i` 未变，则它给 `R_ij` 的下界可能收紧或失效，必须重新考虑 `C_i ← C_h`。为此维护 `var → rows` occurrence、`row → candidate partners` 或保守的 scope epoch；一开始允许多做检查，不允许过度剪枝造成不可预测错误。可选简化没有发现机会只影响性能，但所有影响**实际当前公式身份/冲突/计数**的更新必须准确。

### 8.3 事件调度伪代码

```cpp
void onCommittedAssignment(Lit p) {
    for (RowId r : occurrences(abs(var(p)))) {
        markResidualStateDirty(r);       // 正/负文字两极都要考虑
        invalidateRuleCandidateVersions(r);
    }
}

void onCommittedRewrite(RowDelta d) {
    validateEquivalenceAndScope(d);
    commitReversibleOverlay(d);
    markDirty(d.row);
    wakeAffectedCandidatePartners(d.row);
    bumpMonotonicRevision(d.row);
    markTopologyAndCacheIdentityDirty(d);
}

void onUndoToLevel(int level) {
    restoreReversibleOverlay(level);
    resetOrRebuildDirtyQueues(level);
    invalidateCandidateStampsAcrossBranches();
    restoreAccountingAndOwnerMarkers(level);
}
```

按规则维护 dirty 位图/处理 revision；不得让 BasicCheck 清空同一 dirty 队列后，CS/RPR 看不到变化。建议 `dirty_masks[rule_family]` 或独立队列。失败的 pair/目标 row 检查记录 `(targetID,targetRevision,helperID,helperRevision,scopeEpoch)`；**无变化、无新公式信息**的重复尝试应跳过。新赋值/改写后允许重新尝试；预算归当前节点，不因外层 `repeat` 重置。

### 8.4 工作量上限（先用固定参数，后期自适应）

- 每节点 AP/FLP 最大 probe 次数和传播访问数；
- Multi-row SC 每行最大 Q 条辅助行、每 pair 最大 R 个候选比例、总 nnz 访问上限；
- RSR 每行/每节点最大 pair 比较数；
- RUP 每节点最大目标行数、UP 访问数；
- 单节点、整实例总体时间或工作上限；
- 执行结束标记 `complete_fixpoint` 或 `budget_limited`，不把预算耗尽误说成理论不动点。

**注意**：候选选择、候选数、探测时间本身也属于成本统计。`m^2` pair 可通过 occurrence、top-Q 候选、共享活动界计算降低，但“平均复杂度下降”必须由真实测量支持。

---

## 9. Reversible Overlay、Reason、RoundingSat 与分解一致性

### 9.1 四层事实源

1. `FrozenFormula`：ComponentManager 初始化时复制的不可变原始约束与局部 ClID；
2. `SolverFormula`：RoundingSat 中已 attach 的 `Constr`、watch、slack、Reason、learned；
3. `ResidualOverlay`：当前 decision scope 中已证明等价的行改写、失活、局部公式身份、依赖与 undo；
4. `Accounting/Cache`：当前投影/权重因子、ComponentKey、缓存 value 与分支/分量作用域。

**硬约束**：四层的语义必须一致。只改 RoundingSat 不改 ComponentManager，可能无法获得分解收益；只改分解图而不保留有效的证明前提，可能产生错误分解；只改 overlay 后还使用旧 key 可能误命中。

### 9.2 具体要求

- 实际 rewrite 必须与当前作用域原公式**等价**，且在本作用域始终保留其依赖前提；如果用添加全局被蕴含的 constraint 代替一部分结构 rewrite，应保证添加的是原公式的真正 consequence。
- **不原地修改 attach Constr**。可让旧 solver 原约束继续作为传播冗余约束，但必须证明在新 residual semantics 下不会跨越受限的计数组件边界错误回跳；如果不能证明就暂不让此改写影响分解，或采用安全的重建方案。
- `CompMgr` 计算分量、动态启发式、cache builder 必须读取**同一 residual view**；构造 cache identity 不允许用新 gap + 旧 coefficients template。
- `Reason[v]` 的目标需使用其对应 allocator 的当前合法引用；GC、backjump 后不能保留失效裸 `CRef`。
- 多行删除的辅助前提必须在仍保留行集合中有效。若某行计划被删除并且其他行证明依赖它，必须禁止该组合或重新证明。
- `ProofReasonBridge` 应适配 PBMC 自身选择性 backjump/`limlevel`，不能直接调用标准 SAT solver 的 unrestricted 回跳 API。
- Probe 的临时约束、watch、trail、reason 必须完整撤销；其结果不应自动被永久 learn 到全局。

### 9.3 分量图处理准则

当某条行变成恒真或已证明被其他行蕴含时，可在当前 overlay 中失活；只有当 `searchComponent()` 的遍历真正忽略这条行时才可能新增分解。测试示例：

$$
8x+3y+2t\ge5,\quad 5x+3y+2t\ge5,\quad
5x+3y+2t+z\ge5,\quad z+w\ge1.
$$

CS 后 RPR 删除重复行，RSR 删跨分量桥接的冗余行，得到 $(x,y,t)$ 和 $(z,w)$ 两个独立分量，总模型数 $5\times3=15$。**必须在缓存关闭时也通过这个实例；仅返回相同 count 而没有新增 split 不算实现了“分解导向”的目标。**

---

## 10. Cache 迁移专项：允许暂时关闭，但要有关卡

### 10.1 用户问题的直接答案

**可以**：先完成变量简化（只产生正式 trail 赋值），此时如果没有结构改写，通常仍可使用现有 `VarIDs+ClIDs+Gaps` key；从正式启用**系数、文字或约束失活**起，先将 hash cache **完全 Bypass**，逐个完成算法、分解和回跳验证，再改 Cache key。**但必须先证明 CacheBypass 的组件生命周期正确，且不能把“关缓存”当成解决 solver/ComponentManager 不一致的手段。**

**注意区分**：`view-only` 系数规范化并未改变真实公式，原 key 可继续代表冻结公式（但不要让 view-only 的新拓扑影响分解）；`committed-rewrite` 需要 Bypass 或已验证的 residual-aware key。实际上 RIR 如果只是现有 `gap<=0` 时跳过行，旧键仍可正确；RPR/RSR 的动态 active-mask 删行则应视为结构变更，进入 Bypass。

### 10.2 按阶段允许的模式矩阵

| 阶段/改变 | Legacy cache | Bypass cache | Residual-v2 cache |
|---|---|---|---|
| 空模块、原生传播、逻辑 RV | 允许（先测试） | 允许（先测试） | 未实现 |
| AP/FLP 仅产生已证明赋值 | 通常允许（测试后） | 允许 | 未实现 |
| CS/GCD/Card view-only | 允许：仍按冻结原公式建 key | 允许 | 不必 |
| CS/GCD/Card 实际改系数/支持集 | **禁止** | **允许，前提是 overlay/solver/compmgr 已一致** | 将来允许 |
| RPR/RSR/RUP 动态删行并改分解 | **禁止，除非正式证明兼容旧身份** | **允许，前提是 overlay 正确** | 将来允许 |
| 任意变量代换/消元 | **禁止** | 仍须额外 ledger/证明，本轮不做 | 将来专项 |

实现启动阶段 assert 或配置层验证所有组合；**避免默默使用旧键**。CacheBypass 必须同时跳过 lookup、insert/store 和所有把旧 hash key 作为计数真源的路径，但保留必要的临时 component、Decision、污染清理与 ownership 数据结构。

### 10.3 EDPLLSim 缓存方案可以迁移什么

用户 EDPLLSim 现有 cache 的成熟经验可提炼为：

- **基于当前真实内容的 exact component identity**，不能用原始 row ID 或 `SC=true` 代替系数结构；
- 所有行先做数学上等价的 canonical normalization，再做无歧义序列化；
- hash 只定位 bucket，命中必须完整 key equality；碰撞不能 false hit；
- 若需要共享行模板，按**immutable canonical literal/coefficient 内容** interning；不同 RHS/gap 分开放到 component key；
- 新模板先内容比较，再给唯一 TemplateID；lookup 可以 probe-only，不在 miss 时永久创建模板；
- 模板 ID 在所有引用其内容的 live key 释放前不可复用；eviction/backjump 必须正确管理 refcount 和 ownership；
- cache key 的生成不得为了命中率去改动 solver 中原始系数；Gap saturation 是 key-only，不是实际 CS。

**不能照搬** EDPLLSim 的整数域字段、`<=` row GCD/floor、局部列重命名、`N*M<=20`、age 常数和存储格式；PBMC 自身已有 stable ClID、差分打包和 gap saturation。需先度量现有 ClID 能否作为 immutable template，避免无收益地建立第二套池。

### 10.4 新组件身份的两个可选实现

**方案 A：hybrid immutable template identity（推荐先实现）**

```text
ComponentKeyV2 = (
    component_variable_identity,         // 稳定变量 ID/投影/权重 scope
    ordered active residual rows {
        immutable_template_id,           // literal + positive coefficient content
        residual_gap                   // exact integer, safe key-only PB gap saturation
    },
    required_scope_or_projection_metadata
)
```

- 旧未改变约束可以用原 immutable row template；实际改写后新建**不同内容**的 immutable template；行多次变化时不能复用旧模板 ID 代表不同系数；
- 一条源行在同一节点分裂成多个派生行时，key 编码实际 active set，而不仅是 source ClID；
- 对不同作用域中相同 Boolean residual，只有变量身份/投影权重语义相同才复用；
- `residual_gap` 必须按其 template 的当前未赋值文字计算，再执行安全的 key-only gap saturation；
- template registry 记录 immutable 的 literal 极性、系数、版本及来源（来源用于调试，不应决定数学相等）。

**方案 B：full canonical residual formula（后期增强）**

```text
ComponentKeyV2 = (
   ordered residual variables,
   sorted canonical active rows [(literal, exact coefficient, exact gap), ...],
   projection/weight/elimination metadata needed by count semantics
)
```

方案 B 存储较大但数学语义直观，不依赖稳定模板 ID 可快速验证；可以先做 `ReferenceKey` 作为调试 oracle，再压缩成 A 的 packed 实现。**强烈推荐在发布压缩版前让 A 的每次命中与 ReferenceKey 完整一致**，或使用离线逐项检查。

**重要**：不要把 `revision`、`changed`、branch timing、K、预算、启发式顺序放进数学 key；它们是运行历史，不影响 residual 计数。允许由于不同简化路径造成不同 canonical key（损失命中），绝不允许不同数学剩余系统得到“伪相同”的 key（错误命中）。

### 10.5 模型计数与缓存值的作用域

推荐约定：**cache value 表示该 key 所描述组件的完整、尚未乘入外部作用域因子的计数**。有些计数因子属于该组件输入状态，有些因子属于其父 branch；必须确定 ownership，确保 cache hit/miss 都只应用一次。

- 互斥分支是**求和**；不共享变量的独立组件是**求积**。
- 固定变量在 MC 中贡献因子 1；自由变量在 MC 中因子 2；PMC 的自由非投影变量因子 1。
- WMC/WPMC 的固定投影变量应用被选文字权重一次；WPMC **不能乘非投影变量**的权重。
- `hasmodel_` 不等于 count 非零：零权重模型仍可能满足。
- 缓存命中不能重复应用进入组件前已经结算的 trail/自由变量因子；缓存写入不能包含并不属于该 key 的父作用域因子。

### 10.6 Cache v2 专项关卡顺序

1. **缓存关闭基线**：在没有结构改写情况下验证 Bypass 与 legacy（禁用/启用）所有小实例答案一致；确认分支加法/组件乘法、深层回跳、缓存污染清理不出错。
2. **ReferenceKey**：构造当前真正用于分解的完整 canonical residual formula，包含正负文字、精确系数、gap、活动变量与权重/投影语义；做碰撞与唯一性测试。
3. **V2 builder**：从 overlay 读取真实结构，生成 immutable template ID + gap 或完整 canonical bytes；保证 key 与 solver/compmgr 语义一致。
4. **V2 cache lookup/store**：hash→full compare；cache value/factor owner 对齐；提供详尽统计。
5. **Backjump/eviction**：模板生命周期、GC、缓存污染、descendants/ownership、极小 cache limit 压力测试。
6. **与 Bypass 差分**：四模式小实例无任何数值差异；随机实例、深回跳、巨大整数、hash 强制碰撞、不同历史同式、同 ID 不同系数全部通过。
7. **正式重新启用缓存**：仅在 v2 全套验收通过后允许生产/论文测评使用。

### 10.7 缓存必须具备的反例测试

- **同一 frozen ClID + 同一剩余变量集合 + 同一 residual gap，但系数内容不同**，必须产生不同 V2 identity（或经 exact canonical equivalence 证明相同），不能命中错误结果。
- 例如原始 `4x+2y+2z>=4` 在 `z=0` 与 `z=1` 两个分支分别留下 `4x+2y>=4` 与 `4x+2y>=2`，后者可饱和为 `2x+2y>=2`。这个例子首先用于检查**母版内容与所使用的 gap 必须配套**，并不声称两个不同 gap 会在原生旧键上直接碰撞。更强的专项反例由 Codex 构造：**同一被复用源 ClID、同一剩余变量列表和数值 gap，但不同真实派生系数/依赖前提，且对应 residual 模型数不同**；用 ReferenceKey 区分，并证明旧键在这类动态结构状态下不足以唯一确定计数。若无法构造该反例，必须说明何种冻结母版约定可以保持旧键正确，不能仅凭“修改过系数”就断言一定出现 false hit。
- 当前母版相同、投影身份不同或 WPMC 权重映射不同，不得随意复用；与 cache 不同的问题是同一作用域下的合法值复用，不能把入口已计权前缀再应用两次。
- 不同约束哈希强制同一 bucket，完整 key 不同不得 false hit；不同 canonical 形式但布尔语义相同可不命中（性能损失可接受）。

---

## 11. 关卡 5：LP 专项逐项实现（全部默认关闭）

> 本关可以在 Cache v2 后进行；如果只做 LP dry-run 不写 solver，可提前完成数学评估，但**没有可核验证书不得 commit**。与自适应 K 完全独立。LP 问题不允许复用 EDPLLSim 已知存在 double→int 误判的旧路径。

### 11.1 SB-LP（独立 `.h/.cc`）

- 使用当前作用域完整或选定的约束子集计算 `min x`, `max x` 的 LP bounds。只在对**原布尔可行集**有效的外界经过精确证书确认时，结合整数性固定变量。
- 例如 `min_LP x >0` 可推出布尔 `x=1`，`max_LP x <1` 可推出 `x=0`；必须证明是**严格有界**，不能靠接近 0/1 的容差。
- 为非根推断生成可用于原 PBMC conflict protocol 的 reason，不能只返回 `bool`/直接改 assigns。

**验收**：LP solver 与精确独立 oracle、临界大系数 `3e9` 类反例、浮点无法证明时 UNKNOWN、不影响权重、回跳路径。

### 11.2 SC-LP（独立 `.h/.cc`）

- 仅是 Multi-row SC 的 LP 求界版本，并非 EDPLLSim 原有的独立 SC。对目标 `a_j*l_j+R>=gap`，利用其他保留约束（不包含目标行自身）在 LP 松弛上求 `min R` 的**精确有效下界**；利用 `R` 的整数性作必要向上取整后才能替换目标系数。
- 须证明被保留辅助约束的作用域及最后的 replacement 与原系统等价；局部变更需要 undo/template identity。

**验收**：与非 LP Pairwise SC 的相同示例、LP 更强的三行例、临界系数精确性、完全 `cache off`、组件图一致性、四种计数。

### 11.3 RIR-LP（独立 `.h/.cc`）

- 删除目标行 `C` 的前提：证明在 `F\{C}` 下 `C` 恒真，即目标行其余约束下的 `min L >= degree`；如果证明联合系统不可满足，应通过合法冲突协议返回，而不是误删。
- 使用**经过精确核验的** LP/证明器证书；LP 的失败或数值不确定仅返回 UNKNOWN。RUP 已成功证明的行无需再做 LP。
- 处理自身依赖、冗余证明链及已失活行条件；可把 LP 仅用于候选排序，删除之前由 RUP/精确方法复核。

**验收**：双行易证、联合冗余、浮点边界、反例、非根 scope、回跳、四模式。

### 11.4 LP 全部完成后的关卡报告

三项 LP 规则**分别**附数学/证书验证、开销、成功实例、与对应 Non-LP 的相同/不同成果。不能因为某个 LP solver 输出 `optimal` 就默认满足 PB 模型计数的 exactness 要求。

---

## 12. 关卡 6：所有单项完成后，才添加 Adaptive K

**先完成并冻结**：AP、FLP、CS、GCD、Cardinality、Multi-row SC、RIR、RPR、RSR、RUP 的单项正确性；若准备启用 LP，则对应三项通过精确证书测试；Cache v2 在真实结构改写下通过四模式测试。随后创建独立 `AdaptiveKPolicy.h/.cc` 和专项消融。

**固定阶段顺序不变**：

| 阶段 | 不可关闭的基础 | 原始 Non-LP 增强 | LP 增强（证明合格时） |
|---|---|---|---|
| I 变量 | native UP、RV、BasicCheck | AP+FLP | SB-LP |
| II 系数 | CS→GCD→Cardinality | Pairwise Multi-row SC | SC-LP |
| III 约束 | RIR→RPR→Generalized RSR | RUP | RIR-LP |

仅高级增强技术根据根节点计算一次的 $K^*$ 和当前阶段剩余变量数 `N` 选择路径；根输入自适应 K 公式来自 EDPLLSim 的历史方案，但**必须独立标定 PBMC**，不能机械复制 Jaccard-to-K 的常数与假设。原生 UP 不因 `N>K` 被 LP 取代；预算仍分别控制高成本规则；所有配置可记录可消融。

可选进一步研究基于当前 `m`, `nnz`, overlap, success_rate` 的预算，但不得与 K 规则同时改动使实验归因不清。

---

## 13. 通用测试框架：每关独立验收，未通过不继续

### 13.1 Brute-force Oracle：唯一权威的小实例参照

为 `n<=15`（可根据运行时间再扩大）的小 OPB 直接枚举原变量完整赋值，分别计算：

- **MC**：符合全部约束的完整赋值数；
- **WMC**：每个满足赋值的全部原变量文字权重乘积之和；
- **PMC**：存在非投影满足扩展的不同投影赋值数（每个只计一次）；
- **WPMC**：存在扩展的投影赋值按**原投影变量**文字权重计一次，非投影权重不参与，最后应用 `gweight`。

对于 MC/PMC、自然数权重使用精确整数判断；实数权重如 PBMC 使用 MPFR，应与高精度有理 oracle 在明确误差范围内比较，记录提高精度后的稳定性。不要把“权重和为 0”当成不存在满足模型：`hasmodel` 与数值需分开。

### 13.2 每次提交必做的差分矩阵

| 运行模式 | baseline | 新规则单独开 | cache bypass | legacy/v2 cache | 回跳开/关 |
|---|---:|---:|---:|---:|---:|
| MC | ✓ | ✓ | ✓ | 若安全 ✓ | ✓ |
| WMC（不对称权重） | ✓ | ✓ | ✓ | 若安全 ✓ | ✓ |
| PMC（混合投影/非投影） | ✓ | ✓ | ✓ | 若安全 ✓ | ✓ |
| WPMC（投影权重、不对称） | ✓ | ✓ | ✓ | 若安全 ✓ | ✓ |
| d-DNNF（如可用） | ✓ | ✓ | ✓ | 若安全 ✓ | ✓ |
| deep backjump / SAT 子求解 | ✓ | ✓ | ✓ | 若安全 ✓ | ✓ |

**阶段门槛优先 exactness**：新规则允许性能下降，但不能静默改变计数。出现 count 不一致要自动缩减生成一个最小 OPB、记录随机种子和开关，阻止合并。

### 13.3 固定小实例集（逐项扩充）

1. **UP / RV**：`5x+2y+z>=4`，检查 x 固定且 residual correctly formed；
2. **Conflict**：`2x+3y>=6`（0 个模型）；
3. **CS**：`8x+3y+2z>=5` → `5x+3y+2z>=5`；
4. **GCD**：`8x+4y+2z>=5` → `4x+2y+z>=3`；
5. **Cardinality**：`3x+3y+2z>=5` → `x+y+z>=2`；
6. **AP**：`x+y>=1; ~x+y+z>=2; ~y+t>=1`，两侧推出 y,t；
7. **FLP**：`x+y>=1; x+~y>=1`，推出 x=1；
8. **Pairwise SC**：`8x+3y+2z>=5; y+z>=1`，替换目标后 MC=4；
9. **RPR**：`x+y>=2; x+y>=1`，删弱行；
10. **RSR**：`3x+2y>=3; 4x+3y+z>=3`，删后行；
11. **RUP**：`x+y>=1; x+~y>=1; x>=1`，删第三行；
12. **Decomposition**：上一节五变量桥接行例，MC=15；
13. **Equality**：`x+y=1`，2 个完整模型，原式不可直接 CS；
14. **Signed coefficients / opposite literals**： `-3x+2~y>=4` 与正系数化式对照；
15. **Free variable**：`x>=1` 加声明但未使用变量 z，MC 要含 z 的因子 2；PMC 中非投影自由变量因子 1；
16. **Zero-weight satisfiable model**：可满足但权重为 0，验证 `hasmodel` 与缓存；
17. **Large ints**：超 32 位系数、`3e9 x <= 3e9-1` 经合法方向转换，验证不发生 double 整数误删；
18. **Duplicate/Complement literal**：同一 PB 约束 `2x+5~x>=4` 必须先合并或明确等价解释，禁止原二项 probing 越界；
19. **Backjump**：强制多层非顺序回跳、两个兄弟分支不同 overlay；
20. **Hash collision**：常量 hash + exact compare 不得 false hit。

每关还至少生成随机 5～12 变量的 `>=` PB、负文字、负系数、等式、稀疏/稠密、不同投影集、不同权重；固定 seed；全部对 oracle。若某测试因旧 baseline bug 失败，要记录 baseline 失败而不是静默修改预处理器。

### 13.4 结构改写必须增加的断言

- 改写前后在**当前决策前缀**下满足赋值集合相同；单行恒等改写可对每个自由变量赋值比对；多行规则必须比较整个当前公式（或者独立校验完整证明）。
- 约束支持集、occurrence、dirty/revision、ComponentManager active mask 和 solver proof 都能解释同一个 residual state。
- 撤销到父层后，overlay、reason 引用、lookup/key staging、trail accounting 与节点进入前状态一致。
- 不能对已经失效、被删除或来源改变的辅助前提继续使用旧证明。
- 完整缓存关闭模式不会创建可命中或可写入旧格式 key；缓存启用模式命中后要校验 key 的逐字段 exact equality。

### 13.5 必须记录的统计

```text
per_rule: calls, candidates_seen, skipped, successes, budget_exhausted,
          implied_literals, conflicts, rows_removed, coeffs_changed,
          nnz_removed, gcd_divisions, cardinality_conversions,
          proof_success, proof_failed, proof_unknown, time, work_units
solver: decisions, conflicts, propagations, nodes, components, splits
cache: lookups, hits, misses, bypassed, inserts, evictions, bytes_current,
       bytes_peak, template_interns, template_reuses, exact_compares
correctness: MC/PMC/WMC/WPMC oracle failures, replay_seed
resource: wall_time, cpu_time, peak_RSS, ASan/UBSan status
```

CacheBypass 中 `lookups/hits/inserts=0`（仅指真正 hash cache 操作），`bypassed` 可单独计数。`split created by simplification` 需在同一固定状态下比较分解图或记录删除的桥接边，不能仅根据 split 总数归因。

### 13.6 单关验收报告模板

```markdown
# Acceptance — RuleName / Gate XX
- Commit / branch / build configuration:
- 真实修改文件（必须含 RuleName.h 和 RuleName.cc）：
- 本次实际执行模式（view-only / committed / cache bypass / v2）：
- 数学规则及证明/反例：
- 它是否支持 >= / equality / ~x / big coefficients：
- 调用位置和 scope：
- reason、undo、cache/overlay 影响：
- 单项启用/禁用 CLI：
- baseline/test fixture/random seed：
- MC / WMC / PMC / WPMC oracle 对照：
- backtrack / backjump / SAT subset / d-DNNF：
- ASan / UBSan / stress：
- 运行时间、规则成功率、额外工作：
- 已知限制和未完成风险：
- 验收状态：PASS / FAIL / BLOCKED
- 下一关：仅用户确认后开始
```

---

## 14. 每关 Codex 必须采用的固定工作循环

1. **只读定位**：说明本项数学语义、源码入口、与已有 RoundingSat 功能的重叠，列出需要新建/修改的文件。
2. **生成最小测试**：先写能在旧版中运行的计数 oracle 和正反例；记录旧 baseline 答案。
3. **单独添加 `.h/.cc`**：首版只实现纯数学 helper 或 view-only 版本；开启单项开关；编译并验证。
4. **事务式接入递归**：需要 commit 时才增加 reason/overlay/undo，保证 cache 模式合法，避免影响其他规则。
5. **完整差分验证**：单项开/关、Cache Bypass/合法 Native、四模式、兄弟分支、非顺序回跳、纯非投影 SAT。
6. **性能与统计检查**：运行固定小规模数据，记录真实规则命中和开销；只关注本项效果，不变更其他规则/启发式。
7. **提交验收报告**：变更文件列表、失败案例、修复结果、README/配置说明；停在本关，等待用户批准。

**任何临时“为了测试通过”而关闭 conflict analysis、改权重语义、跳过 SAT 子问题或直接禁用部分计数模式的做法，均视为本关未通过。**

### 14.1 推荐的真正实施顺序（逐项）

| 序号 | 完成项 | 缓存策略 | 必须验收后才能进入 |
|---:|---|---|---|
| 0 | 审计真实仓库、修订指南、保存 baseline | legacy + 审查 bypass | 1 |
| 1 | 空 Scheduler、Result、trail accounting、undo skeleton | legacy | 2A |
| 2A | BasicCheck/UP/RV adapter | legacy 若不改结构 | 2B |
| 2B | ProbeEngine + AP | legacy 若只推导赋值 | 2C |
| 2C | FLP | legacy 若只推导赋值 | 3A |
| 3A | CS | view-only → commit 时 Bypass | 3B |
| 3B | GCD | Bypass（若实改） | 3C |
| 3C | Cardinality | Bypass（若实改） | 3D |
| 3D | Pairwise Multi-row SC | Bypass + overlay | 4A |
| 4A | RIR（正式状态维护） | Bypass 若改拓扑 | 4B |
| 4B | RPR | Bypass + overlay | 4C |
| 4C | RSR | Bypass + overlay | 4D |
| 4D | RUP | Bypass + overlay | 5-cache |
| 5-cache | Cache v2 exact identity、packing、回跳/淘汰 | Bypass↔v2 | 5-LP |
| 5-LP-1 | SB-LP，独立 proof | v2 或 Bypass | 5-LP-2 |
| 5-LP-2 | SC-LP，独立 proof | v2 或 Bypass | 5-LP-3 |
| 5-LP-3 | RIR-LP，独立 proof | v2 或 Bypass | 6 |
| 6 | 自适应 K、预算自适应和消融 | v2 | 最终集成 |

> 这里将 Cache v2 放在三项 LP 实改之前，便于随后所有技术在正确的 cache 语义下测试；如果 LP 在实验中只作 dry-run，可以调换研究顺序，但不能打开未验证的真实改写。

---

## 15. Codex 首次任务、单项任务与验收提示词（可直接复制）

### 15.1 Prompt 0：只审计并修订指南

> 请**完整阅读** `PBMC_Recursive_Simplification_Codex_Implementation_Guide_v1.0_CN.md` 与其第 0.1 节所列两份 PBMC 源码指南、PBMC v3.0 框架、EDPLLSim 迁移/缓存指南（如已提供），然后对本仓库实际源码逐项审计。**这次禁止实现任何新简化技术，不要修改根预处理**。请输出 `docs/inprocessing/00_code_audit.md`，校对真实类名、函数、cache 语义、探测恢复、reason、weighted/projection accounting、回跳、ComponentManager 冻结公式，识别旧指南与当前代码冲突之处，并据此把本指导文档修改成可执行的 v1.1。核实 cache 关闭模式及其对组件栈/回跳是否安全。提供实际编译与小实例 baseline 日志、实现顺序、每项独立 `.h/.cc` 的文件列表、阻塞风险。最后停下，等我审核，不要开始关卡 1。

### 15.2 Prompt 1：只完成下一关

> 只执行已审核指南中的**第 XX 关：TECHNAME**。先说明当前源码调用点与规则证明，再添加 `TECHNAME.h/.cc` 和独立配置；只实现这一项，不能修改其他未安排规则或现有根预处理。按照该关 Gate 要求先保存 baseline，再编写正反例、四模式 oracle、回溯/回跳/缓存模式差分测试。若需要实际结构改写，必须先完成并开启安全 CacheBypass 和 residual overlay，禁止旧 key 命中。出错先缩减实例、修复并重测。产出 `acceptance/XX_TECHNAME.md` 和本次 diff/测试命令，然后停止，不要自动进入下一关。

### 15.3 Prompt 2：验收失败后的修复

> 上一关未通过。仅修复验收报告中的失败项，保留相同随机 seed 和最小反例，不能改动其他规则/缓存算法/启发式掩盖问题；重新跑全部本关测试、四模式 oracle、ASan/UBSan、回跳及 cache bypass，并更新 acceptance 报告。未 PASS 不进入下一关。

### 15.4 Prompt 3：缓存专项

> 现在开始 Cache v2 专项，优先严格对齐 PBMC 的 `VarIDs + ClIDs + Gaps` 原有语义和 EDPLLSim 的**当前真实行内容决定模板身份**原则。先验证现有 CacheBypass 后计数与分解正确，再做 ReferenceKey / exact compare / immutable derived-template IDs / residual gap saturation / cache-value 因子归属 / backjump ownership / eviction。禁止以旧 ClID 加新系数的方式构造 key，禁止在 cache key normalization 时修改已 attach 的 solver 约束。每一小步均有四模式 oracle 和 hash collision 压力测试。完成验收后再开放 v2 cache。

### 15.5 Prompt 4：最后才做 Adaptive K

> 所有独立规则、LP 精确证明和 Cache v2 均通过验收后，才新增 `AdaptiveKPolicy.h/.cc`。保留变量→系数→约束的固定功能顺序，以及 Version A 的 CS→GCD→Cardinality；只通过根节点一次计算 K、每阶段当前 N 选择已验证的 Non-LP 或 LP 增强路径。基础 UP/RV/BasicCheck 不因 K 被关闭。与固定配置逐组消融，比较 solved、runtime、nodes、splits、cache、simplification work，所有四种计数模式一致。

---

## 16. 独立来源与尚未确认之处

### 16.1 已提供的依据

- `PBMC_Final_Simplification_Framework_v3.0_CN.md`：数学选型、Version A、三阶段、Gurobi 38 项研究条目筛选与研发候选；**本文件不重新宣称某个算法已在仓库实现**。
- `PBMC_CODE_ARCHITECTURE_AND_EXTENSION_GUIDE_CN(1).md`：已提供的 `08eb389` 版本入口、数据结构、缓存、三栈计数/回跳、权重与启发式审计结果。
- `PBMC_SIMPLIFICATION_TECHNIQUES_INTEGRATION_GUIDE_CN(1).md`：两种 allocator、ID 编码、对等式/负文字、RoundingSat reason、递归 inprocessing、cache 与四计数模式的工程约束。
- `EDPLLSim_to_PBMC_Simplification_Guide(1).md`：EDPLLSim 八技术向 PB 的数学迁移与例子。
- `EDPLLSim_Cache_AdaptiveK_to_PBMC_Guide.md`（若用户提供给 Codex）：EDPLLSim 实际 cache canonical rows、共享模板、full key、refcount、age/admission 和固定 K 的设计理念。

### 16.2 本指南的限定与需要 Codex 实证的事项

1. 新接口、文件名、命令行名称和模块目录是实施建议，未检查实际 PBMC 编译系统。
2. 未取得真实 PBMC 仓库的可运行源码，故没有声称任何 gate 已在真实项目通过。
3. Pairwise SC、RUP、Generalized RSR 的数学规则需配套 exact checker 和同一当前保留公式前提；不能将前文演示当作完整的递归 reason 证明。
4. LP 技术的正确性证书、已 attach Constr 改写替代方案、计数记账以及 cache v2 的确切接口都需实际源码审计后确定。
5. 38 项 Gurobi 研究条目的筛选留在 v3.0 文档，不在此编写指导中重复罗列，以免 Codex 混淆“文献研究候选”和“当前确定需要实现的代码”。

**最终验收标准**：规则逐项 on/off 的 MC/PMC/WMC/WPMC 在小实例与共同可解数据集上完全一致；非根赋值有合法 reason，Overlay/ComponentManager/solver 在任意回跳下恢复；CacheBypass 能安全工作，残余 Cache v2 不发生 false hit；每条规则有独立 `.h/.cc`、独立开关、独立统计、独立报告；Adaptive K 在最后作为可开关的额外优化，不覆盖规则本身。
