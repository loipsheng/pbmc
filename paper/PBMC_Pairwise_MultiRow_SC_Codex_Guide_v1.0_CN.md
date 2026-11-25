# PBMC：Pairwise Multi-row Coefficient Strengthening 实现与逐关验收指南

> **版本：v1.0｜用途：直接交给 Codex 阅读、审计、修订后逐步编码**  
> **范围：无权精确伪布尔模型计数（Exact PB Model Counting / #PB）**。不开发 WMC/WPMC；若原仓库存在 PMC、d-DNNF 等模式，本模块默认不开启，并避免破坏既有路径。  
> **在总实施计划中的位置**：`PBMC_Recursive_Simplification_Codex_Implementation_Guide_v1.0_CN.md` 的**关卡 3D**；数学背景参见 `PBMC_Final_Simplification_Framework_v3.0_CN.md`。  
> **输入表示约定**：PB 残余约束 `Σ a_i ℓ_i ≥ gap`，`a_i ∈ ℤ_{>0}`，`ℓ_i ∈ {x_i, ¬x_i}`；所有计算须保持**整数精确性**。  
> **注意**：本指南属于设计规范，不等于对用户当前仓库已完成源码验证。Codex 必须先审核真实源码接口，修订文件路径/类型与可行性，再开始编写。

---

## 0. 一句话目标、边界与核心原则

目标是：在普通 PB 系数饱和（CS）执行后，选取目标行 `C_i` 和另一条**有效辅助行** `C_h`，证明目标行其他文字的活动下界，从而进一步缩紧某个已有系数，**不改变当前部分赋值条件下原有布尔变量的满足赋值集合**。首版无条件下界仅缩小**正系数的数值**，通常不会直接改变一行的变量支持集；可能通过后续传播、发现冗余行等**间接**帮助分解。**把单个系数缩为 0 的情形属于后续条件式 SC，首版不实现。**

**首版实现只采用有预算的双行非 LP Multi-row SC：**

1. 不枚举任意多行组合，不调用 LP/MIP，不实现 §7.2.2 完整 Lifting，也**不新增原本不存在的变量项**。
2. 不删除原变量，不执行变量等价替换或一般消元；系数减为 0 后的自由变量交由现有 #PB 计数流程正确处理。
3. 不重写根预处理器，不重写 RoundingSat 的冲突学习；只接入 `count_main()` 中传播后、分解前的递归简化调度。
4. 每条规则独立文件：`PairwiseMultiRowSCRule.h` / `.cc`；可以复用已有公共 IR、活动界及 overlay 接口，严禁额外复制整套传播器。
5. 先 **view-only**（只计算候选和证明，不改求解状态），再 **committed rewrite**（可回溯地更新真实残余视图）。两关必须分开验收。
6. **从 committed rewrite 开始，旧 `VarIDs + ClIDs + Gaps` 缓存不得直接使用**：先通过安全 `CacheBypass`，待残余 Cache v2 独立验收后再开放。
7. 提交改写前必须保证辅助约束的作用域、证明有效性、RoundingSat/ComponentManager/overlay 的数学状态一致；不能用 `changed=true` 代替严格证明。
8. **不做 Adaptive K**。先把规则单独实现、测试和消融，最终框架稳定后再统一集成自适应选择。

**数学约束：** SC 的目标是**系统等价**，不一定是**目标行单独等价**。每次改写的数学保证是

$$
G\land C_i \quad\equiv\quad G\land C'_i,
$$

其中 `G` 是当前范围内**不包含目标行自身**的、仍然有效的辅助前提。绝不能删除辅助行后继续沿用未验证的缩紧结果。

---

## 1. 首先区分 Single-row SC、CS 和 Multi-row SC

- PB 的 **CS (Coefficient Saturation)**：仅使用目标行本身，`a'_j = min(a_j, gap)`；对应一般整数约束 Single-row SC 在正系数布尔文字下的特化。RoundingSat 已有 `saturate()`，应复用。
- **GCD**：各系数除以最大公约数，`gap` 按**向上取整**更新；不等于 Multi-row SC。
- **Cardinality Detection**：检查能否将 PB 行等价转成 `Σℓ_i ≥ q`；同样不等于 Multi-row SC。
- **Multi-row SC**：依赖其他**有效约束**产生比单行 `R ≥ 0` 更紧的下界 `R ≥ L_R`，继续缩紧已有系数。

整个系数简化阶段保持用户确定的 **版本 A**：

```text
CS → GCD → Cardinality Detection
        ↓
Budgeted Pairwise Multi-row SC
        ↓ 仅当确有行变化
NormalizeChangedRowsA(): CS → GCD → Cardinality Detection
        ↓
若产生新推导或冲突：回到 RoundingSat 正式传播/冲突处理
        ↓
约束简化（RIR → RPR → RSR → RUP），随后分解与缓存
```

`NormalizeChangedRowsA()` 为建议封装名，非已存在的 PBMC 函数。**不要为每个候选重新全量规范化所有行**。

---

## 2. 数学定义与证明：必须先搞清才允许编码

### 2.1 目标行、辅助行与作用域

在某个当前搜索节点，移除已赋值的文字贡献后，目标残余行：

$$
C_i:\quad a_j\ell_j + R_{ij}\ge k_i,\qquad
R_{ij}=\sum_{t\ne j}a_t\ell_t,\qquad k_i=\mathrm{gap}_i>0.
$$

辅助行：

$$
C_h:\quad H_h=\sum_t b_t\ell_t\ge k_h,
\quad h\ne i.
$$

此时所有未赋值文字都在 `{0,1}`，并已经合并同变量的重复项/互补项，系数是任意精度整数。只允许利用**当前作用域下有效、且不会在本次提交中被无依据移除**的约束。

> **重要：** 若目标行已经 `gap<=0` 则恒真；若 `U<gap` 则冲突。这些由 BasicCheck / 原生传播处理，不应在 Multi-row SC 中当作普通候选继续求界。

### 2.2 安全的无条件活动下界版本（第一版）

设 `G` 为保留的其他有效约束，并且已经严格证明

$$
G\models R_{ij}\ge L_{ij}, \qquad L_{ij}\in\mathbb Z_{\ge0}.
$$

若 `0≤L_{ij}<k_i`，定义

$$
\boxed{a'_j=\min\bigl(a_j,\; k_i-L_{ij}\bigr)}.
$$

若 `a'_j<a_j`，替换为

$$
C'_i:\quad a'_j\ell_j+R_{ij}\ge k_i,
$$

且其他保留约束**不变**。该替换保持整个系统的满足赋值集合：

- `ℓ_j=0`：新旧行完全相同。
- `ℓ_j=1`：若 `a'_j=a_j` 则不变；若缩紧成功，`a'_j=k_i-L_{ij}`，结合 `R_{ij}≥L_{ij}` 保证新行成立，反方向因 `a'_j≤a_j` 自动成立。

**如果无条件下界 `L_{ij}≥k_i`**，则 `G` 已经保证 `R_{ij}≥k_i`，目标整行在 `G` 下冗余。此时**不要在 SC 中自行无证明删行**，返回候选 `ROW_REDUNDANT`，交由后续冗余删除规则及其 overlay/undo 验收。

**如果 `L_{ij}=0`**，最多恢复到普通 CS 的 `min(a_j,k_i)`；已经饱和后通常不会有增益，直接跳过。

**首版能力边界：** 当 `0≤L_{ij}<k_i` 时必有 `k_i−L_{ij}≥1`，因此无条件 Multi-row SC **不能把某个正系数缩为 0**。`L_{ij}≥k_i` 的处理是将整条目标行转交冗余证明，而不是逐项删边。以后只有加入 §2.3 条件式 SC 等技术后，才会直接使已有项系数变为 0。

### 2.3 条件式 Multi-row SC（第二版可选，不进入首版主干）

若只能证明

$$
G\land(\ell_j=1)\models R_{ij}\ge L_{ij\mid\ell_j=1},
$$

在**证明范围正确**的前提下，仍可使用

$$
a'_j=\min(a_j,\max(0,k_i-L_{ij\mid\ell_j=1})).
$$

关键区别：**条件界 `L≥k_i` 只能推出系数可以缩至 0，不等于目标整行冗余**；因为当 `ℓ_j=0` 时原目标行依然约束 `R≥k_i`。条件界可通过辅助行在 `ℓ_j=1` 下求得，首版不实现、不开启。不要把条件证明误当无条件证明或用原目标行本身形成循环推理。

例：

$$
\begin{cases}
C_1:3x+y+z\ge2,\\
C_2:2\bar x+y+z\ge2.
\end{cases}
$$

在 `x=1` 时由 `C_2` 得 `y+z≥2`；因此在**保留 `C_2`** 的前提下，`C_1` 可替换为 `y+z≥2`。但不能由此删掉 `C_1`。这一扩展可提高能力，也会增加证明、作用域管理和代码复杂度，**必须放在首版之后的独立关卡**。

---

## 3. 普通约束不匹配也能求界：两种算法

### 3.1 首版算法 A：公共文字下界（低开销、易审查）

目标余项

$$R=\sum_{u\in S}a_u\ell_u,$$

辅助行

$$H=\sum_{u\in T}b_u\ell_u\ge k_h.$$

只考虑**文字相同（变量 ID 和正/负极性都相同）**的交集 `I=S∩T`，令辅助独有文字集合为 `T\I`。对辅助独有文字采用最大可能贡献：

$$
U_{\mathrm{private}}=\sum_{u\in T\setminus I}b_u.
$$

则

$$
\sum_{u\in I}b_u\ell_u
\ge
D=\max(0,k_h-U_{\mathrm{private}}).
$$

若 `I` 为空或 `D=0`，这一种算法直接返回下界 0。否则选择**精确有理数**

$$\lambda=\min_{u\in I}\frac{a_u}{b_u},$$

得到

$$
\boxed{L_R=\left\lceil\lambda D\right\rceil}.
$$

因为对公共文字有 `a_u ≥ λ b_u`，目标余项其余文字系数又非负，上界私有项被安全剔除，所以这是有效整数下界。该下界可以用于目标系数的缩紧。

**限制：** 对正/负极性不相同的文字，这一“同极性交集法”只是跳过，不是断言没有其他非 LP 推导；下一种算法能处理更一般的极性关系。

#### 例 A1：完全重合

$$
\begin{cases}
C_1:8x+3y+2z\ge5,\\
C_2:y+z\ge1.
\end{cases}
$$

取 `R=3y+2z`，`I={y,z}`，`D=1`，`λ=min(3,2)=2`，`L_R=2`，故 `a_x: 8 → 3`（普通 CS 只能 `8 → 5`）。

$$
(8x+3y+2z\ge5)\land(y+z\ge1)
\iff
(3x+3y+2z\ge5)\land(y+z\ge1).
$$

完整满足赋值共有 **4 个**，改写前后相同。**注意不可以丢掉 `C_2`。**

#### 例 A2：支持集与系数不完全对应

$$
\begin{cases}
C_1:8x+4y+3z+2w\ge6,\\
C_2:2y+2z+t\ge3.
\end{cases}
$$

对目标 `x` 有 `R=4y+3z+2w`，公共文字只有 `{y,z}`，辅助独有文字 `{t}`：

$$
U_{\mathrm{private}}=1,
\quad D=3-1=2,
\quad \lambda=\min(4/2,3/2)=3/2,
\quad L_R=\lceil 3\rceil=3.
$$

因此 `a_x: 8 → 3`，尽管 `w,t` 不匹配且两个共同变量的系数比不同。前后系统共有 **13 个**完整满足赋值。

#### 例 A3：同名变量有交集，但无法得到正下界

$$
\begin{cases}
C_1:8x+3y+2z\ge5,\\
C_2:y+t\ge1.
\end{cases}
$$

`I={y}`，`U_private=1`，`D=max(0,1-1)=0`，本方法**没有增量收益**。直接跳过，不允许凭“有交集”就任意缩紧。

### 3.2 首版算法 B：通用差值活动界（推荐与 A 共享底层数学）

令辅助行 `H≥k_h`，对任意非负有理数 `λ=p/q`（`p≥0`、`q≥1`、`gcd(p,q)=1`）：

$$
qR=pH+(qR-pH)\ge pk_h+(qR-pH).
$$

所以

$$
R\ge \left\lceil\frac{pk_h+\min_{\{0,1\}^n}(qR-pH)}q\right\rceil.
$$

定义

$$
\boxed{L_R=\max\left(0,\left\lceil\frac{pk_h+\min(qR-pH)}q\right\rceil\right)}.
$$

这里 `min` 对**全部未赋值布尔变量的盒域**求值，因盒域是其他约束可行集的超集，因此求得的是保守有效下界。

将所有文字 `¬x=1-x` 展开，将**同一个变量的正负出现合并**，得到

$$
qR-pH=c+\sum_v d_v x_v.
$$

则：

$$
\boxed{\min_{x\in\{0,1\}^n}(qR-pH)=c+\sum_v\min(0,d_v).}
$$

所有乘法与求和使用任意精度整数；`ceil_div` 使用数学意义的向上取整（负数也必须正确），**禁止直接使用 `double` 和 `EPS`**。比起直接存浮点 `λ`，代码中保存约分后的 `(p,q)`。

> **例：** 对 A2 取 `λ=2`，则 `R-2H= -z+2w-2t`，盒域最小值 `-3`，所以 `L_R=2×3−3=3`。这在变量不完全匹配时仍然有效。

#### 推荐比例候选（有预算、可开关）

- `λ=0`：得到单行基线 `L_R=0`。
- `λ=1`：最简单的差值组合。
- `λ=a_u/b_u`：对于共享变量在规范有符号表示下可能使某项抵消的正比例候选；首版只选少量候选，不穷举所有实数。
- 通过公共文字算法得到的 `λ=min(a_u/b_u)`；若系数比不同，仍可有效。

每对最多选择 `R_max` 个比例，优先固定、可复现的枚举顺序。若实现公共文字法和通用差值法，取二者安全下界的最大值即可；不要把多个独立下界直接相加。首版以 `λ=1` + 一个公共同极性最小比例为最小可用配置。

### 3.3 计算单个目标系数 vs 一对约束的所有目标系数

- 朴素单个系数：合并两条稀疏行并计算最小活动值，约 `O(l_i+l_h)` 次整数操作。
- 朴素为目标行每个系数重建 `R`：一对约束约 `O(l_i(l_i+l_h))`；同长度 `l` 时约 `O(l²)`。
- 固定 `(target,helper,λ)` 时，先构造 `qC_i-pC_h` 的有符号仿射差值、计算全局盒域最小值，再针对每个排除的目标文字做**常数时间差分更新**；从而一对约束所有目标系数的求界开销可接近 `O(l_i+l_h)`，不必每个系数重新扫描整行。**实现该优化前必须有与朴素重算一致的随机测试。**
- 若对一对约束枚举 `r` 个比例，该对约束工作量可设计为 `O(r(l_i+l_h))`（不包括候选生成、哈希和任意精度整数位复杂度）。

**共享计算注意：** `R` 删除的目标项可能含 `¬x`，会同时改变仿射表达式的常数项和 `x` 系数；不能只把 `x` 系数减去 `a_j`。对每个排除项的修正必须按 `ℓ=0/1` 的定义精确验证。

---

## 4. 候选筛选：不要对所有约束进行 m² 扫描

### 4.1 复杂度基线

设 `m` 为活跃行数、`l` 为平均/上界行长、`P` 为实际候选**有向**约束对数量。目标 `C_i←C_h` 与 `C_h←C_i` 不相同。

| 策略 | 单轮粗粒度算术操作复杂度（不含索引预建与大整数位成本） |
|---|---|
| 普通 CS 全行 | `O(ml)` |
| 朴素全量双行 + 每个系数重算 | `O(m²l²)` |
| 全量双行 + 固定比例共享计算 | `O(r m²l)` |
| 每目标行最多 Q 条候选 + 朴素每系数 | `O(m Q l²)` |
| **每目标行最多 Q 条 + 共享计算** | **`O(r m Q l)`** |

复杂度的乘数不仅来自求界：高频变量可能使 occurrence 候选生成本身接近 `O(m²)`；需先对候选去重并限制扫描量，不能只对最终 accepted candidates 计预算。

### 4.2 Occurrence-based Candidate Generation

对待缩紧目标系数 `a_j ℓ_j`，令目标余项的**变量支持集** `S_R = supp(R_ij)`。从

```text
candidates = union(occ[v] for v in S_R)
```

收集活跃辅助行（`helper != target`）。依赖的是当前 residual overlay 下的有效 occurrence，不可把已失活的辅助行作为继续推导的前提，也不能只扫描原始冻结 occurrence 而漏掉派生行。

先用变量 ID（不区分符号）找候选，但在公共文字法中必须再检查**极性相同**；通用差值法可以处理相反极性。若 `supp(R)` 与 `supp(helper)` 的变量交集为空，普通单条辅助约束不提供正的 `R` 下界，保守跳过。

### 4.3 Cheap Filtering：可以丢失机会，不能产生伪证明

对每个候选先做廉价判断：

1. 两行均为当前组件的有效、未失活约束，且 `helper != target`。
2. 目标行满足 `gap>0` 且尚有可以缩紧的系数；已经通过 CS/GCD 处理到单行稳定。
3. `overlap>0`；可用 `Jaccard` 作为候选排序而非正确性依据。
4. 公共文字法的 `D=max(0,gap_h-U_private)>0` 时，优先尝试；若 `D=0`，仅跳过**该方法**，不能断言通用差值法绝无收益。
5. 根据估计的潜在缩紧量排序，优先考虑缩紧幅度、可能触发的传播及整行冗余；**首版无条件系数缩紧本身不会直接删边**。
6. 仅选择 Top-Q（推荐初始实验 Q∈{2,4,8}），记录被预算和 Q 限制跳过的数量。

`Jaccard = |S_R∩S_h| / |S_R∪S_h|` 是结构筛选指标，不是语义等价条件。**最终修改必须由活动界严格证明，不允许只凭 Jaccard 或 overlap 改系数。**

### 4.4 预算建议（仅实验初值，不是性能结论）

- `Q`：每个目标行最多 2/4/8 条辅助行。
- `R_max`：每对约束最多尝试 1/2/4 个非负有理比例。
- `B_nnz`：每次简化调用允许扫描的非零项次数上限，**包含候选生成开销**。
- `B_pairs`：全节点最多检查的有向目标—辅助对数。
- 可选 `B_bigint`：大整数乘加/比较数统计，用于观察超大系数开销。
- `B` 在一次节点简化调用中**不重置**；高级预算耗尽后继续原生 UP/必需同步并允许进入分解。不得假称已达到所有高级规则的数学不动点。

首版不要将 K 自适应与工作量预算混在一起。预算仅限制可选 SC 的性能成本，不影响计数的精确性。

---

## 5. Dirty-row、Revision、重复检测与终止性

### 5.1 为什么一条变化会唤醒其他行？

若 `C_i` 的已有系数缩紧，`C_i` 作为目标行自身需要重新规范化；它作为其他目标行的**辅助行**时，还可能改变可证明的活动界。因此即使 `C_j` 没有变化，也可能需要再次检查 `C_j←C_i`。这就是“helper 变了，target 也要重新评估”的原因。

### 5.2 每条约束状态

建议逻辑字段：

```cpp
struct ResidualRowState {
    RowID id;
    uint64_t revision;
    bool active;
    // 具体结构由审计后的 IR/overlay 决定
};

struct PairProbeKey {
    RowID target_id;
    uint64_t target_revision;
    RowID helper_id;
    uint64_t helper_revision;
    uint64_t scope_generation; // 若有其他行/赋值上下文依赖，则必须进入身份
};
```

**不要将旧 `revision` 与另一分支恢复后同数字的新内容混为一谈。** 可以将 `revision` 设为单调递增的全局代次，或使用 `(row identity, current content signature, context generation)`；实际版本号更新与 undo 策略先由 Codex 审计确认。

### 5.3 增量调度

```text
已赋值 / 约束改写 / 辅助行发生变化
      ↓
标记目标与相关候选 dirty
      ↓
通过 occurrence / reverse-dependency 产生或恢复候选
      ↓
检查 PairProbeKey 是否已经检验过
      ↓
预算允许则计算活动下界
      ├─ 无增益：记录 tested，继续下一候选
      └─ 有增益：生成 proposal，独立验证后 commit
                    ↓
             目标 revision++，唤醒相关行
                    ↓
             CS → GCD → Cardinality（只对变化行）
                    ↓
             若产生新蕴含，返回正式传播
```

**不要只将两条都 dirty 的 pair 入队**；只要其中一条变更，另一条虽未变也可能获得新机会。对于共享变量或条件上下文的边界事件，采取保守重入队/升 scope epoch 的方法，不确定时宁可多算。

### 5.4 终止性与预算

在首版规则中，已有正整数系数只允许**严格减少**，禁止增加任何系数/新增非零项。对某个固定公式状态定义势函数

$$W=\sum_{i}\sum_{j}a_{ij}.$$

每次真正提交的 SC 缩紧至少使 `W` 降低 1；所以**有效系数缩紧**不会无限发生。GCD、后续条件式 SC 的零系数删除也不会增加势函数；首版无条件 SC 不产生零系数。实际在搜索层还会有赋值、回溯等事件，应以每节点独立预算保证成本有限。

**终止 ≠ 高效**：大整数系数使理论上的有效变更次数可能很大；大量无效 pair 也会耗时。必须实现 `Top-Q + PairRevision + B_pairs/B_nnz`，不能执行无预算的 `repeat(AllPairs)`。

---

## 6. 独立文件组织与接口建议

真实路径先按仓库组织审计，首选如下：

```text
core/simplify/
  PairwiseMultiRowSCRule.h
  PairwiseMultiRowSCRule.cc
  # 公共接口已存在就直接复用：
  Rule.h
  SimplifyContext.h
  ResidualFormulaView.h
  SimplificationUndo.h
  # 若确有通用价值且上一关已验收：
  AffineActivityBound.h/.cc    (可选；不强制首版新建)
```

**严格职责**：

- `PairwiseMultiRowSCRule`：选择候选、计算可证明下界、生成 SC Proposal 和统计；不直接调用 `decide()`、不直接修改 RoundingSat watched `Constr`，不接管 Cache 对象。
- `SimplifyScheduler`：统一顺序、工作预算、传播—简化循环、触发后续 `NormalizeChangedRowsA()`。
- `ResidualFormulaView`：从 frozen rows + 当前 `trail` + overlay 读到**准确的有效残余表达式**，明确文字极性、编号、作用域。
- `CommitAdapter`：证明和作用域检查、实际结构 commit、撤销日志、Reason/传播接口与 ComponentManager 对齐。
- `CacheBypassAdapter`：若发生 committed residual rewrite，真正禁止旧键 lookup/insert/store，但不破坏 Decision/Component 栈的计数组合。

### 6.1 数据契约示意（供 Codex 按实际代码修订）

```cpp
struct ExactRational {
    BigInt numerator;   // >= 0
    BigInt denominator; // > 0
};

struct CoefficientRewriteProposal {
    RowID target;
    RowID helper;
    uint64_t target_revision;
    uint64_t helper_revision;
    Literal target_literal;
    BigInt old_coefficient;
    BigInt new_coefficient;
    BigInt target_gap;           // V1 通常不变
    BigInt certified_lower_bound;
    ExactRational lambda;
    ProofWitness witness;       // 保存前提/差值/有效性证明
    ScopeToken scope;
};

struct PairwiseSCStats {
    uint64_t candidate_rows = 0;
    uint64_t candidate_pairs = 0;
    uint64_t pairs_filtered = 0;
    uint64_t pairs_evaluated = 0;
    uint64_t lambda_evaluations = 0;
    uint64_t proposals = 0;
    uint64_t rewrites_committed = 0;
    uint64_t coefficients_reduced = 0;
    uint64_t zero_coefficients = 0;
    uint64_t total_nnz_scanned = 0;
    uint64_t exact_bound_ops = 0;
    uint64_t budget_exhausted = 0;
    // duration、max bits 等采用项目现有统计风格
};
```

`ProofWitness` 在第一版可以先保存可独立重算验证的：目标/辅助规范残余行、`p/q`、仿射差值、最小值、计算的 `L_R`、scope 和前提版本。是否需要生成可供 RoundingSat 消解的真实 PB `Reason`，必须由实际求解器证明系统审计决定；**绝不能伪造 `Reason` 指针**。

### 6.2 单对约束的算法（概念伪代码）

```cpp
vector<RewriteProposal> tryPairwiseSC(TargetRow target,
                                      HelperRow helper,
                                      NodeContext ctx,
                                      Budget& budget) {
    if (target.id == helper.id || !helper.active || !target.active)
        return {};
    if (!inCurrentScope(target, helper))
        return {};

    vector<RewriteProposal> result;
    auto ratios = chooseExactNonnegativeRatios(target, helper, budget);

    // 先统一构造有符号仿射差值，处理 x 与 ~x 的常数项。
    for (const auto& ratio : ratios) {
        if (!budget.consumeEstimatedWork(target, helper))
            break;
        auto shared = precomputeAffineDifference(target, helper, ratio);

        for (const auto& [lit, coeff] : target.terms) {
            if (coeff <= 0 || !budget.consumePerTerm())
                break;

            auto bound = lowerBoundExcludingLiteral(shared, lit, coeff);
            // 必须包含 G 作为前提，且当前 V1 是无条件下界
            if (!exactlyValid(bound))
                continue;

            if (bound >= target.gap) {
                result.push_back(redundancyCandidateWithProof(...));
                continue;
            }
            BigInt newCoeff = min(coeff, target.gap - bound);
            if (newCoeff < coeff)
                result.push_back(rewriteWithProof(...));
        }
    }
    return deduplicateAndKeepStrongestConsistentProposals(result);
}
```

**非常重要的提交策略：** 在固定快照下为某条行的多个系数分别计算 `a'_j`，不代表把所有 `a'_j` **同时写入**就仍然等价！每个证明使用的 `R_{ij}` 包含其他系数的**旧值**；多个系数同时减少可能共同损失满足赋值。最安全首版做法：

**必须放进单测的反例**：

$$
C_i:5x+5y+5z\ge6,\qquad C_h:x+y+z\ge2.
$$

对 `x`、`y`、`z` 的**每一个单独系数**，在其他两个系数仍为 5 时，辅助行都保证“另外两个变量至少一个为真”，所以分别可以安全缩为 `1`。但如果一次性把三个系数全改成 1，就变为 `x+y+z≥6`，产生 **0** 个模型；原系统的完整模型数为 **4**。这不是单项数学规则错，而是**批量应用独立证明时前提改变**。



```text
每次只选一个最有价值的 proposal
→ 基于当前最新 target/helper 重新验证
→ 提交该一个系数的缩紧
→ target revision++，其余 proposal 作废
→ 重新规范化、重新生成后续候选
```

或者使用具有整体等价证书的批量改写（后期），**首版绝对不要把独立求得的系数缩减直接批量应用。** 这点优先级高于共享计算优化。

---

## 7. 何时返回 Reason、何时不返回冲突

Multi-row SC 是**等价系数改写规则**，而不是 FLP/RUP 的临时冲突处理器。V1 应主要返回：

- `UNCHANGED`：无可提交缩紧。
- `ROW_CHANGED`：完整证明、可回溯地更新当前 residual row；可能需要 `NormalizeChangedRowsA()`。
- `ROW_REDUNDANT_CANDIDATE`：其他保留前提已证明目标行冗余，交给专门删行模块决定是否接收。
- `INVALID_PREMISE` / `BUDGET_EXHAUSTED`：跳过，不影响 exact count。

**冲突判定**仍优先交给 `BasicCheck`/原生 UP：如果 `gap>U` 则当前真实分支无解。数学优化产生的局部/临时不可行、未完成候选或预算耗尽，不能直接返回当前搜索分支 0。若派生改写使其它技术发现真实冲突，必须由合法的 PB Reason/冲突对象进入现有选择性学习，或者有严格无解证明时安全顺序回溯。**不能把“有证明一个行改写”等同于“已经有可供 RoundingSat 消解的 Reason”。**

只有 #PB 普通精确计数：不可满足的**当前分支**贡献 0，另一个分支仍需计算；自由变量按原有计数逻辑贡献因子 2（不重复乘）。不为本模块新增任何权重处理。

---

## 8. Cache / ComponentManager / RoundingSat：真实改写的红线

### 8.1 为什么旧缓存键不一定仍安全

PBMC 旧组件键通常依赖：

```text
[未赋值 VarIDs]
+ [活跃原始 ClIDs]
+ [各约束当前 gaps]
```

前提是 `ClID` 能唯一指代固定系数/文字结构。若动态 Multi-row SC 实改系数，同一原始 ClID + 相同 gap 可能对应不同剩余约束，会导致潜在 false hit；这与 EDPLLSim 在 SC 后根据真实当前行内容生成模板身份的原则相同。

### 8.2 可以先关闭缓存，但必须是真 Bypass

**允许以下开发顺序**：

1. view-only：不改求解状态，旧 cache 可继续用于 baseline，但不能声称已取得搜索性能收益。
2. committed rewrite：先完成 `CacheBypass`，全面禁止不安全的旧 hash-key lookup、insert/store；保留组件栈、回溯及分支计数所需的非哈希结构。
3. 完成 Pairwise SC、RPR/RSR/RUP 等并分别验收；之后单独设计 residual-aware Cache v2。
4. Cache v2：使用 `VarIDs + 实际 canonical residual rows` 或安全的 `immutable derived-template ID + canonical gaps` 表示精确身份；原始和派生行的身份都来自当前内容，哈希后**完整键精确比较**；不要为一次 cache miss 无条件创建永久模板。

禁止“仅关闭 cache 就直接在已 attach 的 RoundingSat `Constr` 上原地改系数”；关闭缓存**不能修复**传播 watched data、Reason、ComponentManager 冻结副本及 backjump 的一致性问题。

### 8.3 必须落实的状态一致性

- `RoundingSat`：原约束、watch list、传播/冲突 reason 不得被破坏。若对当前行真正替换，需要设计经审计的重新 attach/作用域策略；第一版使用 immutable frozen base + reversible residual overlay 是较保守做法。
- `ComponentManager`：真实分解使用有效 residual rows，不可仍按冻结 `pbca_` 中已被消除的连接边决定 component；需要 overlay 或受控 rebuild。
- `Constraint ID`：原始 ID 表示 frozen row，派生内容使用新的结构身份/版本；不得用旧 ID 伪装新系数。
- `Undo`：第一分支到第二分支、正常回溯、非顺序回跳、子求解临时状态都必须恢复所有局部改写。
- `Proof dependence`：保留辅助行的有效性，或在任何失活/删行前重新证明目标重写与当前保留系统仍等价；禁止凭历史证明孤立使用派生行。
- `Counting`：`sum(branches)` 与 `product(independent components)` 正确；首版无条件 SC 不会直接使某个正系数归零；未来条件式 SC/其他结构改写确实可能释放自由变量，届时计数因子 2 必须**且仅一次**。

如尚不能证明 `ComponentManager` 和求解器状态一致，必须停留在 view-only 模式，不准进入 committed rewrite。

---

## 9. 建议开发关卡：Codex 每次只做一关，通过后停止

### Gate 0：只审计并修订本文（禁止编码）

审查实际仓库、commit、现有简化调度、RoundingSat 的 `Constr/ConstrExp/CRef` 与 allocator、`postProcess()`、watch/reason、`ComponentManager::init/searchComponent/splitComponent()`、`PackedComponent`、旧 cache 关闭方式、普通和非顺序回跳路径及现有测试。

产出：`docs/PairwiseMultiRowSC_Code_Audit.md` 和本文件的**仓库适配修订版**；列出准确文件/函数定位、计划新文件、数据契约与待解决风险。**这一步只审计、不改求解算法。**

### Gate 1：数学内核（view-only，独立 `.h/.cc`）

实现 `PairwiseMultiRowSCRule.h/.cc`，只接受纯规范 PB 行/目标系数/辅助行，输出建议系数与数学证明。不依赖真实 RoundingSat 可变状态。

先实现公共文字法，再实现通用差值法（或按审计选择一个最可靠的优先），全部整数精确；支持正/负文字、重复及互补文字合并、两个不相同的支持集、空行、已满足行、极大整数。

验收：小公式真值集合枚举，逐候选验证严格下界，随机测试差值下界 `L_R ≤ min_{C_h feasible}R`，无假阳性。每种方法独立开关、日志可复现。

### Gate 2：候选管理 + 预算（仍然 view-only）

通过 occurrence 倒排表产生候选，支持 `Top-Q`、比例上限、pair/nnz 工作预算、revision 去重和 dirty-row。验证辅助行变更会重新唤醒未改变的目标行；预算耗尽无假变更，不出现无限重试；统计结果稳定。

验收：比较全量候选和受限候选，在受限模式中允许错过可选缩紧，但不允许出现错误缩紧。ASan/UBSan、边界数据集通过。

### Gate 3：CacheBypass + Residual Overlay 前置关卡

**此关先独立实现基础设施，不开启系数实改。** 建立安全 cache bypass，保证不访问旧 hash cache 但仍正确完成组件计算、加法/乘法及回跳。建立 overlay/undo marker/作用域与状态校验接口，空 overlay 不改变行为。

验收：cache off baseline 与既有 exact MC 小实例全部一致；强制小内存/多层回跳测试；空 overlay 下分解拓扑与原版一致。

### Gate 4：一次只提交一个系数（committed rewrite）

由 Gate 1/2 产生已验证 proposal，重新基于最新目标/辅助行计算完整 proof，再通过 Gate 3 事务性 commit。保证 `RoundingSat`、overlay、`ComponentManager` 一致。首次只针对一个系数改写，提交后撤销旧 proposals、更新 revision、重新 Normalize A，必要时再次传播。

验收：对每个可达小状态比较完整模型集合（考虑当前 trail）、cache bypass exact count；测试兄弟分支和深层 backjump；未成功提交时所有状态保持原样。

### Gate 5：增量优化与收益分析

只在 Gate 4 全部通过后引入固定比例共享计算、reverse dependency、更多候选比例、score 排序、可选单行再入队。添加耗时与 nnz 访问量统计，对照朴素方法，确保共享优化不改变任何数学输出。

验收：同 seed 下 view-only/proof 输出与未优化版本一致；耗时、decisions、split、模型数、RSS、预算占比可对比。未提升性能时必须可以关掉该优化。

### Gate 6（未来，暂不实施）：条件式、三行以上、LP、Cache v2、Adaptive K

这些属于独立功能，**不得混入 Gate 1–5**。条件式可能缩为零但需要更复杂的条件证明；三行及以上有指数候选风险；LP 必须有可验证精确下界；Cache v2 由总文档统一推进；Adaptive K 在所有规则冻结后再做。

### 每关验收报告固定格式

```text
Gate:
Git commit / diff summary:
Changed files:
Mathematical rule or invariant:
Command lines actually executed:
Small exhaustive oracle: PASS/FAIL
Random differential: PASS/FAIL
Negative literals / big integers / empty rows: PASS/FAIL
Undo / sibling branch / backjump: PASS/FAIL or N/A
RoundingSat / ComponentManager consistency: PASS/FAIL or N/A
Cache mode, lookups/hits/inserts: PASS/FAIL or N/A
Performance stats / budget usage:
Known limitations / minimal failing input:
Result: PASS / FAIL / BLOCKED
```

**Gate 未 PASS，不进入下一个 Gate**。用户要求“先让 Codex 审计本文、结合源码修订最终指南”，Gate 0 是强制前置环节。

---

## 10. 必须覆盖的测试实例与期望结果

下表为无权 exact MC。记 `#F` 为**同时满足所有列出的约束**的完整变量赋值个数，改写前后不允许改变原变量的投影/模型集合。

| ID | 原约束系统 | 正确改写/检查 | #F |
|---|---|---|---:|
| T1 | `8x+3y+2z>=5; y+z>=1` | C1 的 x 系数 `8→3`，C2 保留 | **4** |
| T2 | `8x+4y+3z+2w>=6; 2y+2z+t>=3` | C1 的 x 系数 `8→3`，支持集不一致 | **13** |
| T3 | `8x+3y+2z>=5; y+t>=1` | 公共文字法无法提供正下界；跳过该方法 | 用 oracle 求 |
| T4 | `8x+y+z+w>=3; y+z>=1; y+w>=1; z+w>=1` | 三条辅助行可以证明 x 系数 `8→1`，**不是首版单条辅助行必然得到** | **5** |
| T5 | `3x+y+z>=2; 2~x+y+z>=2` | 条件式可使 C1 的 x 系数 `3→0`，**Gate 6 才做** | **2** |
| T6 | `5x+3y+2z>=5; 3~x+3y+2z>=5` | 条件式可使 C1 的 x 系数 `5→0`，**Gate 6 才做** | **2** |
| T7 | `10x+3y+2z+4w>=7; y+z+w>=2` | 余项下界 `5`，x 系数 `10→2`，辅助行保留 | **6** |
| T8 | `8x+3y+2z>=5` 单条行 | 不得无辅助证明就执行 `8→3` | 用 oracle 求 |

额外必须有：

1. **辅助行删除反例**：T1 在替换后如果丢掉 `y+z>=1`，会引入额外满足赋值；必须能检测/阻止此类失效。
2. **相反极性**：`x` 与 `~x` 同时出现在不同行，验证仿射常数处理与 `λ=p/q` 精确计算。
3. **多个缩紧不能盲目批量**：使用 §6.2 的 `5x+5y+5z>=6; x+y+z>=2`：每个系数单独 `5→1` 等价，但三个同时改成 1 会令模型数从 4 变 0；必须拒绝没有整体证书的批量提交。
4. **回溯**：子分支实际缩紧，返回兄弟分支后不得保留上一分支的系数或 proof scope。
5. **强回跳**：跳过多层 Decision，所有 overlay、candidate revisions 和依赖记录必须恢复/失效。
6. **分解**：首版无条件 SC 不直接把单项系数缩为 0，通常**不改变支持图**；必须通过传播固定变量、后续证明整行冗余等间接效果才能新增 split。条件式 SC（后续关卡）系数减到 0 后才可能直接删边；分解收益必须实测，不得凭系数下降宣称增加 split。
7. **哈希冲突**：未来 Cache v2 强制所有键相同 hash，必须完整键判等，不得 false hit。
8. **极大整数**：例如系数 `> 2^53`、`> 2^63`；任何一步不得先经过 `double/long` 溢出或错误舍入。
9. **等式输入**：明确按照真实 parser 展开为两条 `>=` 行后分别做证明，不能对未拆开的 `=` 直接执行普通 CS。
10. **无公共变量、辅助仅靠私有文字满足、当前 gap 变化、辅助行作用域失效、预算耗尽**。

### 10.1 独立穷举 oracle 参考

```python
from itertools import product

def assert_equivalent(vars_, old_constraints, new_constraints):
    # constraints: list[callable(dict[str,int]) -> bool]
    old_models, new_models = [], []
    for bits in product((0, 1), repeat=len(vars_)):
        env = dict(zip(vars_, bits))
        if all(c(env) for c in old_constraints):
            old_models.append(bits)
        if all(c(env) for c in new_constraints):
            new_models.append(bits)
    assert old_models == new_models, (old_models, new_models)
    return len(old_models)

# T1（完整系统等价，而非单行等价）
vars_ = ('x', 'y', 'z')
helper = lambda v: v['y'] + v['z'] >= 1
old = lambda v: 8*v['x'] + 3*v['y'] + 2*v['z'] >= 5
new = lambda v: 3*v['x'] + 3*v['y'] + 2*v['z'] >= 5
assert assert_equivalent(vars_, [old, helper], [new, helper]) == 4
```

每次自动生成随机小 OPB（建议 `2≤n≤12`，枚举所有 `2^n` 个完整布尔赋值），对**每一个真提交的缩紧操作**比对当前上下文 `F_before`、`F_after` 的完整模型集合，不能只比较 `SAT` 是否一致。对 root-only 情况可遍历原变量全集；对当前节点先固定 trail，再枚举未赋值变量，完整比较局部可达集合。

**模型数一致只是一道必要测试，不是足够证明**：不同模型集合可能恰好拥有相同模型数，所以必须对小实例对比逐个赋值真值。

---

## 11. 统计与消融：怎样证明这项技术值得保留

至少输出以下指标：

```text
multi_sc.nodes_visited
multi_sc.target_rows
multi_sc.candidate_pairs_generated
multi_sc.pairs_filtered_no_overlap
multi_sc.pairs_filtered_private_covers_rhs
multi_sc.pairs_attempted
multi_sc.lambda_candidates
multi_sc.exact_bound_calls
multi_sc.bound_positive
multi_sc.proposals
multi_sc.committed_rewrites
multi_sc.coefficients_reduced
multi_sc.total_coefficient_delta_bigint
multi_sc.zeroed_terms
multi_sc.support_edges_removed
multi_sc.proof_validation_failures
multi_sc.revision_rechecks
multi_sc.nzu_scanned
multi_sc.budget_exhausted
multi_sc.time_candidate_gen
multi_sc.time_bound
multi_sc.time_proof
multi_sc.time_commit
multi_sc.time_total
```

建议主实验对照组：

| 组别 | 开关 | 要回答的问题 |
|---|---|---|
| Baseline | PBMC 原始版本，未启用新递归 SC | 基础成本与正确性 |
| A | 只启用已有 CS→GCD→Cardinality | 单行简化本身收益 |
| B | A + Pairwise SC 公共文字法 | 低成本基础双行收益 |
| C | B + 通用差值界 | 更一般候选是否提高成功率 |
| D | C + Top-Q/Revision/共享计算 | 增量调度是否降低运行开销 |
| E | D + 传播/后续冗余检测及 overlay 分解 | 无条件 SC 是否**间接**增加分解与减少决策 |

最终报告除 `wall time`、求解数外，还要比较 `decisions`、`conflicts`、`components`、`splits`、`cache bypass` 状态、`nnz`、峰值 RSS；**只有证明数学缩紧，不等于证明搜索性能改善**。特别关注系数改写可能增加证明和 cache-key 维护开销；没有收益时必须能完全关闭该规则。

---

## 12. 交给 Codex 的首轮指令（可以直接复制）

> 请阅读《PBMC：Pairwise Multi-row Coefficient Strengthening 实现与逐关验收指南 v1.0》全文，并对照《PBMC 递归简化模块 Codex 分阶段实施指南 v1.0》、PBMC 源码结构分析指南，以及仓库真实代码。**本轮只做 Gate 0：源码审计与修订指南，不允许编写求解算法。** 重点确认：`count_main()` 中传播与分解的真实位置；`ConstrExp::postProcess()` 和系数存储；ComponentManager 的冻结约束、occurrence 与 gap；合法 Reason/冲突接口；已 attach 约束能否安全替换；cache bypass 是否真正生效；每一种 undo/backjump 的调用；原本 `VarIDs+ClIDs+Gaps` 键何时失效。请产出准确到函数级别的源码映射、可复用数据结构表、必要新增 `.h/.cc` 文件、测试计划和 Gate 0 审计报告；如与文档有冲突，以源码事实和精确计数不变量为准，将指南修订为仓库适配版。**报告 Gate 0 完成后停止，不自动进入 Gate 1。**

### 后续逐关通用指令

> 本轮只执行《Pairwise Multi-row SC 指南》的 Gate N；先列数学等价证明、复用接口与失败用例，再修改唯一目标模块及必要的最小接口。每关先建立测试，完成后运行构建、穷举模型集合、随机差分、回溯/缓存相关测试，输出 `acceptance/MultiRowSC_GateN.md`（含真实命令与结果），`PASS` 后停止等待下一次指令。不得顺手开发 Adaptive K、SC-LP、一般 Lifting、根预处理器替换、其他未验收的简化规则或新的 CDCL 系统。

---

## 13. 特别需要 Codex 自查的 12 个常见错误

1. 以目标行 `C_i` 自己证明 `R_ij≥L`，循环论证。
2. 数学上只在 `C_h` 成立时等价，却在删掉 `C_h` 后仍把改写视作单行等价。
3. 使用**条件界**证明却直接删除目标整行。
4. 使用 `double` 求 `λ`、下界或 `ceil`，大整数时误删模型。
5. 将不同变量的 `x` 与 `¬x` 当独立 Boolean literal 求最小活动值。
6. 同一行多个 `a'_j` 分别安全却**未经重验同时提交**。
7. 把 `target/helper` 两个 revision 相同的历史结果跨回溯复用为当前证明。
8. 只重新处理 dirty target，忽略 helper 变化造成的逆向依赖。
9. 候选无效、预算耗尽、探测 UNKNOWN 时错误返回 `CONFLICT`。
10. 只改 `ComponentManager` 或只改 RoundingSat，分解/传播使用了两套不一致公式。
11. 先关掉缓存就以为旧 `ClID` 对系数改写仍然安全，或 `CacheBypass` 只禁 lookup 却不禁 insert。
12. 将首版无条件 SC 错误宣传为直接产生零系数/新增分解；或在未来条件式 SC 系数减到 0 后遗失自由变量计数因子 `2`、重复乘 `2`、回溯后残留错误的残余结构。

---

## 14. 与既有研究文档的关系及最终结论

- 论文背景：Achterberg 等 *Presolve Reductions in Mixed Integer Programming* §3.3 的单行系数强化、§5.4 的多行 bound / coefficient strengthening；本指南的公共文字/差值盒域界、Top-Q、Revision 和共享计算是**面向当前 PBMC 的设计方案**，不应写成原论文已提出的同名算法。
- PBMC 现有核心：RoundingSat 传播/冲突学习 + ComponentManager 显式递归、原始组件缓存；以提供的源码指南为起点，实际路径需 Codex 现场核实。
- EDPLLSim 可迁移原则：**缓存身份由当前真实行内容决定，哈希后完整键精确比较，系数变化后更新结构身份**；不直接复制其 `<=` 行 floor、模板 ID 格式或参数常数。
- 所有新规则在当前阶段**仅服务普通无权精确 MC**；模型结构改写导致的计数变化必须由穷举验证和正式证明排除。

**最终推荐路线：`Gate 0 审计 → Gate 1 数学只读实现 → Gate 2 候选/预算 → Gate 3 CacheBypass+Overlay → Gate 4 单系数提交/回溯 → Gate 5 共享计算与性能优化`。**

只有在已验证正确、可回溯、可证明、分解图同步且缓存安全之后，Multi-row SC 才可以默认在递归搜索中启用。其余更强的条件式、三行、LP、Lifting、Adaptive K 均留到独立后续关卡。
