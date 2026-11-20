# EDPLLSim Cache 与自适应 K 设计总结及 PBMC 迁移指南

> 用途：把当前 EDPLLSim 的 component cache、SC 兼容、行模板复用、packed 编码、缓存准入和自适应 K 设计整理成一份可直接交给 GPT 阅读的技术上下文，并为后续迁移到 PBMC 提供审计清单。
>
> 事实口径：以当前 `src/cache.*`、`src/linear_constraint.*`、`src/solver.cc`、`src/adaptive_k.*` 为准。历史实验和研发过程来自 `EDPLLSim_Component_Cache_Memory_Optimization_Plan.md`。PBMC 部分以仓库历史中保存的 SAT 2025 PBMC 论文和已有调研记录为依据；当前工作区没有 PBMC 源码，因此所有具体类名和接入点在真正修改 PBMC 前都必须重新核对目标版本。

---

## 1. 先给 GPT 的结论摘要

EDPLLSim 当前只保留一套正式缓存路径：

```text
当前 component
  -> 按 <= 整数语义做 canonical normalization
  -> 每行提取“局部变量位置 + 当前真实系数”
  -> 行结构 packed 序列化并在 RowTemplatePool 中内容寻址
  -> 用 TemplateID 代替重复的行左侧结构，RHS 仍留在 component key
  -> domains + sorted(TemplateID, RHS) 再做连续字节编码
  -> 64-bit hash 定位 bucket，完整 bytes 做最终相等判断
  -> ComponentCache 保存精确计数值
```

这里有三个必须分开的“大小控制”概念：

| 层次 | 当前参数或规则 | 真正含义 |
|---|---|---|
| 单项准入 | `N*M <= cache_admission_max_cells`，默认 20 | 当前 component 值不值得尝试 lookup/insert |
| 单项年龄 | `max_age_inserts`，默认 51,200 | entry 插入后允许经历多少次后续新插入 |
| 全局 entry 清理 | `20 * max_age_inserts`，默认 1,024,000 项 | 驻留项数达到阈值后批量淘汰到约 1/4 |

`N*M <= 20` 不是内存上限；`1,024,000` 也不是单项大小。当前正式实现没有内部字节预算，进程总内存由实验脚本在外部限制，并用 Peak RSS 观测。

SC 会真实修改 coefficient 和 RHS，所以缓存绝不能只记录“原始约束 ID + 是否执行过 SC”。EDPLLSim 直接读取 SC 后的当前真实 row，做规范化和内容寻址；规范化后相同的系数结构共享模板，规范化后不同的结构一定得到不同模板内容。

自适应 K 不是在搜索中不断改变 K，而是在读入原始输入后计算一次固定 K：

```text
原始约束支持集的平均 Jaccard
  -> 用 P/(P+1) 对约束对太少的情况向 0 收缩
  -> K = round_half_up(n0 * (1 - Jreg))，限制在 [1,n0]
  -> 整次求解保持不变
```

当前全部简化技术启用时：`N <= K` 使用非 LP 的 SB/RIR，`N > K` 使用更昂贵的 SB-LP/RIR-LP。因此重叠越高，K 越小，LP 越早介入。

迁移到 PBMC 时，建议迁移“分层设计和验证方法”，不要机械复制 EDPLLSim 的 key 字段、`N*M <= 20` 或 Jaccard-to-K 公式。PBMC 的精确 component identity 是未赋值变量 ID、未满足约束 ID及其当前 gap；它已有 PB 特有的 gap saturation。若 PBMC 约束结构保持 immutable，constraint ID 已经相当于一种天然模板 ID，未必需要再建 EDPLLSim 式行模板池。

---

# 第一部分：Component Cache

## 2. 缓存在求解流程中的位置

当前搜索顺序见 `src/solver.cc`：

```text
count(component)
  -> simplify
  -> 若 simplify 已直接得到精确结果：insertCache(result)，直接返回
  -> 删除完全不出现在任何 row 中的 free variables，得到 factor
  -> findCache(residual component)
  -> 命中：cached residual count * factor
  -> 未命中：decompose 或 branch
  -> insertCache(residual count，不包含 factor)
  -> residual count * factor
```

对应代码位置：

- `src/solver.cc:28-90`：搜索、free-variable factor、lookup 和 insert 的顺序；
- `src/linear_constraint.cc:1132-1155`：删除 free variables 并计算乘法因子；
- `src/linear_constraint.cc:1162-1210`：缓存准入、构造 key、lookup 和 insert。

关键不变量是：

```text
cache value = key 所表示的 residual component 的精确计数
return value = cache value * 已从 key 中删除的 free-variable factor
```

不能把 factor 同时存入缓存又在命中后再乘一次，也不能让 key 删除了 free variable、value 却仍包含不同上下文中的 free-variable factor。

### 2.1 为什么先 simplify 再查缓存

当前 key 表示的是简化后的残余整数线性系统。这样做有四个目的：

1. 不同搜索路径只要化到同一个 residual system 就能命中；
2. 已删除的固定变量、冗余行和自由变量不会反复占用 key；
3. SC、RPR 等产生的等价规约能提高跨路径复用；
4. cache value 对应一个清晰、稳定的数学状态，而不是包含尚未消费的临时 solver 状态。

直接被 `simplify()` 求解的状态仍可插入缓存；非终止状态则在删除 free variables 后 lookup。

---

## 3. 精确 key 的数学身份

逻辑 key 由两部分组成：

```text
1. 当前 component-local 变量顺序下的完整 domains：[(l0,u0),...,(lN-1,uN-1)]
2. 当前所有有效约束的 canonical rows：[(terms0,rhs0),...]
```

其中：

```text
terms = [(local_var_id, nonzero_coefficient), ...]
row   = terms + rhs
```

key 不包含：

- 原始全局变量 `PID`；
- 原始约束 `mPID`；
- branching heuristic 状态；
- `kbest`、`dtree_pos`；
- 简化统计计数。

这些字段会影响搜索过程或诊断，但不改变当前局部整数线性系统的精确模型数。使用 component-local column ID 还保留了旧缓存的变量重命名复用语义：两个来源不同但局部 domains 和矩阵完全相同的 component 可以命中。

### 3.1 每一行的 canonical normalization

EDPLLSim 内部 row 统一表示为：

$$
\sum_i a_i x_i \le b.
$$

对每行计算所有 coefficient 的正 GCD `g`：

```text
a_i <- a_i / g
b   <- floor(b / g)
```

使用 mathematical floor 的原因是变量为整数且关系是 `<=`。例如：

```text
6x + 10y <= 17  <=>  3x + 5y <= 8
6x + 10y <= -3  <=>  3x + 5y <= -2
```

不能对负 RHS 使用向 0 截断，也不能因为项目里其他位置存在下界推导就把这里改成 ceil。

全零 coefficient 行单独处理：

```text
0 <= b, b >= 0  -> tautology，丢弃
0 <= b, b < 0   -> contradiction，直接返回精确计数 0
```

随后：

1. 丢弃零 coefficient，只保留稀疏 terms；
2. 按 `(terms, rhs)` 精确排序，消除 row 输入顺序差异；
3. 相同 primitive LHS 的多条 row 只保留最小 RHS，即最紧的一条；
4. domains 保持当前 component-local 顺序并完整进入 key。

规范化由 `src/cache.cc:13-47` 和 `src/cache.cc:270-327` 实现。RPR 与缓存 builder 共用相同的 `NormalizeLeqRow()` 数学核心，所以即使 RPR 被消融，cache key 仍有相同的 GCD/floor 语义。

### 3.2 为什么 hash 不能代替 key

当前使用稳定的 64-bit FNV-1a 对 serialized bytes 计算 hash。hash 只用于选择 bucket和快速过滤，真正命中仍比较完整 byte vector：

```text
hash 相同 + bytes 完全相同 -> hit
hash 相同 + bytes 不同     -> collision，不是 hit
```

因此 hash 碰撞只影响性能，不影响精确计数正确性。PBMC 迁移时也应保留这一原则，除非明确接受并量化 probabilistic caching 的非零错误概率。

---

## 4. SC 与缓存的关系

### 4.1 SC 改了什么

`coefficientStrengtheningSingle()` 会直接修改当前递归副本中的：

- 某个剩余变量的 coefficient；
- 同一行 RHS；
- 极端情况下 coefficient 可能变成 0。

它不是只改变一个布尔标记，也不是只改变 gap。代码见 `src/linear_constraint.cc:631-681`。

### 4.2 为什么“原始 row ID + SC=true”不够

同一个原始 row 在不同 domains、不同赋值和不同 SC 推导下可能得到多个不同的剩余系数结构：

```text
original_row_id = 17, SC = true
```

无法唯一确定最终的 coefficient vector 和 RHS。若把它当 key，会把数学上不同的 residual rows 错误合并，造成 false hit 和错误计数。

### 4.3 当前正确处理方式

缓存 builder 不依赖 SC 的历史，只读取 SC 完成后的当前真实 matrix：

```text
当前 row
  -> GCD/floor normalization
  -> 稀疏 (local_var, coefficient)
  -> packed row bytes
  -> 内容寻址得到 TemplateID
```

结果是：

- SC 后 canonical terms 相同：row bytes 相同，可复用同一模板；
- SC 后 canonical terms 不同：row bytes 不同，绝不会只因原始 ID 相同而合并；
- SC 后再次出现公因数：cache-only normalization 仍能把整数语义等价形式统一；
- SC 改变 lexicographic row order：builder 自己排序，不依赖 solver matrix 恰好有序。

### 4.4 迁移 PBMC 时要先问的问题

PBMC 论文中的 component key 使用 constraint ID，是因为原始 PB constraint 的 coefficient structure 通常是固定的，当前 residual 状态用“未赋值变量 + constraint ID + gap”恢复。

迁移前必须确认目标 PBMC 版本是否存在会永久改变剩余 coefficient structure 的技术：

```text
若 coefficient structure immutable：constraint ID 可以充当天然模板 ID。
若只改变 gap：保留 constraint ID，并把 canonical gap 放入 key。
若会生成或改写 derived constraint：必须给 derived structure 内容寻址，
不能继续只依赖 original constraint ID。
```

这正是 EDPLLSim 的 SC 经验对 PBMC 最重要的迁移价值。

---

## 5. 行模板复用设计

### 5.1 模板保存什么，为什么不保存 RHS

`RowTemplatePool` 的模板只保存：

```text
[(component-local variable position, normalized coefficient), ...]
```

RHS 不在模板里，而在每个 component key 中保存。

原因是搜索中的固定变量删除、赋值和 SC 经常只让 RHS 改变，左侧 coefficient structure 仍可能在许多 component 中重复。把 RHS 放进模板会把“同左侧、不同 RHS”的复用拆散；不保存 RHS 又不会放松 key，因为最终 key 仍包含每行 RHS。

### 5.2 一个池处理原始行和派生行

当前没有 `OriginalTemplatePool` 和 `SCTemplatePool` 两套结构。所有 row 都经过同一个内容寻址入口：

```text
原始 row
删固定变量后的 row
SC 派生 row
GCD 规约后的 row
       -> 同一个 RowTemplatePool
```

模板的身份来自当前 canonical contents，不来自 row 的来源。这降低了实现复杂度，也允许不同来源但最终结构相同的 row 真正共享。

### 5.3 lookup 只 probe，insert 才 intern

两条路径有意不同：

```text
lookup:
  normalize row -> serialize structure -> probe existing template
  只要一条模板不存在，现有 cache 中就不可能有这个完整 key，直接 miss
  不创建模板，不增加 refcount

insert:
  normalize row -> serialize structure -> internAndAcquire
  已存在则复用并增加 refcount；不存在则创建新模板
```

这样避免每次 cache miss 都向模板池永久加入从未被任何 cache entry 引用的结构。实现见 `src/cache.cc:145-174`。

### 5.4 refcount、回收和 ID 复用

每个模板记录维护 refcount：

- 新 component entry 接管 key 引用；
- entry 删除时，从 component bytes 开头解析 delta-coded TemplateIDs 并减少引用；
- refcount 变为 0 的模板进入待回收链；
- 一批 entry 删除完成后统一回收零引用模板；
- 旧 record 完全不存在且不可能再被 live key 引用后，ID 才能进入 free list 重用。

这保证 `TemplateID` 在任何 live cache key 存活期间都不会指向另一种 row structure。

模板池与 component cache 都是 per-instance 生命周期。新输入开始时先让旧 cache 释放所有模板引用，再清模板池并重新绑定；不跨 instance 持久化 TemplateID。

### 5.5 模板复用解决的不是“相同 component 重复存储”

hash table 本来就只为一个 exact component 保存一个 entry。模板池节省的是：

```text
多个不同 component entry 之间重复出现的 row coefficient structure
```

所以高 cache hit rate 不能证明模板池有效，真正应看：

- row template requests；
- row template reuses；
- reuse rate；
- 模板池开销 + key 内 TemplateID 开销；
- 相比每个 key 重复存 row structure 的净字节收益；
- 最终进程 Peak RSS 和 runtime。

历史代表实验曾观察到约 99.6% 的模板复用率、整个 cache 估算字节下降约 10%，但这不是“总内存下降 99%”：row structure 只是 entry、value、bucket、node、allocator 等总开销的一部分。

---

## 6. packed 功能的准确含义

当前最终实现不再暴露 `canonical`、`packed`、`templated` 多模式。它内部同时包含：

1. canonical normalization：决定两个 component 何时数学等价；
2. packed serialization：把逻辑字段无损编码成连续 bytes；
3. template reuse：把重复 row LHS 换成 per-instance TemplateID。

因此“packed”是物理编码，不是独立的数学等价规则。

### 6.1 任意精度整数编码

所有 domain bound、coefficient 和 RHS 都是 GMP 任意精度整数，编码为：

```text
zero:     tag 0
positive: tag 1 + magnitude_byte_count(ULEB128) + magnitude bytes
negative: tag 2 + magnitude_byte_count(ULEB128) + magnitude bytes
```

不能截断到 32/64 bit，不能用 `double`，不能用没有长度边界的十进制字符串拼接。实现见 `src/cache.cc:208-236`。

### 6.2 row template bytes

```text
term_count
(first local_var_id, coefficient 0)
(delta local_var_id 1, coefficient 1)
(delta local_var_id 2, coefficient 2)
...
```

局部变量位置严格递增，所以相邻 ID 用 ULEB128 差分编码通常更短。见 `src/cache.cc:249-267`。

### 6.3 final component bytes

当前 format version 为 2，布局是：

```text
format_version
row_count
sorted delta-coded TemplateIDs
variable_count
ordered domain lower/upper pairs
all RHS values in the same sorted row order
```

rows 先按 `(TemplateID, RHS)` 排序。每个列表有 count，每个 BigInt 有 sign/length，因而字段边界唯一，不会出现拼接歧义。

注意：最终 component bytes 依赖当前 instance 的模板池解释 TemplateID，所以它不是可跨进程、跨实例持久化的 self-contained key。若以后要落盘或跨实例共享，必须改用自包含 row bytes，或连同稳定、版本化的模板字典一起持久化。

### 6.4 为什么保留 format version

如果未来修改字段顺序、整数编码或加入新 identity 字段，version 可以阻止两代 byte grammar 被误认为同一个 key。改变 key 语法时必须同步升级 version，并清空旧 cache；不能在一个实例中混用两种布局。

---

## 7. 缓存准入：`N*M <= 20`

这是迁移时最需要单独理解的一层。

### 7.1 当前代码的精确定义

令：

```text
N = 当前 component 的变量数
M = 当前 component 的 row 数
cells = uint64_t(N) * uint64_t(M)
```

lookup 和 insert 在构造 canonical key 之前都执行：

```text
cache_admission_max_cells <= 0 -> 完全关闭 component cache
cells > threshold              -> bypass，不 lookup、不 insert
cells <= threshold             -> 允许构造 key 并访问 cache
```

默认：

```text
threshold = 20
```

所以 `N=4,M=5` 可以进入，`N=5,M=5` 不能进入；边界 20 是包含的。命令行 `-cache-limit=K` 只是 `-cache-admission-max-cells=K` 的兼容别名。

被 bypass 的状态不计入当前 `Cache lookups/hits/misses`，因此打印的 hit rate 是“通过准入且成功构造 key 的请求”的条件命中率，不是所有递归状态的全局命中率。

### 7.2 为什么使用 `N*M`

它是一个在 key build 之前就能 O(1) 得到的粗粒度成本代理：

1. 原始 solver state 是 `M x (N+1)` 的 dense matrix；
2. canonicalization 至少需要扫描 row 中的 coefficient，成本大致随 `N*M` 增长；
3. 未压缩 key 的逻辑体量也与矩阵规模相关；
4. 先拒绝大 component 可以避免为明确不缓存的状态支付规范化、序列化、hash 和模板访问成本；
5. 小 component 通常 key 较小，递归中也更容易由不同路径再次出现，因此适合作为保守的优先缓存集合。

第 5 点是启发式倾向，不是定理。大的 component 一旦命中可能节省更多搜索工作，因此准入永远是“单项成本、复用概率、命中收益”的折中。

### 7.3 `N*M` 不能表示什么

`N*M` 不是：

- packed key 的真实 bytes；
- `nnz` 非零 coefficient 数；
- GMP 大整数 payload 大小；
- 模板池和 bucket/node 开销；
- cache 的总驻留容量；
- 该 component 的求解难度；
- 未来会不会命中；
- 命中后能节省多少搜索。

两个 `N*M` 相同的状态可能一个极稀疏、系数很小，另一个很稠密且含超大整数；它们的 packed bytes 和构造成本会差很多。

因此代码和论文中都应把它称为 `admission threshold/filter`，不要称为 `cache memory limit`。

### 7.4 为什么默认是 20

当前代码事实只能支持以下谨慎结论：

- 20 来自早期 `MAX_MATRIX=20` 的保守默认，并在后续 cache 重构中为了兼容性和实验可比性保留下来；
- 它不是从 packed key 字节公式推导出来的；
- 它不是正确性边界；改成 0、20、100 只应改变性能和内存，不应改变 exact count；
- 现有历史实验说明 20 是一个有实际收益但仍有内存风险的工作点，不能证明 20 对所有数据集最优。

历史 random 实验记录如下，结果来自旧 cache 阶段，且 `Other fail` 未完全分类：

| admission | Success | Timeout | OOM/kill | Other fail | 聚合 hit rate |
|---:|---:|---:|---:|---:|---:|
| 0 | 1409 | 1431 | 0 | 0 | 0% |
| 20 | 1735 | 190 | 390 | 525 | 95.8679% |
| 100 | 1707 | 0 | 539 | 594 | 86.6719% |

这组数据最多说明：

1. cache 明显有用；
2. 允许更多 component 会改变 timeout/OOM 的权衡；
3. `<=20` 的 eligible 请求聚合 hit rate 很高；
4. `<=100` 的成功实例看起来更快，但成功集合不同，存在 survival bias；
5. 20 和 100 都不能代替全局容量管理；
6. 不能仅凭聚合 hit rate 宣称 20 最优。

### 7.5 PBMC 不应复制 `N*M <= 20`

PBMC key 的物理成本更接近：

```text
unassigned variable IDs
+ unsatisfied constraint IDs
+ gaps
+ 各列表的差分/bit packing 开销
```

它不是保存一个 `N x M` dense coefficient matrix。因此直接套 `N*M` 会错配 PBMC 的真实 key 大小和 build 成本。

PBMC 更合理的候选准入指标应按下面顺序实验：

1. 零成本 proxy：`num_vars + num_constraints`；
2. gap-aware proxy：`num_vars + num_constraints + total_gap_bit_width`；
3. 已完成 build 后的精确 packed bytes；
4. bytes 再结合历史复用率或预计重算工作量；
5. 必要时做两级准入：先用便宜 proxy 拒绝明显大项，再对边界项按真实 bytes 决定。

不能只优化 entry bytes。准入的评估指标至少应包含：

```text
bypass 数
eligible lookup 数
按 size bucket 的 hit rate
hit component size
key build time
cache resident/peak bytes
process Peak RSS
runtime
solved / timeout / OOM / crash
共同成功实例上的 paired 比较
```

---

## 8. insertion-age、lazy cleanup 和全局淘汰

### 8.1 年龄定义

每次真正插入一个新 exact key 时：

```text
insertion_sequence += 1
entry.created_sequence = insertion_sequence
age = current_insertion_sequence - created_sequence
```

默认 `max_age_inserts = 51,200`。stale 条件是严格大于：

```text
age > max_age_inserts
```

即 entry 可以经历恰好 51,200 次后续新插入，第 51,201 次后才被判 stale。

特殊值 `max_age_inserts=0` 不表示关闭 cache：它使任何经历过后续插入的非匹配旧项立即 stale，同时因为派生的全局 clean limit 为 0 而关闭全局 entry-count trigger。真正关闭 cache 使用 admission threshold 0。

hit 和 exact-key update 都不增加 insertion sequence、不续龄、不移动 entry。原因是保持策略简单、避免每次命中修改全局顺序，并与 Cachet 的 insertion-age 设计一致。

一个实现细节：lookup 先检查 exact equality，再检查 stale。因此如果被查询的就是旧 entry，它仍可命中；stale 主要用于在扫描未命中的 bucket chain 时清掉已经太旧的后缀。它不是严格的墙钟 TTL。

### 8.2 bucket 内 lazy cleanup

同一 bucket 的 entry 按新到旧排列。lookup/insert 只扫描当前触及的 bucket：

```text
遇到第一个 stale 且不匹配的 entry
  -> 该 entry 后面一定更旧
  -> 删除它和整个 older suffix
```

优点是平时不用扫描整张 hash table。代价是从不再访问的 bucket 不会仅靠 lazy cleanup 主动清理，所以还需要全局 entry-count trigger。

### 8.3 全局清理阈值为什么是 `20 * age`

当前实现派生：

```text
clean_limit = saturating_multiply(max_age_inserts, 20)
```

默认：

```text
51,200 * 20 = 1,024,000 resident entries
```

新 entry 插入后，若驻留数达到阈值，就从全局 oldest 端删除，直到只保留触发时约 1/4：

```text
keep = max(1, entries_current / 4)
```

这里的乘数 20 是 Cachet 式 entry 管理参数，不是 admission 的 20。两者数字相同只是碰巧，单位和作用完全不同：

```text
admission 20              -> matrix cells / component
age multiplier 20        -> resident entries / age window
```

entry 同时连接在：

- bucket 内的 `bucket_newer/next` 链；
- 全局 `older/newer` 插入顺序链。

因此从 oldest 端删除时能 O(1) 从对应 bucket 摘除，批量清理复杂度与实际删除项数成正比，不需要再扫描全部 bucket。

### 8.4 为什么不在 hit 时续龄

设计理由是：

1. hit 路径不需要调整链表，开销稳定；
2. bucket 的新到旧顺序只由插入建立，stale suffix 删除逻辑简单；
3. 防止极少数热点 entry 永久改变全局清理顺序；
4. 策略与参考实现一致，便于对照。

这不表示 no-renew 一定优于 LRU。若 PBMC 已有成熟的访问年龄或 pinning 规则，应做同预算 A/B，而不是为了代码一致性强行替换。

### 8.5 当前没有内部 byte hard limit

研发过程中曾实现 `cache-max-mb`、target ratio 和逐项字节核算，但最终正式代码已删除。当前只有：

- 单 component 的 `N*M` admission；
- entry insertion-age；
- `20*age` 的 resident-entry 清理；
- 外部进程内存限制；
- OS 报告的 Peak RSS。

迁移 PBMC 时可以重新引入 byte budget，但必须把所有相关内存都纳入同一口径，包括 bucket、entry node、packed payload、model count 大整数、模板/constraint dictionary 和 allocator overhead；否则“key 变小”可能只是把内存藏到池里。

---

## 9. 生命周期与统计

### 9.1 生命周期

当前正式路径是一进程一个 instance：

```text
initializeCacheForInstance()
  -> 旧 component entries 先释放模板引用
  -> 清 RowTemplatePool
  -> 新建 ComponentCache 并绑定新 pool
  -> 重置统计
```

TemplateID 只在当前 instance 内有意义。跨 instance 复用必须重新设计稳定字典，不能直接保留 ID。

### 9.2 正式保留的统计

每次运行输出：

```text
Cache lookups
Cache hits
Cache misses
Cache hit rate
Peak number of cache entries
Number of cache cleanup
Number of deleted entries
Row template requests
Row template reuses
Row template reuse rate
Peak RSS (MB)
```

解释时注意：

- `lookups = hits + misses`；
- disabled/admission-bypass 不进入 lookups；
- builder 发现 contradiction 时直接返回 0，也不计实际 lookup；
- template request/reuse 统计的是 insert-time intern，不是 lookup probe；
- Peak RSS 是整个进程，不是 cache 独占内存。

---

## 10. Cache 每项设计及理由总表

| 设计 | 当前做法 | 主要理由 | 迁移 PBMC 时是否直接复用 |
|---|---|---|---|
| cache 时机 | simplify 后，free-variable 删除后查 residual | 提高规范化复用，value 语义清晰 | 复用原则，重查 PBMC propagation/backjump 时机 |
| exact identity | domains + canonical current rows | 精确计数不能遗漏影响模型数的状态 | 不直接复用字段；PBMC 用 var IDs + constraint IDs + gaps |
| local variable ID | component-local column | 保留变量重命名复用，不依赖 PID | PBMC 通常使用稳定全局 ID，不能照搬 |
| GCD/floor | `<=` 整数行除 coefficient GCD，RHS floor | 合并整数语义等价形式 | PBMC 用自己的 gap/constraint 等价规则 |
| SC | 读取当前真实 coefficient structure | SC bool/原始 ID 不足以唯一标识 | 若 PBMC 改写系数，必须采用同类内容寻址 |
| row template | 只共享 LHS terms，RHS 留在 key | RHS 常变，LHS 高复用，同时保持 exact | constraint ID 可能已是天然模板；先测再加池 |
| lookup probe-only | lookup 不创建模板 | 防止 miss 污染模板池 | 推荐复用 |
| packed bytes | count/tag/length + ULEB128 + BigInt | 小、连续、无歧义、任意精度安全 | 可复用工程原则，编码可按 PBMC bit packing 调整 |
| hash + exact compare | hash 定位，bytes 判等 | 碰撞不影响 exactness | 必须复用 |
| admission | `N*M <= 20` | 便宜的 pre-build 成本代理，保守限制大项 | 不复制 20；改为 PBMC key/cost proxy |
| entry age | 51,200 次后续新插入 | 低成本衡量 entry 新旧 | 可作为候选，与 PBMC 原策略 A/B |
| hit renew | 不续龄 | hit 路径简单，保持链顺序 | 不必强行复用 |
| global cleanup | `20*age` 后留最新约 1/4 | 清理冷 bucket，避免整表 clear | 可复用结构，参数必须重新实验 |
| byte budget | 当前无内部 hard limit | 最终以外部限制和 RSS 为准 | PBMC 可恢复，但要完整核算 |
| pool lifetime | per instance，refcount + batch reclaim | 防 dangling ID 和跨实例污染 | 若新增派生模板池，必须复用 |

---

## 11. Cache 正确性不变量和测试清单

迁移或修改时至少保留以下测试：

当前工作区的 `README.md`/`Makefile` 仍提到历史上的 `test-row-normalizer`、`test-cache-key` 和 cache regression，但当前 `HEAD` 的 `tests/` 中没有这些测试源码。后续正式改 cache 前，应先从对应历史版本恢复或按下面的不变量重建测试，不能把历史文档中的“曾通过”当作当前分支仍可执行的回归保障。

### 11.1 key 数学语义

- row permutation 得到同 key；
- 相同 LHS 的弱 RHS 行被丢弃；
- 改一个 domain、coefficient 或必要 RHS 必须得到不同 key；
- GCD/floor 对正负 RHS 都正确；
- `0<=b` 的 tautology/contradiction 正确；
- arbitrary-precision 字段不截断；
- format 有唯一解析；
- constant hash 强制碰撞时仍无 false hit。

### 11.2 SC/template

- 两个 SC 结果只有在当前 canonical terms 完全相同时才共享 TemplateID；
- lookup miss 不增长模板池；
- insert/update 的引用数守恒；
- entry 淘汰后零引用模板被回收；
- live key 引用期间 ID 绝不复用；
- instance reset 后不复用旧 TemplateID 解释新 key。

### 11.3 value 与搜索

- cache off/on exact count 一致；
- free-variable factor 只乘一次；
- decomposition product 和 branching sum 的缓存值正确；
- contradiction 不作为普通 miss 继续搜索；
- backjump 或状态撤销后不保留无效 component/value。

### 11.4 eviction

- hit 和 exact update 不续龄；
- bucket 始终新到旧；
- stale equality-first 行为与预期一致；
- lazy cleanup 只删 stale suffix；
- `clean_limit=20*age` 无整数溢出；
- 全局清理保留正确的最新 1/4；
- 批量删除后 template refcount 守恒。

---

# 第二部分：自适应 K

## 12. K 在当前简化框架中的作用

K 是“便宜的非 LP 简化”与“昂贵但能联合多约束推理的 LP 简化”之间的 component-size 阈值。

在全部相关技术启用时：

| 当前 component 变量数 | bound strengthening 阶段 | individual-row-removal 阶段 |
|---|---|---|
| `N <= K` | SB | RIR |
| `N > K` | SB-LP | RIR-LP |

一个细节是 RV 位于两个阶段之间：

```text
进入 simplify 时的 N -> 决定 SB 或 SB-LP
执行 RV 删除固定变量
RV 之后的当前 N     -> 决定 RIR 或 RIR-LP
```

消融开关仍可跳过对应技术；特别是禁用 RIR-LP 时，大 component 也回退到基础 RIR。因此 K 是默认技术选择阈值，不是强制覆盖消融配置的总开关。

固定模式由 `-varlimit=K` 指定，默认 14。自适应模式由 `-adaptive-k=1` 开启，读入原始输入后覆盖 `variable_limit`，整次搜索保持固定。

---

## 13. 自适应 K 的最终公式

设原始输入有 $n_0$ 个变量、$m_0$ 条约束，原始 coefficient matrix 为 $A=(a_{ij})$。

第 $i$ 条约束的非零变量支持集：

$$
S_i=\{j\mid a_{ij}\ne0\}.
$$

任意无序约束对的 Jaccard：

$$
J_{ir}=\frac{|S_i\cap S_r|}{|S_i\cup S_r|}.
$$

约束对数量：

$$
P=\frac{m_0(m_0-1)}2.
$$

包含零重叠 pair 在内的平均值：

$$
J=\frac1P\sum_{i<r}J_{ir}.
$$

约束对数正则化后的分数：

$$
J_{\mathrm{reg}}
=\frac{P}{P+1}J
=\frac{\sum_{i<r}J_{ir}}{P+1}.
$$

最后：

$$
K=\min\!\left(n_0,\max\!\left(1,
\left\lfloor n_0(1-J_{\mathrm{reg}})+\frac12\right\rfloor
\right)\right).
$$

取整是非负数的 nearest-integer half-up；恰好 `.5` 时取较大整数。

边界：

- `n0 <= 0`：实现返回 K=0；
- `m0 < 2`：没有 pair，定义 `J=Jreg=0`，所以 K=n0；
- pair 的 union 为空：该 pair 贡献 0，但仍计入 P；
- 正常非空输入最终限制 `1 <= K <= n0`。

代码日志仍叫 `JConf`，论文表述更建议叫 `Jreg` 或 pair-count-regularized Jaccard，因为它不是统计学置信概率。

---

## 14. 每一步的设计理由

### 14.1 为什么看约束支持集重叠

LP 的潜在优势是联合考虑多条约束。如果两条约束没有共享变量，它们之间缺少直接的跨行信息通道；共享变量越多，LP 越可能通过联合关系推导出单行区间法得不到的界或冗余性。

这里必须保持证据边界：共享变量只表示“存在联合推理机会”，不保证 LP 一定更强，更不保证总时间更短。系数符号、系数大小、RHS、domains、重复行和数值稳定性都会影响真实收益。

### 14.2 为什么使用 Jaccard

只用交集大小会偏爱宽约束。Jaccard 用并集归一化：

```text
intersection / union
```

使窄约束和宽约束的重叠都落在 `[0,1]`，并且共同的零 coefficient 不产生虚假相似度。

### 14.3 为什么平均全部 pair

把零重叠 pair 也放进分母，分数同时反映：

- pair 内重叠强度；
- 重叠在整个约束系统中的普遍程度。

局限是稀疏链式图可能全局连通，但多数非相邻 pair 的 Jaccard 为 0，所以 mean Jaccard 可能低估链式结构。

### 14.4 为什么乘 `P/(P+1)`

Random 实验曾发现：当只有两条很宽且高度相似的约束时，原始 Jaccard 会被单个 pair 完全支配，使 K 过小、LP 介入过早。

`P/(P+1)` 等价于在 P 个真实 Jaccard 项之外加入一个值为 0 的虚拟参考项：

```text
m0=2,  P=1  -> 原分数乘 1/2，修正明显
m0=3,  P=3  -> 乘 3/4
m0=10, P=45 -> 乘 45/46，几乎保留原分数
```

它也可由“真实 pair 的平方误差 + 一个单位零中心正则项”求极值得到。当前取单位强度是为了不再引入一个需要按数据集调参的连续超参数。

这仍是项目自己的启发式设计，不是已有论文证明的最优系数，也不能把 P 称为独立样本量。

### 14.5 为什么线性映射到 K

`K_raw=n0(1-Jreg)` 是满足目标方向的最简单单调映射：

- `Jreg=0`：K=n0，优先非 LP；
- 重叠升高：K 单调降低，LP 覆盖更小 component；
- 分数接近 1：K 接近 1；
- 不引入新的分段阈值。

线性映射不是最优 K 的理论推导，必须靠固定 K 对照、消融和跨数据集实验验证。

### 14.6 为什么 K 的下界是 1

若 K=0，所有非空 component 都满足 `N>K`，连单变量 component 也会调用 LP。单变量状态没有多变量耦合，基础区间法通常足够，构建 LP 很难得到额外联合推理收益。

K=1 已经让所有 `N>=2` 的 component 有机会使用 LP，同时保留最小的非 LP 区间。K=0 可以作为人工消融，不作为正常自适应输出。

### 14.7 为什么只在根输入计算一次

当前实现只读取原始 coefficient matrix，不运行 SB/RIR root probe，也不在搜索中更新 K。这样做的工程收益是：

1. 决策开销可单独计时；
2. 不把一次昂贵简化探测混进选择器；
3. 同一 instance 的策略稳定、可复现；
4. 避免“先运行简化才能决定是否运行简化”的循环依赖；
5. 便于与固定 K 做严格 A/B。

代价是根结构可能不能代表深层 component；未来可以研究 per-component controller，但那是新算法，不能和当前 root-fixed K 混称为同一设计。

---

## 15. 实现细节和复杂度

`src/adaptive_k.cc` 使用 bitset 保存每行支持集：

```text
word_count = ceil(n0 / 64)
```

每个 pair 用 bitwise AND/OR + popcount 得到 intersection/union。大致复杂度：

```text
构造 supports: O(m0*n0)
两两比较:     O(m0^2 * ceil(n0/64))
内存:         O(m0 * ceil(n0/64)) words
```

所有 pair fraction、pair sum、Jreg 和 K 选择在 GMP exact rational/integer arithmetic 中完成。`double` 只用于输出 `J` 和 `JConf`；决定 K 的 rounding 不依赖浮点误差。

half-up 在整数上直接计算：

$$
\left\lfloor\frac{2n(den-num)+den}{2den}\right\rfloor.
$$

这避免 `.5` 附近因 double rounding 选择不同 K。

---

## 16. 自适应 K 的已知局限

当前结构指标忽略：

- coefficient 符号；
- coefficient 相对 RHS/degree 的大小；
- domain 宽度；
- 当前 gap 或 slack；
- 重复/近重复约束的信息增益；
- incidence graph 的路径、连通性、社区和树宽；
- LP 数值可表示性与求解成本；
- 深层 component 的结构变化。

所以应表述为：

> Jaccard-regularized K 是一个用原始支持集重叠预测“LP 联合推理机会”的确定性启发式。

不应表述为：

> 高 Jaccard 保证 LP 更快，或公式理论上给出最优 K。

---

# 第三部分：迁移到 PBMC

## 17. PBMC 与 EDPLLSim component identity 的差异

PBMC 论文给出的精确 component 身份是：

```text
unassigned variable IDs
+ yet-to-be-satisfied constraint IDs
+ each constraint's current gap
```

PBMC 将 variable/constraint IDs 排序后做差分和 bit packing。对正 gap 可保存 `gap-1`；clausal constraint 可省略 gap。它还在生成 cache ID 时做 PB 特有的 gap saturation：若 `0 < gap < min coefficient of remaining unassigned literals`，把 cache ID 中的 gap 规范到该最小 coefficient，因为任一剩余 literal 取真都会让约束满足，两个 gap 对剩余模型集合等价。

这和 EDPLLSim 的对应关系是：

| EDPLLSim | PBMC |
|---|---|
| component-local domains | unassigned Boolean variable IDs/assignment state |
| 当前 canonical coefficient rows | immutable constraint IDs + remaining variables |
| RHS | current gap/degree residual |
| GCD/floor normalization | PB gap saturation 等 PB 等价规约 |
| row TemplateID | fixed constraint ID 可能已经承担同类角色 |
| arbitrary-precision varint bytes | ID delta + bit packing + gap encoding |

### 17.1 可直接迁移的设计原则

- 先定义并证明 exact component identity，再谈压缩；
- normalization、serialization、cache manager 分层；
- hash 只定位，完整 packed contents 判等；
- packed grammar 有唯一边界和版本；
- lookup 不应为了 miss 创建永久辅助对象；
- entry 淘汰必须同步释放辅助池引用；
- admission 与全局容量是两套参数；
- cache off/on、packed/unpacked、优化 on/off 必须 exact-count differential；
- 用 paired runtime/RSS，而不是只看 aggregate hit rate。

### 17.2 不能直接迁移的部分

- EDPLLSim 的 `domains + current matrix` 字段；
- component-local variable renaming 语义；
- `<=` row 的 GCD/floor；
- `N*M <= 20`；
- GMP BigInt 编码，如果 PBMC 字段有更小的已证明范围；
- 行模板池：constraint ID 可能已经消除了 coefficient structure 重复；
- free-variable factor 的具体实现；
- 51,200、20 倍、保留 1/4 等参数值；
- Jaccard-to-K 公式。

---

## 18. PBMC cache 建议迁移阶段

### Phase 0：只审计，不修改

让 GPT 在目标 PBMC 源码中回答：

1. component 在 propagation 前还是后 lookup；
2. key 当前包含哪些字段；
3. constraint ID 是否对应 immutable coefficient structure；
4. learned/derived constraints 是否能进入 component；
5. backjump 会删除哪些 cache entry；
6. gap 何时更新、何时 saturation；
7. packed key 是否 exact compare；
8. cache value 是否包含 free-variable 或权重因子；
9. 当前 admission、容量和淘汰策略；
10. 当前统计能否区分 bypass、eligible、hit、eviction 和 bytes。

### Phase 1：冻结 exact baseline

- cache off/on count 一致；
- gap saturation on/off count 一致；
- 保存当前 hit/miss、runtime、RSS、solved 分类；
- 对小 PB 公式做 brute-force differential；
- 注入 constant hash 验证 collision 不 false hit。

### Phase 2：只改 packed representation

- 逻辑 identity 不变；
- 明确 version、field counts、delta 和 gap 编码；
- 验证 logical equality 当且仅当 packed bytes equality；
- 比较 payload、capacity、node 和 RSS。

### Phase 3：迁移 cache manager

- 若现有整表 clear 造成性能突变，再引入 insertion-age + lazy suffix；
- 先保持现有 admission，隔离淘汰策略影响；
- 小阈值单测验证 oldest/newest 和 backjump interaction；
- 若 PBMC 的 recursive component 有 pin/ancestor 依赖，不能淘汰仍被递归栈引用的 entry。

### Phase 4：决定是否需要模板池

先测：

```text
constraint structure 是否已经只存一份
key 中是否只存 constraint ID
derived constraint structure 是否高复用
额外 pool + refcount 的净 bytes 是否小于节省值
```

若固定 constraint ID 已经是稳定模板，不要再加第二层 RowTemplatePool。只对无法由稳定 ID 表示、且确实高复用的 derived structures 内容寻址。

### Phase 5：重新设计 admission

对下列策略做相同预算、相同机器的 paired 实验：

```text
cache off
all components
num_vars + num_constraints 阈值
packed bytes 阈值
两级 proxy + bytes
可选的 predicted-work / bytes score
```

阈值必须由 PBMC 自己的数据选，不能继承 EDPLLSim 的 20。

### Phase 6：再研究 adaptive simplification

只有在 PBMC 源码中找到明确的“两种等价但成本/强度不同的技术”后，才为它设计自适应开关。不要为了迁移 K 而人为制造一个没有算法意义的 K。

---

## 19. 如何判断 PBMC 是否有适合自适应的简化技术

对每项候选技术填写下面的表：

| 问题 | 需要的答案 |
|---|---|
| 是否有 cheap/local 与 expensive/global 两种 exact 版本 | 没有则不需要 K 型选择器 |
| 两种版本的切换现在是否靠固定阈值 | 有固定阈值才是最直接迁移目标 |
| expensive 版本的收益来自跨约束联合信息吗 | 是则支持集/incidence 指标可能相关 |
| 成本主要由变量数、约束数、nnz、gap 还是 coefficient bits 决定 | 决定阈值的 size axis |
| 技术是在根运行一次还是每个 component 运行 | 决定 root-fixed 还是 per-component controller |
| 选择错误只影响性能还是会影响正确性 | 自适应器必须只选择已证明 exact 的路径 |
| 是否能单独计时和统计约简收益 | 不能测就无法训练/验证启发式 |

### 19.1 可迁移的是“自适应设计模板”

若找到候选技术，可以复用下面的方法：

1. 明确 cheap 与 expensive 方法的 exact 等价性；
2. 找到与 expensive 方法收益机制有关的结构特征；
3. 对特征做规模归一化；
4. 对证据过少的输入做保守正则；
5. 用最简单单调映射产生阈值；
6. 给阈值合法上下界；
7. 选择一次还是动态更新要与实际作用域一致；
8. 决策时间与正式简化时间分开统计；
9. 用固定阈值网格验证 adaptive choice 是否落在有效区域；
10. 跨数据集评估，避免只在公式设计集上验证。

### 19.2 PBMC 中 Jaccard 能否直接使用

只有当候选昂贵技术的收益确实来自“多条 PB constraints 共享未赋值变量”时，支持集 Jaccard 才有机制对应。但 PB 约束比 EDPLLSim 更适合考虑额外特征：

- literal polarity；
- coefficient/degree 比；
- 当前 gap；
- 已满足与未满足约束比例；
- incidence graph 连通性；
- constraint width 和 coefficient bit width。

建议先把原始 Jaccard 当可解释 baseline，再逐项做消融，不要一开始就组合复杂指标。若候选技术主要成本来自 gap propagation 或 coefficient arithmetic，而不是跨约束联合推理，Jaccard 可能不是合适的特征。

---

## 20. 给 GPT 的 PBMC 分析提示词

可以把本文件和 PBMC 源码一起提供给 GPT，并使用下面的任务描述：

```text
请先只分析，不修改代码。

你需要以本文件中的 EDPLLSim 设计为参考，审计当前 PBMC 的：
1. component identity、key build、packed layout、hash/equality；
2. variable IDs、constraint IDs、gap 与 gap saturation 的精确语义；
3. lookup/insert 在 propagation、decomposition、branch、backjump 中的位置；
4. cache value 是否含 free-variable/weight factor；
5. admission、总容量、eviction、entry lifetime 和统计；
6. immutable original constraints 与 learned/derived constraints 的区别；
7. 是否已经通过 constraint ID 实现了模板复用；
8. 是否存在类似 EDPLLSim“cheap local vs expensive global”的两套简化技术，
   以及它们是否由固定 size threshold 控制。

输出要求：
- 每个结论给出 PBMC 文件、函数和行号；
- 分成“可直接复用、必须重设计、不建议迁移、信息不足”四类；
- 不要直接复制 N*M<=20、age=51200 或 Jaccard K；
- 先给 exactness invariant，再给内存优化；
- 给出 Phase 0 baseline、最小改动 Phase 1、测试矩阵和 benchmark 指标；
- 如果 PBMC 会改写 coefficient structure，明确指出 constraint ID 是否仍足够；
- 如果 constraint ID 已是稳定模板，解释为什么不需要第二个行模板池；
- 找不到适合自适应的双技术选择点时，明确回答“不应迁移 K”。
```

---

## 21. 源码索引

### 当前实现

- `src/cache.h`：canonical 数据结构、serialized key、RowTemplatePool、ComponentCache 接口；
- `src/cache.cc:13-47`：`<=` row GCD/floor normalization；
- `src/cache.cc:121-175`：模板引用排序与 final component serialization；
- `src/cache.cc:208-267`：ULEB128、BigInt、hash、row template serialization；
- `src/cache.cc:270-337`：component canonicalization 和 lookup/insert builder；
- `src/cache.cc:341-674`：模板池、refcount、回收和 ID 复用；
- `src/cache.cc:677-1023`：insertion-age cache、lazy cleanup 和全局淘汰；
- `src/linear_constraint.cc:631-681`：SC；
- `src/linear_constraint.cc:1132-1210`：free variable、admission、cache 调用；
- `src/linear_constraint.cc:1232-1278`：统计和 per-instance 初始化；
- `src/solver.cc:28-90`：缓存值在 DPLL 计数流程中的语义；
- `src/adaptive_k.cc:17-150`：精确 Jaccard-regularized K；
- `src/main.cc:158-220,304-332`：参数解析、一次性 K 决策和日志。

### 解释与历史记录

- `EDPLLSim_Component_Cache_Memory_Optimization_Plan.md`：cache 从 raw key 到最终统一实现的研发记录；
- `Adaptive_K_Jaccard_Confidence_Formula.md`：K 公式、证据边界和论文表述；
- `Adaptive_K_Jaccard_Confidence_Simple.md`：K 公式简版；
- `Adaptive_K_Ablation_Commands.md`：自适应 K 与简化技术消融配置；
- `README.md`：当前 CLI、cache 和 K 的用户口径。

---

## 22. 最终迁移原则

一句话概括 cache：

> EDPLLSim 先把当前真实 residual system 做 exact canonicalization，再用 packed bytes 和共享行模板减少重复，最后用 admission 与 insertion-age 管理成本；SC 的历史不进入 key，SC 后的真实数学内容才进入 key。

一句话概括 K：

> 自适应 K 用根输入的约束支持集重叠估计 LP 联合推理机会，只选择一次 cheap/non-LP 与 expensive/LP 的切换阈值，并用 pair-count regularization 防止极少数高重叠 pair 让选择过度激进。

一句话概括 PBMC 迁移：

> 迁移 exactness、分层、packing、lifetime、统计和实验方法；重新证明 PBMC 的 identity，保留 gap saturation，先确认 constraint ID 是否已是模板，再重做 admission 和自适应技术选择，绝不机械复制 EDPLLSim 的常数与字段。
