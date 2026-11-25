# PBMC 递归简化模块：Codex 分阶段编码、验收与缓存迁移实施指南（v1.1）

> **用途**：本文件是源码审计后的可执行开发计划。Prompt 0、0B、0E 与 Gate 1 已完成，审计报告见 `docs/inprocessing/00_code_audit.md`，最近验收见 `docs/inprocessing/acceptance/01_skeleton.md`。之后一次只实现一个关卡或一项规则，每项都要独立伪代码、独立审核、独立开关、独立测试和独立验收。
>
> **审计基线**：`819fd7e`，初审日期 2026-11-24，BICP/Equality/Cardinality 与 EDPLLSim component cache 补充审计日期 2026-11-25。本文的调用点、缓存行为和回跳协议已与真实源码核对；行号仅用于该基线，后续优先按函数名定位。所有“建议新增”的接口仍是下一关设计，不是现有 API。
>
> **用户已经确定的设计**：只实现普通无权精确模型计数 **MC**，不为 PMC/WMC/WPMC 开发新简化逻辑；暂不改根预处理算法；新简化位于冲突学习生效后的下一次无冲突传播与分解之间；所有简化技术统一放入 `core/simplify.h` 与 `core/simplify.cc`，沿用 PBMC/RoundingSat 的公共数据结构，不为每条规则拆文件或重复造容器；AP 与 FLP 合并为 **BICP（Bidirectional Implication and Conflict Probing）**；所有规则强制 Equality-aware；Equality-specific Simplification 作为独立增强方向。开发顺序固定为**变量简化完成 → Cache Bypass → 参考 EDPLLSim 完成 residual component cache → 系数简化 → 约束简化**；系数阶段采用 **CS（单行系数饱和）→ GCD → RoundingSat Cardinality normalization → Multi-row SC**，后期再加入经精确复核的 **SC-LP**；先完成并验证各技术，最后才添加 Adaptive K。这里“先完成 cache”是开发关卡顺序，运行时 cache lookup 仍在分量分解之后。
>
> **v1.1 的重要限制**：新框架只允许普通 MC（当前 `-mode=0`）且关闭 d-DNNF；`-mode=1/2/3` 全部拒绝进入新简化框架。PMC/WMC/WPMC 的既有测试只保留在审计记录中，不属于实施范围。0B/0E 的 Release 与 ASan/UBSan 基线已经通过；目前仍没有安全 cache-off。

---

## 0. Codex 的执行合同（必须逐条执行）

1. **当前停止点是 Gate 1-I 验收完成**。空框架已接入并通过 Release、ASan/UBSan；下一步只能先编写并审核 Gate 2A Basic 的独立伪代码，不得直接实现 Basic。
2. **不修改旧 PBInstance 根预处理算法**。保留硬编码的 `simplifyRenumberedConstraint()`、输入解析和现有 `postProcess()` 行为；发现的根层问题先建基线测试、单独排期，不混入递归简化规则。
3. **统一文件，规则可独立开关**。所有简化规则的声明、结果类型和最小共享状态放在 `core/simplify.h`，实现放在 `core/simplify.cc`；规则以 `Simplifier` 的短方法区分并独立消融，不再建立 `Basic.h`、`SC.h`、`Rir.h` 等规则文件，也不为每个概念建立抽象类、adapter 或重复容器。优先复用 `CeSuper/ConstrExp`、trail、Component/Decision、现有 occurrence 和精确整数类型。
4. **一次只做一个可验收版本**。单项变更包含规则、短开关、最小测试、计数差分、必要统计和验收文档；失败时只修该项。
5. **所有新功能默认关闭**。PBMC 的 CLI 采用单横线 MiniSat 风格，因此预定总开关写作 `-inprocess`/`-no-inprocess`，不是 `--inprocess`；最终名称在增加配置的同一关确定。
6. **当前先不实现 K 自适应**。保持三个阶段的固定先后顺序；非 LP/LP 增强分别通过明确开关与固定预算测量。后续独立阶段才引入 K 选择，不改变数学规则。
7. **所有新增递归改写必须守住普通 MC 语义**。本计划只验收完整布尔赋值的无权精确模型数；`-inprocess` 对 PMC/WMC/WPMC 和 d-DNNF 明确拒绝，不能静默运行未实现组合。以后删除 projected/weighted 代码应作为独立清理任务，不与规则实现混做。
8. **禁用不可靠的浮点推断**。涉及系数、gap、逻辑蕴含的判断使用精确整数/有理运算；LP 结果在无可核验的精确证书前只能做候选，不能据此删行、固定变量或替换系数。
9. **不直接原地修改已 attach 的 `rs::Constr`**。真正的残余结构改写必须通过 detached row 与最小可回溯 overlay，或经过证明的重建方案；同步 solver、ComponentManager、reason、缓存和 undo 后才能影响分解。
10. **源码事实优先**。行号变化时按函数名重新定位；`rs::CRef`、`Constr::id`、ComponentManager allocator reference 和本地 `ClID` 不得混用。
11. **未通过关卡，禁止进入下一关**。每关提交：变更文件、数学正确性说明、测试日志、计数对照、性能统计、已知限制、当前开关/缓存模式以及是否通过。由用户确认后继续下一项。
12. **禁止一次性重写求解器或公共数据结构**。维护可回退、可 bisect 的提交；公共结构只做经测试证明必要的最小改动，并同步 `pbmc-mod/` 持久副本。
13. **伪代码必须先于实现并独立审核**。每项技术先单独创建 `docs/inprocessing/pseudocode/NN_<name>.md`，只写数学契约、伪代码、状态恢复、Reason、Equality、cache/overlay、预算和测试；提交后停止。用户审核通过后，下一关才允许修改 C++。禁止在同一关里“边写伪代码边完成实现”。
14. **Equality-aware 是每项规则的准入条件**。输入 `>=`、`<=`、`=` 必须保持完全相同的模型集合；`<=` 先精确规范为 `>=`，`=` 作为两个方向组成的同一 EqualityGroup 管理。任何方向缺失、配对身份丢失或不能事务恢复时，该规则只能 no-change。
15. **Residual component cache 是 CS/SC 前置基础设施**。变量阶段验收后，先完成 Cache Bypass，再完成 EDPLLSim 风格的 canonical residual key、精确判等、行模板生命周期、淘汰、回跳 ownership 和 EqualityGroup identity；全部通过后才能开始 CS，更不能提前进入 Multi-row SC。后续规则只向已经验收的 residual view 发布新 row version，不得为每项技术另造缓存。

### 0.1 资料阅读顺序（Codex 第一次必须通读）

1. `paper/PBMC_Recursive_Simplification_Codex_Implementation_Guide_v1.1_CN.md`（本文件）；
2. `docs/inprocessing/00_code_audit.md`（Prompt 0 的事实基线）；
3. `PBMC_CURRENT_ARCHITECTURE_CN.md`；
4. `PBMC_CODE.md`；
5. `PBMC_SIMPLIFICATION.md`；
6. `PBMC_FILE_AND_DIRECTORY_GUIDE_CN.md`；
7. `AAAI_EDPLLSim/src/cache.h` 与 `cache.cc`，必须完整阅读其 canonical key、probe-only lookup、模板引用计数、完整字节判等、插入年龄和淘汰实现；
8. `AAAI_EDPLLSim/src/solver.cc:count()` 与 `src/linear_constraint.cc:insertCache/findCache/initializeCacheForInstance()`，核对 lookup/store 顺序、admission、单实例生命周期和统计；
9. `AAAI_EDPLLSim/EDPLLSim_Cache_AdaptiveK_to_PBMC_Guide.md`，用于交叉核对 identity/full-compare/生命周期原则，不能代替真实源码；
10. `paper/PBMC_Pairwise_MultiRow_SC_Codex_Guide_v1.0_CN.md`，用于核对两行 Multi-row SC 的数学定理、候选界、批量提交反例与测试；
11. `AAAI_EDPLLSim/src/linear_constraint.cc`、`.h`、`Makefile`、`build_utils.sh` 和 `tests/rir_lp/rir_lp_test.cc`，用于核对方法集中于同一 `LinearConstraint` 实现、单行系数强化、SB-LP、RIR-LP 与 GLPK 接入；
12. 真实源码：`core/Main.cc`、`Instance.*`、`Counter.*`、`Component*`、`roundingsat/`、构建文件与测试。

v1.0 曾引用四个仓库中不存在的 `(1).md`/v3.0 文件，缺失清单已写入审计报告；不得把它们继续当作事实来源。

### 0.2 交付物结构（本指南与代码共同维护）

按关卡逐步建立 `docs/inprocessing/`，不要一次创建空文档或空源码：

```text
docs/inprocessing/
  00_code_audit.md                 # 真实源码定位与本指南差异
  01_invariants_and_semantics.md   # 关卡 1：计数、reason、状态契约
  02_stage_plan.md                 # 关卡与实际配置
  03_rule_register.md              # 独立规则、开关、测试、审计记录
  04_cache_migration.md            # Bypass 与 residual key 证明
  05_test_matrix.md                # oracle、回跳、压力、性能
  06_experiments.md                # 结果和消融表
  pseudocode/                      # 每项技术一份；审核通过后才允许编码
    00_equality_contract.md         # 三种 operator 与 EqualityGroup 基础设施
    01_skeleton.md                  # 空简化框架与传播后、分解前接入
    01_basic.md
    02_bicp.md
    03_component_cache.md          # Bypass、ReferenceKey、模板池、ownership 与淘汰
    04_cs.md
    05_gcd.md
    06_cardinality.md              # RoundingSat 既有转换的接入伪代码
    07_multi_row_sc.md
    08_rir.md
    09_rpr.md
    10_rsr.md
    11_rup.md
    12_equality_specific.md
    13_sb_lp.md
    14_sc_lp.md
    15_rir_lp.md
    16_adaptive_k.md
  acceptance/                      # 每关一份验收报告
```

Prompt 0 只要求已经生成的 `00_code_audit.md` 和本 v1.1。其余文件到对应关卡再创建。

---

## 1. 核心技术决策与明确的不做事项

### 1.1 本轮做什么

- 以 **PBMC 普通无权精确 MC 搜索中的节点简化**为唯一计数目标，集成到实际 `count_main()` 状态机，而不是只在根部运行。
- 三个功能阶段：**I 变量简化**（UP/逻辑 RV/BasicCheck、BICP，后续 SB-LP）；**II 系数简化**（CS、GCD、RoundingSat Cardinality normalization、两行 Multi-row SC，后续 SC-LP）；**III 约束简化**（RIR、RPR、Generalized RSR、RUP，后续 RIR-LP 和独立 Equality-specific 增强）。这里 **CS** 专指单行 `a_i=min(a_i,gap)` 系数饱和；**SC** 保留给依赖其他约束的 Multi-row SC/SC-LP，避免两个概念重名。
- 只在规则真正需要时增加 dirty-row、revision、undo、预算和统计；首版不预建通用框架层。
- 分层实现：先只读或 assignment-only；然后安全的约束失活；最后实际的系数/结构重写。**单项算法数学正确 ≠ 它已被安全接入求解器。**
- 变量阶段完成后，先实现并验证**整次运行固定的 Cache Bypass**，再参考 EDPLLSim 的真实 `cache.h/.cc` 完成 residual-aware component cache；只有整个 cache 关通过后才开始 CS。当前源码没有安全 cache-off，不得把 `-cs=0/1` 当作已经关闭。
- 本计划不实现投影计数、权重记账或 d-DNNF 简化节点；新框架启动时拒绝 PMC/WMC/WPMC/d-DNNF。既有 projected/weighted 代码是否删除，待普通 MC 路线稳定后单独决定。

### 1.2 当前明确不做

- 本轮**不替换、不扩展根预处理器**（未来另写独立计划）；不调整用户现有根 preprocessing 开关默认行为。
- 第一批**不做**一般变量消元、等价/互补变量代换、Lifting 新增非零项、约束跨作用域无证明重写、对称性剪枝、BCE/纯文字 SAT-only 简化。
- 第一批 Multi-row SC 只做**目标行 + 一条辅助行**的两行精确界，并对每次提交重新验证；不做枚举多条辅助行组合的非 LP 聚合、不做条件式目标文字下界、Lifting、Full Backbone 或完整 ADD 构造。SC-LP 放在后期 LP 专项关卡，允许一次使用当前全部活动辅助行产生候选，但必须由精确 PB checker 复核。
- Equality-aware 兼容性不是可选增强；所有规则在 `>=/<=/=` 上通过模型集合测试后才算完成。独立的 Equality-specific 技术只指利用两个方向联合推出额外简化，不得用它弥补普通规则遗漏某个等式方向的问题。
- 不把 RoundingSat 冲突学习里的 weakening/divide-round 当成可以替换原约束的等价 SC。
- 自适应 K、自动选择 LP/Non-LP、动态预算调参、Cache age/renew 参数调优和可选的新分支启发式全部延后；但 cache 的正确 identity、完整判等、生命周期、基础淘汰和回跳 ownership 不再延后，它们是 CS/SC 前置关。

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
- Gap 不是 Slack；缓存中的 key-only gap saturation 不是实际 CS；
- 未展开的 `=` 约束禁止直接使用 `>=` 的 SC。原 PBMC parser 可能把一个等式拆成两个内部不等式，按实际代码确认。

| 名称 | 计数对象 | 本计划是否实现 |
|---|---|---:|
| MC | 原公式全部变量的每个完整满足赋值计 1 次 | **是** |
| PMC | 只看指定投影变量；只要存在扩展，同一投影赋值计 1 次 | 否 |
| WMC | 每个完整满足赋值按文字/变量权重乘积计权后求和 | 否 |
| WPMC | 对可扩展的投影赋值按投影权重计权后求和 | 否 |

“Exact”表示结果不是采样或近似值；MC、PMC、WMC、WPMC 理论上都可以 exact。用户需要的是最普通的 **exact MC**，即完整满足赋值总数。因此后续规则测试只使用独立 MC oracle；PMC/WMC/WPMC 只做启动拒绝测试，审计中的旧结果不安排修复。

### 1.4 Equality-aware 统一契约

0E 前的源码事实是：PBMC 的 `PBconstraint` 只显式记录“普通行或 equality”，普通行默认按 `>=` 输出；RoundingSat 的 OPB parser 遇到 `=` 时先加入一个方向，再 `invert()` 加入第二个方向，当时没有保留可供递归简化使用的 equality sibling/group 身份，PBMC 前端也没有可靠的原生 `<=` 语义。0E 已补齐精确 `<=` canonicalization、`EqGroup/RowInfo` 与 solver/component 双重绑定；后续规则必须消费该接口，不能再自行“拆等式”或重写 comparator parser。

统一入口必须先形成规范行组：

```text
>= : 一个 canonical >= row
<= : 精确乘以 -1/翻转后规范为一个 >= row
=  : EqualityGroup{lower: L>=b, upper: L<=b -> canonical >=}
```

对正系数文字式 $L=\sum_i a_i\ell_i$，上界 $L\le b$ 可精确写成

$$
\sum_i a_i\bar\ell_i\ge \sum_i a_i-b.
$$

一般有符号输入先沿用现有“负系数改互补文字并调整 RHS”的精确规范化，再建立方向。所有规则遵守：

1. 原始 equality 的两个方向共享稳定 `EqualityGroupID`，各有独立不可变 `RowVersion`/solver `CRef`，但 overlay revision、undo marker 和 cache staging 按组事务提交；不能只恢复一边。
2. BICP/UP 的临时 solver、Multi-row/LP 的前提集合、RIR/RPR/RSR/RUP 的删除检查必须同时看见两个活动方向。任意一个方向漏入/漏出都使 proof 无效。
3. 单方向等价规范化仍需记录在所属 group 中；禁止原地改 attached row。声称“删除整个 equality”必须证明两个方向都可删除；若只证明一边冗余，只能把该方向标成有证明依赖的 inactive row，不能把整个 group 标成 removed。
4. cache/residual identity 对 equality 序列化 group marker、两个 canonical 方向的**当前内容**及 active 状态；`RowVersion` 只用于选择当前不可变内容，运行时 version/group ID/revision 本身不是数学 key。两个方向的更新只能整体发布，哈希后仍做完整内容比较。
5. `<=` 已在 0E 通过 checked arithmetic 精确规范为 canonical `>=`；溢出或非法 comparator 明确拒绝，不能静默猜测关系。这项输入语义修复没有授权重写旧根预处理算法。
6. 每项伪代码文档都必须有 `>=`、`<=`、`=` 三栏，列出方向映射、proof 前提、commit/undo 和模型集合 oracle；缺任一栏不得进入实现。

Equality-specific Simplification 是后续独立方法 `Simplifier::equality()`：它研究只有联合两个方向才能得到的额外界、成组冗余或其他等价化简。首版不包含变量代换/消元；在独立数学定义和伪代码获批前只保留默认关闭入口，不把它混进通用 equality-aware 基础设施。

---

## 2. 第 0 关：源码审计与本指南修订（已完成；未编码新规则）

> **结果**：真实证据、基线命令和失败记录见 `docs/inprocessing/00_code_audit.md`。本关只修改文档；用户审核前停止，不自动进入关卡 1。

### 2.1 已核对的源码路径和调用关系

以下是 `819fd7e` 的已核对结果。后续行号变化时按函数名重新定位：

| 核对对象 | 真实锚点 | 已确认事实 |
|---|---|---|
| 主入口 | `core/Main.cc:204-257` | load、renumber、正系数化、硬编码根简化、RoundingSat parsing、TD/VCIS、countModels |
| 根输入规则 | `core/Instance.cc:1050-1147` | `simplifyRenumberedConstraint()` 本轮不改；其 assignment 会作为 unit 重新输出 |
| 主循环 | `core/Counter.cc:394-560` | 每轮传播与记账；冲突处理；无冲突时 split/cache；随后分支 |
| 旧空入口 | `core/Counter.cc:149-155,521-528`; `Counter.h:105` | 函数体为空但确实有条件调用；调用点在 split/cache 后，位置错误 |
| RoundingSat 传播 | `core/Counter.cc:325-369` | `runPropagationWithoutLearnt()` 明确不消费 `learnedStack` |
| 赋值与 reason | `roundingsat/Solver.hpp`, `Solver.cpp` | `trail`, `qhead`, `Reason`, `decide`, `propagate`, `backjumpTo` 的契约 |
| 约束改写 | `roundingsat/ConstrExp.cpp:402-484,655-678,800-861` | detached `CeSuper` 可复用 saturate/GCD/cardinality；`postProcess()` 不等于递归 RV |
| Cardinality attach | `ConstrExp.cpp:65-95,800-872`; `Solver.cpp:467-514`; `Constr.cpp:165-278` | 等价转换发生在 detached row；attach 时才选择专用 Cardinality/watches，不能原地换型 |
| Equality import | `core/Counter.cc::addRows/addEq/addDir`; `roundingsat/ConstrExp.cpp::invert` | 0E 直接导入两个方向并保存稳定 group/version/logical ID；不再经 OPB 字符串重解析 |
| PBMC comparator | `core/Instance.cc::load`; `core/pbconstraint.h` | 0E 明确解析三种关系，`<=` checked 取反后统一为 `>=`；非法/溢出 fail-fast |
| 已 attach 约束 | `roundingsat/Constr.*`, `Solver.cpp` | watch/slack/reason 依赖 allocator-local `CRef`；禁止直接写 coefficients |
| 分解与缓存 | `ComponentManager.cc:84-363` | init 冻结副本/occurrence/ClID；cache lookup 在 split 内；满足行按 gap 跳过 |
| 缓存键 | `Component.h:814-951` | 全局 VarIDs + 本地 ClIDs + gaps；hash 后比较完整 packed 内容；gap saturation 仅用于 key |
| 计数路径 | `count_main()`, `solveSATFixedOrder()` | 新框架只进入普通 MC 的 `count_main()`；现有投影模式的纯非投影 SAT 子求解不接入新简化 |
| 回溯与回跳 | `Counter.cc:642-770,775-1240` | bjResolve、选择性 learned attach、普通 backtrack、SAT 子求解是不同恢复路径 |
| 新框架模式边界 | `core/Config.cc:15-24,62-91`; `Counter.cc:410-425,540-559` | 新框架只接入 mode 0 普通 MC；mode 1/2/3 与 d-DNNF 必须在启动时拒绝 |
| 测试/构建 | `CMakeLists.txt`, `Makefile`, `build.sh` | C++17；`.cc` 显式列举；无单元测试/CI sanitizer 目标；已独立构建 ASan/UBSan |

必须记录 PBInstance / RoundingSat `rs::Lit`（1-based 正负整数）/ Component（0-based）转换；solver `CRef`、solver logical ID、ComponentManager `CRef`、本地 `ClID` **不能混为一个 ID**。同时核查 `PBconstraint::Int=long`、`stol`、当前 `gaps_` 位宽和涉及 `double` 的精确性风险。

### 2.2 三条已确认的真实流程

1. **无冲突计数**：`bpos` → 原生传播 → 当前 trail 记账 → `syncAssign` → `splitComponent` 内创建分量和 cache lookup → 非投影 SAT 或投影分支 → 分支和/分量积 → cache 写回。
2. **冲突及选择性回跳**：冲突 → `analyzePBMC2` → learned weakening/postProcess → `ResolveWithoutLit`；只有允许选择性回跳时 attach learned，否则当前分支置零并走普通 backtrack。分析失败走 `bjResolve`。
3. **新蕴含的计数含义**：普通 MC 中，被合法推导并写入 solver trail 的固定文字贡献因子 1；它只需在分解前由 `syncAssign()` 正确同步。不得为本计划新增 projected/weighted accounting，也不得把新蕴含作为 decision 重复计数。

### 2.3 Cache Off 可行性结论

审计结论：当前没有 cache-off。禁止把 `-cs=0/1` 或只清空 hash bucket当作安全禁用：

- `-cs` 是最小值为 1 的容量，`ComponentManager.cc` 还硬定义 `CACHE`；
- `splitComponent()` 创建的临时 `CachedComponent/ID` 同时承担 key、活动 component ID 和 father/descendant pollution ownership；
- 多个 store/erase/pollution 调用没有完整的 compile-time guard；简单返回空 ID 会破坏状态机；
- Bypass 必须在 `cmpmgr.init()` 前按整次运行选择，成套 guard root/create/request/link/store/erase/cleanup，不能搜索中途清桶切换；
- Component/Decision 栈和分支求和、分量求积必须继续工作。

合法性矩阵见第 10.2 节。具体 Bypass 尚未编码；它是 committed structural rewrite 的前置关卡。

### 2.4 本关实际验收状态

- `00_code_audit.md`、源码锚点、Release 和 sanitizer 命令：**完成**。
- MC 最小例、`=`、冲突和分解例：**通过**；`<=` 当前缺少可靠前端语义，列为 0E 阻断而不是已通过。PMC/WMC/WPMC 的历史结果只保存在审计报告，不进入后续验收。
- UBSan 基线：**失败，`ConfigCounter` 含未初始化 `bool`；阈值字段类型也错误**。
- 非顺序 backjump：源码协议已审计，但 runtime 统计被注释且当前环境不能 ptrace，**动态覆盖尚未完成**。
- 关卡 1：等待用户审核；普通 MC 路线先补 MC oracle、UBSan 和 backjump 可观测性。Cache Bypass 已有计划、尚未实现；projected/weighted 问题不再列入门槛。

以上是 Prompt 0 当时的历史结论。后续 0B 已修复 Config/allocator sanitizer 基线并增加 oracle/backjump 覆盖，0E 已通过三种 comparator 与 EqualityGroup 验收，Gate 1 也已完成空框架接入；最新停止点以 `acceptance/01_skeleton.md` 为准。

---

## 3. 目标架构：一个简化模块、短方法、复用现有结构

### 3.1 唯一简化源码文件

```text
core/simplify.h     # SimpStatus/SimpResult、Simplifier 声明、必要的最小共享状态
core/simplify.cc    # run/basic/bicp/cs/gcd/normalizeCardinality/multiRowSC/
                    # rir/rpr/rsr/rup/equality/sbLP/scLP/rirLP/undoTo
```

实现风格参考 EDPLLSim 把相关操作集中为 `LinearConstraint` 成员的做法：PBMC 只保留一个轻量 `Simplifier`，由 `PBcounter::simplifyNode()` 调用 `run()`；各技术使用上述短方法名，不建立 `ISimplificationRule`、Scheduler、Context、Bridge/Adapter 虚层。方法可以独立开关和统计，不需要独立文件。UP 继续使用 RoundingSat；RV 是 trail 上的 residual 读取，不再实现第二套 solver。

`core/simplify.cc` 必须加入顶层 `CMakeLists.txt` 的显式列表，稳定后与 `core/simplify.h` 一起同步到 `pbmc-mod/`。`Counter.cc/.h` 只做调用点、声明和必要 friend/accessor 的最小接入；cache mode 仍直接改现有 `ComponentManager/ComponentCache`，构建配置仍改 `CMakeLists.txt`。这些接入文件不是第二套简化实现。

由于 `PBcounter<T_data>` 是模板且当前在 `Counter.cc` 显式实例化，关卡 1 必须选定一种无重复定义的方式：要么让非模板 `Simplifier` 仅持有所需接口引用，要么在 `simplify.cc` 对 `mpz_class`/`mpfr::mpreal` 的简化入口作明确实例化。只实现普通 MC 不等于可以破坏现有模板链接；最终选择需由最小编译测试确认，不能把大量实现移回 `Counter.h`。

### 3.2 最小但明确的结果接口

```cpp
enum class SimpStatus {
    UNCHANGED,
    IMPLIED,
    ROW_CHANGED,
    ROW_REMOVED,
    CONFLICT,
    BUDGET
};

struct SimpResult {
    SimpStatus status = SimpStatus::UNCHANGED;
    // CONFLICT 时才携带可供主流程使用的证明/约束句柄；
    // 其他具体 payload 到对应规则关卡再加入。
};
```

不能只返回 `bool`，因为主状态机必须区分新赋值、结构变化、删行、真实冲突和预算停止。首版仍不需要 `ISimplificationRule`、`CountFactor`、通用 proof variant 或多个 delta 容器；某关确实需要 implication、row delta 或 proof 时，再增加准确 payload。

规则通过直接函数调用组合。任何提交仍必须按“证明/作用域检查 → overlay 或合法 attach → enqueue/propagate → trail 记账 → topology/cache identity”执行；不能为了接口简单而伪造 reason。

### 3.3 主状态机：传播—简化联合不动点（目前无 K）

```text
进入或回跳到一个搜索节点
  ↓
RoundingSat native propagation 到不动点
  ├─ 真实冲突 → 原 PBMC conflict analysis / selective learning / backjump
  │              → 在恢复后的节点重新开始 native propagation
  └─ 无冲突
       ↓
     residual RV → BasicCheck
       ↓
     I. Variable: BICP（双向探测一次，共享 failed-literal 与公共蕴含结果）
       ↓
     II. Coefficient: CS → GCD → RoundingSat Cardinality normalization → Multi-row SC
         [late optional: SC-LP]
       ↓
     III. Constraint: RIR → RPR → RSR → RUP
         [late optional: Equality-specific]
       ↓
     IMPLIED / ROW_CHANGED / ROW_REMOVED
       → 合法提交、同步 undo/revision 后回到 native propagation
     UNCHANGED / BUDGET
       → 在必要传播已完成的前提下退出联合循环
       ↓
syncAssign → component decomposition → cache lookup → branch/count
```

上图是**运行时顺序**。开发关卡的先后不同：在 BICP 完成后先把 residual component cache 实现并验收，再开始 CS/SC；这不会把运行时 cache lookup 移到简化或分解之前。

这不是“先简化再处理已知冲突”，也不是“在冲突节点继续简化”。**传播发现真实冲突时必须立即执行原有冲突分析、选择性学习和回跳；回跳后的新状态再传播、简化、分解。**反过来，简化合法提交的新赋值或结构变化必须重新触发原生传播；若随后出现真实冲突，才进入同一套原生冲突流程。因此主顺序是 `Propagation → Simplification → Decomposition → Cache → Branching`，传播与简化在分解前形成交替不动点。

`CONFLICT` 必须区分来源：当前真实节点上有可供 PBMC 使用的合法冲突证明时，才能交给原冲突分析；若已严格证明当前节点无模型但暂时无法构造可解释冲突，只能令**当前分支**贡献 0 并安全顺序回溯，不能伪造 reason 或越过计数分支。BICP/RUP 内部临时假设下的冲突只是该证明过程的证据，不是当前真实节点冲突，不得直接学习、回跳或把真实节点计数置 0。

`BasicCheck` 可提前检查恒真/冲突，Stage III 的 RIR 负责正式管理失活或删行，避免重复提交。预算耗尽只停止可选高级规则；已经产生的新赋值仍必须完成必要传播。重复规范化只处理受影响行。

### 3.4 版本 A：NormalizeA 的定义

```text
NormalizeA(row_or_equality_group):
    canonicalize >= / <=; for = enumerate both linked directions
    check 0 >= gap / gap > U / empty row
    CS(row): a_i <- min(a_i, gap)  [gap>0]
    GcdReduction(row): g=gcd(a_i), a_i/=g, gap=ceil(gap/g)
    RoundingSat.simplifyToCardinality(true) on detached row
    check 0 >= gap / gap > U / newly implied literals
    publish both equality directions atomically when committed
```

RoundingSat 已在 `ConstrExp::postProcess()` 中调用等价保持的 `simplifyToCardinality(true)`；`toConstr()` 随后根据 `propCard` 和最大系数选择专用 `Cardinality` 或通用传播对象。因此 PBMC **保留该功能但不另造一项新算法**：`normalizeCardinality()` 只是 NormalizeA 的内部封装，直接复用现有实现。它仍设独立接入验收关，因为 detached 内容会变为全 1 系数，commit 后可能改变 `Constr` 具体类型、watches、Reason 对象和 cache identity；view-only 成功绝不代表 attached 对象已安全替换。

EDPLLSim 的 `coefficientStrengtheningSingle()` 面向一般整数域 `Ax<=b`，逐行利用变量上下界调整系数和 RHS；对规范布尔 PB `sum(a_i*l_i)>=gap`，本项目采用的安全布尔特例就是 `a_i=min(a_i,gap)`。按用户已有文档术语，本指南将它统一称为 **CS（coefficient saturation，系数饱和）**，实现为 `Simplifier::cs()` 并复用 detached `ConstrExp::saturate()`；不再把它叫单行 SC。`multiRowSC()` 和 `scLP()` 则明确依赖目标行以外的约束。

**注意**：CS → GCD 对固定的规范不等式一次即可达到二者不动点；当赋值、Multi-row SC、SC-LP 或其他合法规则改变行时，再对变化行调用 NormalizeA。不是在 CS/GCD 之间无上限循环。

### 3.5 每项技术的伪代码前置关

每项技术都拆成两个不同回合：

```text
Gate X-P（Pseudocode）
  只创建/修订 docs/inprocessing/pseudocode/NN_name.md
  运行数学小例或只读源码核对
  禁止修改 C++
  提交给用户审核并停止

Gate X-I（Implementation）
  仅在对应 P 文档获批后开始
  按已批准伪代码修改 simplify.h/.cc 和必要最小接入
  若实现迫使数学/状态机发生实质变化，先退回更新 P 文档并再次审核
```

每份伪代码文档必须含：目标与明确不做事项、规范输入及 `>=/<=/=` 三种关系、调用前后不变量、逐步伪代码、返回状态、精确证明、临时假设与 rollback、Reason 生命周期、EqualityGroup 事务、overlay/cache/dependency、预算与 UNKNOWN、正例/反例、完整模型集合 oracle、预计修改接口。Cardinality 文档写的是 RoundingSat 既有转换如何接入，不重复发明算法。

`03_component_cache.md` 是同样强制的基础设施伪代码文档：它不需要虚构数学简化返回状态，而要写清 ReferenceKey 与 packed key 的一一对应、无损大整数编码、lookup/insert 的引用所有权、hit/miss/update/evict/clear/backjump 的状态迁移、EqualityGroup 原子发布、hash collision 反例和 Bypass 计数 oracle。该 P 关获批前不得迁移 cache C++。

### 3.6 固定预算、禁用 K

按真实 CLI 风格，后续候选名称为 `-inprocess`、`-simp-rules=...`、`-simp-budget=...`、`-simp-cache=legacy|bypass|residual` 和 `-simp-dry-run`。名称保持短且只在真正有对应功能时加入。三阶段顺序固定；预算耗尽只跳过可选简化，绝不跳过原生传播、回溯、exact accounting 或证明校验。

---

### 3.7 关卡 0B：既有基线修复（需用户单独批准）

这不是简化算法关卡，但它是可靠差分测试的前置条件。一次只处理一个既有问题：

1. 建立只覆盖普通 MC 的独立 brute-force oracle 和永久小例；
2. 初始化 `ConfigCounter` 所有仍存活的字段，使 UBSan 干净；旧阈值字段在关卡 1 随空入口删除，若决定兼容保留才改成正确类型；
3. 暴露或测试 backjump 统计，覆盖 `bjResolve`、`ResolveWithoutLit` 和普通 backtrack；
4. 增加启动参数测试：启用新框架时 `-mode=1/2/3` 或 d-DNNF 必须明确报错退出，不能静默退回未验收组合。
5. 增加 comparator/equality 基线：证明当前 `=` 在 RoundingSat 中展开为两个方向；为 `<=` 建立应有模型集合测试并记录当前前端不可靠行为。在 Equality contract 实现前，`<=` 必须 fail-fast，不能静默按普通 `>=` 继续。

这些修复不得夹带根简化算法改写。PMC/WMC/WPMC 既有问题不在关卡 0B 修复；后续若删除 projected/weighted 代码，必须另立清理任务并验证没有误删普通 MC 共用路径。

---

### 3.8 关卡 0E：Operator / Equality contract（已完成）

本关不是简化算法，但它是所有规则的输入语义前置条件。

1. **0E-P**：只创建 `docs/inprocessing/pseudocode/00_equality_contract.md`，准确描述 PBMC 输入 comparator、`<=` 转换、RoundingSat equality 双向展开、EqualityGroupID/RowVersion 映射、事务 undo/cache identity 和故障测试；停止等待审核。
2. **0E-I**：获批后才实现最小 comparator/canonicalization 与 group metadata。不得改动旧根简化规则；只修正/拒绝输入关系并保留两方向身份。
3. `<=` 在实现通过前 fail-fast；实现后以原式完整模型集合为 oracle，不以转换后的自比较代替。
4. `=` 两个方向在 solver、ComponentManager frozen rows、ResidualOverlay 和 cache builder 之间必须可追踪到同一 group；原始 row ID、两个 solver CRef 和两个 Component ClID 仍严格区分。
5. 本关不执行 CS/BICP/RIR 等任何新规则。空 group overlay 下，已有 `>=`/`=` MC 行为与 baseline 一致。

**验收**：`x+y>=1`、`x+y<=1`、`x+y=1` 分别得到 3/3/2；负系数与负文字等价转换；故意漏掉一个 equality 方向必须触发断言/测试失败；普通回溯、非顺序回跳和 cache staging 不产生半个 group。

---

## 4. 关卡 1：建立空简化模块与原生传播接入（已完成）

**规则**：仅建立基础设施，不新增数学变换，不动旧根预处理。

**开发任务**：

1. 保留 `count_main()` 的冲突、回跳、组件和分支状态机。新调用只放在 `runPropagationWithoutLearnt(true)` 返回无冲突以后、`syncAssign()`/`splitComponent()` 以前。
2. 冲突分支仍先执行 `analyzePBMC2 → ResolveWithoutLit/bjResolve → continue`。learned constraint 在下一轮传播生效；不要从冲突分支直接调用节点简化。
3. 删除旧 `PBcounter::simplify()` 的声明、空定义、`core/Counter.cc:521` 的过晚调用和成员 `on_simp`。不要触碰 `core/Solver.cc` 中不同用途的 Glucose `simplify()`。
4. 确认 `-rmvsatcl/-rmvsatclthd` 和 `ConfigCounter::remove_sat_cls/rmvsatcl_threshold` 只服务旧空入口后，在同一小变更中删除；若为 CLI 兼容暂留，必须明确标为 deprecated no-op，不能复用成新语义。
5. 新建且只新建 `core/simplify.h/.cc`，增加短入口 `PBcounter::simplifyNode()`、`Simplifier::run()` 和小 `SimpResult`；空入口不制造赋值、不改结构、不生成 cache key。只允许 mode 0 普通 MC且 d-DNNF 关闭；`-no-inprocess` 与空入口开启时都必须与 MC 基线状态机一致。
6. 不为无权路线新增 `trail_accounted`、权重 ledger 或计数因子。以后任何规则产生的正式蕴含必须通过 RoundingSat 的传播接口进入 trail，返回联合循环继续原生传播，并在分解前由现有 `syncAssign()` 同步。
7. 首版只记录必要统计：调用次数、changed/conflict/budget、耗时和 cache mode；不预建 node logger 或通用 proof 框架。
8. 只消费已通过 0E 的 canonical row/EqualityGroup 接口；空框架要继续断言两个方向没有丢失，不在 `simplify.cc` 内再写第二套 comparator parser。

**本关测试**：所有规则关闭时，新旧 PBMC 的普通 MC 在独立 oracle 上数值相同；原生传播、decision/component/cache stats 无无法解释的差异；PMC/WMC/WPMC/d-DNNF 拒绝测试通过。ASan/UBSan 基础测试不过不进入下一关。

**验收**：独立提交、`acceptance/01_skeleton.md`、真实 diff、编译命令、oracle 对照和已知边界。Cache Bypass 不捆绑在本关；它在第一次 committed structural rewrite 前作为独立关卡完成。

---

## 5. 关卡 2：变量简化——逐个实现

### 5.1 关卡 2A：Basic（`Simplifier::basic()`）

- 读取当前有效 residual row 的 `gap`、未赋值贡献 `U`、`slack=U-gap`；`gap<=0` 标记当前已满足，`U<gap` 返回 conflict，`a_i>slack` 识别可能 forced literal。
- 首选已有 RoundingSat watched propagation，不要在 BasicCheck 中第二次直接写 `assigns`。若需提交新蕴含，必须有合法 PB reason；不可把 `decide()` 当作传播。
- 支持负文字与 equality 两方向，且必须去除同变量互补和重复的影响。Basic 自身不做正式 structural delete，交给 Rir 的 overlay 阶段。
- 将 fixed 文字代入**只读残余视图**；将 RV 作为表示功能维护，而不是实现另一套 physical matrix column delete。

**测试**：`5x+2y+z>=4` 强制 x=1；`2x+3y>=6` 冲突；`2x+3y>=0` 恒真；`2x+3y>=3` 仅强制 y=1；负文字 `3~x+2y>=3` 强制 ~x=1（需检查示例是否有其他强制项）。基线无重复固定；回跳后恢复。

**验收门槛**：规则独开时 oracle 一致、所有强制文字有合法推导、未改变原始结构 key；然后再做 BICP。

### 5.2 关卡 2B：BICP 双向探测引擎（`Simplifier::bicp()`）

**BICP = Bidirectional Implication and Conflict Probing（双向蕴含与冲突探测）**。它是原 AP 与 FLP 的唯一模块名：对同一候选变量只执行一轮 `x=0/x=1` 双向探测，两个方向的结果同时服务 failed-literal 和公共蕴含推理，不再维护 `Probe/Assume/FailedLit` 三个公共模块。

BICP 的每个方向返回 `COMPLETE/CONFLICT/UNKNOWN`、经传播得到的文字集合、对应临时 proof 信息和实际 work units：

```text
bicp(x):
  left  = probeBranch(~x)
  assert live state exactly restored
  right = probeBranch(x)
  assert live state exactly restored

  if left=CONFLICT and right=CONFLICT:
      produce current-node-unsat candidate; require exact combined proof
  if left=CONFLICT:
      produce implied x candidate
  if right=CONFLICT:
      produce implied ~x candidate
  if left=COMPLETE and right=COMPLETE:
      produce intersection(left.implied, right.implied) candidates
  UNKNOWN contributes no negative conclusion and no common implication
```

- `probeBranch()` 必须建立假设 marker，使用 RoundingSat 原生传播直到不动点或预算停止，然后恢复 `trail`、`trail_lim`、`qhead`、Level/Pos、临时 Reason、watch/slack、learned/probe 临时对象、overlay、dirty/revision、component/cache staging 和 EqualityGroup 状态。不得复用未覆盖这些字段的 checkpoint helper。
- 临时分支的 Reason 只在该 probe 中有效，rollback 后不得写入 live `Reason[v]`。BICP 最终候选必须通过可生成正式 PB Reason 的合并证明或独立 exact recheck，再经 solver 正式 enqueue；不能把假设当作 decision 残留。
- 一个方向确认冲突即可推出相反文字，即使另一个方向 UNKNOWN；公共蕴含必须两个方向都 `COMPLETE`。两方向均冲突只有在合法组合证明后才可进入真实节点冲突路径。
- equality 的两个方向必须同时出现在每个 probe 的传播公式中；`<=` 必须已规范化。一个方向遗漏或 group revision 在两次 probe 之间变化时，整次 BICP 结果失效重做。
- 预算按整次 BICP 和每个方向分别记录，外层不动点循环不得把同一节点预算重置后无限重试。

**示例**：`x+y>=1`, `~x+y+z>=2`, `~y+t>=1`；两侧均推出 `y=1,t=1`，BICP 提取公共蕴含。验证不发生 trail/Reason 泄漏、不会对 y/t 重复计数。

**验收门槛**：先审核 `pseudocode/02_bicp.md`；随后正反 probe 状态逐字段恢复、UNKNOWN 安全、equality 两方向均参与、普通 MC 与 oracle 一致。view-only 引擎通过后才允许正式提交推导。

### 5.3 关卡 2C：BICP 推导提交（仍为 `Simplifier::bicp()`）

- 从同一次 BICP 获取两个方向结果；不重新探测。
- 假设 `l` 产生**经确认的冲突** → 当前上下文推 `~l`；两方向都完整且推出同一文字 `p` → 当前上下文推 `p`；两方向都冲突 → 当前上下文无解候选。
- 不允许把“没有发现冲突”当 SAT；未完成的探测为 UNKNOWN，不能参加公共蕴含交集。
- 非根冲突必须遵守 PBMC `limlevel`、选择性回跳和 counting scopes；若无法生成可用于非顺序回跳的冲突证明，只能采用已证实合法的局部顺序回溯策略，不能裸调用标准 CDCL 的 `backjumpTo`。
- BICP 只有一个算法开关；统计可分别记录 `failed_literal` 与 `common_implication` 命中，消融时允许关闭某类输出但仍复用同一双向结果，禁止重复传播。

**最小例**：`x+y>=1`, `x+~y>=1`；令 `x=0` 后推出 `y=1` 与 `~y=1`，所以固定 `x=1`。再测公共蕴含、两方向都冲突、无冲突、UNKNOWN、含 equality 的两个方向、深层 backjump/组件边界。

**验收门槛**：通过全部 BICP rollback、Reason、EqualityGroup、cache legacy（只产生赋值时）和普通 MC 对照后，变量阶段暂时冻结。

### 5.4 本阶段 SB-LP 暂不做

把 `Simplifier::sbLP()` 留到**后期 LP 专项关卡**；现在不要把旧 EDPLLSim 使用的 GLPK double 直接迁移到 PBMC 中用于固定布尔变量。必须先解决 exact certificate 及安全回退。

---

### 5.5 关卡 2D：实现整次运行的 Cache Bypass

本关不实现任何数学简化，只在现有 `ConfigComponentManager`、`ComponentManager` 和 `ComponentCache` 内加入一个短模式字段。不要新建 `CacheBypassAdapter`。

1. 模式必须在 `cmpmgr.init()` 前确定，整次 count run 不变；禁止节点中途清桶或 Legacy↔Bypass 切换。
2. 对 root cache entry、component entry create/request/father link、completed value store、stack ID erase 和 pollution cleanup 成套处理。
3. Bypass 下真正的 hash `lookup/hit/insert` 必须为 0，但 Component/Decision 栈、组件边界、分支求和和分量求积仍按原协议运行。
4. 先在公式完全冻结、没有任何新规则 commit 的情况下与 Legacy 差分；再测普通 backtrack、`bjResolve`、非投影 SAT 子求解和极小 cache limit。
5. 不能用 `-cs=1`、`#undef CACHE`、`return 0/nullptr` 或清 bucket 代替本关。

验收文件为 `acceptance/02D_cache_bypass.md`。未通过前，后续系数/active-row 规则只允许 view-only，不允许 committed rewrite。

---

### 5.6 关卡 2E：完成 EDPLLSim 风格的 residual component cache

这是变量阶段与 CS/SC 之间的**硬关卡**，不是以后再做的性能优化。先执行 **2E-P**：只创建 `docs/inprocessing/pseudocode/03_component_cache.md`，写清 identity、序列化、lookup/store、模板引用、ownership、淘汰、回跳、EqualityGroup 和反例；用户审核后才进入实现。实现仍沿用 `core/Component.h`、`core/ComponentCache.cc`、`core/ComponentManager.cc`，不新建第二套 cache manager，也不把缓存代码塞进 `simplify.cc`。

已完整核对 `AAAI_EDPLLSim/src/cache.h/.cc`、`solver.cc::count()` 和 `linear_constraint.cc::insertCache/findCache/initializeCacheForInstance()`。本关采用“**迁移经过验证的 cache 内核，重写 PBMC 状态适配层**”：

1. **2E1 — Reference identity**：从真正用于分解的 residual view 生成唯一 canonical `ReferenceKey`；包含有序活动变量、每条活动 PB 行的文字极性/精确系数/gap，以及 EqualityGroup 两个方向的当前内容与 active 状态。RowVersion 负责定位内容但其编号不进数学 key。先在冻结公式上与 Legacy/Bypass 差分，再用测试构造的不同 row content 验证不会 false hit。
2. **2E2 — 紧凑键与行模板池**：迁移 EDPLLSim 的版本化字节格式、ULEB128 思路、带符号大整数无损编码、`hash + full bytes equality`、`probe()` 与 `internAndAcquire()` 分离、`TemplateID + gap` 结构和引用计数。`ReferenceKey` 只保留为测试 oracle，不建立第二套生产 cache。
3. **2E3 — PBMC ownership 接入**：把紧凑键接回当前 `CachedComponent/ComponentCache/ComponentManager` 的 root、component stack ID、father/descendant pollution、store、erase 和 backjump 路径。lookup 必须 probe-only；miss 不得污染模板池。临时 lookup key、活动 ownership entry 与持久 cache entry各自是否持有模板引用必须明确，任何 hit、重复 insert、eviction、pollution cleanup 和实例结束都要成对 release。
4. **2E4 — 生命周期与淘汰**：迁移“插入序列年龄、命中不续期、bucket 内完整判等、批量释放后回收零引用模板”的机制，同时保留 PBMC 现有 MB hard limit。EDPLLSim 的 `N*M<=20`、`50*1024` age、`20*age` 清理阈值和 bucket 数只作为测量起点，不直接复制成 PBMC 正确性常数。
5. **2E5 — Cutover**：在无结构改写时做 `Legacy ↔ Bypass ↔ Residual` 三路普通 MC 对照；再覆盖强制 hash collision、巨大系数、不同历史同 residual、同旧 ClID 不同系数、EqualityGroup 半更新拒绝、深 backjump、极小内存、重复 insert 和跨实例 clear。全部 PASS 后 residual 成为后续结构改写的默认测试模式，Bypass 保留为 correctness oracle。

允许直接迁移或轻改的代码形状是 `SerializedComponentKey`、完整字节判等、行模板池的 probe/intern/release/reclaim、插入序列链和批量淘汰；必须按 PBMC 重写的部分是 EDPLLSim 的稠密整数域 `Ax<=b` normalizer、`ranges`、局部列号、GCD/floor、递归复制式 `LinearConstraint` 状态和 `Int` payload 接口。PBMC 的 `rs::BigVal` 必须无损序列化，不能继续把 residual gap 压成 `int`，也不能用 `double/log` 决定会截断数学身份的位宽。

本关完成的含义是 cache 核心和动态 row-version 接口均已验收；以后 CS/GCD/SC 只通过同一个 residual overlay 发布新版本并触发既有 key builder。若某项规则迫使 cache identity 或 ownership 重新设计，应退回 2E 更新伪代码和测试，不能在该规则内部补一套专用 key。

验收文件分为 `acceptance/02E1_reference_key.md`、`02E2_template_key.md`、`02E3_ownership.md`、`02E4_eviction.md` 和 `02E5_cutover.md`。五项未全部 PASS，不进入 3A CS。

---

## 6. 关卡 3：系数简化——先只读，再实际结构替换

> **重要工程顺序**：从这一关开始明确引入两种模式：
> 1. `view-only / dry-run`：从 residual row 和 trail 构造规范视图，**不修改**已经 attach 的 `Constr`；视图仅供规则自身筛选/统计，不能让 ComponentManager 在未发布 row version 前以为 solver 已改写。
> 2. `committed-rewrite`：真正改变当前 residual 约束的系数、degree、支持集，需要最小可回溯 overlay、对应的传播/证明契约与回溯；必须同时通过已验收的 Residual cache 和 Bypass oracle 差分。
>
> 进入本关的前提是 2D 与 2E1–2E5 全部 PASS。单纯设置 `cache=off` **不能**解决 solver 与 ComponentManager 不一致；每条实际改写仍要证明 solver、overlay、ComponentManager 和 residual identity 同步，才允许计数主循环使用。
>
> `occlists_` 可先作为保守超集：本框架不新增非零项，遍历时过滤 inactive row/removed term 即可。没有性能证据前不复制第二套 occurrence。`postProcess()` 的 level-0 unit removal 也不能代替当前递归层 RV。

### 6.1 关卡 3A：CS（`Simplifier::cs()`）

- 参考 `AAAI_EDPLLSim/src/linear_constraint.cc::coefficientStrengtheningSingle()` 的逐行设计，但不照搬一般整数域 `Ax<=b` 的下标/RHS 更新代码。
- 对规范正系数布尔 PB `>=` 残余行执行 `a_i <- min(a_i,gap)`；gap≤0 交由 BasicCheck。这个操作在本文统一叫 **CS（coefficient saturation）**，不是 Multi-row SC。**不要对未展开的 equality 直接做 CS**。
- 直接复用 detached `CeSuper/ConstrExp::saturate()`，不再维护手写第二套实现；区分实际 CS 与 cache 的 key-only gap saturation。
- CS 只读取目标行自身及当前 trail 形成的 residual gap，不读取辅助行，不新增非零项，也不产生多行 proof dependency。
- 测试 `8x+3y+2z>=5 ↔ 5x+3y+2z>=5`、负文字 `8~x+3y+2z>=5`、已赋值后 residual gap 减少、大整数溢出、`gap>sum` 冲突。
- 首次以 view-only 正确性为验收，然后在下一次**独立变更**中启用 committed residual rewrite（须使用已通过的 residual cache + Bypass oracle + overlay）。

**验收**：数学等价验证、与 EDPLLSim 单行强化的布尔特例及 RoundingSat 原 `saturate()` 比对、每次 row version 变化都改变或精确复用 residual identity、Residual↔Bypass 下实际 rewrite 前后普通 MC 一致。

### 6.2 关卡 3B：GCD（`Simplifier::gcd()`）

- `g=gcd(a_i)>0`；新系数 `a_i/g`，新 gap `ceil(gap/g)`；对正负 gap、极大整数、空行、互补文字严格测试。GCD 使用**规范化后当前整数表示**，不要混用一般 `<=` 形式需要的 floor。
- 单行测试 `8x+4y+2z>=5 ↔ 4x+2y+z>=3`，再测正式顺序 `CS → GCD`。
- 区分 `GCD for actual residual rewrite` 与 `GCD solely for key canonicalization`，两条路径不能偷偷互改母版。

**验收**：单项可开关；序列化前后的 exact 数值一致；同一行 repeated call 幂等；commit 路径的 cache/undo/propagation 契约通过。

### 6.3 关卡 3C：RoundingSat Cardinality normalization 接入（`normalizeCardinality()`）

**结论：保留功能，不把它单列为 PBMC 新算法。** 源码已经确认：

- `ConstrExp::postProcess()` 在 detached row 上依次 remove/saturate、GCD、`simplifyToCardinality(true)`；输入约束和 learned constraint 本来就在 attach 前走这条路径。
- `simplifyToCardinality(true)` 是等价保持转换：满足条件时把当前行变成全 1 系数和相应 degree；递归 CS/GCD/Multi-row SC 改变 residual row 后可能出现根阶段没有的新机会。
- `ConstrExp::toConstr()` 在 `propCard` 开启且最大系数为 1 时创建专用 `Cardinality`；否则仍可能用通用 watched/counting 对象。因此“数学内容已是 cardinality”和“物理对象使用 Cardinality propagator”要分别记录。

PBMC 只写一个很薄的内部调用：在最新 detached row 上复用 `simplifyToCardinality(true)`，不复制判定公式。它没有独立研究开关；跟随 NormalizeA 自动执行。为了消融和定位，可记录 `cardinality_detected/content_changed/specialized_attached` 统计，并提供仅调试用禁用选项，但论文中不得将它包装成新提出的技术。

工程上仍保留独立 3C 验收，因为 commit 会改变系数内容，并可能改变 allocator 中的具体 `Constr` 类型、watch 布局和后续 Reason。禁止给已 attach 对象原地换 type 或系数；必须创建新版本并遵守第 9 节生命周期。对 equality，两个方向分别在 detached 状态调用，随后按 EqualityGroup 原子发布/撤销；cache identity 同时更新两边。

**测试**：先审核 `pseudocode/06_cardinality.md`；验证 `3x+3y+2z>=5 ↔ x+y+z>=2`、非 cardinality 反例、负文字、`<=` 规范行、equality 两方向、`propCard` 开/关、Reason pin、GC、兄弟分支和 cache identity。比较每个完整赋值，且把 `postProcess()` 输出与 PBMC wrapper 逐字段对照。

### 6.4 关卡 3D：两行 Multi-row SC（`Simplifier::multiRowSC()`）

对当前作用域中的规范目标行

$$
C_i:a_j\ell_j+R_{ij}\ge k_i
$$

令 $G$ 为证明所用且不含目标行 $C_i$ 自身的活动前提。若能精确证明

$$
G\models R_{ij}\ge L_{ij},\qquad 0\le L_{ij}<k_i,
$$

则只把该目标系数替换为

$$
a'_j=\min(a_j,k_i-L_{ij}).
$$

此时 $G\land C_i$ 与 $G\land C'_i$ 完全等价：若 $a'_j=a_j$ 则没有改写；真正缩紧时 $a'_j=k_i-L_{ij}$。当 $\ell_j=0$ 时新旧两行相同；当 $\ell_j=1$ 时，$R_{ij}\ge L_{ij}$ 保证 $a'_j+R_{ij}\ge k_i$，而 $C'_i\Rightarrow C_i$ 显然成立。若 $L_{ij}\ge k_i$，则 $G$ 已蕴含整个目标行，只能产生 `ROW_REMOVED` 候选并交给约束阶段的独立删除验证。本关禁止用目标行自己证明 $L_{ij}$。

第一版将 `G` 限为**一条辅助行**，所以是目标行加辅助行的两行 Multi-row SC；这是 `PBMC_Pairwise_MultiRow_SC_Codex_Guide_v1.0_CN.md` 中可直接落地的部分。它不枚举三条以上辅助行组合，也不使用条件 $\ell_j=1$ 下的界。因为采用无条件界且 $L_{ij}<k_i$，新系数至少为 1，本关不会直接删掉一项或增加分量 split；收益来自更强传播、后续 GCD/Cardinality 或整行冗余。

#### 6.4.1 两个精确的非 LP 下界

辅助行写为 $H_L=\sum_t b_t h_t\ge k_h$。所有重复文字、互补文字与常数先按现有精确规范化处理，计算全程使用大整数/精确有理数，不经过 `double`。

1. **公共同极性文字界**。令 $I$ 为 $R_{ij}$ 与 $H_L$ 的相同文字集合，$P$ 为辅助行其余文字，$U_P=\sum_{t\in P}b_t$，$D=\max(0,k_h-U_P)$。若 $I\ne\varnothing$ 且 $D>0$，取

   $$
   \lambda=\min_{u\in I}\frac{a_u}{b_u},\qquad
   L=\left\lceil\lambda D\right\rceil.
   $$

   这是安全界，因为辅助行迫使公共部分贡献至少 $D$，而目标余项在每个公共文字上的系数至少是 $\lambda b_u$。辅助私有文字用最大贡献上界处理，因此支持集不必相同。

2. **通用精确差值界**。对候选 $\lambda=p/q\ge0$（既约整数），将负文字按 $\bar x=1-x$ 展开。由

   $$
   qR_{ij}=pH_L+(qR_{ij}-pH_L)
   $$

   以及 $H_L\ge k_h$，若仿射式 $qR_{ij}-pH_L=c+\sum_v d_vx_v$，则其布尔盒域精确最小值为 $m=c+\sum_v\min(0,d_v)$，从而

   $$
   L=\max\left(0,\left\lceil\frac{pk_h+m}{q}\right\rceil\right)
   $$

   是安全下界。候选比例首版只取 `0`、`1`、共享文字系数比和公共文字法的最小比；不同候选得到的下界取最大值，**不能相加**。相反极性、不同支持集和辅助行含目标变量均由同一仿射展开处理。

公共文字法是便宜过滤器，通用差值界是更一般的精确后备。候选由 frozen occurrence 的保守超集产生，再按当前 active row、scope、revision 和真实 residual 内容过滤；使用 top-Q、pair/nnz/大整数预算时，只允许漏掉可选缩紧，不允许接受未经完整计算的界。

#### 6.4.2 提交、依赖和终止

- 一个 proof 只提出一个 `(target row, target term, helper row, L)`。提交前基于最新两个 row version 和 scope 重新计算；成功后一次只改**一个系数**，立即使该目标行的其余 proposal 失效，递增 revision，并重新执行 `CS→GCD→RoundingSat cardinality normalization→传播`。若目标属于 equality，两个方向的 group staging、identity 和 undo 必须原子更新，即使另一方向内容未变化也要记录同一 group revision。
- 禁止把多个“分别安全”的系数同时提交。反例 `5x+5y+5z>=6; x+y+z>=2` 中，每个系数相对旧行都可单独 `5→1`，但三个一起改成 1 会把模型数从 4 变成 0；逐个提交并重算不会犯此错误。
- 派生行依赖辅助行继续有效。辅助行必须是 frozen 原行，或其完整 proof dependency closure 不含当前目标行；禁止目标↔helper 的循环强化。辅助行若要失活、删除或换 scope，必须先撤销派生版本或在当前保留系统中重新证明；不能删除 helper 后孤立使用旧重写。
- 非 LP 规则自身只返回 `UNCHANGED/PROPOSED/INVALID/BUDGET` 等内部结果；只有完成 proof、overlay、solver/ComponentManager 同步后，主入口才返回 `ROW_CHANGED/ROW_REMOVED`。它不直接产生 Reason 或真实节点 `CONFLICT`。
- 每次真缩紧都使正整数系数势函数严格下降，所以在固定状态和有限预算下终止。回溯恢复旧版本后，revision/scope 必须避免 ABA 复用。

**固定验收例**：

- `8x+3y+2z>=5; y+z>=1`：目标 x 的 `8→3`，完整系统改写前后都是 4 个模型；helper 必须保留。
- `8x+4y+3z+2w>=6; 2y+2z+t>=3`：目标 x 的 `8→3`，支持集不同，前后都是 13 个模型。
- `10x+3y+2z+4w>=7; y+z+w>=2`：精确差值界得到余项下界 5，x 的 `10→2`，前后都是 6 个模型。
- 无辅助行的 `8x+3y+2z>=5` 只能先由 CS 得到 `5x+3y+2z>=5`，不得冒充 Multi-row SC 得到 `8→3`。
- 另测 helper 删除、相反极性、批量提交反例、兄弟分支、强回跳、等式展开、`>2^53`/`>2^63` 和预算耗尽；小实例必须比较**完整模型集合**，不能只比较模型数或 SAT。

本关仍遵循 view-only → committed rewrite 两步验收。实际提交前必须通过 residual cache、Bypass oracle、overlay、Reason 生命周期和分量一致性关卡；首版无条件 Multi-row SC 不宣称直接增加 split。

---

## 7. 关卡 4：约束简化——逐个实现

### 7.1 关卡 4A：RIR（`Simplifier::rir()`）

- 将 BasicCheck 提供的 `gap<=0` 恒真、`gap>U` 冲突状态转成**当前作用域可撤销的结果**。BasicCheck 不重复正式失活；RIR 只在一次正确验证后提交 active-mask 的变化或冲突返回。
- 已满足行本来就被 `searchComponent()` 忽略，因此先验证本规则是否仅做已有功能的增量维护；不必为了“技术数”制造重复扫描。
- 禁止永久删除一个只在当前分支已满足的约束；undo 后必须恢复。

**验收**：满足约束不再连接分量、回跳后恢复；BasicCheck 和 RIR 不重复计算收益或因子。

### 7.2 关卡 4B：RPR（`Simplifier::rpr()`）

- 将 canonical 后的相同 LHS（相同 literal/系数，不仅 support 相同）约束分组，保留最大 RHS 的 `>=` 约束，删除较弱行。正比例缩放要经过精确整数归一化。
- `same LHS`, `different RHS` 简单例：`2x+2y>=2` 与 `x+y>=1` 规范化后重复；`x+y>=2` 蕴含 `x+y>=1`，应保留前者。
- 对互补方向，不得把不一致等式当作普通平行冗余删除；检查约束方向和 canonical literal polarity。
- hash 只筛选 bucket；**完整行内容+degree 精确比较**后才能删除。任何写入 overlay 的删除要通过“当前其余保留公式蕴含目标行”验证。

**验收**：行重排/哈希碰撞/取反文字/多 RHS/等式展开测试；无 false delete；删除桥接行使分量数增加时 exact count 一致。

### 7.3 关卡 4C：Generalized RSR（`Simplifier::rsr()`）

- 先实现传统 support subset 规则的精确 PB 充分条件，再实现“一般系数/极性/不同 RHS 的双行差值活动界”扩展；每一层单独验收。
- 对 `C_i:L_i>=k_i`（保留）与 `C_j:L_j>=k_j`（拟删），若能证明

$$
\min_{x\in\{0,1\}^n}(L_j-L_i)\ge k_j-k_i,
$$

则 `C_i` 蕴含 `C_j`，可删 `C_j`；允许有常数项和正负文字，按逐变量有符号系数计算最小值。带正比例的版本另作为增强选项，不与基础版同时改。
- 示例 `3x+2y>=3` 蕴含 `4x+3y+z>=3`；反例 `3x+y>=3` 与 `x+3y>=3` **互不蕴含**，不得删。
- 在某当前公式中按顺序删除时每次证明基于“尚保留的其余约束”；禁止两条互相蕴含的行在同一批次都因对方存在而被删。
- 不把“RSR 调用成功”自动等同于新增 decomposition；实际测新连通分量的变化。

**验收**：精确 entailment oracle、反向极性、不同 RHS、哈希桶/候选索引、重算失效、undo、普通 MC、缓存 Bypass。

### 7.4 关卡 4D：RUP（`Simplifier::rup()`）

目标是证明目标约束被**其余保留约束**蕴含。对 `C:sum a_i*l_i>=k`，`U=sum a_i`，它的精确布尔否定为

$$
\neg C:\sum_i a_i\bar\ell_i\ge U-k+1.
$$

- 构造**临时独立的 propagation/proof context**：从活动集合去除目标 `C`，加入 `¬C`，执行 RUP 的 UP 闭包；冲突则允许删除；没有冲突或预算耗尽只表示**RUP 未证明**，不能判定该行非冗余。
- 禁止借助目标行本身、由目标行派生且失去独立证明的 learned constraints 或两条同时删除的行形成循环论证。使用干净的当前保留公式/证明依赖集合。
- 递归局部冗余只在当前 scope 有效，提供 active-mask、undo 和完整 proof；原 solver 中仍可能保留原行作 propagation，必须检查跨组件语义和冲突分析。
- 样例：`x+y>=1`, `x+~y>=1` 蕴含 `x>=1`；删除目标行 `x>=1` 后用 `~x>=1` 进行 RUP，传播导出 `y` 与 `~y` 冲突。
- 后续可探索 PBCounter DeleteConstraint 的 ADD 假路径，但与 RUP 是不同实现，不属于本关。

**验收**：有证明删除、无证明不删、目标行依赖污染、含负文字/等式/当前分支、deep undo/非顺序回跳、普通 MC 全通过。

### 7.5 Equality-specific Simplification（`Simplifier::equality()`，独立扩展）

这是一项独立增强，不等于第 1.4 节的强制 Equality-aware 兼容层。通用规则即使关闭本方法，也必须正确处理 equality 的两个方向。

首份 `pseudocode/12_equality_specific.md` 必须先回答“相比分别处理两个方向，联合处理实际新增了什么可证明简化”。第一版只允许研究：两个方向联合得到的更紧活动界、EqualityGroup 整体冗余证明、两个方向的成组 canonical replacement。暂不做变量代换、Gaussian elimination、消元或改变自由变量计数。

- 删除整个 equality 必须在排除该 group 两个方向后，分别精确证明保留系统蕴含 lower 与 upper；不得用 sibling 互相证明后同时删除。
- 成组改写一次生成两个新不可变 row version，并以一个 undo marker 发布；任一方向 proof/attach/cache staging 失败则整组回滚。
- 若独立伪代码与小实例不能显示超过“两个普通 `>=` 行分别运行现有规则”的新增效果，则该技术保持未实现，不为凑技术数增加重复 pass。

### 7.6 SB-LP、SC-LP、RIR-LP 尚不入主干

三个 LP/MIP 增强将在结构简化与 cache 改造后按独立关卡实现，但代码仍统一放在 `simplify.cc/.h`。不将它们捆绑到同一个 PR 中，也不预设它们优于 BICP、Multi-row SC、RIR 或 RUP。SC-LP 的数学契约见第 11.3 节。

---

## 8. 结构规则需要时再加入的增量状态与预算

### 8.1 三个不同概念

- `changed`：**已经提交**的新赋值、等价行改写、有效删行/失活所产生的数学进展；只读 cache normalization 不是 committed rewrite。
- `dirty`：旧活动界、gap、签名、候选检测可能失效，需要重新检查；dirty 不保证会有新化简。
- `revision`：一次具体状态的身份；约束内容、有效作用域或影响该规则的前提变化后递增/生成新身份；回溯不能复用同一个 revision 解释不同的行（防 ABA）。

### 8.2 为什么多行删除/蕴含规则要跟踪依赖

Multi-row SC、SC-LP、RPR、RSR、RUP、Equality-specific 或 RIR-LP 若用其他保留行证明目标改写/删行，那么辅助行变化或失活后，旧证明可能失效。首版直接复用 ComponentManager 已有 frozen occurrence 作为保守超集，过滤 inactive row/removed term；必要时再增加轻量 dependency stamp，不先复制 `var → rows`。CS/GCD/RoundingSat cardinality normalization 是单行等价规范化，不产生辅助行依赖，但 equality group commit 仍要同步两个方向的版本。

### 8.3 事件调度伪代码

```cpp
void assigned(Lit p) {
    for (RowId r : occurrences(abs(var(p)))) dirty(r);
}

void rewritten(RowDelta d) {
    check(d);
    undo.push(d);
    residual.apply(d);
    dirty(d.row);
    ++revision[d.row];
    topology_dirty = true;
}

void undoTo(int level) {
    undo.restore(level, residual);
    rebuildDirty();
}
```

一个共享 dirty 位图先保证正确性，各阶段在自己的 pass 中读取后由节点循环统一清理；不要让某条规则提前清掉其他规则需要的信息。只有重复蕴含检查成为真实开销后，才记录 `(target, target_rev, helper, helper_rev, scope)` 跳过无变化重试。预算归当前节点，不因外层 repeat 重置。

### 8.4 工作量上限（先用固定参数，后期自适应）

- 每节点 BICP 最大候选变量数、双向 probe 次数和传播访问数；
- Multi-row SC 与 RSR 每行/每节点最大 pair 比较数、nnz 和精确有理运算量；
- RUP 每节点最大目标行数、UP 访问数；
- SB-LP 每节点最大候选变量/MIP 次数，SC-LP 与 RIR-LP 最大目标行/目标系数/MIP 次数；
- 单节点、整实例总体时间或工作上限；
- 执行结束标记 `complete_fixpoint` 或 `budget_limited`，不把预算耗尽误说成理论不动点。

**注意**：候选选择、候选数、探测/MIP 时间本身也属于成本统计。`m^2` 行比较可通过 occurrence、签名和 top-Q 候选降低，但“平均复杂度下降”必须由真实测量支持。

---

## 9. Reversible Overlay、Reason、RoundingSat 与分解一致性

### 9.1 四层事实源

1. `FrozenFormula`：ComponentManager 初始化时复制的不可变原始约束与局部 ClID；
2. `SolverFormula`：RoundingSat 中已 attach 的 `Constr`、watch、slack、Reason、learned；
3. `ResidualOverlay`：当前 decision scope 中已证明等价的行改写、失活、局部公式身份、依赖与 undo；
4. `Counting/Cache`：当前普通 MC 作用域、ComponentKey、缓存 value 与分支/分量作用域。

**硬约束**：四层的语义必须一致。只改 RoundingSat 不改 ComponentManager，可能无法获得分解收益；只改分解图而不保留有效的证明前提，可能产生错误分解；只改 overlay 后还使用旧 key 可能误命中。

### 9.2 具体要求

- 实际 rewrite 必须与当前作用域原公式**等价**，且在本作用域始终保留其依赖前提；如果用添加全局被蕴含的 constraint 代替一部分结构 rewrite，应保证添加的是原公式的真正 consequence。
- **不原地修改或释放仍可能作为 Reason 的 attached `Constr`**。结构改写先生成新版本；旧版本的传播、watch、Reason 和回收按下一节生命周期处理。不能仅因为 overlay 已换成新行就销毁旧对象。
- `CompMgr` 计算分量、动态启发式、cache builder 必须读取**同一 residual view**；构造 cache identity 不允许用新 gap + 旧 coefficients template。
- `Reason[v]` 的目标需使用其对应 allocator 的当前合法引用；GC、backjump 后不能保留失效裸 `CRef`。
- 多行删除的辅助前提必须在仍保留行集合中有效。若某行计划被删除并且其他行证明依赖它，必须禁止该组合或重新证明。
- reason 提交 helper 必须适配 PBMC 的选择性 backjump/`limlevel`；先放在已有 Counter/RoundingSat 接口附近，不为它预建一层 Bridge 类，也不能直接调用 unrestricted 回跳 API。
- BICP 的临时约束、watch、trail、reason 和 EqualityGroup staging 必须完整撤销；其结果不应自动被永久 learn 到全局。
- EqualityGroup 的两个方向可以有不同 row content/version，但一次 committed group 更新只有一个事务结果；solver attach、CompMgr overlay 或 cache staging 任一失败都撤销两边，禁止半发布。

### 9.3 Reason 约束的不可变版本与生命周期

源码已确认：`Reason[v]` 保存 solver allocator 中的 `CRef`；`Solver::reduceDB()` 会跳过当前仍被任一变量 Reason 引用的约束，allocator GC 会重映射 watch 和 `Reason` 中的 `CRef`。新框架必须保留这两项安全性质，不能绕过 solver 的回收与重映射协议。

还要注意，当前 `removeLastAssignment()` 弹出 trail 时没有清空 `Reason[v]`；这个 stale 引用会让 `reduceDB()` 保守地继续保留旧约束，通常是内存/回收延迟而不是 use-after-free。版本管理在未证明 RoundingSat 允许清空该字段前，不得擅改公共回溯语义；判断 `RECLAIMABLE` 时要么接受延迟回收，要么另行证明并测试安全的 reason-pin 释放接口。

对每条可提交改写的行使用简洁但明确的身份：

- `TemplateID`（或母版 ID）标识不可变的 canonical literal/coefficient 内容；
- `RowVersion` 标识源行、精确内容版本、gap、有效 scope 与当前 solver 引用；
- raw `CRef` 只在所属 allocator 和当前 GC 世代中有效，不能充当跨 GC、跨 ComponentManager 的永久 ID；
- 母版 ID 不能单独代表 cache 数学身份；版本必须能恢复实际系数、文字、gap 与依赖前提。

最小生命周期为：

```text
ACTIVE
  └─ 生成并启用新版本后 → RETIRED_REASON
       └─ solver 中已无 Reason/证明/临时依赖 → RECLAIMABLE
```

具体协议：

1. `ACTIVE` 版本可参与当前 solver 传播和 residual 视图；改写时创建新对象/版本，不原地改系数、文字或 degree。
2. 旧版本若仍被 live trail 的 `Reason` 引用，转为 `RETIRED_REASON`：不得销毁，其内容保持不可变。是否移除其 watches 必须由一个能“停止未来传播但保留 Reason 对象”的安全接口完成；在该接口实现并验证前，committed rewrite 不得上线。
3. 回溯/回跳先撤销 overlay 和活动版本，再按已验证接口处理被弹出赋值的 reason pin；没有该接口时保持保守 pin。随后恢复父 scope 的版本，兄弟分支不能看到上一个分支的活动版本。
4. 仅当 solver 实际 `Reason` 引用、冲突证明和临时 probe 均不再依赖旧版本时才标记 `RECLAIMABLE`，并交给 solver 的合法回收路径处理。不得只看 live trail 就手工释放，也不得留下指向已释放 allocator 对象的裸 `CRef`。
5. GC 后检查所有 version→solver 引用和 `Reason[v]` 都指向重映射后的对象；`reduceDB()` 现有“Reason 锁定则跳过删除”的检查必须继续生效。

验收至少覆盖：某行先推出文字并成为 Reason，随后该行被缩紧/替换/失活；在冲突分析、普通回溯、非顺序回跳、GC/reduceDB 和兄弟分支中，原 Reason 均可解释且不会继续错误传播。

### 9.4 分量图处理准则

当某条行变成恒真或已证明被其他行蕴含时，可在当前 overlay 中失活；只有当 `searchComponent()` 的遍历真正忽略这条行时才可能新增分解。测试示例：

$$
8x+3y+2t\ge5,\quad 5x+3y+2t\ge5,\quad
5x+3y+2t+z\ge5,\quad z+w\ge1.
$$

CS 后 RPR 删除重复行，RSR 删跨分量桥接的冗余行，得到 $(x,y,t)$ 和 $(z,w)$ 两个独立分量，总模型数 $5\times3=15$。**必须同时在已验收 residual cache 与 Bypass oracle 下通过这个实例；仅返回相同 count 而没有新增 split 不算实现了“分解导向”的目标。**

### 9.5 学习约束与分量分解

源码行为已经核实：`ComponentManager::init()` 在 `count_main()` 开头复制当时的 solver constraints 并建立 occurrence/ClID；搜索中 `ResolveWithoutLit()` 后续 attach 的 learned constraint 不会再复制进这个冻结分量图。因此当前 PBMC 实际上已经采用“基础公式决定分量，搜索期学习约束用于传播/冲突剪枝”的策略。

该策略保留，但必须满足以下边界：

- 只有已证明是**当前基础公式的全局逻辑后果**的 learned constraint，才可作为 solver-only 行，不进入分量图和 cache identity；这不会改变基础公式的模型集合。
- 基础行的等价替换、局部删行、系数/支持集变化不是普通 learned acceleration 行；它们必须进入 `ResidualOverlay`，并同步影响分量图与 residual cache identity。
- BICP/RUP 临时假设产生的冲突或局部 scope 结论不能无 guard 地 attach 成全局 learned constraint。若依赖当前决策前缀，必须有合法 guard/scope、undo 和 proof。
- 即使 learned row 不参与分量构图，一旦它成为某个 trail 赋值的 Reason，也必须遵守 9.3 的生命周期；“不进分量图”不等于可以提前释放。
- 必须增加跨分量测试：solver-only learned row 可在基础分量之间触发传播或冲突时，组件栈、cache ownership、选择性回跳与普通 MC 仍正确。

因此，不把所有学习约束加入依赖图可以避免人为合并可独立计数的基础分量；但这项优化的前提是全局蕴含证明和严格区分 learned acceleration 与 residual replacement，而不是简单按“learned”标签忽略任何新行。

---

## 10. Cache 详细设计：关卡 2D/2E，必须在 CS/SC 前完成

### 10.1 用户问题的直接答案

**采用用户建议：变量简化完成后立即做 cache，cache 完成后才做 CS/SC。** 当前代码仍不能直接关闭 cache，所以先完成关卡 2D 的整次运行 Bypass，再完成 2E 的 residual component cache。`-cs` 只是最小值为 1 的容量，源码还硬定义 `CACHE`，所以 `-cs=0/1`、清 bucket、取消单个宏都不是答案。

**注意区分开发顺序与运行顺序**：cache 虽然先开发，运行时仍是 `Simplification → Decomposition → Cache lookup`。`view-only` 系数规范化不发布新 row version；`committed-rewrite` 必须发布 residual identity。实际上 RIR 如果只是现有 `gap<=0` 时跳过行，旧键可能仍正确；但 CS/GCD/Cardinality/Multi-row SC/SC-LP 改系数、RPR/RSR 动态 active-mask 删行以及 EqualityGroup 更新，都统一走 residual key，不再为每种规则保留例外生产路径。

### 10.2 按阶段允许的模式矩阵

| 阶段/改变 | Legacy cache | Bypass cache | Residual cache |
|---|---|---|---|
| 空模块、原生传播、逻辑 RV | 允许（先测试） | 关卡 2D 后允许 | 2E 后允许并与前两者差分 |
| BICP 仅产生已证明赋值 | 通常允许（测试后） | 关卡 2D 后允许 | 2E 后允许 |
| CS/GCD/RoundingSat card normalization/Multi-row SC/SC-LP view-only | 允许：仍按冻结原公式建 key | 关卡 2D 后允许 | 允许，但不得发布未 commit 的候选 row |
| CS/GCD/RoundingSat card normalization/Multi-row SC/SC-LP 实际改系数/支持集 | **禁止** | **只作无复用 correctness oracle** | **2E 后的生产模式** |
| RPR/RSR/RUP 动态删行并改分解 | **禁止** | **只作无复用 correctness oracle** | **2E 后允许** |
| EqualityGroup 任一方向改写/失活或整体删除 | **禁止** | **Bypass + 双向原子 overlay 后仅作 oracle** | 两方向成组 identity 后允许 |
| 任意变量代换/消元 | **禁止** | 仍须额外 ledger/证明，本轮不做 | 将来专项 |

启动阶段必须拒绝不安全组合。Bypass 在 `cmpmgr.init()` 前选定并保持整次运行不变；成套处理 root/create/request/link/store/erase/pollution 路径，同时保留 Component/Decision 栈、分量边界、分支求和和分量求积。不能搜索中途在 Legacy 与 Bypass 间切换。

### 10.3 EDPLLSim 缓存方案可以迁移什么

本次不是只读设计说明，而是完整核对了 EDPLLSim 的真实实现：

- `cache.h/.cc::ComponentNormalizer` 生成规范逻辑 component；`ComponentKeyBuilder::buildForLookup()` 只 probe 模板，`buildForInsert()` 才 intern 并 acquire；
- `SerializeRowTemplate()/SerializeComponentKey()` 用版本号、ULEB128、delta-coded `TemplateID` 和带符号大整数编码；64-bit FNV-1a 只定位 bucket，`operator==` 比较完整 bytes；
- `RowTemplatePool::releaseKeyReferences()/reclaimUnreferenced()` 保证 live key 引用期间 ID 不复用；
- `ComponentCache::find()/insert()` 按插入序列老化，命中/更新不续期；删除 entry 时释放模板引用；
- `LinearConstraint::findCache()` 在 template missing 时直接 miss，`insertCache()` 才取得引用；`initializeCacheForInstance()` 明确清理实例生命周期；
- `Solver::count()` 的缓存值是简化后 residual system 的 count，自由变量因子在 cache value 外单独乘。

这些实现提炼为以下不变量：

- **基于当前真实内容的 exact component identity**，不能用原始 row ID 或 `SC=true` 代替系数结构；
- 所有行先做数学上等价的 canonical normalization，再做无歧义序列化；
- hash 只定位 bucket，命中必须完整 key equality；碰撞不能 false hit；
- 若需要共享行模板，按**immutable canonical literal/coefficient 内容** interning；不同 RHS/gap 分开放到 component key；
- 新模板先内容比较，再给唯一 TemplateID；lookup 可以 probe-only，不在 miss 时永久创建模板；
- 模板 ID 在所有引用其内容的 live key 释放前不可复用；eviction/backjump 必须正确管理 refcount 和 ownership；
- cache key 的生成不得为了命中率去改动 solver 中原始系数；Gap saturation 是 key-only，不是实际 CS。

代码迁移边界如下：

| EDPLLSim 部分 | PBMC 决定 |
|---|---|
| `SerializedComponentKey`、hash 只定位、full byte equality | **直接迁移代码形状**，换成 PBMC 命名并保留来源注释 |
| ULEB128、带符号大整数序列化、TemplateID delta encoding | **迁移并适配 `rs::BigVal`**；必须同时覆盖 `LOW_PREC` 与 GMP build |
| `RowTemplatePool` 的 probe/intern/refcount/reclaim | **迁移核心算法**，由 PBMC cache/manager 实例拥有，不使用 EDPLLSim 的 `LinearConstraint` 静态全局 |
| 插入序列、命中不续期、bucket/oldest 链 | **迁移机制**，与 PBMC 现有 MB hard limit 和 pollution ownership 合并 |
| `ComponentNormalizer` 的 `ranges + dense Ax<=b + GCD/floor` | **不能迁移逻辑**；改写为布尔 literal、规范 `>=` residual row 和 EqualityGroup 双方向 builder |
| `N*M` admission、`50*1024` age、`20*age`、bucket 常数 | **不直接复制**；作为 benchmark 初值，最终由 PBMC 压力测试决定 |
| `Solver::count()` 的递归复制/自由变量位置 | **不能照搬流程**；PBMC 保留显式 Decision/Component 栈、分支和/组件积协议 |

当前仓库根目录提供 MIT `LICENSE`，但未在 `AAAI_EDPLLSim/` 子树找到单独 license 文件；实际复制非平凡代码前仍应确认根 license 对该子树适用，并在迁移处保留来源说明与许可文本要求。不能以“直接迁移”为由复制出与现有 `ComponentCache` 并行的第二套 manager。

### 10.4 新组件身份只走一条分阶段路线

先实现直观的调试 `ReferenceKey`：

```text
ReferenceKey = (
    ordered global residual variables,
    sorted standalone active rows [(literal, exact coefficient, exact gap), ...],
    sorted equality groups [(direction A content/state, direction B content/state), ...]
)
```

它从实际 residual view 生成，行内容包含文字极性、精确系数和与该内容配套的 residual gap；equality 的两个方向当前内容及 active 状态成组序列化，不能只写原始 equality ID，也不能把运行历史中的 revision 当作数学身份。hash 之后必须完整比较。先用它作为 oracle 证明 Bypass 与 residual cache 的每次命中都等价，再把每条 immutable row 内容 intern 成 `TemplateID + gap`。`ReferenceKey` 只用于 debug/test；生产只有紧凑模板 key，一次运行不能同时维护两套可命中的 cache。

PBMC row canonicalizer 至少执行：按全局变量与极性排序、合并重复文字、精确处理互补文字产生的常数、移除零项、对 residual assignment 消去已赋值文字、使用只依赖该行的精确 Boolean saturation/GCD 规范化，并按 `(signed literal, coefficient, gap)` 排序行。相同 canonical LHS 的 `>=` 行只保留最大 gap；相同完整行去重。所有操作只构造 key，不修改 attached solver row。若 scope/guard 不同且不能证明在当前 component 上等价，则必须保留区分。builder 返回 `NORMAL/TAUTOLOGY/CONTRADICTION/INVALID`；传播不动点后若仍得到 `CONTRADICTION`，应回到真实节点冲突协议或安全计数 0 路径，不能伪装成普通 cache hit。

当前 cache 是单实例、全局变量 ID，不跨实例持久化，也没有组件内局部变量重命名。本计划第一版保留这些边界，不照搬 EDPLLSim 的局部列号；固定为普通 MC，因此 mode 不重复塞进每个 key。每个新 instance 必须 clear persistent entries 后再清模板池，销毁顺序保证 entry 先释放模板引用；若以后加入局部重命名、变量消元或跨实例复用，再增加格式版本和所需元数据。

旧未改行可以沿用 immutable 内容；派生系数不同的行必须得到不同模板身份。一条源行的 revision、changed、branch timing、K、预算和启发式顺序是运行历史，不属于数学 key。允许等价公式因 canonical 不完整而少命中，绝不允许不同 residual 系统 false hit。

### 10.5 模型计数与缓存值的作用域

本计划只维护 cache payload 中的 `mpz_class` 无权组件 count；分支求和和组件求积继续由 `Decision` 完成。组件外已经结算的自由布尔变量因子不属于子组件 key/value。现有 weighted/d-DNNF payload 不扩展；启用新框架时这些模式已被拒绝，既有 `NodeIndex` 接口只保留兼容哨兵值而不进入 residual identity。

- 互斥分支是**求和**；不共享变量的独立组件是**求积**。
- 固定变量在普通 MC 中贡献因子 1；自由布尔变量贡献因子 2。
- 缓存命中不能重复应用进入组件前已经结算的自由变量因子；缓存写入不能包含不属于该 key 的父作用域因子。

### 10.6 Residual cache 专项关卡顺序（即 2D/2E）

1. **Bypass 基线**：复用关卡 2D，在没有结构改写时验证 Bypass 与 Legacy 的 oracle 答案一致；确认分支加法/组件乘法、深层回跳和状态清理不出错。
2. **ReferenceKey**：构造当前真正用于分解的完整 canonical residual formula，包含全局活动变量、正负文字、精确系数、gap，以及每个 EqualityGroup 的两个方向/active 状态；普通 MC mode 由运行上下文断言，不重复序列化进每个 key。
3. **Residual builder**：从 overlay 读取真实结构，先生成完整 canonical bytes；保证 key 与 solver/compmgr 语义一致。
4. **Packed/template key**：迁移 probe-only lookup、insert-time intern/acquire、版本化 bytes、`TemplateID + gap` 和 full compare；ReferenceKey 保留为测试 oracle。
5. **Residual lookup/store**：hash→full compare；cache value/node ownership、临时 active entry 与 persistent entry 的引用责任对齐。
6. **Backjump/eviction**：模板生命周期、GC、缓存污染、descendants/ownership、实例 clear 和极小 cache limit 压力测试；当前 `deleteEntries()` 无 deletable entry 时不得访问空 `scores`。
7. **与 Bypass 差分**：普通 MC 小实例无任何数值差异；随机实例、深回跳、巨大整数、hash 强制碰撞、不同历史同式、同 ID 不同系数全部通过。
8. **Cutover**：residual cache 全套验收通过后才进入 3A CS；后续生产/论文测评用 residual，Bypass 继续作为无缓存 oracle。

### 10.7 缓存必须具备的反例测试

- **同一 frozen ClID + 同一剩余变量集合 + 同一 residual gap，但系数内容不同**，必须产生不同 residual identity（或经 exact canonical equivalence 证明相同），不能命中错误结果。
- 例如原始 `4x+2y+2z>=4` 在 `z=0` 与 `z=1` 两个分支分别留下 `4x+2y>=4` 与 `4x+2y>=2`，后者可饱和为 `2x+2y>=2`。这个例子首先用于检查**母版内容与所使用的 gap 必须配套**，并不声称两个不同 gap 会在原生旧键上直接碰撞。更强的专项反例由 Codex 构造：**同一被复用源 ClID、同一剩余变量列表和数值 gap，但不同真实派生系数/依赖前提，且对应 residual 模型数不同**；用 ReferenceKey 区分，并证明旧键在这类动态结构状态下不足以唯一确定计数。若无法构造该反例，必须说明何种冻结母版约定可以保持旧键正确，不能仅凭“修改过系数”就断言一定出现 false hit。
- 当前母版相同但 residual scope/guard 不同，不得随意复用；即使同一作用域下可以合法复用 count，也不能把父作用域已经结算的自由变量因子再应用一次。本轮不为 projected identity 增加字段。
- 不同约束哈希强制同一 bucket，完整 key 不同不得 false hit；不同 canonical 形式但布尔语义相同可不命中（性能损失可接受）。
- 同一 canonical row template、不同 exact gap 必须共享模板但产生不同 component key；`>2^63` 和 `LOW_PREC` 上限附近的系数/gap 序列化后逐字节 round-trip。
- lookup 遇到不存在的 row template 必须直接 miss，模板池大小和引用计数不变；insert 才 intern/acquire。重复 insert/update 必须释放本次多取得的引用，不能让 refcount 随命中次数增长。
- hit 后删除临时 lookup/ownership entry、pollution cleanup、age eviction、cache clear 和实例结束都逐项检查 refcount；最后一个 live key 删除后模板才能 reclaim/复用 ID。
- EqualityGroup 两个方向顺序 canonical；只改一个方向的数学内容或 active state 时 key 必须变化。仅 revision 编号变化而内容相同可以得到同一 key；半更新 group 必须在 publish 前拒绝而不是进入 cache。
- MB hard limit 下没有 deletable entry 时必须安全跳过/延后淘汰，不得像当前 `deleteEntries()` 那样对空 `scores` 取中位数。

---

## 11. 关卡 5：SB-LP、SC-LP 与 RIR-LP（全部默认关闭）

> 本关在 residual cache 后进行。EDPLLSim 将这些函数命名为 LP，但源码实际把列设为 `GLP_IV`，先 `glp_simplex()` 再 `glp_intopt()`，所以执行的是整数优化/MIP。PBMC 文档沿用技术名 SB-LP、RIR-LP，但不能把 GLPK 的浮点 `optimal` 当作 exact proof。

### 11.1 可直接参考与不能照搬的部分

已完整核对 `AAAI_EDPLLSim/src/linear_constraint.cc`：

- `LP()` 把一般整数域的 `Ax<=b` 装入 GLPK，行用 `GLP_UP`，变量用 `GLP_DB + GLP_IV`；
- `OptimizeIntegerObjective()` 统一处理 simplex/MIP 的 optimal、infeasible、unknown；
- SB-LP 对每个变量分别求最大值和最小值并缩紧 domain；
- RIR-LP 暂时放开目标行，在其余行下最小化/最大化目标 LHS，再决定冲突、保留或删除；
- SC-LP 不照抄 EDPLLSim 中现成函数，而是复用同一 GLPK 建模/状态/representability guard，在 `F\{target}` 下最小化目标余项，并套用第 6.4 节的等价定理；
- 所有 range、系数、RHS 以及可能的 LHS 绝对值不能精确落入 double 连续整数区间时，直接返回 no-change；
- `tests/rir_lp/rir_lp_test.cc` 已覆盖 infeasible relaxation、常量冲突、冗余行、`2^53` 边界、`3e9`、随机 brute-force 和 K 路径。

PBMC 的变量固定为 Boolean，输入是带正/负文字的 `sum(a_i*l_i)>=degree`。装入 GLPK 前必须先在当前 residual scope 展开 `~x=1-x`，得到带符号的 `x` 系数和正确常数项；可以转成 `<=` 后复用 EDPLLSim 方向，也可以使用 `GLP_LO`，但全项目只能选择一种并用 exact oracle 校验。不得直接把 PB 的 `degree` 当成 EDPLLSim 的 `b`。

### 11.2 SB-LP（`Simplifier::sbLP()`）

- 在当前活动 residual 系统下，对候选未赋值 Boolean 变量求 `min x` 和 `max x`。GLPK/MIP 返回 `min=1` 时只产生 `x=1` 候选，返回 `max=0` 时只产生 `x=0` 候选；其他结果不固定。
- 每个候选必须再由 RoundingSat 精确验证相反假设导致冲突，并生成合法 PB Reason；验证失败或预算耗尽返回 UNKNOWN。不能把 GLPK 的 double 目标值直接写入 trail。
- 当前真实系统被 GLPK 判 infeasible 时，也必须由原生 PB 冲突路径复核后才能作为节点冲突。
- 复用 EDPLLSim 的 representability guard 和状态处理，但 Boolean reason、trail、回跳、ComponentManager 与 cache 接入必须按 PBMC 重写。

**验收**：普通 MC brute-force、正负文字、当前赋值后的 residual 常数、`2^53`/`3e9` 边界、GLPK unknown、精确复核成功/失败、Reason 生命周期和回跳。

### 11.3 SC-LP（`Simplifier::scLP()`）

SC-LP 与第 6.4 节使用**同一个等价定理**，区别只在于余项下界的候选器可以联合当前全部活动辅助行，而不是只看一条 helper。对目标行与目标项

$$
C_i:a_j\ell_j+R_{ij}\ge k_i,
$$

令 $G$ 为当前 residual 系统中除目标行 $C_i$ 以外的保留约束。GLPK/MIP 在 $G$ 下最小化 $R_{ij}$，只产生整数下界候选 $L$；目标行必须同时从优化模型和 exact checker 的前提中排除，任何依赖目标行本身的派生行也不得形成循环证明。

候选提交前必须精确验证

$$
G\models R_{ij}\ge L.
$$

若 $R_{ij}=\sum_t a_t\ell_t$ 且 $U_R=\sum_ta_t$，可把反例条件 $R_{ij}\le L-1$ 精确编码为

$$
\sum_ta_t\bar\ell_t\ge U_R-L+1,
$$

然后在不含目标行的隔离 RoundingSat proof context 中检查 `G ∧ (R<=L-1)`。RUP 冲突可以作为充分证书；RUP 未证明时允许调用有明确预算、状态完全恢复的完整 PB UNSAT checker。返回 SAT、UNKNOWN、超预算或无法生成可核验结果都只能 `UNCHANGED`，不能采用 GLPK 的浮点结论。

- 若 exact checker 通过且 `0<=L<k_i`，只提交 `a'_j=min(a_j,k_i-L)`；
- 若 `L>=k_i`，只生成目标行冗余候选，交给与 RIR/RUP 相同的删除、依赖与 undo 协议；
- 若 GLPK 认为 $G$ infeasible，只能生成当前节点冲突候选，仍需原生 PB 精确验证；
- 与非 LP Multi-row SC 相同，一次只提交一个目标系数，然后使同一目标行的其他候选失效，重建/更新优化状态并重新执行 `CS→GCD→RoundingSat cardinality normalization→传播`；
- learned row 只有在已独立证明为基础 residual 系统的全局逻辑后果且生命周期有效时才能辅助 checker；局部 probe/旧派生依赖不得混入 $G$。

固定联合多行例：

```text
8x+y+z+w>=3
y+z>=1
y+w>=1
z+w>=1
```

后三行联合证明 `y+z+w>=2`，所以 SC-LP 可将目标 x 的系数 `8→1`，完整系统前后都是 5 个模型。任意一条 helper 单独只能给更弱的界；这个结果不得写成两行 `multiRowSC()` 必然达到。

**验收**：至少覆盖上述联合例、目标行污染、候选 L 偏大后 exact checker 拒绝、相反文字、`>2^53` 拒绝、GLPK UNKNOWN、一次一系数、helper 失活、回跳/兄弟分支、Bypass/residual cache 与完整模型集合比较。

### 11.4 RIR-LP（`Simplifier::rirLP()`）

- 对目标 `C:L>=degree`，在 `F\{C}` 下最小化 `L`；只有 GLPK 提出 `min L>=degree` 且 exact RUP/native checker 再次证明 `F\{C}∧¬C` 冲突时，才能删除 C。
- 若 GLPK 认为 `F\{C}` 不可满足或认为加入 C 后不可满足，只能生成冲突候选，仍交给原生 PB 冲突验证；不能直接返回当前节点 0。
- 目标行必须从优化模型和 exact checker 的前提集合中同时排除。先前已删除行保持排除，避免互相依赖和循环删除；每次成功删除都记录 dependency、revision、undo 和 residual cache identity。
- RUP 已经证明的行无需再调用 GLPK；GLPK unknown、不可精确表示、超预算或 exact checker 未通过都返回 UNCHANGED。

**验收**：移植 EDPLLSim RIR-LP 回归反例并改成 Boolean OPB；增加目标行污染、连续删除依赖、负文字、等式展开、非根 scope、GC/Reason、回跳、Bypass/residual cache 和普通 MC 对照。

### 11.5 GLPK 依赖与构建接入

EDPLLSim 已提供 `utils/glpk-4.65.tar.gz`，`build_utils.sh` 通过 `configure → make → sudo make install → ldconfig` 安装，Makefile 用 `-lglpk` 链接。当前机器也已经存在 GLPK 4.65 的头文件和动态库。

PBMC 实现时不把下载、解压或 `sudo` 安装偷偷放入 CMake。增加默认关闭的 `PBMC_USE_GLPK`，用 `find_path(glpk.h)` 和 `find_library(glpk)` 检测；找不到时配置阶段明确失败。`simplify.cc` 始终是唯一简化实现文件：关闭选项时三项 LP 方法编译为明确的 disabled/no-change 路径，开启时才编译 GLPK 代码并由 `pbmc` target 传递链接依赖。静态构建需要另测 GLPK 的传递依赖，不能假设现有 `PBMC_STATIC_BUILD` 自动成功。

### 11.6 LP 专项报告

SB-LP、SC-LP 与 RIR-LP 分别报告 GLPK 调用数、candidate 数、exact checker 成功数、UNKNOWN/representability 拒绝数、时间和 MC 差分。SC-LP 额外报告目标项数、下界改善、系数缩紧与整行冗余候选。不能因为 GLPK 返回 `optimal` 就宣称满足 exact model counting；最终提交依据必须来自 RoundingSat 可验证的 PB 推导。

---

## 12. 关卡 6：所有单项完成后，才添加 Adaptive K

**先完成并冻结**：BICP、CS、GCD、RoundingSat cardinality normalization 接入、Multi-row SC、RIR、RPR、RSR、RUP 的单项正确性；Equality-specific 若启用则另行通过；若准备启用 LP，则 SB-LP/SC-LP/RIR-LP 通过“GLPK 候选 + RoundingSat exact checker”测试；residual cache 在真实结构改写下通过普通 MC 测试。随后才在 `simplify.h/.cc` 中加入 `adaptiveK()` 和专项消融，不创建 `AdaptiveK.h/.cc`。

**固定阶段顺序不变**：

| 阶段 | 不可关闭的基础 | 原始 Non-LP 增强 | LP 增强（证明合格时） |
|---|---|---|---|
| I 变量 | native UP、RV、BasicCheck | BICP | SB-LP |
| II 系数 | CS→GCD→RoundingSat cardinality normalization | Multi-row SC（首版一条 helper） | SC-LP（全部活动 helper 候选 + exact checker） |
| III 约束 | RIR→RPR→Generalized RSR | RUP；独立 Equality-specific（若证明有新增效果） | RIR-LP |

仅高级增强技术根据根节点计算一次的 $K^*$ 和当前阶段剩余变量数 `N` 选择路径；根输入自适应 K 公式来自 EDPLLSim 的历史方案，但**必须独立标定 PBMC**，不能机械复制 Jaccard-to-K 的常数与假设。原生 UP 不因 `N>K` 被 LP 取代；预算仍分别控制高成本规则；所有配置可记录可消融。

可选进一步研究基于当前 `m`, `nnz`, overlap, success_rate` 的预算，但不得与 K 规则同时改动使实验归因不清。

---

## 13. 通用测试框架：每关独立验收，未通过不继续

### 13.1 Brute-force Oracle：唯一权威的小实例参照

为 `n<=15`（可根据运行时间再扩大）的小 OPB 直接枚举原变量完整赋值：

- **MC**：符合全部约束的完整赋值数；

Oracle 使用精确整数计数。PMC/WMC/WPMC、投影集合和权重均不属于本测试框架。

### 13.2 每次提交必做的差分矩阵

| 运行模式 | baseline | 新规则单独开 | cache bypass | legacy/residual cache | 回跳开/关 |
|---|---:|---:|---:|---:|---:|
| MC | Prompt 0 PASS | 必须 | 必须 | 若安全必须 | 必须 |
| deep backjump | 尚未动态覆盖 | 必须 | 必须 | 若安全必须 | 必须 |

另设启动拒绝测试：`-inprocess` 与 `-mode=1/2/3` 或 d-DNNF 组合必须非零退出并给出明确诊断。这是范围保护，不是对这些模式做算法验收。

**阶段门槛优先 exactness**：每格以独立 oracle 为准，不以“新旧输出相同”代替正确性。新规则允许性能下降，但不能静默改变计数；出现不一致时缩减最小 OPB，记录 seed 和开关并阻止合并。

### 13.3 固定小实例集（逐项扩充）

1. **UP / RV**：`5x+2y+z>=4`，检查 x 固定且 residual correctly formed；
2. **Conflict**：`2x+3y>=6`（0 个模型）；
3. **CS**：`8x+3y+2z>=5` → `5x+3y+2z>=5`；
4. **GCD**：`8x+4y+2z>=5` → `4x+2y+z>=3`；
5. **RoundingSat cardinality normalization**：`3x+3y+2z>=5` → `x+y+z>=2`；
6. **Multi-row SC**：`8x+3y+2z>=5; y+z>=1`，目标 x 的 `8→3`，完整系统 MC=4；
7. **Multi-row SC 不同支持集**：`8x+4y+3z+2w>=6; 2y+2z+t>=3`，目标 x 的 `8→3`，MC=13；
8. **Multi-row SC 批量反例**：`5x+5y+5z>=6; x+y+z>=2`，禁止把三个 5 未重算就同时改为 1；
9. **SC-LP 联合多行**：`8x+y+z+w>=3; y+z>=1; y+w>=1; z+w>=1`，目标 x 的 `8→1`，MC=5；
10. **BICP 公共蕴含**：`x+y>=1; ~x+y+z>=2; ~y+t>=1`，两个完整 probe 都推出 y,t；
11. **BICP failed literal**：`x+y>=1; x+~y>=1`，`x=0` 冲突后推出 x=1；
12. **RPR**：`x+y>=2; x+y>=1`，删弱行；
13. **RSR**：`3x+2y>=3; 4x+3y+z>=3`，删后行；
14. **RUP**：`x+y>=1; x+~y>=1; x>=1`，删第三行；
15. **Decomposition**：上一节五变量桥接行例，MC=15；
16. **Operators / Equality-aware**：`x+y>=1` 为 3 个模型，`x+y<=1` 为 3 个模型，`x+y=1` 为 2 个模型；equality 两方向必须共存，原式不可当单行直接 CS；
17. **Signed coefficients / opposite literals**： `-3x+2~y>=4` 与正系数化式对照；
18. **Free variable**：`x>=1` 加声明但未使用变量 z，MC 要含 z 的因子 2；
19. **Large ints**：超 32 位系数、`3e9 x <= 3e9-1` 经合法方向转换，验证不发生 double 整数误删；
20. **Duplicate/Complement literal**：同一 PB 约束 `2x+5~x>=4` 必须先合并或明确等价解释，禁止原二项 probing 越界；
21. **Reason version**：旧行推出文字后被新版本替换，冲突分析、GC、回跳和兄弟分支仍能读取正确 Reason；
22. **Backjump / Hash**：强制多层非顺序回跳、两个兄弟分支不同 overlay；常量 hash + exact compare 不得 false hit。

每关还至少生成随机 5～12 变量的 `>=`、`<=`、`=` PB、负文字、负系数和稀疏/稠密实例；固定 seed；普通 MC 全部对 oracle。`=` 还要注入“第二方向漏失”“一边 commit 失败”“只恢复一边”的故障测试。若某测试因旧 baseline bug 失败，要记录 baseline 失败而不是静默修改预处理器。

### 13.4 结构改写必须增加的断言

- 改写前后在**当前决策前缀**下满足赋值集合相同；单行恒等改写可对每个自由变量赋值比对；多行规则必须比较整个当前公式（或者独立校验完整证明）。
- 约束支持集、occurrence、dirty/revision、ComponentManager active mask 和 solver proof 都能解释同一个 residual state。
- 撤销到父层后，overlay、reason/version 引用、lookup/key staging 与节点进入前状态一致。
- 不能对已经失效、被删除或来源改变的辅助前提继续使用旧证明。
- Bypass 模式不会创建可命中或可写入旧格式 key；缓存启用模式命中后要校验 key 的逐字段 exact equality。
- EqualityGroup 的两个方向在 commit、undo、cache publish 和 sibling branch 中共享同一事务边界；任何时刻不得出现半更新 group。

### 13.5 按关卡记录实际用到的统计

```text
per_rule: calls, successes, budget_exhausted, time, work_units,
          plus only that rule's real outputs (implied/removed/changed/proof)
bicp: variables_probed, branch_complete, branch_conflict, branch_unknown,
      failed_literals, common_implications, rollback_checks, reason_rechecks
cardinality: detected, content_changed, specialized_attached
equality: groups_seen, two_sided_checks, atomic_commits, atomic_rollbacks
multi_row_sc: pairs_attempted, bound_positive, proposals, commits,
              coefficients_reduced, proof_rechecks, dependency_invalidations
sc_lp: mip_calls, lower_bound_candidates, exact_checker_passed,
       coefficients_reduced, redundant_candidates, unknown_or_rejected
solver: decisions, conflicts, propagations, nodes, components, splits
cache: lookups, hits, misses, bypassed, inserts, evictions, bytes_peak
correctness: MC oracle failures, rejected_mode_failures, replay_seed
resource: wall_time, cpu_time, peak_RSS, ASan/UBSan status
```

不要在关卡 1 就加入所有计数器。Bypass 中真正 hash `lookups/hits/inserts=0`，`bypassed` 单独计数；模板统计到 residual cache 关卡再加。

### 13.6 单关验收报告模板

```markdown
# Acceptance — Gate XX / ShortName
- Commit / branch / build configuration:
- 真实修改文件（算法关卡只在既有 `core/simplify.h/.cc` 增加对应短方法；另列必要接入文件）：
- 本次实际执行模式（view-only / committed / cache bypass / residual）：
- 数学规则及证明/反例：
- `>=` / `<=` / `=` 三种关系的方向映射与模型集合结果：
- 调用位置和 scope：
- reason、undo、cache/overlay 影响：
- 单项启用/禁用 CLI：
- baseline/test fixture/random seed：
- MC oracle 对照：
- PMC / WMC / WPMC / d-DNNF 启动拒绝：
- backtrack / backjump：
- ASan / UBSan / stress：
- 运行时间、规则成功率、额外工作：
- 已知限制和未完成风险：
- 验收状态：PASS / FAIL / BLOCKED
- 下一关：仅用户确认后开始
```

---

## 14. 每关 Codex 必须采用的固定工作循环

1. **P 关只读定位**：说明数学语义、源码入口、与已有 RoundingSat 功能的重叠；只创建该技术的独立 pseudocode 文档，不修改 C++。
2. **P 关先定 oracle**：写出 `>=/<=/=` 正反例、完整模型集合、Reason/rollback/cache/equality 失败路径和预算 UNKNOWN；提交文档并停止等待用户批准。
3. **I 关只改统一模块**：获批后才在 `core/simplify.h/.cc` 增加该短方法；骨架/缓存关卡沿用现有文件，不为规则创建新 `.h/.cc` 或空类。
4. **事务式接入递归**：需要 commit 时才增加 reason/overlay/undo；EqualityGroup 两方向原子处理，cache 模式合法。
5. **完整差分验证**：单项开/关、`>=/<=/=`、Cache Bypass/合法 Native、普通 MC、兄弟分支和非顺序回跳。
6. **偏离伪代码即暂停**：实现若需要改变数学条件、状态返回或恢复协议，先停止 C++，更新 P 文档并重新审核。
7. **性能与统计检查**：运行固定小规模数据，记录真实规则命中和开销；只关注本项效果，不变更其他规则/启发式。
8. **提交验收报告**：列出获批伪代码版本、变更文件、失败案例、测试和配置；停在本关，等待用户批准。

**任何为了测试通过而关闭 conflict analysis 或把内部 probe 冲突冒充真实节点冲突的做法，均视为未通过。验收结论只能声明普通 MC；不得宣称 PMC/WMC/WPMC 或 d-DNNF 获得支持。**

### 14.1 推荐的真正实施顺序（逐项）

| 序号 | 完成项 | 缓存策略 | 必须验收后才能进入 |
|---:|---|---|---|
| 0 | 审计真实仓库、修订指南、保存 baseline | Legacy；只审计 Bypass | 用户审核 |
| 0B | MC oracle、Config UBSan、backjump 覆盖、模式拒绝、operator 基线 | Legacy | 0E |
| 0E | Equality contract：`<=` canonicalization、`=` sibling/group identity | Legacy | 1 |
| 1 | 删除旧空入口；`simplifyNode`、`SimpResult`、联合循环 | Legacy | 2A |
| 2A | Basic/UP/RV view | Legacy，若不改结构 | 2B |
| 2B | BICP 双向引擎与完整 rollback | Legacy，view-only | 2C |
| 2C | BICP failed-literal + 公共蕴含正式提交 | Legacy，若只推导赋值 | 2D |
| 2D | 整次运行 Cache Bypass | Legacy↔Bypass | 2E-P |
| 2E-P | `03_component_cache.md` 完整设计并审核 | 只读/文档 | 2E1 |
| 2E1 | canonical ReferenceKey 与动态 row-version 接口 | Legacy↔Bypass；ReferenceKey oracle | 2E2 |
| 2E2 | EDPLLSim 风格 packed key + RowTemplatePool | hash + full compare | 2E3 |
| 2E3 | PBMC component stack / father-descendant / backjump ownership | Bypass↔Residual | 2E4 |
| 2E4 | 模板生命周期、实例 clear、age + MB 淘汰 | Bypass↔Residual | 2E5 |
| 2E5 | Legacy↔Bypass↔Residual cutover 验收 | Residual；Bypass 作 oracle | 3A |
| 3A | CS（单行系数饱和） | Residual↔Bypass | 3B |
| 3B | GCD | Residual↔Bypass | 3C |
| 3C | RoundingSat cardinality normalization 接入验收（非新算法） | Residual↔Bypass | 3D |
| 3D | Multi-row SC（目标行 + 一条 helper，一次一系数） | Residual↔Bypass + overlay | 4A |
| 4A | RIR（正式状态维护） | Residual↔Bypass | 4B |
| 4B | RPR | Residual↔Bypass + overlay | 4C |
| 4C | RSR | Residual↔Bypass + overlay | 4D |
| 4D | RUP | Residual↔Bypass + overlay | 4E |
| 4E | Equality-specific（仅在证明有独立新增效果后） | Residual↔Bypass + equality-group overlay | 5-LP |
| 5-LP-1 | SB-LP，GLPK candidate + exact checker | residual 或 Bypass | 5-LP-2 |
| 5-LP-2 | SC-LP，全活动 helper 候选 + exact lower-bound checker | residual 或 Bypass | 5-LP-3 |
| 5-LP-3 | RIR-LP，GLPK candidate + exact checker | residual 或 Bypass | 6 |
| 6 | Adaptive K、预算消融 | residual | 最终集成 |

> residual cache 现在放在**所有系数/约束结构简化之前**。这样 CS、Multi-row SC 以及后续 RPR/RSR 都从第一天起在正确 identity 下测试；Bypass 始终保留为无缓存计数 oracle。

> 表中每个算法项都隐含 `X-P → 用户审核 → X-I`。`0E` 也先写 `pseudocode/00_equality_contract.md`。没有获批 P 文档，不得开始对应 I 关。

---

## 15. Codex 首次任务、单项任务与验收提示词（可直接复制）

### 15.1 Prompt 0：只审计并修订指南

> （已于 2026-11-24 执行并按用户决定补充）完整阅读第 0.1 节真实存在的资料和源码，只输出 `docs/inprocessing/00_code_audit.md` 与本 v1.1。核对调用点、cache、Reason 生命周期、学习约束可见性、普通 MC、回跳、frozen ComponentManager，以及 EDPLLSim CS/SB-LP/RIR-LP/GLPK 与 Pairwise Multi-row SC 指南；运行 Release、最小例与 sanitizer；如实记录 baseline FAIL。禁止修改根预处理和简化算法。完成后停下等待审核。

### 15.2 Prompt P：只写下一项伪代码

> 只执行**第 XX-P 关：ShortName Pseudocode**。完整核对真实源码，但只创建或修订 `docs/inprocessing/pseudocode/NN_short_name.md`，写清 `>=/<=/=`、数学证明、逐步伪代码、状态、Reason、BICP/临时假设恢复、EqualityGroup、overlay/cache/dependency、预算、反例和完整模型集合 oracle。禁止修改任何 C++。交付文档后停止，等待用户审核。

### 15.3 Prompt I：获批后只实现该项

> 对应的 `NN_short_name.md` 已由用户批准后，只执行**第 XX-I 关**。严格按获批伪代码在统一 `core/simplify.h/.cc` 和必要最小接入文件中实现，不创建规则专属源码，不顺带实现其他技术。先保存 oracle baseline，再做 `>=/<=/=`、回溯/回跳和合法 cache 模式差分。需要结构改写时，必须先通过 2D/2E 全部 cache 关并使用 residual/equality-group overlay，同时以 Bypass 作 oracle。产出 `acceptance/XX_ShortName.md`、diff 和真实测试命令，然后停止。

### 15.4 Prompt 2：验收失败后的修复

> 上一关未通过。仅修复验收报告中的失败项，保留相同随机 seed 和最小反例，不能改动其他规则/缓存算法/启发式掩盖问题；重新跑全部本关 MC oracle、模式拒绝、ASan/UBSan、回跳及 cache bypass，并更新 acceptance 报告。未 PASS 不进入下一关。

### 15.5 Prompt 3：缓存专项

> 现在开始**关卡 2E residual component cache**，对应 `pseudocode/03_component_cache.md` 已获批。先复验 2D Bypass；再按 2E1–2E5 完成 canonical `ReferenceKey` oracle、EDPLLSim 风格 `TemplateID + exact gap` 紧凑键、probe-only lookup、insert acquire、full compare、PBMC father/descendant ownership、模板 refcount、实例 clear 和 age+MB 淘汰。只维护普通 MC count payload。禁止用旧 ClID 代表新系数，禁止在 key normalization 时修改 attached solver 约束。全部验收后停止，等待用户批准进入 3A CS。

### 15.6 Prompt 4：最后才做 Adaptive K

> `pseudocode/16_adaptive_k.md` 获批，且所有独立规则、SB-LP/SC-LP/RIR-LP exact checker 和 residual cache 均通过验收后，才在 `simplify.h/.cc` 增加 `adaptiveK()`。保留变量→系数→约束和 CS→GCD→RoundingSat cardinality normalization；K 只选择已经验收的 BICP、Multi-row SC、SC-LP 等高成本增强路径，不关闭 UP/RV/Basic。与固定配置逐组消融，只报告普通 MC。

---

## 16. 独立来源与尚未确认之处

### 16.1 实际可用的依据

- `PBMC_CURRENT_ARCHITECTURE_CN.md`：当前入口、主循环、分解/cache 与接入风险；
- `PBMC_CODE.md`：项目结构、计数状态和扩展位置；
- `PBMC_SIMPLIFICATION.md`：简化与冲突学习的数学/工程背景；
- `PBMC_FILE_AND_DIRECTORY_GUIDE_CN.md`：构建、持久副本与目录职责；
- `AAAI_EDPLLSim/EDPLLSim_Cache_AdaptiveK_to_PBMC_Guide.md`：cache identity 和 Adaptive K 的迁移原则；
- `AAAI_EDPLLSim/src/cache.h/.cc`、`solver.cc` 与 `linear_constraint.cc::findCache/insertCache/initializeCacheForInstance`：component cache 的 canonical key、模板池、lookup/insert、生命周期和淘汰真实实现；
- `paper/PBMC_Pairwise_MultiRow_SC_Codex_Guide_v1.0_CN.md`：两行 Multi-row SC 定理、精确下界、一次一系数反例与测试来源；
- `docs/inprocessing/00_code_audit.md`：`819fd7e` 真实源码和运行证据。

v1.0 所列四份缺失文档不再作为依据。

### 16.2 本指南的限定与需要 Codex 实证的事项

1. 新接口、短方法名和命令行名称仍是后续实施建议；简化源码固定为 `core/simplify.h/.cc`，已确认 CMake 需要显式添加 `core/simplify.cc`。
2. 已取得并编译真实源码；Prompt 0 后的 0B/0E 已让普通 MC oracle、backjump 动态覆盖、三种 comparator、EqualityGroup、Release 与 ASan/UBSan 基线通过；PMC/WMC/WPMC 历史结果只保留在审计记录中，不属于实施门槛。
3. BICP、Multi-row SC、RUP、Generalized RSR、Equality-specific、SB-LP、SC-LP、RIR-LP 需配套精确 proof/checker 和同一当前保留公式前提；不能将临时 probe、演示、历史 proposal 或 GLPK `optimal` 当作完整的递归证明。
4. LP 技术的正确性证书与 attached Constr 的替代方案仍需逐关证明；residual cache 的接口与 EDPLLSim 迁移边界已审计，但其实现正确性必须在 2E1–2E5 逐项验收，不能由 Prompt 0 代替。
5. 0E 已建立 `=` 两方向的递归层 group identity 与精确 `<=` 前端语义；这只是所有规则的输入基础，不代表任何后续简化规则已经 Equality-aware 验收。
6. 本指南只保留已确定规则，不把未提供的研究候选清单写成实现承诺。

**最终验收标准**：每项技术先有独立且获批的伪代码文档；关卡 0B/0E 先让普通 MC oracle、三种 operator、EqualityGroup、模式拒绝与 sanitizer 基线通过；随后每条规则独立 on/off 的 `>=/<=/=` 模型集合与 oracle 一致；BICP 双向共享结果且完整恢复假设 trail/Reason；Multi-row SC/SC-LP 一次只提交一个系数且 helper 依赖有效；非根赋值有合法且生命周期安全的 Reason，EqualityGroup/Residual/ComponentManager/solver 在任意恢复路径一致；只有全局逻辑后果的 learned row 可保持 solver-only；Bypass 安全，residual cache 不 false hit；GLPK 只产生候选，最终变化通过 RoundingSat exact checker；RoundingSat cardinality conversion 只作为既有规范化能力接入；所有算法规则集中在 `simplify.h/.cc`、使用短方法名、沿用共有数据结构、只增加必要接口与统计；Adaptive K 最后加入且不覆盖规则本身。
