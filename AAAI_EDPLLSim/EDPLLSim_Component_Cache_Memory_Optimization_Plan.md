# EDPLLSim Component Cache 内存优化实施方案（代码与论文校订版）

> 目标：在不改变 EDPLLSim 精确计数语义的前提下，降低 component cache 的总内存和单项开销，并用 exact count、缓存统计和真实 RSS 验证收益。
>
> 本文档用于后续逐阶段修改代码。每个 Phase 都必须单独提交、单独回归；不要把 PR、key 语义、物理编码和淘汰策略一次性混在一个改动中。
>
> **最终实现状态（2026-09-28）**：求解器只保留一套正式 component cache。
> 原 `canonical` 的逻辑规范化与原 `packed` 的无损连续字节编码已经合并为内部步骤，
> 不再作为独立模式、容器或命令行参数；最终 key 使用共享行模板。
> 默认 `age_limit=51,200`，派生 `clean_limit=20*age_limit=1,024,000`，达到上限后
> 沿 oldest 链保留最近约 1/4。内部字节硬上限、target ratio、估算字节统计及对应
> 清理分支均已删除，正式内存结果使用操作系统报告的 Peak RSS。后文 Phase 0～6
> 保留为研发过程和既有对照实验记录，以本状态为准。最终版本不再提供
> `off/basic/full` 统计级别，固定输出精简后的真实计数；规范化时发现的矛盾仍直接返回
> 0，但不再单列 conflict 统计，也不计入实际 cache lookups。未保留输出的 probe、
> collision、exact-compare、lazy-cleanup 和模板回收等纯诊断计数也已从正式代码删除；
> 插入序号、当前 entry 数和模板 refcount 属于淘汰算法状态，仍必须保留。

最终实现的准确数据流是：

```text
current component -> ComponentNormalizer -> NormalizedComponent
-> 逐行 SerializeRowTemplate -> RowTemplatePool 分配/查找 TemplateID
-> ComponentKeyBuilder 把 domains + (TemplateID, RHS) 序列化为 SerializedComponentKey
-> ComponentCache
```

因此不是“先序列化完整 component，再做模板复用”；模板池之前序列化的只是每行左侧系数
结构，拿到 TemplateID 后才进行最终 component key 的连续字节序列化。

---

# 0. 校订后的核心结论

这几条是后续实现时不能偏离的设计结论。

1. **EDPLLSim 当前内部约束统一是 `<=`。**
   - PR 和 cache row GCD 规约只实现 `floor(rhs / gcd)`。
   - 不需要为 cache row 引入 relation tag，也不需要 `ceil_div`。
   - 但 `boundStrengtheningSingle()` 中负系数推导变量下界所用的 `mpz_cdiv_q`，以及 LP 求下界时的 `ceil`，仍然是必要的；“所有约束是 `<=`”不等于“项目中任何地方都不需要 ceil”。

2. **初始 raw cache key 本身是 exact 的，主要问题是重和无界式增长，不是 SC 导致了错误命中。**
   - 初始 key 保存完整 `ranges + matrix`，SC 修改后的系数已经包含在 key 中；该 raw
     legacy 实现现已在最终 exact-count 对照后删除。
   - 只有把完整 matrix 替换成 ID 后，SC/删变量产生的当前系数结构如何获得稳定身份才成为必须解决的问题。

3. **最终实现把规范化、字节编码和行母版池合并为一条路径。**
   - 原计划同时维护 `SolverTemplatePool + CacheCanonicalTemplatePool` 两套池，侵入性高，也容易重复保存结构。
   - 先短暂实现“自包含精确稀疏键”，用于确认 GCD、SC、row 排序和序列化完全正确；它是过渡版和对照组，不是放弃母版方案。
   - 最终实现一个 `RowTemplatePool`（缓存行母版池）：相同 coefficient structure 只保存一次，component key 只保存 `TemplateID + RHS`。
   - 原始 row 和 SC 派生 row 都按当前真实 coefficient structure 进入同一个池，不需要两套模板池。

4. **默认保持当前 key 的变量重命名语义。**
   - 当前 key 不包含 `PID`；只要 `ranges` 和完整 matrix 相同，即使来自不同原始变量，也会命中。这对计数是安全的，因为它们是同一个局部整数线性系统。
   - 第一版 canonical key 使用 component-local column index，不强行加入 global variable ID，避免比旧 key 更严格而损失命中。
   - `PID/mPID` 当前还受 `Heuristic::global_state_enabled` 控制，不能直接作为所有 heuristic 下的 cache 正确性依赖。

5. **最终缓存淘汰策略采用 Cachet 的 insertion-age + lazy cleanup。**
   - entry 插入时记录插入序号，按“此后又插入了多少 entry”判断是否过期；cache hit 不刷新年龄。
   - 平时只懒清理当前访问/插入的 bucket chain，避免每次操作扫描整张表。
   - 默认 `age_limit=51,200`，`clean_limit=20*age_limit=1,024,000`；达到
     clean limit 时沿全局 oldest 链直接淘汰，保留最近约 1/4。
   - bucket 内也维护反向链接，因此全局批量淘汰无需扫描所有 bucket，也无需在 bucket
     内重新查找待删节点；模板引用整批释放后统一回收零引用模板。
   - 原 canonical 整表清空策略仅保留在历史实验结果中，最终代码已经删除。

6. **最终只保留 `N*M` 准入限制和 entry-age 管理两个层次。**
   - `-cache-limit` 当前只是 admission threshold：只有 `N*M <= limit` 的状态才尝试缓存。
   - 默认正式策略按 Cachet exact 模式的 entry 数触发全局清理；它不是每项大小的估计。
   - component cache 不再维护内部字节预算；每个测例的 10 GiB 总内存限制由实验脚本
     在进程外部控制，真实内存结果记录 Peak RSS。

## 0.1 常用中英对照与计量单位

后文保留英文主要是为了能直接对应代码标识符。中文含义和计量单位如下。

| 英文/代码名 | 中文 | 它表示什么 | 计量单位 |
| --- | --- | --- | --- |
| component | 分量/子问题 | 当前递归中相互连通的一组变量与约束 | 个 |
| cache entry | 缓存项 | 一个 component key 及其精确计数结果 | 项 |
| key | 缓存键 | 判断两个 component 是否相同的数据 | 字节，或 1 个逻辑对象 |
| template / master row | 行母版 | 一种规范化后的 coefficient structure | 个 |
| `TemplateID` | 母版编号 | 行母版池内的无符号编号 | 编号，无物理单位 |
| RHS | 右端常数 | `a·x <= b` 中的 `b` | 任意精度整数 |
| exact | 精确 | 比较完整内容，不接受 hash 碰撞导致误命中 | 布尔性质 |
| sparse | 稀疏 | 只记录非零 coefficient，不存 matrix 中的零 | `nnz` 个非零项 |
| self-contained | 自包含 | 不查母版池，仅凭 key 自身就能还原当前数学状态 | 布尔性质 |
| component-local variable | 分量局部变量 | 在当前 component 内按列编号 `0..N-1` 的变量 | 编号，无物理单位 |
| arbitrary-precision `Int` | 任意精度整数 | 不限制在 32/64 bit 内的 GMP 整数 | 数值；存储量按字节 |
| lossless byte encoding | 无损字节编码 | Int 编码后可精确恢复，不能用 `double` 或截断 | 字节 |
| admission threshold | 准入阈值 | 单个 component 是否值得进入 cache 的过滤条件 | 当前为 `N*M` 个矩阵格 |
| byte memory limit | 字节容量上限（历史方案，最终已删除） | 过去用于限制 cache 的估算预算 | byte/MB/GiB |
| insertion age | 插入年龄 | 某 entry 插入后又发生了多少次新插入 | 次插入 |
| touch age | 访问年龄 | 某 entry 自上次命中/访问后经历了多少次操作；本方案不实现 | 次操作 |
| renew on hit | 命中续龄 | hit 时把 entry 的访问年龄重置；本方案不实现 | 次命中 |
| lazy cleanup | 懒清理 | 访问某个 bucket 时顺便清掉其中过期 entry | 次/项 |
| hit rate | 命中率 | `hits / (hits + misses)`；conflict 不进入哈希表 | 比例或百分比 |

三个容易误解的长名称：

1. **self-contained exact sparse key（自包含精确稀疏键）**
   - “自包含”：每个 key 自己保存 domains、非零 coefficients 和 RHS，不依赖母版池；
   - “精确”：hash 相同后仍比较全部内容；
   - “稀疏”：一行只存 `(局部变量编号, 非零系数)`；
   - 主要量：`N` 个变量、`M` 条 row、`nnz` 个非零 coefficient、`key_bytes` 字节。

2. **component-local variable identity（分量局部变量身份）**
   - 当前 component 的第 0、1、2…列就是局部变量编号；
   - 它不等于原始输入中的 `PID`；
   - 编号本身没有物理单位，数量是 `N` 个变量。

3. **lossless encoding of arbitrary-precision Int（任意精度 Int 的无损编码）**
   - 每个 Int 编成“符号 + 长度 + 绝对值字节”；
   - 无论超过 64 bit 多少都不截断；
   - `magnitude_byte_count` 的单位是 byte，整个 key 用 `key_bytes` 计量。

---

# 1. 已核对的论文与参考实现

## 1.1 Cachet

参考：

- [`cache papers/cachet.pdf`](<cache papers/cachet.pdf>)
- [`cache solvers/Cachet/hash.cpp`](<cache solvers/Cachet/hash.cpp>)
- [`cache solvers/Cachet/hash.h`](<cache solvers/Cachet/hash.h>)

可直接借鉴的点：

- separate chaining hash table；
- 每个 component 带 sequence number；
- 年老 entry 的复用价值通常下降；
- 访问/插入相关 chain 时清理旧 entry；
- 容量压力下允许低频的全局批量清理。

需要准确表述的点：

- Cachet 的 `sequence_number` 在插入时设置；
- `in_hash_table()` 命中时没有刷新 sequence；
- chain 按新 entry 插在表头，因此发现第一个过期 entry 后可以删除其后的旧后缀；
- `clean_cache()` 会扫描整张表；它不是原计划所写的“永不全表扫描的 incremental sweep”。

因此本项目直接采用 Cachet 的 insertion-age，不再增加 touch-age/renew 分支。

## 1.2 SharpSAT

参考：

- [`cache solvers/SharpSAT/code/sharpsat/src/component_types/difference_packed_component.h`](<cache solvers/SharpSAT/code/sharpsat/src/component_types/difference_packed_component.h>)
- [`cache solvers/SharpSAT/code/sharpsat/src/component_cache.cpp`](<cache solvers/SharpSAT/code/sharpsat/src/component_cache.cpp>)
- [`cache solvers/SharpSAT/code/sharpsat/src/statistics.h`](<cache solvers/SharpSAT/code/sharpsat/src/statistics.h>)

可直接借鉴的点：

- component 由有序 variable IDs 和 clause IDs 表示；
- 相邻 ID 使用 difference encoding；
- 差值使用按该 component 最大差值确定的 bit width 打包；
- hash 只是定位 bucket，命中仍比较完整 packed data；
- cache 内存统计区分基础设施、packed component 和大整数 model count；
- 达到字节上限后按创建时间批量删除较老的可删 entry，而不是整表 `clear()`。

注意：SharpSAT 参考实现是 bit packing，不是 LEB128。EDPLLSim 可以选择 varint，但这是我们的工程选择。

## 1.3 PBMC

参考：

- [`cache papers/PBMC_On Top-Down Pseudo-Boolean Model Counting.pdf`](<cache papers/PBMC_On Top-Down Pseudo-Boolean Model Counting.pdf>)
- [`cache solvers/pbmc/pbmc-mod/core/Component.h`](<cache solvers/pbmc/pbmc-mod/core/Component.h>)
- [`cache solvers/pbmc/pbmc-mod/core/ComponentManager.cc`](<cache solvers/pbmc/pbmc-mod/core/ComponentManager.cc>)

PBMC 的 component identity 是：

```text
unassigned variable IDs
+
unsatisfied constraint IDs
+
current gaps
```

实现中：

- variable/constraint IDs 排序后差分并按位打包；
- gap 与 constraint ID 一起进入 exact packed representation；
- gap saturation 是 PB 布尔约束特有的额外等价规约。

不能直接照搬的地方：

- PBMC 的 constraint ID 能代表固定的原始 coefficient structure；
- EDPLLSim 的 `deleteVariable()` 会改变 active term 集合；若同时保存稳定变量集合且保留 immutable master row，这部分理论上还可从原始 row 恢复；
- 但 `coefficientStrengtheningSingle()` 会真的修改剩余变量的 coefficient，因此即使已有“原始 row ID + active variables + RHS”，仍不足以唯一表示当前 row。

## 1.4 Ganak 与 CP 2021 工作

参考：

- [`cache papers/Ganak.pdf`](<cache papers/Ganak.pdf>)
- [`cache papers/LIPIcs.CP.2021.8.pdf`](<cache papers/LIPIcs.CP.2021.8.pdf>)
- [`cache solvers/SharpSAT/code/ganak`](<cache solvers/SharpSAT/code/ganak>)

结论：

- Ganak 的 probabilistic component caching 只保存短 fingerprint，存在有界碰撞概率，本轮不采用；
- Ganak 的 CacheScore / CSVSADS 属于 branching heuristic，本轮不和 cache representation 混做；
- CP 2021 论文主要讨论 tree-decomposition-guided branching，并指出平均 cache-hit component size 比单纯 hit rate 更有解释力；它不是 ID packing 的主要依据。

---

# 2. EDPLLSim 当前实现事实

对应代码：

- [`src/linear_constraint.h`](src/linear_constraint.h)
- [`src/linear_constraint.cc`](src/linear_constraint.cc)
- [`src/solver.cc`](src/solver.cc)
- [`src/main.cc`](src/main.cc)

## 2.1 当前 key 与 hash

当前声明：

```cpp
static unordered_map<LinearConstraint, Int, LChash> cache;
```

`LinearConstraint::operator==` 比较：

```text
N
M
ranges
matrix（包含每行 RHS）
```

不比较：

```text
PID / mPID
kbest / dtree_pos
simplification_stats
heuristic state
```

这些被排除的字段只影响搜索顺序或统计，不影响当前 component 的精确计数，因此不应随意加入新 key。

当前 `LChash` 最终只有约十亿范围的模数 hash。碰撞不会造成错误，因为 `unordered_map` 仍调用 exact `operator==`，但大量数据下可能增加 chain/probe 成本。新实现应使用至少 64-bit 混合 hash，仍保留 exact equality。

## 2.2 当前 admission 与容量行为

```text
if cache_matrix_limit <= 0:
    cache disabled

if N*M > cache_matrix_limit:
    lookup/insert bypass

if cache.size() > 9,000,000:
    cache.clear()
```

注意：

- `MAX_CACHE` 是 entry 数，不是字节；
- 判断使用 `>` 且发生在插入前，实际可短暂超过常量；
- 正常内存压力策略是整表清空；
- 当前统计只对通过 `N*M` admission 的 lookup 计数，无法知道有多少请求被 bypass。

## 2.3 cache 调用位置与 free-variable factor

`Solver::count()` 的顺序为：

```text
simplify
→ 若直接求解则插入当前 state 的结果
→ free_variable，得到 factor
→ lookup
→ decompose / branch
→ 插入不含 free-variable factor 的 residual result
→ 返回 residual result * factor
```

新 cache API 必须保持这个语义。不能因为重构 builder 而把 `factor` 错存进“已经删除 free variables”的 key。

## 2.4 所有 row 都是 `<=`

证据：

- 输入 matrix 每行最后一个 `Int` 是 RHS；
- `redundancyDetectionSingle()` 用 `max_activity <= rhs` 判断冗余，用 `rhs < min_activity` 判断冲突；
- GLPK 行界使用 `GLP_UP`；
- SC、RPR、SB 都按 `a·x <= b` 操作。

因此 cache row 不需要 relation 字段，PR 不需要 `ceil_div` 分支。

## 2.5 当前 RPR/GCD 的实际问题

`parallelRows()` 当前：

- 用 coefficient GCD 生成临时方向向量以识别平行行；
- 只有 `gcd(coefficient_gcd, rhs) > 1` 时才会对 solver row 做整除；
- 没有对 `rhs / coefficient_gcd` 做 mathematical floor。

所以单独一行：

```text
6x + 10y <= 17
```

不会被真正规约为：

```text
3x + 5y <= 8
```

这既错失 solver-state 简化，也使 cache key 无法自然统一整数语义等价形式。

## 2.6 SC 与 row order

SC 直接修改当前递归副本的 `matrix`，这在现有 full-matrix key 下是 exact 的。

但 SC 修改 coefficient 后可能改变 lexicographic row order，而当前代码注释认为“不删行所以无需重排”。新 `CacheKeyBuilder` 必须无条件对 canonical rows 排序，不能依赖 `matrix` 当前恰好有序。

## 2.7 cache 生命周期

后续 random/application 输入都按“一次程序运行求解一个 instance”处理，不再设计 application 内多个 case 的 cache 共享。

目标生命周期固定为：

```text
程序开始 / 读入一个 instance
→ 初始化 component cache 和行母版池
→ 完成该 instance 的求解与统计输出
→ 统一释放 cache 和母版池
```

`resetCacheStats()` 只负责清统计；测试仍可保留显式 `clearForTest()`，但正式求解路径不需要 `process|instance` scope 开关，也不做跨输入复用。

---

# 3. 已有 cache 实验如何解读

现有结果位于 [`result/cache/random`](result/cache/random)，三组主实验均为 commit `7a16ce5`，配置：

```text
-heur=2 -varlimit=14 -ablation=5:7
```

其中 ablation `5:7` 表示 RPR 和 RIR-LP 被关闭。因此这些结果不能验证 PR 修复收益，但能说明现有 cache 的时间/内存矛盾。

| `N*M` admission | Success | TO | OOM/kill | Other fail | 成功项平均时间 | 成功项平均外部峰值内存 | Lookups | Hits | 聚合 hit rate |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 1409 | 1431 | 0 | 0 | 161.57 s | 6.49 MB | 0 | 0 | 0% |
| 20 | 1735 | 190 | 390 | 525 | 92.40 s | 379.42 MB | 8,792,710,548 | 8,429,383,198 | 95.8679% |
| 100 | 1707 | 0 | 539 | 594 | 22.22 s | 492.28 MB | 2,905,346,061 | 2,518,118,499 | 86.6719% |

可得出的结论：

1. cache 对可解规模和速度有明显价值；
2. 启用 cache 后出现了大量高内存/OOM，说明容量控制是首要问题；在完成逐实例 paired 分类前，不直接断言某个 timeout 一定被转化成了 OOM；
3. `N*M <= 20` 的聚合 hit rate 很高，但聚合值可能被少数超多 lookup 的实例支配；
4. `N*M <= 100` 在“成功实例平均时间”上更快，但成功集合不同，不能直接把 22.22 s 与 92.40 s 当作 paired speedup；
5. 525/594 个 `Other fail` 尚未按退出码细分，必须先分类，否则 solved/failed 比较不完整；
6. 所有时间和内存均只对成功实例求平均，存在 survival bias。

对“命中率已经很高，还要不要改”的直接判断：**要保留 cache，也仍然要做内存优化。** 高 hit rate 证明 cache 有用，并不能证明当前表示省内存；现有结果同时出现 390/539 个 OOM/kill，正说明不能因命中率高而保留当前无字节上限的设计。

但高 hit rate 也不能单独证明行母版池一定省内存。当前 map 对相同完整 component 本来只保存一个 entry；母版池节省的是“多个不同 component entry 之间重复出现的 coefficient structure”。因此要另外测 `row_structure_reuse_rate` 和 pool 总字节。母版池仍按本计划实现，但最终默认保留与否看**总 RSS/总 cache bytes**，不能只看 entry 变小。

后续报告必须同时给出：

- 所有实例的 solved / timeout / OOM / crash；
- 三个配置共同成功实例上的 paired runtime 和 paired peak RSS；
- 每实例 hit rate 的 median / quartiles；
- 聚合 hit rate；
- hit component 的 `N`、`M`、canonical key bytes 分布；
- 如果可行，记录 hit 后避免的递归工作近似量，而不是只看 hit 次数。

---

# 4. 本轮范围

## 4.1 必须完成

1. 完整、可解释的 cache instrumentation；
2. `<=` row 的共享 GCD + floor canonicalization；
3. 修复 solver RPR 对 RHS 的整数规约；
4. 统一 `CacheKeyBuilder`；
5. exact canonical logical key；
6. arbitrary-precision-safe 的紧凑字节编码；
7. 按字节限制的 component cache；
8. 非整表清空的懒清理/批量淘汰；
9. Cachet insertion-age + lazy cleanup；
10. 单一、可核算内存的 `CacheRowTemplatePool`；
11. 全阶段 exact-count 回归和 paired benchmark。

## 4.2 母版池目标

`CacheRowTemplatePool` 是本轮计划的一部分。self-contained packed key 作为正确性基准和 fallback 保留到实验结束；行结构复用统计用于量化母版方案节省了多少内存，而不是用来取消母版方案。

## 4.3 本轮不做

- PBMC gap saturation 对 EDPLLSim 一般整数域的推广；
- `[l,u] -> [0,u-l]` domain translation；
- Ganak probabilistic fingerprint；
- cache-aware branching / CacheScore；
- 跨 variable permutation 的图同构级 canonicalization；
- 与 cache 无关的大范围 solver 数据结构重写。

---

# 5. 不变量与安全边界

## 5.1 exactness

```text
hash 相等
≠
key 相等
```

任何 lookup 必须：

```text
hash 定位 bucket
→ 完整 key bytes/fields 精确比较
→ 相等后才返回 count
```

不能只保存 64-bit/128-bit fingerprint 后直接认为相等。

## 5.2 CacheKeyBuilder 是纯函数

```cpp
BuildResult CacheKeyBuilder::build(const LinearConstraint& state) const;
```

它不得：

- 修改 `state.matrix`；
- 修改 `state.ranges`；
- 修改 PID/mPID；
- 触发 solver simplification；
- 依赖某个 ablation 是否启用；
- 依赖 row 当前顺序。

## 5.3 solver normalization 与 cache normalization 共用数学核心

不要分别手写两份 GCD/floor 逻辑。

建议：

```text
NormalizeLeqRow(input row)
    → canonical temporary row / tautology / contradiction
```

- RPR 调用后把 canonical row 写回 solver state；
- CacheKeyBuilder 调用后只使用返回的临时 row。

这样能避免一边修了负 RHS，另一边仍用截断除法。

## 5.4 key 只编码影响 model count 的状态

必须包含：

- 当前每个局部变量的 `[lower, upper]`；
- 当前每条 residual row 的 coefficient structure；
- 当前每条 residual row 的 RHS。

不得包含仅影响搜索顺序的字段。

---

# 6. `<=` 行的 GCD + floor 规范化

## 6.1 数学规则

对整数变量：

```text
Σ ai xi <= b
g = gcd(|a1|, |a2|, ...), g > 0
```

等价于：

```text
Σ (ai/g) xi <= floor(b/g)
```

示例：

```text
6x + 10y <= 17
6x + 10y <= 16
3x +  5y <= 8
```

三者对整数变量完全等价，canonical row 都应为：

```text
3x + 5y <= 8
```

## 6.2 负 RHS

C++ 整数 `/` 向 0 截断，不能表示数学 floor：

```text
-1 / 2 == 0          // C++ 截断
floor(-1 / 2) == -1  // 数学语义
```

只实现项目实际需要的 helper：

```cpp
Int floorDiv(const Int& numerator, const Int& positive_denominator) {
    assert(positive_denominator.cmp(0) > 0);
    Int quotient;
    mpz_fdiv_q(quotient.a, numerator.a, positive_denominator.a);
    return quotient;
}
```

至少测试：

```text
floorDiv( 17, 2) =  8
floorDiv( 16, 2) =  8
floorDiv( -1, 2) = -1
floorDiv( -3, 2) = -2
```

不要新增未使用的 `ceil_div`。

## 6.3 规范化返回状态

建议返回：

```cpp
enum class RowStatus {
    kNormal,
    kTautology,
    kContradiction,
};
```

规则：

1. 去掉零 coefficient；
2. 若无非零 coefficient：
   - `0 <= rhs` 且 `rhs >= 0`：tautology，key 中丢弃；
   - `0 <= rhs` 且 `rhs < 0`：contradiction；
3. 计算所有非零 coefficient 绝对值的 GCD；
4. coefficient 全部除以 GCD；
5. RHS 使用 `floorDiv(rhs, gcd)`；
6. 不允许为了让首 coefficient 为正而把整行乘 `-1`，因为这会把 `<=` 翻成 `>=`。

## 6.4 RPR 写回 solver state

修复 `parallelRows()` 时，先逐行执行上述 canonicalization，再做平行/重复行处理。

同方向 primitive LHS 相同时：

```text
a·x <= b1
a·x <= b2
```

只保留较紧的：

```text
a·x <= min(b1,b2)
```

反方向：

```text
a·x <= b
-a·x <= c
```

若：

```text
-c > b
```

则整数区间必为空，可判 UNSAT。不要把相反方向的 row 通过乘 `-1` 合成同一个 `<=` 模板。

行被删除、交换或排序时，若 `mPID` 存在，必须同步更新。RPR 修复后重新排序 matrix，避免 coefficient 变化破坏 canonical row order。

## 6.5 CacheKey 中再次规范化

即使 solver RPR 已修复，builder 仍必须调用相同规范化函数，因为：

- 实验配置可能 ablate RPR；
- 当前已有 cache 实验正是 `-ablation=5:7`；
- SC 后 coefficient GCD 可能再次变化；
- key exactness/等价性不能依赖某个 simplify pass 是否运行。

---

# 7. 第一版 logical canonical key

## 7.1 为什么先不用 global VarID

旧 key 以 component-local column order 表示变量，并不包含原始变量 ID。两个局部系统只要 domains 与 matrix 相同，就会复用 count；这是安全的变量重命名等价。

第一版保持该性质：

```text
local variable 0..N-1
```

对应当前 `ranges` 和 matrix column。这样：

- 不依赖 heuristic 才维护的 PID；
- 不会因加入 global ID 而无意降低 hit；
- 更容易用旧 `LinearConstraint::operator==` 做差分验证。

如果未来 solver 增加 column permutation，届时再统一维护稳定 global IDs 或做列 canonicalization；本轮不要扩大范围。

## 7.2 逻辑结构

第一版先用易调试结构：

```cpp
struct CanonicalTerm {
    uint32_t local_var;
    Int coefficient;
};

struct CanonicalRow {
    std::vector<CanonicalTerm> terms;
    Int rhs;
};

struct LogicalComponentKey {
    std::vector<std::pair<Int, Int>> domains;
    std::vector<CanonicalRow> rows;
};
```

`N` 和 `M` 可由 vector size 获得，但物理序列化仍要显式写入 count/length，避免歧义。

## 7.3 builder 流程

```text
BUILD_LOGICAL_KEY(state):

    1. 按当前 local column order 复制 domains

    2. 对每条 matrix row：
       a. 转为稀疏 (local_var, coefficient)
       b. 调用 NormalizeLeqRow
       c. tautology -> 丢弃
       d. contradiction -> 返回 terminal contradiction
       e. 得到 canonical sparse row + canonical RHS

    3. 按 (terms, rhs) 做 exact lexicographic sort

    4. 对相同 terms 的多行只保留最小 RHS

    5. 返回 domains + rows
```

第 4 步是 conjunction 下的 exact 等价规约，也能让 builder 不依赖 RPR 是否执行。

## 7.4 SC 如何处理

第一版直接读取当前 `matrix`：

```text
SC 前 coefficient structure
≠
SC 后 coefficient structure
```

因此不同 SC 结果一定产生不同 exact row bytes；相同 SC 结果会产生相同 bytes。无需：

```text
original_row_id + sc=true
```

也无需先把 solver 的每条 row 改造成带 TemplateID 的对象。

## 7.5 terminal state

正常 lookup 预计发生在 `simplify()` 和 `free_variable()` 之后，通常不会看到 contradiction 或空约束异常状态；builder 仍应显式处理并统计，不要静默构造普通 key。

建议：

```cpp
struct BuildResult {
    BuildStatus status; // normal / tautological-system / contradiction
    LogicalComponentKey key;
};
```

若 builder 发现 contradiction，debug 构建应断言并定位 simplify 漏洞；release 构建应通过显式 terminal status 安全返回 count 0。不要把 contradiction 当普通 cache miss 后继续搜索，也不要把 terminal 处理隐藏在 hash-table 内部。

---

# 8. 精确物理编码

## 8.1 不要只压 ID，忽略 GMP `Int`

EDPLLSim 的 domain、coefficient、RHS 和 model count 都是 arbitrary precision `Int`。原计划只写了 ID varint，没有定义大整数编码，无法形成完整、无歧义的 exact key。

需要两个基础编码函数：

```text
appendULEB128(unsigned integer)
appendBigInt(Int value)
```

## 8.2 canonical BigInt 编码

建议 sign-magnitude：

```text
tag:
  0 = zero
  1 = positive
  2 = negative

non-zero:
  tag
  magnitude_byte_count as ULEB128
  magnitude bytes from mpz_export in fixed byte order
```

要求：

- zero 只有一种编码；
- magnitude 不允许前导零；
- byte order 固定；
- 不假设数值能放入 `int64_t`；
- 编码/比较不使用 `to_double()`。

## 8.3 self-contained packed key 布局

建议带版本号，避免未来布局变化时误读：

```text
format_version
num_variables

for each local variable:
    lower BigInt
    upper BigInt

num_rows

for each sorted row:
    num_terms
    first local_var as ULEB128
    subsequent local_var deltas as ULEB128
    coefficient BigInt for every term
    rhs BigInt
```

因为 row 已经稀疏化，不再保存 matrix 中的大量零。

所有列表都有显式 count，所有 BigInt 都有 tag/length，因此不存在拼接歧义。

## 8.4 ID difference encoding 的适用范围

第一版 local var IDs 位于单行内部并严格递增，可做 delta + ULEB128。

若后续使用 row template IDs：

- rows 先按 `(template_id, rhs)` 排序；
- template ID 非递减，可差分；
- 同一 template 多个不同 RHS 时 delta 可以为 0。

不要对有符号 coefficient 或 arbitrary precision RHS 生硬套 `uint64_t` ZigZag。

## 8.5 key object 与 allocation

正确性阶段可先使用：

```cpp
struct CompactComponentKey {
    std::vector<uint8_t> bytes;
    uint64_t hash;
};
```

统计内存时使用 `bytes.capacity()`，不能只用 `bytes.size()`。

如果大量小 key 的独立 heap allocation 成为主要开销，再单独实现 arena/slab 或 small-buffer optimization；不要与首次序列化同时做。

## 8.6 hash

要求：

- 对 canonical bytes 使用稳定的 64-bit 非密码学 hash；
- hash 缓存在 key/entry 中，避免每次 chain compare 重算；
- 测试允许注入 constant hasher，强制所有 key 冲突，验证 exact byte comparison；
- 不把 hash 当作 key 本身。

---

# 9. `CacheRowTemplatePool`（缓存行母版池）

## 9.1 先量化母版复用收益

先用 self-contained key 统计：

```text
row_structure_intern_requests
unique_canonical_row_structures
row_structure_reuse_rate
direct_row_structure_bytes_total
projected_template_pool_bytes
projected_template_id_key_bytes
```

这些量的单位为：

```text
requests / unique structures：个
reuse rate：比例或百分比
其余 *_bytes：byte
```

最终检查是否达到下面的目标：

```text
projected pool total
= template records + pool hash/index + IDs in cache keys
<
self-contained row-structure bytes total
```

这一步用于报告母版池的净收益，并指导是否需要更积极地回收低复用母版。不能只报告 component entry 变小，而忽略母版池自身的字节。

## 9.2 只需要一个 cache row pool

定义：

```cpp
struct CanonicalRowStructure {
    std::vector<CanonicalTerm> terms; // 已 GCD-normalized
};
```

RHS 不放入 structure template，因为 assignment/删 fixed variable 经常只改变 RHS。

流程：

```text
current row
→ sparse current terms
→ GCD normalize
→ exact intern by canonical term bytes
→ CacheRowTemplateID
```

原始 row、删变量后的 row、SC 派生 row 全部走同一内容寻址入口。无需区分 OriginalTemplateID 与 DerivedTemplateID，也无需 `SC=true` tag。

## 9.3 pool exactness

intern 必须：

```text
hash 定位
→ exact canonical term bytes comparison
→ 相同结构复用 ID
```

`[6,10]` 与 `[3,5]` 经 GCD 后可复用同一 ID；未做 GCD 时不可仅凭 hash 合并。

## 9.4 pool 生命周期和回收

母版池与当前 instance 的 component cache 同时创建、同时销毁，并计入同一个总字节预算。

建议使用引用计数：

- cache entry 插入时增加所引用 template 的 refcount；
- eviction/update 时减少；
- refcount 为 0 的 template 可以暂留以便再次复用；
- 达到内存上限时优先回收 refcount 为 0 的 template；
- ID 在仍可能被 component key 引用时绝不复用。

不能使用“不计入 cache bytes、永不回收”的全局派生模板池。

## 9.5 lookup 时避免零引用模板泄漏

若一次 lookup 遇到从未见过的 row structure，则不可能命中已有 ID key。实现可以：

- 先只 probe pool；不存在则直接判 cache miss，等 entry 真正被 admission 后再 intern；或
- 临时 intern，但 lookup 后未插入时立即回收 refcount 0 的模板。

不要让每个未缓存状态都永久创建模板。

---

# 10. 按字节管理的 ComponentCache

## 10.1 API 与配置命名

建议把当前含糊的 `-cache-limit` 逐步拆成：

```text
-cache-admission-max-cells=N  // 保留 N*M 过滤，0 表示关闭 cache
-cache-max-mb=N               // 可选 cache 总预算；0 表示不启用，默认 0
-cache-target-ratio=0.85      // 非零字节上限触发后的目标占用
-cache-max-age-inserts=N      // 允许 entry 经历的后续插入次数，默认 51,200
```

为兼容脚本，旧 `-cache-limit` 暂时作为 `-cache-admission-max-cells` 的 alias，并在输出中使用新名称解释。

正式运行依靠 entry 数策略和外部进程 10 GiB 限制；需要研究内部 hard byte limit 时，
再显式指定多个非零预算。

这里有两个不同层次的限制：

```text
单个 component 到来
    ↓
N*M <= admission threshold ?
    ├── 否：这个 component 不进入 cache
    └── 是：允许 lookup/insert
              ↓
        resident entries 达到 20 * age_limit ?
              ├── 是：沿 oldest 链保留最近约 1/4
              └── 否：继续
              ↓
        cache-max-mb 是否非零且超过 byte memory limit ?
              ├── 否：完成
              └── 是：继续从 oldest 端清到 byte target
```

- `N*M` 的单位是“矩阵格数量”，例如 `N=5, M=4` 时为 20；它控制**单个 component 的准入**。
- entry `clean_limit` 的单位是“驻留缓存项数”，默认 1,024,000。
- `cache-max-mb` 的单位是 MB；非零时控制**所有 cache entries + 母版池的总容量**，
  0 表示只统计、不作为触发条件。
- 三者不能混用：`N*M` 决定单项是否准入，entry limit 决定默认全局回收时机，
  byte limit 是可选的第二道安全阀。

## 10.2 基础结构

第一版可用显式 separate chaining：

```cpp
struct CacheEntry {
    CompactComponentKey key;
    Int value;
    uint64_t created_seq;
    CacheEntry* next;
    size_t accounted_bytes;
};

class ComponentCache {
    std::vector<CacheEntry*> buckets;
    uint64_t insertion_seq = 0;
    size_t bytes_current = 0;
    size_t bytes_peak = 0;
};
```

后续若每 entry 一个 `new` 的 allocator overhead 明显，再换 slab；第一版先保证删除、统计和 exact equality 清晰。

## 10.3 Cachet insertion-age

```text
age = insertion_seq - created_seq
```

- 插入时设置 `created_seq`；
- 命中不改变 age；
- 新 entry 插到 bucket chain 表头，因此同一 chain 从新到旧排列；
- 扫描时遇到第一个 stale entry，可以删除它以及后面的旧后缀；
- 不实现 hit renew/touch-age。

这就是“懒处理”：平时只清当前触及的 chain，避免每次插入都全表扫描。完全不再访问的
bucket 由全局 entry clean 处理：达到 `20*age_limit` 后，从 oldest 端 O(删除项数)
批量淘汰到约 1/4，不额外扫描 bucket 数组。

选择它的主要理由是实现简单、每次 hit 不需要改年龄或调整链表，并且与 Cachet 参考代码一致；它不代表在所有数据集上命中率必然高于 touch-age。效果仍以相同 byte budget 下的 solved/OOM/runtime 为准，但不再为此维护第二套策略。

## 10.4 lookup

```text
LOOKUP(state):

    stats.lookup_requests++

    if cache disabled:
        stats.bypass_disabled++
        return MISS

    if admission rejects state:
        stats.bypass_admission++
        return MISS

    build canonical key
    stats.eligible_lookups++

    scan target bucket:
        compare cached hash
        exact compare key bytes
        if equal:
            stats.hits++
            return value

        if current entry stale:
            remove current and the remaining older suffix
            break

    stats.misses++
    return MISS
```

由于 hit 不 renew、entry 始终从表头插入，chain 保持从新到旧，才可以安全删除 stale 后缀。

## 10.5 insert/update

```text
INSERT(key, value):

    stats.insert_attempts++

    if disabled/admission rejected:
        stats.insert_bypasses++
        return

    scan bucket and remove stale entries

    if exact key already exists:
        assert/debug-check old value == value
        update/keep value
        stats.insert_updates++
        return

    if single entry itself exceeds budget:
        stats.oversize_rejections++
        return

    allocate entry
    update byte counters
    stats.new_inserts++

    if entries_current >= 20 * age_limit:
        EVICT_OLDEST_TO_NEWEST_QUARTER()

    if max_bytes != 0 and bytes_current > max_bytes:
        EVICT_OLDEST_TO_BYTE_TARGET()
```

同一个 exact component 的 model count 应唯一；debug 中 old/new value 不同必须立即报错。

## 10.6 eviction

禁止：

```cpp
if (capacity_pressure)
    cache.clear();
```

但允许在以下场景显式 `clear/reset`：

- 当前 instance 求解结束时由 cache destructor 统一释放；
- 单元测试 teardown / `clearForTest()`。

正式 application/random 求解路径没有“case 结束后继续复用同一 cache”的分支，也不需要用户可选的 scope/reset 策略。

采用三种清理时机：

1. **平时 lazy cleanup**
   - lookup/insert 只检查当前 bucket chain；
   - 遇到第一个 stale entry 时删除它及其更老的后缀；
   - 不额外维护 touch-age，也不每次扫描其他 bucket；

2. **达到 entry clean limit 时的全局批量清理（默认主策略）**
   - `clean_limit = 20 * age_limit`，默认 `20*51,200=1,024,000`；
   - 沿全局 oldest→newest 双向链从最老项开始删除，直到只剩触发时约 1/4；
   - 每个 entry 通过 bucket 反向指针 O(1) 摘除，因此不扫描整个 bucket 数组；
   - 整批引用释放完成后，只调用一次 `reclaimUnreferenced()` 回收零引用模板；

3. **达到可选 hard byte limit 时的批量清理**
   - 仅当 `cache-max-mb>0` 才启用；
   - 同样沿 oldest 链继续删除到 `target_ratio * max_bytes`；
   - 单个 entry 本身超过预算时拒绝插入；即使所有 entry 都很新也能终止。

最终实现保留 Cachet 的“插入年龄 + 当前链懒清理 + 20 倍 entry-count 全局触发”，
并增加可选 byte safety valve；正常淘汰不做整表 `clear()`，全局批量淘汰也不做
全 bucket 扫描。

## 10.7 byte accounting

至少包含：

```text
bucket array capacity
entry node sizeof
key byte-vector capacity / arena allocation
stored key hash and age fields
Int object
model-count GMP used/allocated limbs 的可解释估计
template pool records and hash index
allocator overhead estimate（单独列出）
```

GMP payload 可先以 limb 数给出逻辑估计，但必须标注是否为 used bytes 还是 allocated bytes。最终判断以内外部 peak RSS 为准，内部统计用于解释差异。

## 10.8 load factor 与 chain 统计

输出：

```text
bucket_count
occupied_buckets
load_factor
max_chain_length
average_nonempty_chain_length
hash_collision_probes
exact_key_compares
```

否则 key 变小后若 hash/chain 退化，无法解释 runtime 回退。

---

# 11. 开发期统计与最终保留策略

开发阶段曾使用分级诊断统计；最终版本已删除 `-cache-stats=off|basic|full`，每次求解固定
输出一组低开销、直接可解释的计数。hash chain、histogram、插入 bypass、估算字节、
probe/hit/miss、hash collision 和 exact compare 等纯开发期计数不再维护。

## 11.1 请求与命中

```text
Cache lookups
hits
misses
hit_rate = hits / lookups
```

`Cache lookups` 只在 component 通过 `N*M` 限制且成功构造有效 key 后增加。规范化发现
恒假约束时直接返回 0，不查缓存，也不计入 lookups。因此不变量简化为：

```text
lookups = hits + misses
```

## 11.2 插入与淘汰

```text
Peak number of cache entries
Number of cache cleanup
Number of deleted entries
```

其中 cleanup 只统计达到 `20*age_limit` 后进行的全局批量清理次数；deleted entries 同时
包含 bucket 懒删除的过期 entry 和全局批量删除的 entry。

## 11.3 行模板复用

```text
Row template requests
Row template reuses
Row template reuse rate
```

request 表示插入 component 时请求保存一条规范化系数结构；reuse 表示该结构已在模板池中，
只需复用现有 TemplateID。`reuse rate = reuses / requests`。不同 RHS 共享同一行模板，RHS
仍保存在最终 component key 中。

## 11.4 内存口径

```text
Peak RSS (MB)
```

这是操作系统报告的整个求解进程峰值常驻内存，不声称是 component cache 单独占用的内存。

---

# 12. 测试计划

## 12.1 floor 与 row normalization

### 正 RHS

```text
6x + 10y <= 17  ->  3x + 5y <= 8
6x + 10y <= 16  ->  3x + 5y <= 8
6x + 10y <= 18  ->  3x + 5y <= 9
```

要求：

```text
Key(17) == Key(16)
Key(17) != Key(18)
```

### 负 RHS

```text
6x + 10y <= -1  ->  3x + 5y <= -1
6x + 10y <= -3  ->  3x + 5y <= -2
```

### 负 coefficient

```text
-6x + 10y <= 17 -> -3x + 5y <= 8
```

不得把整行乘 `-1`。

### zero row

```text
0 <= 0   -> tautology
0 <= 5   -> tautology
0 <= -1  -> contradiction
```

## 12.2 RPR

- 单独一条非整除 RHS 的 row 也必须被规约；
- 同方向平行 row 只保留 tightest RHS；
- 相反方向可发现明显空整数区间；
- 删除/交换 row 后 matrix 与 mPID 一一对应；
- RPR enabled 和 disabled 的 exact count 一致。

现有主实验 ablate 了 RPR，所以必须另建 `parallelRows()` 单元测试，不能依赖全量 benchmark 恰好覆盖。

## 12.3 row permutation

相同 rows 的不同排列必须得到相同 key。

## 12.4 duplicate rows

```text
a·x <= 8
a·x <= 9
```

与只保留：

```text
a·x <= 8
```

key 相同。

## 12.5 local variable identity

两个 component 的 PID 不同，但 local domains 和 coefficient matrix 完全相同：

```text
Key 必须相同
```

这保持旧 cache 的变量重命名复用语义。

但只改变一个 domain 或一个 coefficient：

```text
Key 必须不同
```

## 12.6 SC

- 两条当前真实 coefficient structure 相同的 SC 结果产生相同 row bytes/template ID；
- coefficient structure 不同则 key 不同；
- 不能用 `original_row_id + used_sc` 代替当前结构；
- SC 后再出现 coefficient GCD 时，cache-only normalization 仍统一。

## 12.7 arbitrary precision serialization

测试超过 64-bit 的：

- lower/upper；
- positive/negative coefficient；
- positive/negative RHS。

同时测试 zero 唯一编码、长度前缀和不同字段组合不会出现拼接歧义。

## 12.8 forced hash collision

测试 hasher 固定返回同一个值：

- 不同 key 不能误 hit；
- 相同 key 仍能 hit；
- chain 删除后剩余 entry 正常可查。

## 12.9 free-variable factor

构造带完全 free variable 的 component：

- 首次求解 count 正确；
- 第二次 cache hit 后仍乘正确 factor；
- cache 中 stored value 不重复包含 factor。

## 12.10 eviction

- insertion-age 下 hit 后 age 不变；
- chain 始终保持从新到旧；
- lazy cleanup 只删除符合策略的 entry；
- hard limit 下即使所有 entry 都很新，也能清到 target；
- 不出现 capacity-triggered `clear()`；
- bytes_current 在 insert/update/delete 后守恒。

## 12.11 单 instance 生命周期

- 一个 solver 进程只初始化一个 instance 的 cache 和母版池；
- 求解结束后两者都完整释放；
- 下一个输入启动新的求解进程/实例，不复用旧 TemplateID 或 cache entry；
- 单元测试可分别调用 stats reset 和 cache teardown，验证两者职责不同。

## 12.12 property/differential test

对小变量、小 domain 的随机系统：

1. brute force 得到 exact count；
2. legacy cache on/off；
3. canonical cache on/off；
4. RPR on/off；
5. row shuffle；
6. 等价 GCD scaling。

全部 count 必须一致。

额外验证：只要两个 canonical key 相等，就对小 domain brute force 验证它们的 model count 相等。这是发现过度 canonicalization 的关键测试。

---

# 13. Benchmark 方法

## 13.1 固定实验元数据

每个结果文件记录：

```text
git commit + dirty state
compiler/version/flags
machine/CPU/RAM
dataset path/hash or manifest
solver flags
cache key mode
cache policy mode
cache byte budget
timeout/memory limit
concurrency
```

## 13.2 先修复失败分类

`scripts/vm_random_cache_compare.sh` 当前把非 0、非 124、非明显 OOM 的退出统一记为 `OTHERFAIL`，且没有把 stderr/exit code 保存在汇总结果中。

Phase 0 先增加：

```text
exit_code
signal
最后若干行 stderr
failure category
```

在 525/594 个 `Other fail` 被解释前，不把 solved 数差异全部归因于 cache 策略。

## 13.3 比较配置

至少：

```text
A. cache off
B. legacy full key + legacy clear
C. canonical logical key + legacy map（隔离 key 语义影响）
D. packed self-contained key + legacy map/clear（隔离物理编码影响）
E. D + byte-bounded Cachet insertion-age/lazy cleanup
F. E + row-template pool
```

每个主要阶段只改变一个因素。

## 13.4 数据集

至少覆盖：

```text
tests/extreme_cases
tests/my_cases
small random correctness subset
random full set
application
thin
STN / knapsack（仓库存在且脚本稳定的集合）
```

## 13.5 主要指标

优先级：

1. exact count；
2. crash / wrong answer 为 0；
3. solved、TO、OOM；
4. cache/process peak bytes；
5. common-success paired runtime；
6. effective hit rate 与 hit-size distribution；
7. key-build/eviction overhead；
8. 模板池净字节收益。

平均 runtime 只在相同成功实例集合上做 paired 比较，并同时报告 median/geometric mean 或 cactus plot。

---

# 14. 推荐实施顺序

## Phase 0：统计与可复现实验基线

- [x] 把 `lookup_requests` 与 `eligible_lookups` 分开；
- [x] 增加 insert/entry/clear 统计；
- [x] 增加按 component `N/M/N*M` 的 lookup/hit histogram；
- [x] 记录 cache entry/key/value 的初步估计字节；
- [x] 修复 benchmark 的 `OTHERFAIL` 分类和日志保留；
- [x] 建立快速 exact-count regression manifest；
- [x] 保存 legacy cache off/20/100 的 paired baseline。

验收：只增加观测，不改变任何 solution count、key 或淘汰行为。

Phase 0 已完成。实现说明：

- 当时的 `basic/full` 分级用于控制 histogram 成本；该开发期接口在最终版本中已经删除；
- key/value 字节是用于比较版本的保守估计，包含对象、vector 存储和 GMP **used limbs**；allocator metadata 等不可移植开销仍以 external peak RSS 为准；
- 当时为读取历史结果保留 `eligible_lookups` 口径；最终字段已简化为实际有效 key 的 `Cache lookups`；
- 快速回归位于 `tests/cache_regression`，`phase0_legacy_stats.tsv` 固化了改动前 `0/20/100` 的 eligible lookup/hit/miss；
- 本阶段没有修改 key/hash/equality、`N*M` admission、`MAX_CACHE` clear 条件或 `Solver::count()` 的控制流。

## Phase 1：共享 `<=` row canonicalization 与 RPR 修复

- [x] 实现唯一的 `floorDiv()`；
- [x] 实现纯函数 `NormalizeLeqRow()`；
- [x] 修复 `parallelRows()` 写回 coefficient/GCD/floored RHS；
- [x] 处理 zero row、同向重复 row、反向明显冲突；
- [x] 保持 mPID 同步；
- [x] 补负 RHS 和 solitary-row tests；
- [x] RPR on/off regression。

验收：不增加 `ceil_div`；不修改 SB/LP 中必要的 ceiling 运算。

Phase 1 已完成。实现说明：

- 新增 `row_normalizer.{h,cc}`，`NormalizeLeqRow()` 是不修改输入的共享纯函数；dense solver row 的局部变量列位置保持不变，后续 sparse cache key builder 将复用同一数学核心并跳过零项；
- GCD 只由 coefficients 计算，所有 coefficients 除以 GCD，RHS 使用 GMP `mpz_fdiv_q` 做 mathematical floor；因此 `6x+10y<=17`、`6x+10y<=16` 和 `3x+5y<=8` 都规约成同一整数语义；
- `parallelRows()` 会把规约结果真正写回 solver state，再按 primitive coefficient vector 合并同向 row、保留最小 RHS，并用 `-opposite_rhs > upper_rhs` 检测反向整数区间为空；
- zero row 的 `rhs>=0` 作为 tautology 删除，`rhs<0` 立即判 contradiction；删除 row 时按原顺序同步压缩 `matrix` 与 `mPID`；
- SC 改变 coefficient 后会设置重新排序标记，保证 `matrix` 及其 `mPID` 在 simplify 结束时同步恢复 canonical row order；
- 没有新增 `ceilDiv()`，SB 中处理负 coefficient 所需的 `mpz_cdiv_q` 以及 LP 逻辑均未改动。

Phase 1 验证结果：

- `make test-row-normalizer`：通过；覆盖正/负 RHS、超过 64 bit 的负数、solitary row、zero row、同向/反向 row 和 `mPID` 同步；
- RPR disabled（`ABLATION_IDS=5:7`）缓存回归：18/18 通过，并与 Phase 0 保存的 eligible lookup/hit/miss 完全一致；
- RPR enabled（`ABLATION_IDS=7`）缓存回归：18/18 通过；
- `tests/extreme_cases` 在 RPR enabled 和七项简化全部 ablate 下均为 35/40，通过数与 Phase 0 相同；剩余 5 项仍是既有的整数 token 校验和 rational input 支持问题，不是本阶段回归。

## Phase 2：Logical `CacheKeyBuilder`

- [x] 使用 component-local column identity；
- [x] domains 完整保留 `[l,u]`；
- [x] 构建 canonical sparse rows；
- [x] row sort；
- [x] 同 LHS 只保留最小 RHS；
- [x] exact hash/equality；
- [x] 增加 `legacy|canonical` key mode 便于 A/B；
- [x] differential/property tests；
- [x] full regression。

此阶段先使用普通 vector/Int，不做 varint，不做模板池，不改淘汰策略。

Phase 2 已完成。实现说明：

- 新增 `cache_key.{h,cc}`，逻辑 key 由完整的 component-local domains 和 canonical sparse rows 构成；不编码 PID、mPID、原始 row ID 或启发式元数据；
- lookup 与 insert 在 canonical 模式下都调用同一个纯 `CacheKeyBuilder::Build()`；builder 再次调用 Phase 1 的 `NormalizeLeqRow()`，因此即使 RPR 被 `-ablation=5` 关闭，cache key 仍会进行 coefficient GCD + floored RHS 规范化；
- 每条 row 只保存非零 `(local_var, coefficient)`，随后按 `(terms,rhs)` 精确排序；相同 primitive LHS 只保留最小 RHS，tautology 丢弃；contradiction 和 malformed internal row 使用显式 build status，不会伪装成普通 key；
- `LogicalComponentKeyHash` 对任意精度 Int 的 sign、limb count 和全部 used limbs 做 hash；lookup 最终仍调用完整 domains/rows equality，因此 hash collision 不会产生错误命中；
- 新增 `-cache-key=legacy|canonical`，默认仍为 `legacy`，方便和 Phase 0 基线严格对照；两种模式使用独立 map，但 admission 仍使用原来的 component `N*M`，`MAX_CACHE` 整表清空行为也暂时不变；
- Phase 0 的内存估算现在按当前 active key 类型计算；canonical 中只核算 logical vector/Int，尚未宣称是最终 packed-byte 内存。

Phase 2 验证结果：

- clean `make`：EDPLLSim 与 Sim 均无 warning 构建通过；
- `make test-row-normalizer` 与 `make test-cache-key`：通过；后者包含 300 组 deterministic 小整数域 property test、任意精度字段、row permutation、duplicate/tightest RHS、PID 独立性、builder purity 和 constant-hash collision；
- 其中前 40 组随机系统及其 scaling/shuffle/duplicate 变体，共执行 640 个 real-solver differential 配置，覆盖 legacy/canonical、cache on/off、RPR on/off，全部与 brute-force exact count 一致；
- 快速 cache regression 的四个组合（legacy/canonical × RPR off/on）各 18/18，共 72/72 通过；legacy + RPR off 的 Phase 0 eligible lookup/hit/miss 仍完全一致；
- extreme cases 在 legacy 与 canonical 下均为 35/40，与既有基线一致；5 个已知 parser/rational-input 失败没有变化；
- benchmark 脚本已支持 `CACHE_KEY_MODE=legacy|canonical`，模式写入命令元数据和文件名；canonical 单输入 smoke test 通过。

诊断实例 `10_7_5.in`、`N*M<=20`、RPR off 的单次结果如下。该表只用于确认机制生效，不作为 Phase 6 的正式性能结论：

| key mode | solution count | eligible | hits | raw hit rate | entries | estimated key bytes | estimated total bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| legacy | 17,923,488,200 | 15,448 | 4,455 | 28.8387% | 25,891 | 17,250,712 | 18,518,696 |
| canonical | 17,923,488,200 | 15,444 | 6,105 | 39.5299% | 14,689 | 6,407,104 | 7,133,072 |

canonical 命中后会提前剪掉递归，所以两种模式的后续 lookup/insert 总数不要求相同；需要保持的是 exact solution count 和各自统计守恒。正式的 paired runtime/RSS、全数据集 solved/TO/OOM 结论留到 Phase 6。

## Phase 3：Self-contained exact byte key

- [x] ULEB128；
- [x] canonical arbitrary-precision BigInt encoding；
- [x] sparse local-var delta encoding；
- [x] 显式 count/length/version；
- [x] cached 64-bit hash + exact byte equality；
- [x] forced-collision test；
- [x] 统计 size 与 capacity；
- [x] 与 Phase 2 logical key 一一对照。

验收：logical key 相等当且仅当 packed bytes 相等。

Phase 3 已完成。实现说明：

- 新增 `PackedComponentKey { vector<uint8_t> bytes; uint64_t hash; }` 和 `PackedCacheKeyBuilder`；stored key 不再包含 domains/rows 的多层 vector 和逐字段 GMP `Int` 对象；
- byte format 以显式 format version 开头，依次编码 variable count、所有 `[lower,upper]`、row count、每行 term count、local-var delta/coefficient 和 RHS；所有 list 都有 count，所有非零 BigInt 都有 sign tag 与 magnitude length，因此字段边界无歧义；
- unsigned count、length 和 local-var/delta 使用 ULEB128；同一 sparse row 的 local variable 严格递增，首 ID 直接编码，后续编码正 delta；
- arbitrary-precision `Int` 使用 canonical sign-magnitude：`0` 只有单字节 tag；非零值使用 positive/negative tag、ULEB128 magnitude length 和固定 big-endian magnitude bytes，不经过 `int64_t` 或 `double`；
- 编码前精确计算 payload size 并一次 `reserve()`，当前 libstdc++ 下 stored byte vector 的 `capacity == size`，避免增长策略带来的额外容量；
- 64-bit FNV-1a 在构建时计算并缓存，unordered_map hasher 只返回该值；`operator==` 始终比较完整 byte vector，所以 hash collision 不能造成错误 hit；
- 新增 `-cache-key=packed`，并保留 `legacy` 与 `canonical`：canonical 是语义基准，packed 是其物理编码；admission、value、`MAX_CACHE` clear 策略均未改变；
- 新增 packed payload/capacity current/peak 统计，benchmark summary 也记录单实例最大 payload/capacity；estimated key bytes 另包含 `PackedComponentKey` 对象本身。

Phase 3 验证结果：

- clean `make`：EDPLLSim 与 Sim 均无 warning 构建通过；
- ULEB128 覆盖 `0/127/128/624485/UINT64_MAX`；BigInt 覆盖 zero、positive、negative、跨字节边界和超过 64 bit 的 domain/coefficient/RHS；
- exact layout 测试逐字节核对 format version、counts、local-var delta、sign、length 和 magnitude；constant hasher 强制所有 packed keys 冲突时，不同完整 bytes 仍能分别存取；
- 300 组 property cases 两两检查 `logical_key_i == logical_key_j` 当且仅当 `packed_bytes_i == packed_bytes_j`；scaling/shuffle/duplicate 变体也得到相同 bytes/hash；
- real-solver differential 扩展为 legacy/canonical/packed × cache on/off × RPR on/off；前 40 组原始/变换随机系统共 960 个配置，全部与 brute-force exact count 一致；
- 三种 key × RPR off/on 的快速 cache regression 各 18/18，共 108/108 通过；legacy + RPR off 的 Phase 0 eligible/hit/miss 仍完全一致；
- extreme cases 在 legacy/canonical/packed 下均为 35/40，与既有基线一致；5 个已知 parser/rational-input 失败没有变化；
- `CACHE_KEY_MODE=packed` benchmark 单输入 smoke test 通过，结果文件、命令元数据和 summary 均记录 packed mode 与 byte statistics。

诊断实例 `10_7_5.in`、`N*M<=20`、RPR off 的单次结果：

| key mode | solution count | hits | entries | estimated key bytes | estimated total bytes | packed payload/capacity |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| legacy | 17,923,488,200 | 4,455 | 25,891 | 17,250,712 | 18,518,696 | — |
| canonical logical | 17,923,488,200 | 6,105 | 14,689 | 6,407,104 | 7,133,072 | — |
| packed bytes | 17,923,488,200 | 6,105 | 14,689 | 1,151,973 | 1,877,941 | 681,925 / 681,925 |

canonical 与 packed 的 lookup、hit/miss、递归动作完全一致，说明本阶段只改变物理表示。相对 canonical logical，packed 的 estimated key bytes 在该实例下降约 82%，estimated total cache bytes 下降约 74%；这仍是内部估算和单实例诊断，allocator/RSS 与全数据集结论留到 Phase 6。

## Phase 4：按字节限制的 cache manager（历史阶段，最终策略见 Phase 7）

- [x] 分离 admission limit 与 memory limit；
- [x] separate chaining 或等价的可控容器；
- [x] bytes current/peak；
- [x] Cachet 式 lazy bucket-chain cleanup；
- [x] hard-limit batch eviction to target；
- [x] capacity-triggered clear 计数必须为 0；
- [x] 单 instance 的初始化、释放与测试 teardown；
- [x] load-factor/chain stats；
- [x] memory-limit stress tests。

本阶段只实现 Cachet 的 insertion-age；hit 不 renew，不再实现第二套 age policy。

Phase 4 已完成。实现说明：

- 新增 `component_cache.{h,cc}`，`packed` 模式不再使用
  `unordered_map<PackedComponentKey,Int>`，而是使用显式 separate chaining
  的 `PackedComponentCache`；每个 bucket chain 始终按 `created_sequence`
  从新到旧排列，hash 只用于选 bucket/快速过滤，命中仍比较完整 packed
  bytes；
- `age = insertion_sequence - created_sequence`；只有真正插入新 entry 才推进
  `insertion_sequence`，hit 和 exact-key update 都不续龄、不移动节点。lookup
  按 Cachet 的顺序先尝试 exact hit，再在遇到第一个 stale entry 时删除它和
  后面的旧后缀；普通 insert/lookup 不扫描其他 bucket；
- Phase 4 当时以 byte hard limit 为主；Phase 7 已改为 Cachet exact 的 entry-count
  主策略。当前 hard byte limit 仅作为可选安全阀，触发时直接沿全局 insertion-order
  oldest 链批量删除，不再先全 bucket 扫描 stale suffix；单个 entry 超过预算时拒绝；
- 新参数为 `-cache-admission-max-cells`、`-cache-max-mb`、
  `-cache-target-ratio` 和 `-cache-max-age-inserts`；旧 `-cache-limit` 保留为
  admission alias。最终默认 byte limit 为 0（禁用内部硬上限）、target ratio 为
  0.85、age window 为 51,200 次后续插入；关闭 cache 仍使用 admission cells = 0；
- budget 计入 bucket vector capacity、entry node、packed key vector capacity、
  `Int` 对象及 GMP **allocated limbs**，并对 node allocation 与 packed-byte
  allocation 各加入两个 machine-word 的 allocator metadata 保守估计。Phase 0/3
  的旧统计主要按 GMP used limbs，两个阶段的 total estimated bytes 不能直接当成
  完全相同口径；最终仍用 RSS 校准；
- bucket directory 从最多 1024 个桶起步，预计 load factor 超过 2 时倍增，
  而不是按 1 GiB 预算预先分配大量空桶；rehash 后仍恢复每条 chain 的新到旧
  顺序。自动桶数上限为 262,144，且目录最多使用约 1/16 byte budget；输出
  bucket count、occupied buckets、load factor、最大/平均非空 chain 长度、hash
  collision probes 和 exact key compares；
- `initializeCacheForInstance()` 在一次求解开始时创建干净的 packed manager；
  destructor/显式测试 teardown 释放全部 node。`resetStatistics()` 只重置观测量，
  不改变 entries 或年龄；正式求解没有 application case scope 分支；
- 容量压力下 managed cache 不调用 `clear()`；`clear()` 只用于 instance 初始化和
  单元测试 teardown。Phase 7 最终对照完成后已删除 raw legacy 容器、对应 CLI 和
  `Cache capacity clears` 旧统计字段；canonical 仅保留为逻辑对象诊断基准。
- Makefile 增加 `-MMD -MP` 自动头文件依赖，避免修改 cache class layout 后遗漏
  重编引用该头文件的 object；干净构建和增量构建使用同一依赖关系。

Phase 4 验证结果：

- `make test-cache-key` 与 `make test-row-normalizer` 通过；新增测试覆盖 hit 不续龄、
  update 不续龄、只清当前 bucket、forced cached-hash collision、lazy suffix 删除、
  自动 bucket growth/rehash、stats reset 与 teardown 职责分离、所有 entry 都很新时
  的 hard eviction、oversize rejection 和字节守恒；
- Phase 4 当时的 real-solver/property differential 覆盖 legacy/canonical/packed × cache on/off ×
  RPR on/off，exact count 全部与 brute force 一致；三种 key 的快速回归各 18/18
  通过；packed 另以 0.05 MiB hard budget、0.50 target、超大 age window 跑完
  18/18；
- packed extreme cases 为 35/40，与 Phase 1–3 基线相同；剩余 5 项仍是已知的
  integer token 校验/rational input 问题；benchmark 脚本的命令元数据、文件名和
  summary 已记录 byte budget、target、age、hard cleanup/lazy eviction/hard
  eviction/oversize rejection；单输入 smoke test 通过；
- 诊断实例 `10_7_5.in` 在默认 1024 MiB/0.85/1,048,576 配置下仍得到
  `17,923,488,200`，hit=6,105、entry=14,689，与 Phase 3 packed 完全一致；没有
  eviction/clear。新口径为 key 1,151,973 B、value 324,920 B、bucket 65,536 B、
  node/allocator/全局年龄链 overhead 940,096 B，总计 2,482,525 B；bucket=8,192、
  load factor=1.793、max chain=9；
- 同一实例在 0.05 MiB（52,428 B）、target=0.50、age=100,000,000 的强制压力
  配置下 exact count 仍相同；发生 291 次 hard cleanup、51,778 项 hard eviction，
  lazy eviction=0、capacity clear=0，reported peak 恰为 52,428 B，结束时
  `bytes_current <= hard limit`。

## Phase 5：实现单一 CacheRowTemplatePool

- [x] 输出 row structure reuse 和 projected bytes，并保留 packed 对照；
- [x] 只实现一个 content-addressed `CacheRowTemplatePool`；
- [x] pool exact intern；
- [x] pool bytes 纳入预算；
- [x] refcount/reset 生命周期；
- [x] lookup miss 不泄漏零引用模板；
- [x] SC/GCD/template tests；
- [x] 支持与 self-contained packed key 比较 cache bytes 和总 RSS。

母版池是本方案的目标实现；若实测总内存没有下降，保留 self-contained exact sparse key 作为安全 fallback，并用数据定位是复用率不足还是 pool/index 开销过大。

### Phase 5 已完成的代码实现（2026-09-21）

- 新增可切换的 `-cache-key=templated`（模板化键）模式；`packed`（完整打包键）
  不删除，继续作为安全回退和 A/B 对照。二者使用完全相同的 canonical 逻辑，
  因而模板化只改变存储表示，不改变约束语义；
- `CacheRowTemplatePool`（缓存行模板池）只保存 GCD/floor 规约后的稀疏
  `[(local variable ID, coefficient)]`，不保存 RHS。component key 仍保存完整
  domains、每行 `TemplateID`（模板编号）和与其对应的 RHS；
- 每个模板先用 64-bit hash 定位 bucket，再比较完整任意精度字节。hash 相同但
  字节不同的模板不会合并；ID 只在旧模板引用计数为 0 且已经回收后才允许复用；
- lookup 使用 probe-only（只探测）路径。任一行模板不存在即可直接判定 cache
  miss，且不会创建零引用模板；insert 才执行 intern-and-acquire（查重并增加引用）。
  重复 component、超预算拒绝和异常回退都会撤销本次临时引用；
- component entry 被 lazy/hard eviction 或 clear 删除时会同步减少模板 refcount。
  正常情况下 refcount=0 的模板可以暂留供近期复用；发生 byte pressure 时优先回收
  它们，再按 Phase 4 的 insertion-age 淘汰 component；
- 模板记录、模板 hash bucket、ID 索引、component bucket、key、value 和 node
  overhead 全部进入同一个 `CACHE_MAX_MB` 预算。模板池因一次待定插入产生的临时
  对象按未链接 entry 处理；每次公开 cache 操作返回时总驻留字节不超过 hard limit；
- 新增中英含义明确的统计：`row template reuse rate`（行模板复用率）、
  `unique row templates`（唯一行模板数）、`row template pool bytes`（模板池总字节）、
  `direct row structure bytes`（若每个 live key 都直接重复存行结构的反事实字节）、
  `template ID bytes`（模板编号字节）和 `projected templated row bytes`
  （模板池字节 + 模板编号字节）。benchmark summary 也会汇总这些字段；
- 测试覆盖 lookup miss 零写入、强制 hash collision、任意精度 coefficient、
  引用增加/撤销/回收、ID 安全复用、SC 改写后与 GCD/floor key 等价、超预算拒绝
  无泄漏，以及 legacy/canonical/packed/templated 四种模式的 solver differential。

### Phase 5 当前验证结果

- `make test-cache-key` 和 `make test-row-normalizer` 通过；real-solver property
  differential 现在覆盖 1,280 个配置；templated 快速 exact-count 回归 18/18 通过；
  extreme cases 为 35/40，与 Phase 1--4 基线相同，剩余 5 项仍属于输入格式问题；
- 代表实例 `10_7_5.in`（`N*M <= 100`，无 eviction）在 packed 与 templated 下
  都得到 `17,923,488,200`，hits 都为 6,105，live entries 都为 16,650，说明改变的
  是 key 的物理表示而不是命中语义；
- 该实例共产生 67,184 次模板 intern，其中 67,071 次复用，复用率 99.8318%；
  live component 的行结构若直接重复存需要 379,486 B，而模板池 14,425 B 加
  TemplateID 44,338 B，共 58,763 B，行结构部分减少 320,723 B；
- 同一口径下 packed cache 总计 2,983,764 B，templated 为 2,663,041 B，减少
  320,723 B（约 10.75%）。一次本机 RSS 观测为 packed 7.75 MiB、templated
  7.00 MiB，但单次 RSS 会受 allocator/进程环境影响，最终结论仍留到 Phase 6
  用全量 common-success paired benchmark 判断；
- 在 0.05 MiB（52,428 B）、target=0.50、超大 age window 的压力测试中 exact
  count 仍一致；发生 305 次 hard cleanup、55,528 次 hard eviction、回收 1,818
  个模板，reported cache peak 恰为 52,428 B，操作结束后 hard-limit invariant 为真。

## Phase 6：代表性 benchmark 与结论

- [x] correctness 全通过；
- [x] random/application/thin/STN/knapsack 各 10 个代表性快速测例；
- [x] 多个 byte budgets；
- [x] common-success paired metrics 汇总工具；
- [x] solved/TO/OOM/crash/error 分类；
- [x] cache bytes breakdown；
- [x] hit-size distribution；
- [x] Cachet insertion-age/lazy 清理的 eviction 与开销汇总；
- [x] self-contained key vs template pool 配对比较；
- [x] 更新 README 参数和运行方法。

### Phase 6-A：实验基础设施已完成（2026-09-22）

- 新增 `scripts/cache_phase6_benchmark.py`。一个 task 明确定义为同一
  `(dataset, input, byte budget)` 下的一对 `packed`/`templated` 运行；两种模式
  在同一 worker 中前后相邻执行，并按稳定 hash 交替先后顺序，降低固定“谁先跑”
  带来的系统偏差；
- 覆盖 `random`、`application`、`thin`、`STN` 和 `multiple_knapsack`。application
  继续严格按当前一个文件一个实例处理，不再引入旧 case 生命周期；thin 自动使用
  `varlimit=6`，其他数据集使用 `varlimit=14`；
- 支持任意多个 `CACHE_MAX_MB` 预算，默认正式组合为 1/16/256/1024 MiB；也可另跑
  0.05 MiB 强压力组。`N*M` admission threshold、target ratio、age window、timeout、
  virtual-memory limit 和并发数全部写入 campaign metadata；
- 每次运行写入可追加的 `records.jsonl`，中断后用相同命令断点续跑。resume
  fingerprint 包含可执行文件 SHA256 和全部求解/资源参数；参数或二进制不同时拒绝
  混写，避免把不可比较的数据汇总在一起；
- 每个结果区分 solved、timeout、OOM、crash 和普通 error。汇总只在双方都 solved
  且 exact count 相同的 common-success 集合上计算 runtime/RSS/cache ratios；同时
  单列 packed-only solved、templated-only solved 和 count mismatch；
- 自动生成 `paired_cases.csv`、`summary.csv/md`、`cache_bytes_breakdown.csv`、
  `hit_size_distribution.csv`、`correctness_issues.csv`、
  `cache_invariant_issues.csv` 和 `validation.json`。除解数外，还核对 lookup/eligible/
  insert 守恒、无旧式整表 clear、reported peak 不超过 byte limit；
- 五个数据集各 1 个实例、1 MiB 的基础设施 smoke 共 5 对/10 次运行：全部 solved，
  paired count mismatch=0、cross-configuration count issue=0、cache invariant issue=0；
- `10_7_5.in` 的 0.05/1/1024 MiB 三预算配对共 3 对/6 次运行：全部得到
  `17,923,488,200`，无 count/invariant 问题。1024 MiB 无压力时 templated cache
  仍比 packed 少 10.75%；0.05 和 1 MiB 下两者都接近 hard limit，命中与淘汰次序
  会发生变化，单实例结果不足以判断哪一种更快，必须依赖下面的全量实验；
- 不再运行 7,251 个输入的全量组合。新增固定清单
  `benchmarks/order_benchmarks/cache_phase6_representative_10.tsv`，从已有结果中为五类
  数据各选 10 个测例：旧实验必须 solved、DPLL actions 至少 100、运行时间约 5 秒
  以内，并覆盖不同耗时和结构规模。这样排除“根节点直接结束、完全测不到 cache”
  的伪快测例，同时把正式实验缩小到 50 输入 × 4 预算 × 2 模式 = 400 次运行；
- runner 新增 `--selection-file`，逐项校验数据集、文件存在性和重复项；显式清单不能
  与 `--input-limit`/`--file-regex` 混用。清单的精确文件序列也进入 resume
  fingerprint，防止清单内容改变后把新旧结果误混。

### Phase 6-B：50 个代表性快速测例结果（2026-09-22）

- 首轮按旧结果选择后，发现 `thin_6_3.in`、`thin_5_5.in`、`thin_7_4.in` 在当前
  Phase 6 配置下会超时，因此它们只保留为诊断证据，不混入正式快速样本；使用 16 MiB
  小规模 pilot 验证后，以 `thin_3_10.in`、`thin_3_15.in`、`thin_4_16.in` 替换；
- 最终清单五类数据各 10 个，共 50 个输入；1/16/256/1024 MiB 四档预算、packed /
  templated 两种模式共 200 对、400 次运行，400/400 全部 solved；无 timeout、OOM、
  crash、count mismatch、跨预算解数不一致或 cache hard-limit invariant 问题；
- 汇总表中的比值均为 templated / packed。四档预算下 runtime ratio 分别为
  1.0088 / 1.0022 / 1.0137 / 1.0357，即平均表现从几乎持平到约 3.57% 变慢；RSS
  ratio 分别为 0.9934 / 0.9774 / 0.9806 / 0.9808，进程峰值内存降低约
  0.66%～2.26%；
- estimated cache-byte ratio 分别为 0.9667 / 0.8998 / 0.8981 / 0.9024：1 MiB
  压力档只降低 3.33%，其余预算降低约 9.76%～10.19%。16 MiB 下若每个 key 都直接
  重复行结构需 6,215,550 B，模板引用与模板池实际合计 940,864 B，行结构部分降低
  84.86%，但 value、hash node、bucket 等不会随模板化同比下降，因此整个 cache
  只降低约 10%；
- template reuse rate 为 99.61%～99.64%。这说明行模板确实被高度复用，但“复用率
  很高”不自动等于“总内存同比大幅下降”：行结构只占总 cache 的一部分；
- Phase 6 当时的结论是 templated 在快速样本上 RSS 收益较小、并有轻微时间代价；
  最终取舍在 Phase 7 调整为 **templated 默认、packed 自包含回退**，以优先减少 cache
  内部结构重复。Phase 6 的正式原始结果仍保留于
  `result/cache/phase6_representative_50_final/`。

### “差分 ID”和 packed key 的关系

- 已实现 local variable ID difference encoding（局部变量 ID 差分编码）：每个稀疏行
  内的变量编号严格递增，第一个编号直接写入，后续只写“当前编号减前一个编号”；
- 差值再用 ULEB128 可变长整数编码，因此相邻变量编号接近时通常只需 1 byte；
- 这只是 packed physical encoding（紧凑物理编码）的一部分，不等于 canonical key。
  canonical key 指经过 GCD/floor、稀疏化和排序后得到的完整逻辑内容；packed 是把这份
  完整内容无损编码成 bytes；ID 差分只是 packed 在写变量编号时采用的一种压缩方法；
- coefficient、RHS 和 domain 没有做“相邻值差分”，它们使用任意精度整数的
  sign-magnitude（符号 + 长度 + 绝对值字节）无损编码。因而 packed key 仍然
  self-contained：不依赖上一条 cache key，也不需要外部状态才能解读或比较。

## Phase 7：最终 Cachet entry 策略、legacy 清理与默认模式（2026-09-23）

### Phase 7-A：最终缓存管理规则

- 默认 `age_limit` 从 1,048,576 改为 Cachet exact 分支使用的 51,200；
- `clean_limit` 不作为第二个独立参数，而是无溢出地派生为
  `20 * age_limit`，默认 1,024,000 resident entries；
- 新 entry 插入后若 resident entries 达到 clean limit，则保留触发时最近约 1/4；
  hit 和 exact update 都不增加 insertion sequence，也不续龄；
- entry 同时处于两条双向关系中：全局 `older/newer` 插入顺序链，以及 bucket 内
  `bucket_newer/next` 链。这样从 oldest 端删除一个 entry 时，可在 O(1) 时间从其
  bucket 摘除；一次全局清理总复杂度为 O(实际删除的 entry 数)，不再额外扫描所有
  bucket，也不为排序临时分配 entry 数量级数组；
- templated 模式删除一批 entry 时，逐项只减少引用计数；整批结束后统一调用一次
  `reclaimUnreferenced()`，避免每删除一项就扫描/压缩模板池；
- 保留实时/峰值字节统计。`cache-max-mb` 仍可显式设为非零安全阀，但正式默认是 0；
  非零时才按 `target_ratio` 沿 oldest 链继续清理。外部每实例 10 GiB 进程限制不变。

### Phase 7-B：最终三种模式

```text
canonical  规范化逻辑对象；用于检查语义和可读诊断
packed     同一规范化逻辑内容的自包含连续字节；安全回退/A-B 对照
templated  行左侧结构放入共享模板池；component key 保存模板 ID、RHS 和 domains
```

- 三者的数学 identity 都来自同一个 `CacheKeyBuilder`，因此 canonical/packed/
  templated 不是三套不同的 GCD 或约束语义；差别是物理存储方式；
- `templated` 为最终默认，`packed` 保留为不依赖模板池的 self-contained fallback；
- raw `legacy` 的结果文件与 `tests/cache_regression/phase0_legacy_stats.tsv` 保留为历史
  基线。删除代码前最后运行 legacy/canonical 快速 exact-count 对照：两者均 18/18
  通过，legacy eligible/hit/miss 仍与 Phase 0 快照一致；
- 对照通过后删除 `-cache-key=legacy`、`unordered_map<LinearConstraint,...>`、旧
  `LChash` 和 9,000,000 阈值，不再维护四套模式。Phase 8 在 canonical 中恢复了
  “整表 clear”这一旧策略，但阈值统一改为 1,024,000，并非恢复 raw legacy 容器。

### Phase 7-C：templated 的实际数据结构

从逻辑上可以向导师画成下面两层：

```cpp
// 共享一次：相同规范化左侧 terms 只保存一份
struct RowTemplate {
    TemplateID id;
    vector<CanonicalTerm> terms;  // (component-local variable, coefficient)
    uint64_t refcount;
};

// 每个 component cache entry 各自保存
struct TemplatedComponentKey {
    vector<pair<Int, Int>> domains;
    vector<pair<TemplateID, Int>> rows; // (row LHS template, RHS)
};
```

这段是**讲解用逻辑结构**。实际代码为了继续节省对象、指针和小 vector 开销，使用：

```cpp
struct CacheRowTemplatePool::Record {
    CacheRowTemplateID id;
    PackedComponentKey structure;  // term_count + Δlocal_var + coefficient
    uint64_t refcount;
    Record* next;                  // pool hash bucket
    Record* zero_next;             // 待批量回收链
};

struct PackedComponentKey {
    vector<uint8_t> bytes;         // component 的唯一、无损连续编码
    uint64_t hash;                 // 只定位 bucket，不决定命中
};
```

templated component bytes 的确定布局是：

```text
format_version
row_count
sorted delta-coded TemplateIDs
variable_count
all ordered (domain_lower, domain_upper)
all RHS values in the same sorted row order
```

其中 rows 按 `(TemplateID, RHS)` 排序。TemplateID 不是全局变量/全局约束 ID，而是
当前求解实例的模板池句柄；同一个 ID 只表示完整的规范化稀疏左侧
`[(local_var, coefficient), ...]` 完全相同。entry 存活期间 refcount 保证 ID 不失效；
refcount 归零且批量回收后才允许复用。RHS 不在行模板中，所以相同左侧、不同 RHS
共享一个模板，但 component key 仍不同；domains 也完整保留，所以不会错误命中。

### Phase 7-D：packed 连续字节为什么不会歧义

packed 不是把十进制字符串随意连起来，而是一套可唯一分段的二进制语法：

- 每个 list 前写元素数量；
- 每个非零任意精度整数写 `sign tag + magnitude byte length + magnitude bytes`，零只有
  唯一 tag；
- variable ID/TemplateID 的非负差值用 ULEB128，ULEB128 每字节的最高位明确表示
  “后面是否还有字节”；
- 固定 format version 防止未来布局变化后把两代 key 混用；
- hash 只选择 bucket，最终命中比较完整 `bytes`，所以 64-bit hash 碰撞不会误命中。

因此每个逻辑字段都有 tag、长度或 count 边界，canonical 逻辑 key 到 packed bytes
是确定且无损的一对一编码。property test 已检查 300 组系统满足
`logical_key_a == logical_key_b` 当且仅当 `packed_bytes_a == packed_bytes_b`，并用常量
hash 强制碰撞验证仍不会 false hit。

### Phase 7-E：最终验证

- `make test-cache-key` 与 `make test-row-normalizer` 通过；新增小阈值单元测试验证
  `clean_limit=20*age_limit`、触发时 100 项保留最新 25 项、被删/保留边界正确，
  并验证 templated 同批释放的 75 个零引用行模板被一次批量回收；
- legacy 删除前：legacy/canonical 快速回归各 18/18；删除后 canonical/packed/
  templated 快速回归及 property/real-solver differential 继续通过；
- 新策略下重新运行 50 个代表性输入、packed/templated 各一次，共 100/100 solved；
  count mismatch、跨配置 count issue、cache invariant issue 均为 0；
- 本轮默认 `cache-max-mb=0`，50 个快速实例都未达到 1,024,000 resident entries，
  因而全局 entry cleanup 和 byte cleanup 都为 0，这是预期结果；
- 该轮总体 templated/packed：runtime 0.9821、RSS 0.9890、estimated cache bytes
  0.8981、template reuse 0.9963。短实例时间差只作回归观测，不宣称稳定加速；
- 原始结果保存在 `result/cache/phase7_entry_policy_50/`，旧 Phase 0～6 结果均未覆盖。

## Phase 8：canonical 恢复整表清空基线（2026-09-23）

- canonical 恢复原始缓存管理含义：容量达到上限后，下一次符合准入的插入操作前执行整表
  `clear()`，然后存入当前 entry；
- 不再使用历史 `MAX_CACHE=9,000,000`，而是和 Cachet 策略共用无溢出的
  `clean_limit=20*age_limit`；默认 `age_limit=51,200`，因此上限为 1,024,000；
- `cache-max-age-inserts=0` 在 canonical 和 packed/templated 中都关闭全局 entry-count
  trigger；
- canonical 清理后重置当前 key/value 字节估算，但保留 peak；新增 whole-clear runs
  和 evictions 统计，便于解释命中率和运行时间突变；
- packed/templated 行为不变：达到同一阈值后仍沿 oldest 链保留最近约 1/4，不整表
  清空；
- 新增小阈值单元测试：`age_limit=1` 时上限为 20，前 20 项保留，第 21 次插入前
  清除 20 项并只留下新项。

## Phase 9：正式实验统计输出精简（2026-09-23）

- 本阶段早期曾提供 `off/basic/full`；最终版本已经进一步精简并删除该命令行选项；
- 固定输出 lookup/hit/miss/hit-rate、entry 峰值、清理/删除数和模板请求/复用/复用率；
- 删除 conflict、unique-template peak、statistics-consistent，以及 histogram、hash-chain、
  insert/bypass 等开发期终端输出；最终进一步删除未输出的 collision、exact-compare、
  probe 和模板回收等纯诊断计数器；
- `0 <= -1` 等矛盾仍由 key 规范化器防御性识别并直接返回 0，但不属于实际 cache
  lookup，故不再统计；
- 回归脚本继续在内部校验 `lookups = hits + misses`，无需把一致性判断打印给最终用户。

---

# 15. 最终代码边界

最终不把 cache 逻辑堆进 `LinearConstraint`，也不再为四个紧密相关的小模块分别维护
一组 `.h/.cc`。规范化、key、模板池和缓存管理统一收敛到 `cache.h/cache.cc`，内部仍以
不同类和分节保持职责清晰。

最终结构为：

```text
src/
├── big_int.*
├── linear_constraint.*
├── solver.*
└── cache.*
```

职责：

```text
cache.h/cache.cc
    <= row 的 GCD/floor 数学语义
    logical canonicalization
    exact serialization
    hash/equality
    RowTemplatePool 与 TemplateID 生命周期
    ComponentCache lookup/insert
    admission
    age/eviction
    instance lifecycle/release
    最终保留的低开销统计

linear_constraint
    solver state 与 simplification
    cache admission 和调用入口
```

`RowTemplatePool` 与引用其 `TemplateID` 的 `ComponentCache` 位于同一实现文件并保持一致
生命周期；entry 存活期间 ID 不得失效或被复用。单行规范化虽然定义在 `cache.*`，但仍由
RPR 和 cache key builder 共享同一数学实现。

---

# 16. 明确禁止的实现方式

## 16.1 不要在 GCD row 规约中引入 `ceil_div`

本项目 row 是 `<=`，RHS 只用 mathematical floor。

同时不要误删 SB/LP 推导下界所需的 ceiling。

## 16.2 不要用原始 row ID 加一个 SC bool

```text
row_id=17, sc=true
```

不能唯一确定当前 coefficient structure。

## 16.3 不要让模板池掩盖内存

cache entry 变小但 unbounded template pool 变大，不算优化。

## 16.4 不要把 PID/mPID 直接作为新 key 的必需字段

它们当前并非所有 heuristic 都维护，而且会改变旧 key 的变量重命名复用范围。

## 16.5 不要用 `to_double()` 编码或比较 exact key

所有 key 字段必须保持 arbitrary precision exactness。

## 16.6 不要使用无长度、无 tag 的字节拼接

每个列表和 BigInt 都必须有唯一边界。

## 16.7 不要仅凭 hash/fingerprint 返回缓存值

必须 exact compare。

## 16.8 不要把 `N*M` 继续称为 cache memory limit

它只是 admission filter。

## 16.9 最终 managed cache 不要在容量压力下 `cache.clear()`

packed/templated 允许显式 lifecycle reset；不允许用 clear 作为正常 eviction。
canonical 为复现旧管理策略而有意保留整表 clear，仅作为基线对照。

## 16.10 不要只报 aggregate hit rate

必须结合 bypass、每实例分布、hit component size、内存和 paired runtime。

---

# 17. 最终验收条件

## Correctness

- [ ] 所有 regression exact counts 与基线一致；
- [ ] 小实例 brute-force/property tests 通过；
- [ ] forced hash collision 不产生 false hit；
- [ ] cache off/on（Cachet insertion-age）的 count 完全一致；
- [ ] RPR on/off count 一致。

## `<=` normalization

- [ ] 只实现 floor RHS 规约；
- [ ] 负 RHS 正确；
- [ ] solitary row 也真正规约；
- [ ] builder 不依赖 RPR 是否启用；
- [ ] 未破坏 SB/LP 中必要的 ceiling。

## Key exactness

- [ ] row order 不影响 key；
- [ ] 同 LHS 冗余弱 row 被合并；
- [ ] SC 不同结构不会误合并；
- [ ] arbitrary precision 数值无截断；
- [ ] packed bytes 有唯一解析；
- [ ] hash collision 后仍 exact compare。

## Memory management

- [x] 默认 `clean_limit=20*age_limit`，触发后保留最近约 1/4；
- [x] 全局淘汰通过 oldest/bucket 双向链接完成，不扫描整个 bucket 数组；
- [x] packed/templated 正常容量压力不执行整表 `clear()`；canonical 在相同
  1,024,000 默认阈值下整表清空，作为旧策略基线；
- [x] template pool 零引用记录按删除批次统一回收；
- [x] 可选非零 byte hard limit 下，单个超预算 entry 被拒绝且清理回到 target；
- [x] template pool bytes 纳入字节统计及可选同一预算；
- [x] 内部 bytes 与 process RSS 的差异可解释。

## Statistics

- [ ] requests/eligible/bypass/hit/miss 守恒；
- [ ] insert/update/reject/eviction 原因齐全；
- [ ] key/value/overhead/pool 分项内存齐全；
- [ ] per-instance 与 aggregate hit 指标齐全；
- [ ] hit component size 齐全；
- [ ] `Other fail` 有具体 exit code/signal。

## Performance report

- [ ] common-success paired runtime/RSS；
- [ ] solved/TO/OOM/crash；
- [x] 历史 Phase 6 多 byte budget 曲线保留；
- [x] Cachet insertion-age/lazy/entry-limit eviction 均有独立统计；
- [x] template pool 相对 self-contained key 的净内存收益；
- [ ] 不用成功集合不同的简单平均值宣称 speedup。

---

# 18. 后续研究方向

本轮完成后再分别研究：

1. EDPLLSim 一般整数域下、经过严格证明的 residual-capacity/min-activity normalization；
2. domain translation：`[l,u] -> [0,u-l]` 与所有 RHS 的一致平移；
3. PBMC gap saturation 在非 Boolean、含负 coefficient、任意 domain 下的安全推广；
4. cache-aware branching 与已有 DHP/BC/SHP 的组合；
5. probabilistic fingerprint 作为单独的非零错误概率实验；
6. admission policy：encoded bytes、预计求解成本、历史复用率；
7. arena/slab、small-buffer、robin-hood 等容器级优化；
8. 跨变量排列的更强 canonicalization。

这些内容都不能与本轮 exact key 基线混合，否则无法判断收益来源。

---

# 19. 一句话总结

本轮先把 EDPLLSim 从：

```text
完整 LinearConstraint key
+ N*M admission
+ 9,000,000-entry 后整表 clear
```

稳妥地改成：

```text
共享的 <= GCD/floor 语义
+ 内部统一的 exact canonical normalization
+ arbitrary-precision-safe continuous byte encoding
+ exact hash-table equality
+ Cachet exact 的 51,200 age / 1,024,000-entry clean limit
+ oldest 链保留最近约 1/4
+ 单一 RowTemplatePool（TemplateID + RHS）
+ 固定、精简的真实运行统计
+ 完整 paired benchmark
```

实施顺序曾先用 self-contained key 验证 exactness，再接入单一行母版池；最终代码只保留
合并后的模板化路径。原始 row 与 SC 派生 row 都按真实 coefficient structure 进入同一个
池，最终命中仍比较完整 component bytes，不因模板共享而放松精确性。
