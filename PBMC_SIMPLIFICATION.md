# PBMC 无权精确模型计数简化技术与冲突学习指南

> 适用代码基线：当前仓库提交 `08eb389`。
>
> 配套总览：[PBMC 代码框架与四个改进模块开发指南](PBMC_CODE.md)。总览负责解释整个求解器；本文专门用于评审、调整和实现外部论文或 GPT 整理出的 PB 简化技术。
>
> 行号只用于第一次定位。代码变化后应优先按类名、函数名和调用关系搜索。

> 当前研究范围限定为 `-mode=0`：统计全部原始布尔变量上的精确模型数。本文后续的数学结论、接口设计和测试标准都以这一模式为准。

本文的目标不是给出一个“简化技术名称列表”，而是回答每项技术落到 PBMC 时必须解决的工程问题：

1. 它保持的是完整模型集合、精确模型数，还是仅 SAT/UNSAT？
2. 它是否支持负文字、负系数和等式？
3. 应在根预处理还是递归节点内执行？
4. 它产生的是等价改写、蕴含赋值、学习约束、冲突，还是计数因子？
5. 推导文字怎样进入 RoundingSat 的 `trail`、`Reason` 和冲突分析？
6. 改写怎样影响 ComponentManager 的分解图、缓存键和计数因子？
7. 回溯和非顺序回跳时需要撤销什么？
8. 用什么最小实例和暴力枚举证明实现没有改变精确计数？

后续把外部 GPT 整理出的技术交给另一个 GPT 时，应同时提供本文，并要求它逐项填写第 15 节的“技术迁移评审模板”。没有完成该模板的技术，不应直接改进 PBMC 源码。

阅读路线：

- 第一次建立语义基础：第 0～5 节；
- 判断某项技术能否用于无权精确计数：第 1、6、12、15 节；
- 准备接入递归简化：第 8～11 节；
- 学习 PB 冲突学习：第 9 节；
- 把材料交给 GPT 评审：直接给出全文，并指定填写第 15 节；
- 准备实现和验收：第 16～20 节。

---

## 0. 先记住九条硬规则

1. `~x` 的布尔值是 `1-x`；代码中的负整数只是文字极性的编码，不能按算术负数处理。
2. 负文字和负系数是两回事。`3 ~x` 与 `-3 x` 只有在同时调整常数/RHS 后才等价。
3. 同一约束内的重复项和 `x/~x` 必须在 unit、probing、VCIS 和其他规则之前 canonicalize。
4. `>=` 的规则不一定适用于 `=`。尤其不能把不等式的 coefficient saturation 直接作用于一条未展开的等式。
5. SAT-preserving 不等于 count-preserving。纯文字赋值、普通变量消元、BCE 等技术不能默认用于精确 MC。
6. 递归简化推导的文字必须有合法 PB reason；不能用 `decide()` 冒充传播，也不能只改 `assigns`。
7. 简化应处在“传播到不动点之后、组件分解和缓存查询之前”，并与传播组成同一个内部不动点。
8. 只产生赋值的简化通常可继续使用现有缓存键；删除、替换约束或变量消元必须重新论证 residual identity。
9. 任何结构变换都必须同时给出 undo、模型重构、计数因子、缓存语义和暴力枚举测试。

建议用下面五种结论标记每项候选技术：

```text
SAFE_CANONICAL       逐赋值保持约束值或模型集合，可直接规范化
SAFE_WITH_REASON     产生蕴含/冲突，但必须接入 reason 和传播
SAFE_WITH_LEDGER     有双射或计数因子，必须维护映射与模型重构
SAT_ONLY             只保持可满足性，不能直接用于精确计数
HEURISTIC_ONLY       只影响执行顺序，不应进入公式或缓存语义
```

---

## 1. 简化技术首先要按“保持什么语义”分类

令原公式为 `F(V)`，其中 `V` 是全部布尔变量；`F'` 是简化后的公式。本文只研究所有完整满足赋值的精确数量。

### 1.1 模型集合等价

对原变量的每个完整赋值 `α`：

```text
F(α) = F'(α)
```

这类规则最安全，直接保持无权 MC。例如：

- 合并同极性重复项；
- 合并同一约束中的 `x` 和 `~x` 并调整 RHS；
- 删除零系数；
- 正确的 GCD 约简；
- 删除完全重复的约束；
- 把一个等式精确展开为两个方向的不等式。

“通常安全”仍不代表可以忽略工程状态：如果规则改变了变量编号、约束 ID、排序或 ComponentManager 所见公式，仍需同步相应数据结构。

### 1.2 模型双射

简化前后的变量集合不同，但模型之间存在可逆的一一对应。例如：

```text
y = x
y = ~x
```

然后用代表变量替换 `y`。这对无权 MC 通常没有额外倍数，但必须记录：

- `y` 的恢复方式；
- 模型输出时如何恢复原文字；
- 变量编号、组件和缓存表示怎样同步更新。

因此它应标记为 `SAFE_WITH_LEDGER`，不是普通的 `SAFE_CANONICAL`。

### 1.3 带计数因子的等价

有些规则满足：

```text
Count(F) = factor × Count(F')
```

例如删除全局自由计数变量。必须明确：

- `factor` 属于全局根、当前组件还是当前分支；
- 它是否依赖剩余变量赋值；
- 它何时乘入缓存值；
- 缓存命中时是否还需要再次应用。

只有与剩余状态无关的标量才能简单保存为一个 `factor`。若扩展数依赖剩余赋值，必须保存一个更丰富的函数/表示，不能假装成常量。

### 1.4 仅等可满足

只保证：

```text
SAT(F) ⇔ SAT(F')
```

普通 SAT 预处理中很多规则属于这一类，例如不带重构和扩展计数的变量消元、纯文字赋值、blocked clause elimination。它们可以作为探测或证明工具；

但不能直接替换主计数公式。

### 1.5 蕴含约束添加

若 `C` 只使用原公式已有变量，且 `F ⊨ C`，把 `C` 加入公式不会删除任何原变量模型，因此对所有计数模式安全。这包括冲突学习约束。

若 `C` 引入辅助变量，则不能只写 `F ⊨ C`：新增变量可能为每个原模型制造多个扩展并改变 MC。必须证明它是唯一确定的 definitional extension，或明确处理扩展数量。

但需要区分两件事：

- 添加一个被蕴含的约束：不改变模型集合，可以作为传播加速；
- 删除或替换原约束：必须另行证明新公式与原公式等价。

不能因为一个学习约束是安全的，就据此假设任意 strengthening 都能替换原约束。

### 1.6 无权 MC 快速安全矩阵

| 技术类型 | 精确 MC | 主要附加条件 |
|---|---:|---|
| 逐赋值等价 canonicalization | 是 | 同步公式表示与约束 ID |
| 添加逻辑蕴含约束 | 是 | 有合法证明/reason；不引入自由辅助变量 |
| 固定蕴含变量 | 是 | 固定变量只有一个取值，计数因子为 1 |
| 删除全局自由变量 | 是 | 每个自由布尔变量产生因子 2，且只能应用一次 |
| 等价/互补变量替换 | 条件安全 | 必须证明模型双射并保存恢复映射 |
| 普通 existential VE | 否 | 只保持可满足性，丢失每个剩余赋值的扩展数 |
| 纯文字直接赋值 | 通常否 | 会丢掉该变量另一取值对应的模型 |
| BCE/一般 SAT strengthening | 通常否 | 只保持 SAT 不够 |
| 启发式重排 | 是 | 不得改变公式语义 |

---

## 2. PBMC 中一条约束的完整生命线

理解简化应该修改哪一层，必须先区分同一约束的几种表示。

```text
OPB 文本
  ↓ PBInstance::load()
PBconstraint
  ↓ renumberVar()
重编号 PBconstraint
  ↓ makeRenumberedCoeffPositive()
正系数 PBconstraint（文字仍可为负）
  ↓ simplifyRenumberedConstraint()
当前根预处理结果
  ↓ getRenumberedOpb()
内存 OPB 文本
  ↓ roundingsat::parsing::opb_read()
ConstrExp / CeSuper
  ↓ addConstraint()/postProcess()/toConstr()
RoundingSat Constr + CRef + watches
  ↓ ComponentManager::init()
ComponentManager 冻结副本、occurrence、ClID、gaps_ 容器
  ↓ 每次 searchComponent()
按当前赋值计算动态 residual gap
```

关键落点：

| 阶段 | 文件/函数 | 适合做什么 |
|---|---|---|
| OPB 轻量解析 | `core/Instance.cc:420` | 输入检查、静态预处理准备 |
| 正系数化 | `core/Instance.cc:1016` | 翻转负系数项并修正 RHS |
| 当前根预处理 | `core/Instance.cc:1051` | unit、赋值代入、二项 probing |
| RoundingSat 解析 | `roundingsat/parsing.cpp:52` | 任意精度约束构建、等式展开 |
| 约束展开表示 | `roundingsat/ConstrExp.*` | canonicalization、冲突分析、学习 |
| 固定传播表示 | `roundingsat/Constr.*` | watch、slack、propagation |
| 主计数状态机 | `core/Counter.cc:394` | 递归内简化调度、冲突、回跳 |
| 分量视图 | `core/ComponentManager.cc:84` | 分解、动态 gap、缓存键输入 |

### 2.1 `PBconstraint`

定义在 `core/pbconstraint.h:19`：

```cpp
struct PBconstraint {
    vector<Int> lits;
    vector<Int> coeffs;
    Int rhs;
    bool equals;
    Int constraintId;
};
```

它采用平行数组：

```text
coeffs[i] × value(lits[i])
```

其中 `lits[i]` 是有符号、1-based 的变量编号。

### 2.2 `ConstrExp` / `CeSuper`

`ConstrExp` 是可展开、可修改的 PB 表示，主要用于：

- 输入构建；
- 正规化；
- 冲突分析；
- weakening/division/resolution；
- 生成学习约束。

`CeSuper` 是指向不同系数位宽 `ConstrExp` 实例的统一智能句柄。它不是稳定的公式约束 ID，也不是 ComponentManager 的缓存 ID。

### 2.3 `Constr` / `CRef` / `ID`

- `Constr`：allocator 内的紧凑、可 watch 的约束基类；实际可能是 clause、cardinality、counting 或 watched PB 约束。
- `CRef`：`ConstraintAllocator` 中的位置引用，只能由对应 allocator 解释。
- `ID`：逻辑/证明日志使用的约束身份，不等同于 `CRef` 偏移。

Solver 和 ComponentManager 各自有 allocator。不能把 solver `ca` 的 `CRef` 直接拿去索引 ComponentManager 的 `pbca_`。

### 2.4 ComponentManager 的 `ClID`

`ComponentManager::init()` 把初始化时 solver 的约束复制到自己的 `pbca_`，然后用 `constraints_` 数组下标作为本地 `ClID`。缓存键中的约束编号是这套本地 `ClID`，不是直接把 `Constr::id` 打包进去。

因此新增结构简化时应明确区分：

```text
solver CRef
solver Constr::id
component-manager CRef
component-manager ClID
```

### 2.5 三套文字编码

| 模块 | 正文字 | 负文字 | 变量下标 |
|---|---:|---:|---:|
| OPB / `PBconstraint` | `+N` | `-N` | 1-based |
| RoundingSat `rs::Lit` | `+N` | `-N` | 1-based |
| Glucose `Lit` | `2*v`/打包 sign | `2*v+1`/打包 sign | 0-based |

Glucose 与 RoundingSat 的桥接还使用了本项目自己的 polarity 约定，见 `core/Counter.cc:161-203`。新增 phase、文字输出或其他跨表示代码时必须调用集中转换函数，禁止直接 `static_cast`。

---

## 3. 负文字、负系数、重复项和互补项

### 3.1 负文字不是负变量值

布尔变量 `x∈{0,1}`：

```text
value(x)  = x
value(~x) = 1-x
```

真值表：

| `x` | `x` | `~x` |
|---:|---:|---:|
| 0 | 0 | 1 |
| 1 | 1 | 0 |

代码把 `~x5` 存为整数 `-5`，只是为了同时编码变量编号和极性。进行算术变换时必须先取 `abs(lit)` 得到变量，再根据符号计算文字值。

### 3.2 负文字和负系数严格区分

```text
+3 ~x5       正系数乘负文字
-3 x5        负系数乘正文字
-3 ~x5       负系数乘负文字
```

对任意布尔文字 `l` 和 `a>0`：

```text
-a*l = a*(1-l)-a = a*~l-a
```

所以在约束中：

```text
-a*l + R >= d
```

严格等价于：

```text
a*~l + R >= d+a
```

等式同样成立。PBMC 在 `getPositiveCoeffRenumberedConstraint()` 中执行：

```cpp
rhs -= negative_coefficient;
lit = -lit;
coefficient = -coefficient;
```

例子：

```text
-3 x1 + 2 ~x2 >= 4
```

变成：

```text
3 ~x1 + 2 ~x2 >= 7
```

`-3 ~x1` 则会翻转成 `+3 x1`，RHS 同样增加 3。

### 3.3 固定赋值代入

对正系数项 `a*l`：

- 若 `l` 在当前赋值下为真：删除项，并令 `rhs -= a`；
- 若 `l` 为假：删除项，RHS 不变；
- 若变量未赋值：保留项。

该规则对 `>=` 和 `=` 都是逐赋值等价。当前 PBInstance 实现在 `core/Instance.cc:1105-1123`，ComponentManager 对负文字的 gap 计算在 `core/ComponentManager.cc:311-320`。

### 3.4 同极性重复项

```text
a*x + b*x       → (a+b)*x
a*~x + b*~x     → (a+b)*~x
```

这是最简单的逐赋值等价规则。合并前要使用足够宽的整数类型检查加法溢出。

### 3.5 互补项合并

对同一变量：

```text
A*x + B*~x
```

因为 `~x=1-x`：

```text
A*x + B*~x = B + (A-B)*x
```

使用正系数文字表示时：

| 条件 | 保留项 | 新 RHS |
|---|---|---|
| `A>B` | `(A-B)*x` | `rhs-B` |
| `B>A` | `(B-A)*~x` | `rhs-A` |
| `A=B` | 不保留该变量 | `rhs-A` |

等价的统一写法：

```text
m = min(A,B)
rhs -= m
只保留系数差及较大一侧的极性
```

例子：

```text
7 x1 + 3 ~x1 + 2 x2 >= 6
```

变成：

```text
4 x1 + 2 x2 >= 3
```

这个恒等式对等式也完全安全。

该合并只适用于同一条约束内部。若 `x` 出现在约束 `C1`、`~x` 出现在另一条约束 `C2`，不能把两项直接相消；跨约束处理属于 PB resolution、变量消元或蕴含分析，必须同时处理新约束和完整模型的扩展计数。

### 3.6 RoundingSat 已经会合并，但发生得太晚

`ConstrExp::addLhs()` 在 `roundingsat/ConstrExp.cpp:332` 中为每个变量只保留一个有符号系数槽：

- 正文字累加正系数；
- 负文字通过 RHS 调整后累加负系数；
- 同极性自动相加；
- 相反极性自动相消；
- 零系数随后删除。

但是当前执行顺序是：

```text
PBInstance probing/VCIS
    在前
RoundingSat addLhs 合并
    在后
```

因此不能以“后端最终会合并”为理由省略前端 canonicalization。

### 3.7 当前二项 probing 的已验证风险

`probeInferAssign()` 只看 `lits.size()==2`。若两项实际上属于同一变量，例如：

```text
2 x1 + 5 ~x1 >= 4 ;
```

`applyProbe()` 给 `x1` 赋值时会同时删除两项，得到空约束；`inferProbe()` 随后仍访问 `coeffs.at(0)`，会抛出 `std::out_of_range`。

同一变量的两项即使极性相同，也会破坏“二项约束赋一个变量后恰好剩一个文字”的假设。因此规范化必须位于 probing 之前。

### 3.8 本地 OPB 数据扫描事实

扫描范围：

```text
benchmarks/opb/auction
benchmarks/opb/graph
benchmarks/opb/knapsack
benchmarks/cost-graph
```

不包括 `benchmarks/cnf` 中压缩的 CNF 文件。共扫描：

```text
5000 个 OPB 文件
16,703,384 条约束
781,208,944 个 coefficient/literal 项
```

结果：

- 原始 `~x` 只出现在 auction；
- 999 个 auction 文件含负文字；
- 9,306 条约束含负文字；
- 共 127,612 个原始负文字项；
- 同一约束任意位置出现同变量 `x/~x`：0；
- 相邻互补项：0；
- 第一项与第二项互补：0；
- 同变量同极性重复项：0；
- 按“原极性 XOR 系数为负”完成 PBMC 正系数化后，有效互补项和重复项仍为 0。

因此互补合并在当前原始基准上暂无直接命中，但它仍是新预处理器的必要前置不变量，因为自定义输入、约束组合、变量消元或其他简化可能产生这种结构。

这些数字是基于文首提交和上述目录内容得到的本地快照，不是 PBMC 运行时自动统计。复现时应逐条解析 OPB 的 coefficient/literal 对，以“变量编号 + 极性”聚合；再按第 3.2 节把负系数翻转极性后重复统计。不能只用文本搜索 `x`/`~x`，也不能把跨约束的正负出现误报成同一约束内的互补项。benchmarks 增删后应重新生成快照，不能把“当前为 0”当作输入格式的永久保证。

---

## 4. 等式支持与等式专用规则

### 4.1 PBMC 支持标准 OPB 等式

前端遇到 `=` 时设置 `PBconstraint::equals=true`，见 `core/Instance.cc:529-536`。该标记在重编号、正系数化和当前输入简化中保留。

RoundingSat parser 在 `roundingsat/parsing.cpp:104-116` 中把：

```text
LHS = rhs
```

转换为：

```text
LHS  >= rhs
-LHS >= -rhs
```

第二条就是 `LHS<=rhs`。因此核心求解器通过两个不等式精确支持等式。

建议始终使用标准带空格格式：

```opb
+1 x1 +1 x2 = 1 ;
```

当前 RoundingSat 对等式第二方向的识别使用字符串 `" = "`，而 PBMC 自己重新输出 OPB 时会生成该格式。`<=` 不是当前正式输入语言；PBInstance 对非 `=` 比较符可能静默当作 `>=`，不得直接输入。

### 4.2 数据集中的等式

在上述 5000 个 OPB 文件中：

| 数据集 | 含等式文件 | 等式约束 |
|---|---:|---:|
| auction | 272 | 329 |
| knapsack | 480 | 480 |
| graph | 0 | 0 |
| cost-graph | 0 | 0 |
| 合计 | 752 | 809 |

例子：

```text
benchmarks/opb/auction/auction-bench-755-90-7-0.54.opb:9
benchmarks/opb/knapsack/k-dim-knapsack-bench-134-6-18.opb:3
```

简单实例 `x1+x2=1` 的精确模型数为 2，当前 PBMC 可以正确求解。

### 4.3 对 `>=` 和 `=` 都安全的规则

- 负系数转正并调整 RHS；
- 同极性重复项合并；
- `x/~x` 互补项合并；
- 固定赋值代入；
- 删除零项；
- 完全重复等式删除；
- 若所有系数和 RHS 同时被同一正整数整除，整体约分；
- 把等式精确展开成两个方向不等式。

这些规则来自代数恒等式，不只是等可满足变换。

### 4.4 等式的冲突和强制赋值

正系数 canonical equality：

```text
Σ a_i l_i = k
```

设 `S=Σa_i`：

- `k<0` 或 `k>S`：冲突；
- 空等式：`k==0` 恒真，否则冲突；
- `k==0`：所有文字必须为假；
- `k==S`：所有文字必须为真；
- 若 `g=gcd(a_i)` 且 `k mod g != 0`：冲突；
- 若某文字系数 `a_i>k`，该文字不能为真；
- 若 `S-a_i<k`，该文字必须为真。

更强的等式推理可能退化为 subset-sum 风格问题，不能假设所有推理都能线性时间完成。实际实现可以通过“两条不等式分别传播”获得可靠基础能力。

### 4.5 不等式 saturation 不能直接作用于未展开等式

对正系数不等式：

```text
Σ a_i l_i >= k, k>0
```

可以把每个系数替换成 `min(a_i,k)`。

但：

```text
2x = 1
```

本来无解，若直接把系数截成 1，就错误地变成 `x=1`。

正确选择有两个：

1. 保留 equality IR，使用等式专用规则；
2. 先精确展开为 `>=` 和 `<=` 两个不等式，再分别使用对不等式安全的 saturation。

RoundingSat 采用第二种方法。

### 4.6 等式带来的双重视图

当前静态层和搜索层对等式的计数口径不同：

```text
PBInstance / VCIS：一条 equality 对象
RoundingSat / ComponentManager / cache：两个内部 inequality 约束
```

这会影响：

- occurrence 次数；
- VCIS 与 TD 所见公式；
- constraint ID 数量；
- 分量图边；
- 缓存键中的 ClID/gap；
- 冲突学习可能使用等式任一方向作为 reason。

新预处理器和新启发式必须明确选择“原始等式口径”还是“展开后口径”，不要把两者混在一个分数中。

---

## 5. 推荐的统一 canonicalization 流水线

所有后续简化技术都应消费同一种 canonical constraint。建议每条输入约束按以下顺序处理。

### 5.1 标准顺序

```text
1. 验证比较符只为 >= 或 =
2. 验证变量编号、非零文字、系数/RHS 数值范围
3. 删除零系数项
4. 把负系数改为正系数 + 翻转文字 + 调整 RHS
5. 按 abs(variable) 聚合同极性重复项
6. 合并 x/~x 公共部分并调整 RHS
7. 删除净系数为零的变量项
8. 检查空约束、恒真、直接冲突
9. 按运算符执行安全的 saturation/GCD 规则
10. 检测 clause/cardinality/equality 等专用类型
11. 按“系数降序、变量 ID、极性”确定性排序
12. 生成 canonical fingerprint/统计
```

### 5.2 互补聚合伪代码

```cpp
for (term : constraint) {
    Lit lit = term.lit;
    BigInt coeff = term.coeff;

    if (coeff == 0) continue;
    if (coeff < 0) {
        rhs -= coeff;       // coeff 为负，因此 RHS 增加 |coeff|
        coeff = -coeff;
        lit = -lit;
    }

    Var v = abs(lit);
    if (lit > 0) pos[v] += coeff;
    else         neg[v] += coeff;
}

for (Var v : all_used_vars) {
    BigInt m = min(pos[v], neg[v]);
    rhs -= m;

    if (pos[v] > neg[v]) emit(pos[v]-neg[v], +v);
    if (neg[v] > pos[v]) emit(neg[v]-pos[v], -v);
}
```

这段流程必须使用精确整数。当前 `PBconstraint::Int` 是 `long`，解析使用 `stol`，而 RoundingSat 后端可以使用更大的整数。长期应统一为任意精度 IR；在此之前至少要在每次加法、取负和 RHS 调整前检测溢出。

### 5.3 空约束判断

canonicalization 后若没有任何项：

| 约束 | 恒真条件 | 冲突条件 |
|---|---|---|
| `0 >= k` | `k<=0` | `k>0` |
| `0 = k` | `k==0` | `k!=0` |

不要把“变量项被消掉”直接解释为自由变量。它只说明变量不再出现在当前约束；只有检查整个 residual formula 和所有定义/重构关系后，才能判定全局自由。

### 5.4 `>=` 的 saturation 与 GCD

对 positive canonical inequality：

```text
Σ a_i l_i >= k
```

- `k<=0`：恒真；
- `Σa_i<k`：冲突；
- saturation：`a_i←min(a_i,k)`；
- `g=gcd(a_i)` 时，可改成：

```text
Σ (a_i/g) l_i >= ceil(k/g)
```

RoundingSat 对应实现：

- `ConstrExp::saturate()`：`roundingsat/ConstrExp.cpp:461`；
- `ConstrExp::divideByGCD()`：`roundingsat/ConstrExp.cpp:655`；
- `ConstrExp::postProcess()`：`roundingsat/ConstrExp.cpp:670`。

### 5.5 equality 的 GCD

对：

```text
Σ a_i l_i = k
```

令 `g=gcd(a_i)`：

- 若 `k mod g != 0`：立即冲突；
- 否则所有系数和 RHS 同除以 `g`。

不能使用不等式的 `ceil(k/g)` 来“修复”不可整除的等式。

### 5.6 确定性排序与 fingerprint

canonicalization 后建议固定排序：

```text
系数降序
→ abs(variable) 升序
→ polarity 固定顺序
```

确定性排序用于：

- RoundingSat watch/gap 假设；
- 可复现实验；
- 相同约束检测；
- canonical cache signature；
- 单元测试的稳定输出。

fingerprint 只能作为哈希筛选，最终等价判断仍要比较完整 canonical 内容，避免哈希碰撞破坏精确计数。

### 5.7 canonicalization 的后置不变量

每条约束至少满足：

```text
所有系数 > 0
每个变量最多出现一次
不存在 x 与 ~x 同时出现
不存在零系数
文字与系数数组等长
排序确定
operator 明确为 >= 或 =
数值无溢出
空/恒真/冲突已经分类
```

unit、probing、VCIS、dominance、缓存 fingerprint 和递归简化都应依赖这些显式不变量，而不是依赖当前 benchmark 恰好没有重复项。

---

## 6. 简化技术目录与 PBMC 落地要求

本节把常见 PB/CNF 简化按风险分组。外部技术若不完全对应这些名称，应先抽象出它究竟执行了哪一种语义变换。

### 6.1 恒真、冲突和边界检查

对 canonical inequality：

```text
C: Σ a_i l_i >= k, a_i>0
S = Σ a_i
```

立即可判定：

- `k<=0`：恒真，可删除；
- `S<k`：冲突；
- `S==k`：所有文字必须为真；
- 空约束按第 5.3 节处理。

对 equality 见第 4.4 节。

落地属性：

```text
类别：SAFE_CANONICAL / SAFE_WITH_REASON
根预处理：可永久执行
递归：可检测；冲突应返回 reason/CeSuper 或顺序回溯
缓存：只产生赋值时通常由 vars+gap 反映
```

### 6.2 PB generalized unit propagation / forced literal

对当前部分赋值，定义：

```text
gap = k - 已经为真的文字系数和
remaining_sum = 所有未赋值文字的系数和
slack = remaining_sum - gap
```

则：

- `gap<=0`：约束已满足；
- `slack<0`：冲突；
- 对未赋值文字 `l_i`，若 `a_i>slack`，则 `l_i` 必须为真。

等价理解：若把 `l_i` 置假，即使其他所有未赋值文字都为真，也无法达到 RHS。

RoundingSat 的 watched propagation 已实现这一基础规则，见：

- `roundingsat/Solver.cpp:195-256`；
- `roundingsat/Constr.cpp:285-355`；
- `roundingsat/Solver.cpp:185-189`。

因此新增“forced literal 简化”前应先明确它是否比现有传播更强。若只是重新扫描相同 slack 条件，可能只增加开销。更有价值的方向包括：

- equality 两方向联合推理；
- 多约束组合产生的强制文字；
- probing 后的强制文字；
- dominance/局部上界带来的新 slack；
- 简化后约束变短，使现有 propagation 更早触发。

递归中推导文字必须通过 `propagate(lit, reason)` 或等价封装进入当前层，不能调用 `decide()`。

### 6.3 固定变量代入

若 `F ⊨ l`，可把 `l=true` 代入所有约束。需要区分：

- 根预处理：永久记录 fixed literal，更新公式；
- 递归：加入当前层 trail，由回跳自动撤销；
- 无权 MC：固定变量只剩一个合法取值，计数因子为 1；
- 若需要输出原模型，必须在重构 ledger 中保存该固定文字。

固定变量本身的无权计数因子是 1，不是 2。

### 6.4 GCD、saturation 和 cardinality detection

这三类规则容易被混为一谈：

- GCD：等价缩小所有系数与 degree；
- saturation：对 `>=` 截断过大系数；
- cardinality detection：证明一个一般 PB 约束与某个单位系数约束等价。

RoundingSat `postProcess()` 会：

```text
removeUnitsAndZeroes
→ saturation
→ divideByGCD
→ simplifyToCardinality(true)
```

其中 `postProcess()` 注释明确限定为 equivalence-preserving。相反，冲突分析中的 `heuristicWeakening()`、`weaken()` 和 `divideRoundUp()` 可生成较弱但被蕴含的约束，适合学习，不代表可以拿来替换原始公式。

### 6.5 精确重复约束

两条约束 canonical 后：

```text
operator、RHS、文字、系数完全相同
```

可以删除一条。它对四种计数模式都安全。

根预处理应永久删除并重建 occurrence；递归中若只把一条在 active-mask 中失活，需要让 overlay 和缓存语义确定性地反映这一结果。

### 6.6 同 LHS 的简单支配

对相同 canonical LHS 的两个不等式：

```text
LHS >= k1
LHS >= k2
```

若 `k1>=k2`，第一条蕴含第二条，可删除第二条。

同一约束乘正整数、再经 GCD canonicalization 后也应归并到同一形式。

等式不能套用该规则。`LHS=k1` 和 `LHS=k2`：

- `k1==k2`：重复；
- `k1!=k2`：两者同时存在时冲突，而不是删除其中一条。

### 6.7 一般 PB dominance / subsumption

一般 PB 蕴含不是简单比较变量集合或系数大小。一个可靠定义是：要删除 `C2`，必须证明：

```text
F_without_C2 ⊨ C2
```

等价地检查：

```text
F_without_C2 ∧ ¬C2
```

是否不可满足。

一个只用于快速过滤的充分条件是：存在 `λ>0`，使保留约束 `C1:Σa_i l_i>=d1` 与候选 `C2:Σb_i l_i>=d2` 满足：

```text
b_i >= λ*a_i  对所有文字成立
λ*d1 >= d2
```

则 `C1` 蕴含 `C2`。这只是充分条件，不是完整 PB subsumption 判定；缺失文字、极性和 canonicalization 都必须统一处理。

建议落地分三层：

1. fingerprint 找完全重复；
2. 相同 support/LHS 的廉价规则；
3. 有预算的 PB implication check。

不能根据“支持集更小”或“某些系数更大”直接删除约束。

### 6.8 当前赋值下已满足约束

在一个递归节点，`gap<=0` 的约束无需连接未赋值变量。ComponentManager 当前已经在 `searchComponent()` 中忽略这种约束。

这不是永久删除原公式约束：回跳后赋值撤销，约束可能重新活跃。所需状态通常由赋值和 gap 重算，不必创建一个永久 rewrite。

### 6.9 failed literal / probing

基本规则：

```text
假设 l → propagation conflict     则推出 ~l
假设 l 和 ~l 都冲突              则当前状态冲突
两个分支都推出 m                  则当前状态推出 m
```

根预处理可以在临时 solver 状态上执行并永久写回结果。递归 probing 必须：

- 限制在当前组件；
- 设置每节点变量数、传播数、深度和时间预算；
- 临时假设后完整恢复 trail、watch/slack、简化 overlay；
- 为最终推导生成全局有效或带上下文条件的 reason；
- 区分“当前上下文蕴含”和“根公式全局蕴含”。

当前 PBInstance probing 位于 `core/Instance.cc:1235-1366`，只处理二项约束，使用 `double` 比值，并存在第 3.7 节的空约束越界风险。建议不要在其上继续堆复杂 probing，而是先修 canonicalization 和精确整数推理，再设计统一的 probe context。

### 6.10 backbone / 公共蕴含

若在 `l` 与 `~l` 两个完整探测分支中都推出 `m`，则当前上下文蕴含 `m`。若两个分支得到相同的非单位 PB consequence，也可以组合成当前上下文的蕴含约束。

但“两个临时分支中都看到了某个实现状态”并不自动给出可用于回跳的 PB proof。需要保存产生它的 reason 链，或重新构造一个被当前公式蕴含的约束。

### 6.11 等价变量与互补变量替换

形式：

```text
y = x
y = ~x
```

在无权完整模型计数中，这是模型双射：代表变量 `x` 的每个满足取值都唯一恢复 `y`，所以删除 `y` 不产生额外倍数。

但必须保存映射：

```text
y=x   → 重构时 y 取 x 的值
y=~x  → 重构时 y 取 x 的反值
```

如果替换关系不是全局等价，或某个剩余赋值能对应 `y` 的多个取值，就不是模型双射，不能使用这条规则。

该规则会影响：

- 变量编号和内部映射；
- 原变量到代表变量的映射；
- phase、VCIS、TD 特征；
- Component 变量集合；
- 缓存键；
- 原模型重构。

因此建议只在根预处理器具有完整 `ReconstructionLedger` 后加入，递归版本放到后期。

### 6.12 定义变量消除

若某变量由其他变量唯一确定，消除可能形成完整模型之间的双射，比一般变量消元安全。必须证明：

- 定义在所有满足赋值下唯一；
- 替换不会引入系数溢出或指数级项数；
- 每个简化后模型都能唯一恢复被消变量；
- 原变量模型输出可以通过 ledger 恢复；
- equality/负文字处理正确。

“看起来像定义”不等于唯一决定。PB equality 可能允许多个组合，必须进行正式判定。只要定义不是唯一函数，普通删除就会改变完整模型数。

### 6.13 一般变量消元

普通 Davis–Putnam/Bucket elimination 或 PB resolution 通常保持：

```text
∃x F(x,Z)
```

而不是完整模型数。对不同 `Z`，被消变量可能有 0、1 或 2 个扩展，因此 MC 不能只保留存在性。

对无权 MC，必须保存每个剩余赋值的扩展数量；普通 existential VE 只保存“是否至少有一个扩展”，因此不够。

递归内 VE 还会产生/删除约束、改变分解和 key，并要求完整 undo。它应在 assignment-only、reason、overlay 和缓存升级全部稳定之后再做。

### 6.14 纯文字规则

纯文字赋值在 SAT 中安全，但在模型计数中通常不安全。例如：

```text
x ∨ y
```

`x` 只有正出现。令 `x=true` 会丢掉模型 `x=false,y=true`。

默认分类：`SAT_ONLY`。

### 6.15 自由变量

变量在整个 residual formula 中不再出现时：

| 情况 | 无权 MC 贡献 |
|---|---:|
| 一个全局自由布尔变量 | `2` |
| `k` 个相互独立的全局自由布尔变量 | `2^k` |

“从一条约束中消失”不是“全局自由”。还必须检查其他约束和任何定义/重构关系。

### 6.16 BCE、autarky 和 SAT 专用 strengthening

Blocked clause/constraint elimination、autarky、某些 self-subsuming resolution 和 SAT strengthening 可能保持可满足性，却改变模型个数或扩展数。

移植时必须要求技术提供：

- 原变量模型集合是否保持；
- 若只存在映射，映射是否双射；
- 被删除变量/约束的扩展计数如何恢复；
- 完整模型扩展数量如何恢复。

若论文只证明 SAT equisatisfiability，默认标记为 `SAT_ONLY`。

### 6.17 添加 resolvent / cutting-plane consequence

只添加被公式蕴含的 resolvent/cut，不删除原约束时，对所有计数模式安全。它可能加强传播，但也会：

- 增加 watch 和冲突分析开销；
- 增大学习数据库；
- 若错误加入 ComponentManager，可能用冗余边连接组件；
- 需要删除/淘汰策略。

这类技术更接近约束学习，而不是模型集合改写。

### 6.18 分解导向的冗余失活

若某原始约束已被其他保留约束蕴含，可以在当前 residual view 中将它失活，从而删除二部图中的连接边，发现更多组件。

根预处理可以永久删除；递归中需要：

```text
active-mask / residual overlay
decision-level undo marker
searchComponent() 读取 overlay
cache key 或 canonical determinism 证明
```

如果只从 solver allocator 删除而不更新 ComponentManager 冻结副本，分解不会得到任何收益。

### 6.19 技术落地总表

| 技术 | 根预处理 | 递归内 | reason | undo | 现有 key |
|---|---:|---:|---:|---:|---:|
| 负系数/重复/互补 canonicalization | 是 | residual view 可做 | 否 | 结构改写时要 | 根重建后可用 |
| 恒真/冲突检测 | 是 | 是 | 冲突最好需要 | 否/少量 | 通常可用 |
| forced literal | 是 | 是 | 必须 | trail 自动 | 通常可用 |
| GCD/安全 saturation | 是 | 谨慎 | 否 | 若改结构要 | 改 ID 内容则不够 |
| duplicate 删除 | 是 | 是 | 蕴含证明简单 | active-mask 要 | overlay 后需论证 |
| dominance/subsumption | 是 | 是 | 删除证明 | active-mask 要 | 需论证/扩展 |
| failed literal | 是 | 有预算 | 必须 | probe 状态要 | 推导赋值通常可用 |
| 学习约束添加 | 是/搜索中 | 是 | 本身即证明 | solver 管理 | 不必进入公式 key |
| 等价变量替换 | 后期 | 更后期 | 映射证明 | 必须 | 必须升级 |
| 一般变量消元 | 条件严格 | 最后 | resolvent/ledger | 必须 | 必须升级 |
| 纯文字/BCE | 默认禁用 | 默认禁用 | SAT 证明不够 | — | — |

---

## 7. 当前根预处理器：能做什么、不能做什么

### 7.1 当前真实调用顺序

`core/Main.cc:221-231`：

```text
renumberVar()
→ makeRenumberedCoeffPositive()
→ simplifyRenumberedConstraint()
→ computeVarCoeffScore()
→ getRenumberedOpb()
→ RoundingSat parser
```

在 **PBInstance 层**，当前唯一显式的全公式预处理循环是 `PBInstance::simplifyRenumberedConstraint()`。遗留 `PPMC::Preprocessor` 操作 CNF `Instance`，主 PBMC 流程没有把 `PBInstance` 交给它。

这不表示后端没有继续规范化：每条输入进入 RoundingSat 的 `addInputConstraint()` 后，还会执行 `postProcess()`（去 unit/零项、saturation、GCD、cardinality detection）并在根层调用 `runPropagation(true)`。因此应区分“PBInstance 全公式预处理器”和“RoundingSat 逐约束规范化/根传播”。

### 7.2 当前规则

`simplifyRenumberedConstraint()` 循环执行：

- 单文字推导；
- 空约束冲突判断；
- `>=` 恒真删除；
- 固定赋值代入；
- 仅二项约束的局部 probing；
- 直到没有变化。

推导变量不会从变量集合真正删除。`getRenumberedOpb()` 会把推导结果重新输出成单位 PB 约束，让 RoundingSat 在根层固定它们。

### 7.3 开发前必须处理的风险

1. `PBconstraint::Int=long`，前端可能在后端任意精度接手前溢出。
2. unit 和 probing 使用 `double rhs/coef`，大整数精度不可靠。
3. probing 假定二项约束含两个不同变量，canonicalization 缺失会越界。
4. `bool preprocess=true` 硬编码，配置没有真正控制。
5. inferred UNSAT 当前使用异常退出路径，不是正常输出计数 0。
6. 重输出 OPB 的头部约束数没有计入追加的单位约束。
7. equality 在静态层一条、后端两条，特征口径不一致。
8. 某些 failed-probe 写入成员 map，函数结尾又用局部 map 覆盖，结果可能丢失。

### 7.4 新根预处理器的建议架构

```text
Canonical Formula IR
        ↓
Pure Rule Engine
        ↓
StaticFormulaAdapter
        ├─ 永久 rewrite
        ├─ fixed/free/count factor ledger
        ├─ old↔new variable mapping
        └─ reconstruction metadata
        ↓
最终 canonical formula
        ↓
重建 RoundingSat / ComponentManager / VCIS / TD
```

建议结果对象：

```cpp
template<class CountT>
struct PreprocessResult {
    Formula simplified_formula;
    bool unsat = false;
    std::vector<Lit> fixed_literals;
    std::vector<Var> old_to_new;
    std::vector<Var> new_to_old;
    CountT global_count_factor = 1;
    ReconstructionLedger reconstruction;
    RuleStats stats;
};
```

规则引擎应只判断数学变换；根预处理 adapter 与递归 adapter 分别负责永久修改和可回溯修改，避免两套规则逐渐分叉。

---

## 8. 递归内简化的正确接入协议

### 8.1 PBMC 的“递归”是状态机

主搜索不是 C++ 自递归函数。`PBcounter::count_main()` 使用：

```text
RoundingSat trail/trail_lim
ComponentManager Decision 栈
Component 栈
```

模拟深度优先递归。因此“每个递归节点执行简化”应理解为：每次新决策或组件进入后，传播结束、分解开始前运行简化。

### 8.2 当前空入口不适合直接填代码

`PBcounter::simplify()` 位于 `core/Counter.cc:152`，当前直接返回 `true`。调用位置在 `Counter.cc:521`：

- 已经晚于 `splitComponent()` 和 cache lookup；
- 受 `checkfixedDL()` 等特殊条件限制；
- 不是每个节点都会调用；
- `bool` 无法描述推导、冲突、结构变化和计数因子。

应停用旧入口，在 `Counter.cc:416` 传播后、`468-469` 分解前建立新协议。

### 8.3 必须是“传播—简化”的联合不动点

推荐控制流：

```text
保存本节点 simplification/count-factor marker
        ↓
RoundingSat propagation
        ├─ conflict → 统一冲突分析/回溯
        └─ no conflict
        ↓
syncAssign()
        ↓
RecursiveSimplifier
        ├─ IMPLIED
        │    └─ 带 reason enqueue，回到 propagation
        ├─ ENTAILED_CONSTRAINT
        │    └─ 规范化并 attach，回到 propagation
        ├─ CONFLICT
        │    └─ 返回 CeSuper 或顺序分支失败
        ├─ TOPOLOGY_CHANGED
        │    └─ 更新 residual overlay，继续规则不动点
        ├─ COUNT_FACTORED / SOLVED
        │    └─ 提交一次 factor/count
        └─ UNCHANGED
        ↓
splitComponent() + cache lookup
```

不能“传播一次、简化一次”后立刻分解；简化产生赋值后，RoundingSat 还可能继续传播并产生冲突或更多赋值。

### 8.4 无权 MC 的计数因子记账

在无权 MC 中，trail 上的固定文字本身不产生额外乘数；它只有一个合法取值。真正需要单独记账的是：

- 全局自由变量产生的 `2^k`；
- 某个有严格证明的结构规则产生的常数扩展因子；
- 分解后独立组件计数的乘积。

每个 factor 必须明确属于 root、当前 component 或当前 branch，并且只在一个位置应用一次。回跳时，branch/component factor 要随 simplification marker 恢复；写入缓存前要明确缓存值是否已经包含该 factor。

### 8.5 建议结果接口

```cpp
enum class TerminalStatus {
    Continue,
    Conflict,
    Solved
};

template<class CountT>
struct SimplifyResult {
    TerminalStatus terminal = TerminalStatus::Continue;
    bool changed = false;
    std::vector<Implication> implied;
    std::vector<rs::CeSuper> entailed;
    std::optional<rs::CeSuper> conflict;
    SimplificationDelta delta;
    CountT factor = 1;
    FactorScope factor_scope = FactorScope::None;
    bool topology_changed = false;
    uint64_t residual_signature = 0;
};
```

`implied`、`entailed`、`delta`、`factor` 和 `topology_changed` 可能同时发生，不能用一个互斥枚举丢掉其中某项。只有 `Conflict`/`Solved` 是终止状态。adapter 应使用固定事务顺序：验证证明与 delta → 处理终止状态 → 提交公式 delta/attach consequence → enqueue implication → 提交一次计数因子 → 更新拓扑与 residual signature → 回到传播—简化不动点。`factor_scope` 必须明确是 root、component 还是 branch，且只能提交一次。

每条规则还应声明元数据：

```text
supports_ge
supports_equality
supports_negative_literals
supports_exact_mc
produces_reason
changes_variables
changes_topology
needs_undo
changes_cache_identity
```

### 8.6 可回溯状态

新增第四条与现有三条栈对齐的状态：

```text
decision level
├─ RoundingSat trail marker
├─ Component/Decision marker
├─ Simplification undo marker
└─ count-factor marker
```

必须覆盖：

- 第一分支切换第二分支：`cancelCurDL()`；
- 第二分支结束返回父层：`backtrack()`；
- 非顺序回跳：`bjResolve()`；
- restart；
- 最终 `backjumpTo(0)`；
- 中断/异常清理。

不要只在普通 `backtrack()` 撤销，因为非顺序回跳会跨过多个中间层。

### 8.7 不要原地修改已 attach 的 `Constr`

RoundingSat `Constr` 已经建立：

- watch list；
- 增量 slack；
- allocator 布局；
- reason 引用；
- activity/LBD；
- ComponentManager 冻结副本。

递归中直接改其文字、系数或 degree 会破坏这些结构。第一阶段应：

- 赋值推导通过 reason/trail；
- 结构失活通过 residual overlay；
- 根层永久改写后统一重建 solver 和 ComponentManager。

### 8.8 预算与调度

递归简化不能在每个节点无上限扫描整个公式。建议预算：

```text
每节点最大规则调用数
每节点最大约束/非零项访问数
probing 最大变量数和传播数
只处理当前 Component
按深度/组件大小启停
全局 inprocessing 时间占比
规则成功率过低时暂时关闭
```

统计应至少包括 calls、success、implied、conflicts、removed edges、new splits、time。

---

## 9. PB 传播、reason、冲突分析与约束学习

约束冲突学习不是普通“公式简化”，但递归简化要产生蕴含、发现冲突和非顺序回跳，就必须遵守这套协议。

### 9.1 当前 MC 学习路径与标准 RoundingSat 路径

| 路径 | 入口 | 冲突处理 | 用途 |
|---|---|---|---|
| 无权完整模型精确计数 | `PBcounter::count_main()` | `analyzePBMC2()` + 选择性回跳 | 本文关注的主计数路径 |
| 标准 RoundingSat | `rs::Solver::solve()` | `analyze()` + `learnedStack` | 不是主计数直接路径 |

`PBcounter` 继承 `rs::Solver`。PB 传播和冲突学习在 `roundingsat/`，不是 `core/Solver.cc` 中遗留的 Glucose CNF 冲突引擎。在 `-mode=0` 下，所有变量都按完整模型计数处理，本文不展开 `solveSATFixedOrder()` 的其他模式用途。

### 9.2 核心状态

`roundingsat/Solver.hpp` 中：

| 状态 | 含义 |
|---|---|
| `ca` | solver 的约束 allocator |
| `constraints` | 当前数据库中的 `CRef` |
| `adj` | signed-literal watch lists |
| `trail` | 当前赋值文字序列 |
| `trail_lim` | 每层在 trail 中的开始位置 |
| `Level[l]` | 有符号文字为真的层 |
| `Pos[v]` | 变量在 trail 中的位置 |
| `Reason[v]` | 推导变量的原因约束 `CRef` |
| `qhead` | 已传播到 trail 的位置 |
| `learnedStack` | 尚未正式 attach 的学习约束 |

两种赋值入口语义不同：

```text
decide(l)
  → 新建决策层
  → Reason[v] = CRef_Undef

propagate(l, reason)
  → 保持当前决策层
  → decisionLevel()>0 时 Reason[v] = reason
  → decisionLevel()==0 时 uncheckedEnqueue() 会把 Reason[v] 重置为 CRef_Undef
```

因此简化推导绝不能用 `decide()` 添加。根层 `Reason[v]==CRef_Undef` 是 RoundingSat 的有意设计：根单位原因不再保存在 `Reason` 数组中；启用 proof logger 时会另外记录 unit proof。非根层推导则必须保留合法 reason，供冲突分析消解。

### 9.3 slack、传播与冲突

对：

```text
Σ a_i l_i >= d, a_i>0
```

RoundingSat 使用：

```text
slack = 所有“尚未为假”文字的系数和 - d
```

- `slack<0`：即使所有未知文字取真也无法满足，冲突；
- 未赋值文字 `l_i` 满足 `a_i>slack`：`l_i` 被强制为真。

trail 中 `p` 成真后，`-p` 刚变假，所以统一传播器扫描 `adj[-p]`。

约束会根据形状使用不同实现：

- degree 1：`Clause`；
- 所有系数为 1：`Cardinality`；
- 一般约束：`Counting` 或 `Watched`；
- 大系数：对应 `Safe` 版本。

这些类型共同通过 `checkForPropagation()` 返回 KEEP、DROP 或 CONFLICTING。

### 9.4 四类约束对象不要混淆

| 对象 | 用途 |
|---|---|
| 原始公式约束 | 定义模型集合；ComponentManager 冻结复制 |
| propagation reason | 证明某个 trail 文字为何必须为真 |
| conflict constraint | 当前赋值下具有负 slack，交给分析器 |
| learned constraint | 由原公式蕴含、回跳后可传播的 PB consequence |

一个约束可以在不同时间扮演多个角色，但它必须满足对应不变量。例如冲突约束当前具有负 slack，不能直接调用要求“当前不冲突”的 `attachConstraint()`。

### 9.5 标准 RoundingSat 冲突学习

标准链路：

```text
runPropagation()
  ↓ negative slack conflict
prepareConflictConstraint()
  ↓
沿 trail 从后向前
  ↓
getReasonConstraint(l)
  ↓
reason.resolveWith(conflict, l)
  ↓
直到 learned constraint 在较早层 asserting
  ↓
learnConstraint()/processLearnedStack()
  ↓
backjump + attach + propagation
```

关键函数：

- `roundingsat/Solver.cpp:296` `prepareConflictConstraint()`；
- `roundingsat/Solver.cpp:315` `getReasonConstraint()`；
- `roundingsat/Solver.cpp:346` `analyze()`；
- `roundingsat/ConstrExp.hpp:483-522` PB 通用消解；
- `roundingsat/Solver.cpp:517` `learnConstraint()`；
- `roundingsat/Solver.cpp:527` `processLearnedStack()`；
- `roundingsat/Solver.cpp:470` `attachConstraint()`。

### 9.6 PB generalized resolution 做了什么

这不是简单 CNF 1-UIP clause resolution。通用过程大致为：

```text
refine reason
  ├─ 删除 level-0 unit 和零项
  ├─ 可选 weakening
  ├─ saturation/位宽控制
  └─ divide-and-round

generalized resolution
  ├─ 取 reason 与 conflict 中待消文字系数的 gcd
  ├─ 分别乘适当倍数
  ├─ 相加并消去互补文字
  └─ 再 saturation
```

目标是在消去该 trail 文字后，resolvent 仍具有负 slack，最终形成 asserting constraint。

语义上必须区分：

- `postProcess()`：注释明确为 equivalence-preserving；
- conflict analysis 中的 weakening/divide-round：生成被原公式蕴含的较弱约束，适合“添加学习约束”，不等价于原约束。

因此不能把冲突学习中的 weakening 代码直接复制到根预处理器，用来替换原公式约束。

### 9.7 PBMC 主计数使用特殊学习路径

主路径真实调用链：

```text
count_main()
  → runPropagationWithoutLearnt()
  → analyzePBMC2()
  → heuristicWeakening + postProcess
  → ResolveWithoutLit()
  → 可选 bjResolve()
  → attachConstraint()
```

#### 主传播跳过 learnedStack

`runPropagationWithoutLearnt()` 是标准传播的复制版本，但故意删除了开始处的 `processLearnedStack()`。

因此简化器若只调用：

```cpp
learnConstraint(...);
```

结果只会进入 `learnedStack`，下一轮主计数传播不会自动处理它。必须：

- 显式调用兼容主计数边界的处理接口；或
- 在正确回跳层完成规范化后直接 attach；或
- 重构主传播，让 learned stack 与计数边界统一。

#### `analyzePBMC2()` 使用模拟回退

它复制 `Level→Sim_Level`、`Pos→sim_Pos`，在模拟状态中逆序扫描 trail、消解 reason 和寻找 assertion level；真实 trail 尚未立即改变。

#### 不是每次冲突都保存学习约束

`ResolveWithoutLit()` 只有在满足选择性回跳条件时才：

```text
bjResolve(target)
→ attach learned constraint
```

否则当前分支记 0，执行普通计数回溯，刚生成的学习约束不会 attach。默认还受到 `bj_threshold`、`limlevel` 和回跳次数限制。

因此 PBMC 主路径准确说是：

> 选择性非顺序回跳时保存学习约束，而不是每次冲突都执行持久学习。

### 9.8 `limlevel` 是计数组件边界

普通 CDCL 可以按 assertion level 回跳；PBMC 还必须避免跨过尚未正确组合计数的组件/分支边界。真实目标层受 `limlevel` 限制。

简化器发现冲突后不能自行裸调用 `backjumpTo(assertionLevel)`。应通过一个同时协调：

```text
RoundingSat trail
Component/Decision 栈
simplification undo
cache pollution
count-factor state
```

的回跳接口。

### 9.9 简化器怎样生成合法 reason

#### 情况 A：单条现有约束直接传播

若现有 `Constr` 的 slack 已满足传播条件，优先让现有 watch propagation 完成。在非根决策层，其 `CRef` 自然成为 reason；level 0 的赋值虽由该约束合法推出，`uncheckedEnqueue()` 仍会有意把 `Reason[v]` 清成 `CRef_Undef`。

#### 情况 B：多约束组合推出文字

必须构造一个被原公式蕴含、在当前状态下传播该文字的 PB constraint：

1. 用合法 PB resolution/cutting-plane 推导；
2. canonicalize、saturate、排序；
3. 设置 `Origin::LEARNED` 或明确的 derived origin；
4. attach；
5. 通过 `propagate(lit, cref)` 或 watch 初始化入 trail。

#### 情况 C：只在当前分支上下文成立

裸 residual constraint 未必是全局蕴含。必须：

- 把依赖的决策文字编码成 guard；或
- 只放在可回溯 local overlay；或
- 不做非顺序学习，只把当前分支计为 0 后顺序回溯。

#### 情况 D：简化发现冲突

最好返回一个当前赋值下负 slack、且由原公式蕴含的 `CeSuper`，交给统一分析器。只有 `bool conflict` 而没有证明时，第一阶段只能进行顺序分支失败，不能凭空跳过中间计数层。

### 9.10 `attachConstraint()` 的前置条件

待 attach 约束必须：

- 按系数降序；
- 已 saturation；
- 无零系数；
- 无 level-0 unit 项；
- 非空，即 `constraint->vars.size()>0`；
- 非 tautology；
- 当前状态下不具有负 slack；
- 有合法 `Origin`。

冲突约束当前具有负 slack，应先分析/回跳，不能直接 attach。

### 9.11 学习数据库与生命周期

`reduceDB()`：

- 不删除仍作为 `Reason` 的约束；
- 保留 external；
- 二项和低 LBD 约束优先保留；
- 其他学习约束按 LBD/activity 删除一部分；
- allocator 浪费超过阈值时执行 GC。

GC 会移动约束并重写 solver 自己保存的 `CRef`。新简化模块不能长期保存裸：

```text
CRef
Constr*
Constr&
```

除非它注册 GC remap 回调。更稳妥的是保存稳定逻辑 ID，再通过正式注册表解析当前 `CRef`。

如果 residual overlay 的正确性永久依赖某条约束存在，就不能把它当普通可淘汰 learned constraint。

### 9.12 当前冲突/传播代码的审计项

以下应在大改前用最小测试确认：

1. `Constr::isCorrectlyConflicting()`、`isCorrectlyPropagating()` 和若干 slack debug helper 开头直接 `return true`，现有断言没有实际重算传播正确性。
2. 主计数冲突只增加 `PBcounter::conflicts`，不增加 `rs::stats.NCONFL`；DB 清理阈值主要由 SAT 子求解冲突推动。
3. 主计数学习与选择性 backjump 绑定，关闭回跳后学习约束通常不保存。
4. `unitcls` 目前只找到读取路径，没有明确写入路径，属于遗留状态。
5. `checkpointSolverState()/resumeSolverCheckpoint()` 没有正常调用者，且恢复 slack 的循环为空，不能直接当作 probing/简化 checkpoint。
6. `Counter.cc:458` 计算的 `lim_backtrack_level` 未被使用，需要核对模拟 assertion level 与真实受限回跳层的一致性。

这些是审计项，不应和第一批简化规则混在一个提交里同时修复。

---

## 10. 结构简化、组件分解与 residual overlay

### 10.1 ComponentManager 使用冻结公式

`ComponentManager::init()` 在计数开始时把 solver 当前约束复制到：

```text
pbca_
constraints_
occlists_ / occ_pool_
```

`init()` 同时只按约束数调整 `gaps_` 的容量，并不在这里复制或计算实际 gap。每次 `searchComponent()` 才根据当前赋值重新计算活跃约束的 residual gap，经 gap saturation 后写入 `gaps_[c]`。

搜索期间新增 learned constraints 不会自动加入。只修改 solver allocator，也不会改变分量图。

### 10.2 学习约束不进入分量图为什么安全

全局 learned constraint 是冻结原公式的逻辑推论，不改变模型集合。组件分解按原始公式的独立因子结构计算仍然正确。

而且，把一条跨组件的冗余学习约束加入二部图，反而可能人为连接原来独立的组件，减少分解。因此：

```text
忽略纯 learned constraint：通常保留原公式分解，语义安全
忽略真正替换原约束的 rewrite：可能错误或错过新分解
```

两者不能混淆。

### 10.3 residual overlay 的职责

建议保留冻结原公式，并增加每层可回溯 overlay：

```text
Frozen Formula
   +
Decision-level Overlay
   =
Current Residual View
```

overlay 可以记录：

- 当前层失活的原始 `ClID`；
- 约束 transform version/fingerprint；
- 新增局部约束；
- 变量替换关系；
- 删除/新增的二部图边；
- 局部 count factor；
- undo marker。

`searchComponent()`、动态启发式和 cache key builder 必须消费同一个 residual view，不能各自维护一份近似状态。

### 10.4 topology changed 的定义

以下都可能改变分解拓扑：

- 推导赋值使约束满足；
- 删除/失活冗余约束；
- 删除某项使变量不再出现在约束；
- 替换变量；
- 添加非冗余的 replacement constraint；
- 变量消元产生 resolvent。

只产生赋值时，现有 `searchComponent()` 可通过 assignments/gap 重算反映变化。结构 rewrite 则必须同步 overlay/occurrence。

### 10.5 不能让 solver 与 ComponentManager 各看一个公式

若 solver 已删除约束，但 ComponentManager 仍保留：

- 计数语义可能仍正确但分解收益消失，或在 replacement 情况下变得不一致；
- cache key 仍编码旧 constraint ID/gap；
- 动态 frequency/启发式基于旧边。

若 ComponentManager 失活约束，但 solver 仍依赖它传播，语义可以安全，因为失活要求其他保留公式蕴含它；但必须确保缓存键代表的 residual model set 不依赖历史上“为什么判冗余”。

### 10.6 overlay 的确定性要求

若 active-mask 完全是当前 residual formula 的确定性函数，那么两个相同 residual 状态会得到相同 mask，可考虑不把规则执行历史加入 key。

若 mask 受以下因素影响：

- 时间预算；
- 规则调用顺序；
- 启发式选择；
- 随机数；

则相同 vars/gap 可能得到不同 residual 表示。要么：

1. key 仍以冻结原公式语义为准，不利用结构删除改变 key；
2. 把 canonical residual identity 纳入 key；
3. 强制 deterministic canonical fixpoint。

不要把执行历史本身随意塞入 key；应编码最终影响模型集合/组件计数的 residual identity。

---

## 11. 简化技术与组件缓存

### 11.1 当前缓存键

`PackedComponent` 位于 `core/Component.h:814`，主要编码：

1. 当前组件未赋值变量 ID 集合；
2. ComponentManager 冻结公式中的活动本地 `ClID` 集合；
3. 每条活动约束的 saturated residual gap。

它不包含：

- 学习约束；
- active-mask/transform version；
- 改写后的系数/文字；
- 变量替换映射；
- count factor；
- 启发式状态。

### 11.2 不同技术对现有 key 的影响

| 简化类型 | 现有 key 是否通常足够 | 说明 |
|---|---:|---|
| 只推导赋值 | 是 | 未赋值变量与 gap 反映结果 |
| 忽略已满足约束 | 是 | `searchComponent()` 已处理 |
| 添加冗余 learned constraint | 是 | 不改变原公式 model set |
| 证明冗余但仍保留图边 | 是但保守 | 正确，得不到新 split |
| active-mask 删除冗余边 | 需论证 | key/确定性 residual view 要一致 |
| 改系数/文字但复用同 ID | 否 | ID+gap 不再唯一确定约束 |
| 等价变量替换 | 否 | 变量集合和映射变化 |
| 变量消元 | 否 | 新约束、factor、重构变化 |
| 只改启发式 | 不应改 key | 不改变 residual model set |

### 11.3 cache value 应保存什么

推荐定义：

> 缓存值等于 key 所代表的“简化前组件”的完整计数。

这样局部 factor 在写缓存前合并一次，命中方不必了解内部用了哪些规则。

禁止：

```text
factor 乘进 branch models
同时写进 cache value
命中后又再乘一次
```

### 11.4 key 的升级路径

第一阶段：保留现有 key，只允许 assignment-only 简化。

第二阶段：混合 key：

```text
原始 ClID
+ transform version/signature
+ normalized gap
+ active variable fingerprint
```

第三阶段：完整 canonical residual key：

```text
canonical variable identities
+ sorted residual constraints
+ elimination/reconstruction/count-factor metadata
```

第三阶段命中潜力最大，但构造成本与正确性证明最难，应最后实现。

### 11.5 learned constraint 与 key

只要 learned constraint 是冻结原公式的逻辑推论，它不改变 residual model set，不必进入 key。它可以让某条路径更快传播，但相同 key 的真实计数不变。

如果一条“derived constraint”是 replacement formula 的组成部分，而不是冗余 consequence，它就不再只是 learned constraint，必须进入 residual identity。

---

## 12. 无权精确 MC 的计数与重构 ledger

### 12.1 正式计数对象

对全部变量集合 `V`：

```text
MC(F) = |{ α_V : F(α_V)=true }|
```

每个完整满足赋值恰好计一次。变量变换若不是逐赋值等价，就必须证明模型双射或给出精确的扩展计数函数。

### 12.2 固定变量与自由变量

- 被公式蕴含而固定的变量只有一个合法取值，因子为 1；
- 一个全局自由布尔变量有两个合法取值，因子为 2；
- `k` 个相互独立的全局自由布尔变量产生 `2^k`；
- factor 必须属于 root、component 或 branch 中唯一一个所有者。

### 12.3 等价/互补变量替换

`y=x` 或 `y=~x` 若在所有满足模型中成立，则删除 `y` 不产生倍数，因为剩余模型能唯一恢复 `y`。必须记录代表变量、极性关系和原编号，供模型输出、后续结构规则和缓存身份使用。

### 12.4 一般消元需要扩展计数函数

对剩余赋值 `z`：

```text
ext(z) = |{ 被消变量赋值 e : F(z,e)=true }|
```

只有 `ext(z)` 对所有相关 `z` 都是同一个常数时，才能用一个标量 factor。普通 SAT 变量消元只保存 `ext(z)>0`，会把 1 个扩展与 2 个扩展混为一谈，因此不能直接用于精确 MC。

### 12.5 推荐 ledger 内容

```cpp
struct ReconstructionLedger {
    std::vector<FixedLiteralRecord> fixed;
    std::vector<EquivalenceRecord> equivalences;
    std::vector<EliminationRecord> eliminations;
    std::vector<FreeVariableRecord> free_variables;
    CountFactor global_factor;
};
```

每条记录至少说明：

```text
原变量
当前代表变量或唯一定义
正负极性关系
原模型怎样恢复
计数因子属于 root/component/branch 中哪一层
计数因子何时进入 cache value
```

---

## 13. 简化结果怎样进入 VCIS 和新启发式

### 13.1 静态特征必须基于最终公式

根预处理完成后再计算：

- VCIS；
- TD primal graph；
- phase；
- 预处理器产生的新特征。

否则已删除约束、消除变量和旧编号仍会污染分数。

### 13.2 递归简化可以提供动态特征

适合的动态特征：

- 某变量赋值后预计触发的 forced literals；
- 预计满足约束数量；
- 预计删除二部图边数量；
- 预计新增组件数；
- probing 产生冲突/公共蕴含的次数；
- 当前 gap 下正负极性的 impact；
- 规则命中历史。

这些特征只能影响分支顺序，不能改变模型集合。

### 13.3 启发式状态不进入缓存键

TD、VCIS、activity、phase、rule score 和随机 tie-break 都不改变 residual formula 的真实计数，因此不应进入 key。

只有简化最终造成的 formula identity、变量映射或 count-factor 状态才可能进入 key。

### 13.4 负文字和 equality 的评分口径

新启发式要明确：

- `x` 与 `~x` 是同一变量的两个极性，不是两个变量；
- canonicalization 后按净系数评分；
- equality 按一条原始对象还是两个展开不等式评分；
- phase 分数是否分别计算正负极性；
- fixed/eliminated 变量是否已从候选中删除。

---

## 14. 推荐的软件边界与规则接口

### 14.1 一套规则、两个 adapter

```text
PB Rule Engine
├─ StaticFormulaAdapter
│    根预处理：永久改写、重建映射
└─ ResidualComponentAdapter
     递归节点：可回溯 delta、reason、overlay
```

同一数学规则应共享：

- 前置条件；
- 变换公式；
- 语义分类；
- 证明/检查器；
- 统计名称。

但不能共享底层修改动作。例如 duplicate 删除：

- 根阶段可永久 erase 并重建 occurrence；
- 递归阶段只在 active-mask 中失活并记录 undo。

### 14.2 规则接口建议

```cpp
struct RuleCapabilities {
    bool supports_ge;
    bool supports_equality;
    bool supports_negative_literals;
    bool supports_mc;
    bool needs_reason;
    bool needs_undo;
    bool changes_topology;
    bool changes_cache_identity;
};

struct RuleApplication {
    TerminalStatus terminal;
    bool changed;
    ProofOrReason proof;
    FormulaDelta formula_delta;
    MappingDelta mapping_delta;
    CountEffect count_effect;
    RuleStatsDelta stats;
};
```

规则函数不应直接访问全局 `PBcounter` 的所有成员。它应消费只读 context，返回可验证 delta，由 adapter 统一 commit。

### 14.3 建议目录

```text
core/
├─ pb/
│  ├─ Formula.h
│  ├─ Constraint.h
│  ├─ Literal.h
│  └─ Normalization.{h,cc}
├─ simplify/
│  ├─ Rule.h
│  ├─ RuleEngine.{h,cc}
│  ├─ SimplifyResult.h
│  ├─ StaticFormulaAdapter.{h,cc}
│  ├─ ResidualComponentAdapter.{h,cc}
│  ├─ UndoTrail.{h,cc}
│  └─ rules/
│     ├─ ForcedLiteral.{h,cc}
│     ├─ Canonicalize.{h,cc}
│     ├─ Duplicate.{h,cc}
│     ├─ Dominance.{h,cc}
│     └─ Probe.{h,cc}
├─ heuristic/
│  ├─ Features.h
│  └─ BranchHeuristic.{h,cc}
└─ cache/
   ├─ ComponentKey.{h,cc}
   └─ CachePolicy.{h,cc}
```

不必现在一次性移动所有文件。先在 `core/` 中建立接口并保持行为不变，测试稳定后再做机械移动。

### 14.4 规则检查器

建议 Debug 构建提供一个小约束 truth-table checker：

```text
变量数 <= 15 时枚举当前规则涉及的局部变量
比较 rewrite 前后每个完整赋值的约束值/真假
检查声明的模型双射或计数 factor
```

对“添加蕴含约束”检查：

```text
F_local ∧ ¬C_derived
```

在小支持集上是否 UNSAT。该检查器用于开发验证，不必在正式大实例中一直开启。

### 14.5 统一回跳协调器

长期建议避免业务代码直接裸调用 `backjumpTo()`，而是提供：

```cpp
void PBcounter::restoreSearchStateTo(int level) {
    rs::Solver::backjumpTo(level);
    recursive_simplifier.undoToLevel(level);
    cmpmgr.backjumpTo(level);
    cmpmgr.removeCachePollutions();
    count_factor_state.undoToLevel(level);
}
```

实际调用顺序需根据对象依赖测试确定，但必须只有一个受控入口，防止某条异常/子求解路径漏恢复。

---

## 15. 把外部或 GPT 整理的技术迁移到 PBMC：固定评审模板

下面模板应为每一项技术单独填写。不要把十几项技术混在一个表述里，否则无法定位计数错误。

### 15.1 完整模板

```text
技术编号与名称：
论文/来源：
原始规则的数学形式：
规则输入是一条约束、两条约束、一个变量，还是整个组件：

一、前置条件
- 支持 >= 吗：
- 支持 = 吗：
- 支持负文字吗：
- 支持负系数吗，还是要求先正系数化：
- 是否要求同变量项已经聚合：
- 是否要求系数降序/已 saturation/GCD：
- 是否引入辅助变量；若引入，属于定义扩展还是原变量替换：
- 输入等式是否已经展开成两个方向；两方向 provenance 怎样关联：
- 是否只对根层成立：
- 是否依赖当前部分赋值：
- 是否依赖规则执行顺序；是否要求 deterministic fixpoint：

二、语义证明
- 属于 SAFE_CANONICAL / SAFE_WITH_REASON / SAFE_WITH_LEDGER / SAT_ONLY / HEURISTIC_ONLY 中哪类：
- 是否逐完整赋值保持公式真假：
- 若不是，是否有模型双射：
- 若有计数因子，写出 Count(F)=...?：
- factor 是否依赖剩余赋值：
- 新约束是否由原公式逻辑蕴含：
- 若引入定义变量，为什么它不是可任意赋值的新模型维度：

三、无权精确 MC
- MC 是否安全，证明：
- 是否逐赋值等价、模型双射，或带常数计数因子：
- 若消除变量，剩余赋值的扩展数量怎样保留：
- 原模型恢复需要什么记录：

四、PBMC 根预处理落点
- 应放在 canonicalization 的哪一步之后：
- 修改 PBconstraint 还是新 Formula IR：
- 是否永久删除/添加约束：
- old_to_new/new_to_old 怎样更新：
- VCIS/TD 是否要重算：

五、PBMC 递归落点
- 是否放在 propagation↔simplification fixpoint：
- terminal 是 Continue / Conflict / Solved 中哪一种：
- 同时返回哪些 changed/implied/entailed/delta/factor/topology 字段；factor scope 是什么：
- 是否产生 implied literal：
- reason 是哪条约束或如何构造：
- 冲突是否能返回 CeSuper：
- 是否修改 active-mask/overlay：
- undo delta 是什么：
- probing/SAT 子求解预算是什么：

六、分解与缓存
- 是否改变变量—约束拓扑：
- ComponentManager 怎样看到变化：
- 当前 vars+ClID+gap key 是否仍足够：
- 需要什么 transform signature：
- count factor 在写 cache 前还是命中后应用：

七、数值和复杂度
- 使用什么整数类型：
- 加法/乘法/取负是否可能溢出：
- 最坏时间和空间复杂度：
- 系数/约束长度是否可能爆炸：
- 节点预算和停止条件：

八、最小测试
- 负文字测试：
- 等式测试：
- 冲突/恒真/强制文字测试：
- 精确 MC 预期答案：
- 第一分支→第二分支 undo 测试：
- 非顺序回跳测试：
- cache hit/miss 一致性测试：
- brute-force oracle 测试：

最终评审结论：
- 可直接实现 / 需 ledger / 只能 SAT 子过程使用 / 暂不采用
- 推荐阶段：
- 仍未证明的问题：
```

### 15.2 给另一个 GPT 的推荐提示词

可以把本文和技术清单一起交给 GPT，并使用下面的指令：

```text
请依据《PBMC 简化技术落地与冲突学习指南》逐条评审我提供的简化技术。

要求：
1. 每项技术单独填写第 15.1 节模板；
2. 必须先给数学语义证明，再给代码改动；
3. 明确区分 >= 与 =、负文字与负系数；
4. 必须证明无权精确 MC 安全，不能只证明 SAT；
5. 指出 reason、trail、回跳、ComponentManager、cache key、模型映射和计数因子影响；
6. 若规则只能保持可满足性，明确标记 SAT_ONLY，不得建议直接加入主计数公式；
7. 所有源码位置以函数名为主，并与当前仓库代码核对；
8. 最后给出最小 OPB 测试、预期精确计数和实施阶段；
9. 不要一次提交所有技术，按低风险到高风险分批实现。
```

### 15.3 GPT 方案的拒绝条件

出现以下任一情况，应退回要求补充：

- 只说“保持等价”但没有说明是哪种等价；
- 没有区分 `~x` 与负系数；
- 把等式直接套用不等式 saturation；
- 推导文字使用 `decide()` 或只修改 `assigns`；
- 结构改写没有 undo；
- 变量消元没有逐剩余赋值的扩展计数证明；
- 删除约束没有说明 ComponentManager 和 cache key；
- 冲突只有布尔值却建议非顺序回跳；
- 没有暴力枚举 oracle；
- 依赖长期保存裸 `CRef`/指针却没有 GC remap；
- 同一 factor 可能在 branch、cache、global 三处重复应用。

---

## 16. 测试与验证体系

### 16.1 暴力枚举 oracle 是最高优先级

为 5～15 个变量的 OPB 枚举所有完整赋值，独立计算：

- 每个完整赋值是否满足全部 PB 约束；
- 所有满足赋值的整数总数 MC。

不要用 PBMC 自己作为唯一 oracle。对每个随机实例运行：

```text
规则全部关闭
只根预处理
只递归简化
两者都开
cache 开/关或小容量压力模式
backjump 开/关
TD/VCIS 开/关
```

PBMC 的无权 MC 结果使用任意精度整数，必须与 oracle 完全相等，不能使用浮点容差。

### 16.2 canonicalization 最小测试

至少覆盖：

```opb
* 负系数翻转
-3 x1 +2 x2 >= 1 ;

* 负文字上的负系数
-3 ~x1 +2 x2 >= 1 ;

* 同极性重复
+2 x1 +5 x1 >= 4 ;

* 互补项 A>B
+7 x1 +3 ~x1 +2 x2 >= 6 ;

* 互补项 A=B 后变量消失
+4 x1 +4 ~x1 >= 4 ;

* 互补项消失后冲突
+4 x1 +4 ~x1 >= 5 ;

* equality 互补项
+7 x1 +3 ~x1 = 5 ;
```

对每条约束枚举所有局部赋值。负系数正化、重复项聚合、`x/~x` 互补聚合等代数恒等变换，应比较 rewrite 前后的 `LHS-rhs` 以及约束真假；原始 LHS 本身可能因常数被同步移到 RHS 而不同。对 saturation、GCD 除法等只保证模型集合的变换，只比较每个赋值下的真假/模型集合，不要求 raw LHS 或 slack 数值相等。

### 16.3 equality 测试

- `x1+x2=1`，模型数 2；
- `2x1=1`，UNSAT；
- `2x1+4x2=3`，GCD 不整除，UNSAT；
- `2x1+4x2=4`，可精确约分；
- `x+~x=1`，canonical 后恒真；
- `x+~x=0`，冲突；
- equality 两个内部方向都能产生传播/冲突。

### 16.4 reason 与冲突学习测试

- `decisionLevel()>0` 的简化推导文字具有非空且合法的 `Reason[v]`；
- 非根 reason 在当前状态下确实传播该文字；
- level 0 推导允许 `Reason[v]==CRef_Undef`，应改查根单位推导、赋值和 proof log（若启用）是否正确；
- 非根推导文字参与后续冲突时，`analyzePBMC2()` 不访问空 reason；
- 学习约束 canonical、saturated、无零项；
- 接受回跳时 attach，拒绝回跳时不泄漏临时状态；
- `limlevel` 以上的组件计数不被跨越；
- DB reduction 不删除当前 reason；
- GC 后简化模块没有悬空 CRef/指针。

### 16.5 联合不动点和回溯测试

构造：

```text
决策 d
→ RoundingSat 推导 a
→ 简化规则推导 b
→ b 再触发 RoundingSat 推导 c
```

验证：

- 分解前 `a,b,c` 全部进入 trail；
- 该场景在非根层运行，`b` 有合法 reason；
- 简化发现的 count factor 只应用一次；
- 回跳撤销 b/c、overlay 和 factor marker；
- 第二分支不继承第一分支的 factor/overlay。

### 16.6 overlay、分解与缓存测试

- 简化前一个组件，冗余约束失活后分成两个组件；
- 第一分支失活的边在第二分支恢复；
- 相同 residual formula 经不同历史路径得到相同 key；
- 不同 transform signature 不能误命中；
- cache value 已包含局部 factor，命中后不重复乘；
- learned constraint 的有无不改变 key 和答案；
- cache 小容量淘汰、回跳污染清理与 overlay 同时运行。

### 16.7 数值测试

- 接近 `LONG_MAX` 的系数和 RHS；
- 聚合加法溢出；
- `LONG_MIN` 取负；
- GCD/ceil division 的负 RHS；
- RoundingSat 任意精度可以处理但 PBInstance 不能处理的输入；
- 不得使用 `double` 判断整数可除性或等号。

### 16.8 Sanitizer 与 Debug 重算

建议：

```text
ASan：use-after-free、allocator mismatch、CRef/overlay 生命周期
UBSan：有符号溢出、移位、窄化转换
Debug slack recompute：绕过当前直接 return true 的 helper
```

随机小实例应在 sanitizer 下跑所有规则组合和深回跳。

### 16.9 性能统计

每条规则输出：

```text
calls / success / skipped_by_budget
implied literals / conflicts
removed constraints / terms / graph edges
new component splits
reason construction time
rule time / total inprocessing time
pre/post vars, constraints, nonzeros
cache hits/misses/key time
decisions/conflicts/propagations
```

正确性通过前不要只汇报“总运行时间变快”。

---

## 17. 推荐实施顺序

### 阶段 0：锁定基线

1. 完成无权精确 MC 暴力枚举 oracle；
2. 建立负文字、等式、自由/固定变量和分解小实例；
3. 开启 ASan/UBSan；
4. 记录当前求解答案与主要统计。

### 阶段 1：前端 canonicalization

1. 零项；
2. 负系数；
3. 同极性重复；
4. `x/~x` 合并；
5. 空/恒真/冲突；
6. 确定性排序；
7. 用整数重写 unit/probing 判断；
8. 修复二项同变量越界。

这是所有后续技术的共同前置条件。

### 阶段 2：递归简化空框架

1. 新 `SimplifyResult`；
2. 传播—简化内部不动点；
3. count-factor marker；
4. undo marker；
5. 普通回溯和非顺序回跳恢复；
6. 默认规则集为空，验证行为不变。

### 阶段 3：assignment-only 规则

1. residual 恒真/冲突检测；
2. 比现有传播更强的 forced literal；
3. 合法 reason；
4. failed literal/probing 预算；
5. 不修改 ComponentManager 结构和 cache key。

### 阶段 4：冗余约束与分解 overlay

1. exact duplicate；
2. 相同 LHS dominance；
3. 有证明的 subsumption；
4. active-mask；
5. `searchComponent()` 消费 overlay；
6. deterministic residual/key 论证。

### 阶段 5：形成新根预处理器

1. 抽取纯规则引擎；
2. `StaticFormulaAdapter`；
3. 任意精度 Formula IR；
4. mapping/reconstruction/count ledger；
5. 最终公式上重算 VCIS/TD；
6. 规则独立开关与消融统计。

### 阶段 6：结构变换和缓存升级

1. 等价/互补变量；
2. 定义变量；
3. 有逐赋值扩展计数证明的变量消元；
4. transform signature/canonical residual key；
5. 原模型重构；
6. 最后才考虑一般递归变量消元。

任何阶段一旦出现简化开关改变精确答案，应停止性能实验，先用最小反例和 oracle 定位。

---

## 18. 源码定位表

### 18.1 输入与根预处理

| 文件/函数 | 作用 |
|---|---|
| `core/pbconstraint.h:19` | `PBconstraint`、当前 `Int=long` |
| `core/Instance.cc:420` | OPB 轻量解析 |
| `core/Instance.cc:529-560` | `=`、`xN`、`~xN` 解析 |
| `core/Instance.cc:790` | 重输出 OPB、追加 inferred units |
| `core/Instance.cc:903` | VCIS 与初始 phase |
| `core/Instance.cc:1016` | 负系数正系数化 |
| `core/Instance.cc:1051` | 当前 PB 根预处理循环 |
| `core/Instance.cc:1150` | unit 推断，当前使用 double |
| `core/Instance.cc:1235` | probing 代入 |
| `core/Instance.cc:1258` | probing unit 推断 |
| `core/Instance.cc:1314` | 二项 probing |
| `core/Main.cc:221-247` | 根流程总编排 |

### 18.2 RoundingSat 约束正规化

| 文件/函数 | 作用 |
|---|---|
| `roundingsat/parsing.cpp:52` | 最终 OPB parser |
| `roundingsat/parsing.cpp:104-116` | equality 展开 |
| `roundingsat/ConstrExp.cpp:332` | 每变量系数槽、互补合并 |
| `roundingsat/ConstrExp.cpp:402` | remove units/zeroes |
| `roundingsat/ConstrExp.cpp:461` | saturation |
| `roundingsat/ConstrExp.cpp:493` | invert inequality |
| `roundingsat/ConstrExp.cpp:574` | exact divide/GCD RHS ceil |
| `roundingsat/ConstrExp.cpp:655` | divideByGCD |
| `roundingsat/ConstrExp.cpp:670` | equivalence-preserving postProcess |
| `roundingsat/ConstrExp.cpp:800` | cardinality detection |

### 18.3 传播、冲突和学习

| 文件/函数 | 作用 |
|---|---|
| `roundingsat/Solver.hpp` | trail、Level、Pos、Reason、adj、learnedStack |
| `roundingsat/Solver.cpp:136-189` | enqueue、propagate、撤销、decide |
| `roundingsat/Solver.cpp:195` | 标准 watch propagation |
| `roundingsat/Solver.cpp:296` | prepare conflict |
| `roundingsat/Solver.cpp:315` | get reason |
| `roundingsat/Solver.cpp:346` | 标准 PB analyze |
| `roundingsat/ConstrExp.hpp:483-522` | PB generalized resolution |
| `roundingsat/Solver.cpp:470` | attach constraint |
| `roundingsat/Solver.cpp:517` | push learnedStack |
| `roundingsat/Solver.cpp:527` | process learnedStack |
| `roundingsat/Solver.cpp:686` | reduceDB / GC |
| `roundingsat/Constr.cpp:285-355` | general PB slack propagation |
| `roundingsat/Constr.cpp:821-898` | 当前被短路的 debug checker |

### 18.4 PBMC 主状态机

| 文件/函数 | 作用 |
|---|---|
| `core/Counter.cc:152` | 当前空 `simplify()` |
| `core/Counter.cc:327` | 主计数传播，不处理 learnedStack |
| `core/Counter.cc:394` | `count_main()` |
| `core/Counter.cc:441` | `analyzePBMC2()` 调用 |
| `core/Counter.cc:468-469` | 推荐简化入口附近、splitComponent 前 |
| `core/Counter.cc:521` | 旧且过晚的空简化调用 |
| `core/Counter.cc:563` | 模拟状态 PB 冲突分析 |
| `core/Counter.cc:642` | solver/component/cache 协调回跳 |
| `core/Counter.cc:681` | 选择性 attach learned constraint |
| `core/Counter.cc:710` | 两分支计数组合和普通回溯 |

### 18.5 分量与缓存

| 文件/函数 | 作用 |
|---|---|
| `core/ComponentManager.cc:84` | 冻结复制 solver 初始约束 |
| `core/ComponentManager.cc:178` | split + cache query |
| `core/ComponentManager.cc:278` | residual graph、gap、frequency |
| `core/ComponentManager.cc:347` | gap saturation |
| `core/Component.h:814` | packed component key |
| `core/ComponentCache.cc:157` | 临时缓存项创建 |
| `core/ComponentCache.cc:193` | cache lookup |
| `core/ComponentCache.cc:220` | cache eviction |

---

## 19. 开始实现某项技术前的最终检查表

### 数学与输入

- [ ] 已明确 `>=` 与 `=` 的差异；
- [ ] 已明确负文字与负系数；
- [ ] 输入已满足 canonicalization 后置不变量；
- [ ] 变换使用精确整数，无 `double` 等号判断；
- [ ] 给出了模型集合等价、模型双射或计数因子证明；

### 计数语义

- [ ] 已证明无权精确 MC 安全；
- [ ] 固定/自由变量因子只应用一次；
- [ ] 等价替换/定义消元的原模型映射可恢复；

### 递归状态

- [ ] 插入传播—简化联合不动点；
- [ ] 非根 implied literal 有合法 reason；level 0 推导按根单位赋值/证明协议验证；
- [ ] conflict 有 CeSuper 或只做顺序回溯；
- [ ] count factor 不漏记、不重复；
- [ ] 第一/第二分支和非顺序回跳都恢复；

### 分解与缓存

- [ ] ComponentManager 能看到所需结构变化；
- [ ] learned consequence 与 replacement rewrite 已区分；
- [ ] 当前 key 是否仍唯一表示 residual model set 已证明；
- [ ] transform signature/overlay 是否确定性；
- [ ] cache value 包含正确 factor；

### 生命周期与测试

- [ ] 不长期保存会被 GC 移动的 CRef/指针；
- [ ] 设置了时间/节点/系数增长预算；
- [ ] 有最小负文字和 equality 测试；
- [ ] 有无权精确 MC 的 brute-force oracle；
- [ ] 有回跳、cache、ASan/UBSan 压力测试；
- [ ] 规则可单独开关并输出统计。

---

## 20. 最适合作为第一项真实改动的技术

从当前代码状态看，第一项不应是变量消元或复杂 dominance，而应是：

> 在 `PBInstance` 的正系数化之后、现有 simplify/probing 之前增加统一的 constraint canonicalization：合并同极性重复项、合并 `x/~x`、删除零项、处理空约束，并用精确整数重写 unit/probing 判断。

原因：

1. 它是逐赋值等价，对无权精确 MC 安全；
2. 修复当前二项 probing 的隐含越界；
3. 为所有后续简化规则建立稳定不变量；
4. 让 VCIS 使用净系数而不是重复项；
5. 与 RoundingSat `addLhs()` 的后端语义一致；
6. 容易用局部真值表和暴力枚举完整验证。

第二项才是“空的递归简化框架 + 联合不动点 + undo + count-factor marker”，仍不启用真实规则。第三项加入一条能够生成合法 reason 的 forced-literal 规则。

最重要的原则是：

> 先证明一条规则怎样保持精确计数，再决定它怎样提高性能；先建立 reason、回退、计数记账和缓存协议，再增加规则强度。

PBMC 中最危险的错误不是立即崩溃，而是在大数据上安静地返回一个看似合理、实际错误的精确计数。
