# 基于约束支持集重叠与约束对数正则化的自适应 K

> 实现中的命令行名称仍是 `jaccard-confidence`，但论文中建议把
> $P/(P+1)$ 称为“约束对数正则化（pair-count regularization）”，而不要称为
> 严格统计意义上的“置信度”。系数矩阵是完整观测，$P$ 个约束对也不是相互独立
> 的随机样本。

## 1. 一句话设计原理

共享变量为 LP 跨约束联合推理提供结构通道，因此用约束支持集的平均 Jaccard
重叠衡量这种机会；再用一个单位强度的约束对数正则项保守处理只有极少约束对
的输入，最后让重叠分数越高时 K 越小，使 LP 覆盖更多分量。

这是一条由 LP 机制启发、由实验验证的确定性启发式，不是已有论文给出的最优
K 定理。

## 2. 最终公式与 K 的算法含义

设原始输入有 $n_0$ 个变量、$m_0$ 条约束，系数矩阵为 $A=(a_{ij})$。约束
$i$ 的非零变量支持集为

$$
S_i=\{j\mid a_{ij}\ne0\}.
$$

任意两条不同约束 $i,r$ 的支持集 Jaccard 重叠为

$$
J_{ir}=\frac{|S_i\cap S_r|}{|S_i\cup S_r|}.
$$

令所有无序约束对的数量为

$$
P=\frac{m_0(m_0-1)}{2},
$$

并对所有约束对取平均，包括重叠为 0 的约束对：

$$
J=\frac1P\sum_{i<r}J_{ir}.
$$

若一对约束的并集为空，则该项按 0 计。若 $m_0<2$，定义 $J=0$。对平均
Jaccard 施加约束对数正则化：

$$
\boxed{
J_{\mathrm{reg}}
=\frac{P}{P+1}J
=\frac{\sum_{i<r}J_{ir}}{P+1}
}.
$$

最后只在读入原始约束系统时计算一次固定 K：

$$
\boxed{
K=\min\!\left(n_0,\max\!\left(1,
\left\lfloor n_0(1-J_{\mathrm{reg}})+\frac12\right\rfloor
\right)\right)
}.
$$

$\lfloor z+1/2\rfloor$ 的含义是把非负数 $z$ 取到最近的整数；恰好处于两个
整数中间时取较大整数。最外层只保证 $1\le K\le n_0$。

在当前 EDPLLSim 中，K 的方向必须解释清楚：

|当前分量变量数|所选技术|
|---|---|
|$N\le K$|非 LP 的 SB 和 RIR|
|$N>K$|SB-LP 和 RIR-LP|

所以 $J_{\mathrm{reg}}$ 越大，K 越小，LP 适用的分量范围越大；反之 K 越接近
$n_0$，越优先使用开销较低的非 LP 版本。

## 3. 公式是怎样一步一步设计出来的

下面严格区分“文献事实”“机制推理”和“本文设计”。

### 3.1 为什么需要在非 LP 与 LP 之间选择

**[文献事实]** Bestuzheva 等人在 OBBT 部分明确区分了逐个处理非线性的 FBBT
与同时考虑约束系统松弛的 OBBT：后者可能得到更紧的界，但需要更高计算开销。
其 LP 实现还会针对每个候选变量分别求最小化和最大化 LP。

**[对本框架的对应]** EDPLLSim 的 SB/RIR 使用较局部的非 LP 判断，而
SB-LP/RIR-LP 求解由约束系统构造的 LP。于是 K 实际控制的是一个
“局部简化开销”与“联合约束推理能力”之间的选择。

文献支撑的是这种强度—开销权衡，不是某个固定 K，也不是 Jaccard 公式。

### 3.2 为什么考察约束之间的共享变量

**[文献事实]** Gemander 等人指出，单独检查行或列只使用局部信息，削减通常
较弱；同时检查两行或更大部分可能得到更强削减，但运行开销也会提高。该文的
特定 two-row bound-tightening 方法还要求约束共享变量，并进一步依赖共享变量
系数的符号关系。

**[机制推理]** 如果两组约束的变量支持集完全不相交，约束—变量二部图分成
独立块。一个块内变量的 LP 界不需要另一个独立块的约束；因此不存在直接的跨块
联合推理。约束共享变量后，一行中的界或线性关系才可能通过公共变量影响另一行。

例如

$$
x+y\ge10,\qquad x-y\ge0,\qquad 0\le x,y\le10.
$$

单独看任意一行都不能推出 $x\ge5$，而联合两行可得到 $2x\ge10$。这里共享的
$x,y$ 是跨行推理的通道。

**必须保留的边界：**共享变量只说明 LP 存在联合推理机会，不保证一定产生更强
约简，更不保证总运行时间一定缩短。重复约束即使 $J_{ir}=1$，也可能完全不增加
新信息；系数符号、数值、右端项、变量界和数值条件也是本指标没有描述的因素。

### 3.3 为什么用 Jaccard，而不是只数共享变量

**[已有定义]** Jaccard 系数用集合交集相对于集合并集的比例衡量两个集合的共同
程度。

**[本文迁移]** 本文把两个集合替换为两条约束的非零变量支持集：

- 分子 $|S_i\cap S_r|$ 是直接的跨行连接通道数；
- 分母 $|S_i\cup S_r|$ 按这两行实际作用范围归一化；
- 两条窄约束和两条宽约束因此处于同一个 $[0,1]$ 尺度；
- 共同的零系数不会产生虚假相似度。

使用并集归一化还能避免“约束越宽，仅因包含变量更多就天然得到越高分”的偏差。
但是，把 Jaccard 用作 LP 介入代理量是本文的建模选择；Jaccard 的原始论文只定义
集合共同度，并没有讨论 LP。

### 3.4 为什么对全部约束对取平均

平均值 $J$ 回答的是：任取一对不同约束，它们的有效作用范围平均有多大比例
重叠。把零重叠对也计入分母，使分数同时反映重叠强度与重叠在整个系统中的普遍
程度，而不是只看最相似的少数行。

这也是本文设计，不是 Gemander 等论文规定的聚合方式。它有明确局限：链式 STN
可以在约束图上全局连通，但大量不相邻约束对的 Jaccard 为 0，因此 Mean-Jaccard
可能低估这种稀疏的全局连通性。本文只能通过 STN 实验检查该局限，不能声称平均
Jaccard 完整刻画了约束图结构。

### 3.5 为什么还要乘 P/(P+1)

这个系数不是因为“约束少就一定不适合 LP”。两个约束也可能产生很强的联合
推理。它解决的是更窄的问题：当 $m_0=2$ 时只有 $P=1$ 个约束对，一个很大的
$J_{12}$ 会直接成为整个系统的高分，并使 K 大幅降低。现有 Random 实验中确实
观察到了“只有两条很宽且相似的约束”触发过于激进 K 的失效模式。

令 $j_1,\ldots,j_P$ 表示全部 $P$ 个 Jaccard 项，对最终结构分数 $d$ 使用单位
强度、零中心的二次正则：

$$
d^*=\arg\min_d\left[
\sum_{p=1}^{P}(d-j_p)^2+d^2
\right].
$$

求导并令其为 0：

$$
2(P+1)d-2\sum_pj_p=0,
$$

所以

$$
d^*=\frac{\sum_pj_p}{P+1}
=\frac{P}{P+1}J
=J_{\mathrm{reg}}.
$$

它也等价于在 $P$ 个真实重叠项之外加入一个重叠为 0 的虚拟参考项。更一般的
正则形式是

$$
J_{\lambda}=\frac{P}{P+\lambda}J.
$$

当前取 $\lambda=1$，含义是采用一个单位零基准，不引入需要针对数据集调整的新
连续参数。它使 $m_0=2$ 时原分数减半，而约束对增多后快速回到原 Jaccard：

|$m_0$|$P$|$P/(P+1)$|
|---:|---:|---:|
|2|1|0.500|
|3|3|0.750|
|5|10|0.909|
|10|45|0.978|

**[跨领域方法先例]** Koren 使用
$n/(n+\lambda)$ 将低支持量的相似度向 0 收缩；Herlocker 等也用 significance
weighting 降低少量共同观测产生的相关系数权重。这两篇论文支撑“小支持量的高
相似度应保守处理”的一般思想，Koren 还提供了同型函数。

**[证据边界]** 两篇论文研究的是协同过滤，不是约束系统。它们没有把
$P=m_0(m_0-1)/2$ 当作支持量，也没有规定 $\lambda=1$。这里的 P 只是参与
平均的可观察约束对数量，不能称为“独立样本量”。$P$ 的定义、单位正则以及
它在 LP 选择中的用途，都是本文提出并需要通过消融实验验证的选择。

### 3.6 为什么线性映射到 K

$$
K\approx n_0(1-J_{\mathrm{reg}})
$$

是满足以下边界行为的最简单单调映射：

- $J_{\mathrm{reg}}=0$ 时，$K=n_0$，根分量优先使用非 LP 技术；
- 分数增大时 K 单调减小，LP 逐步适用于更小的分量；
- 分数接近 1 时 K 接近 1，除单变量分量外优先使用 LP；
- 不再引入一个需要按数据集调节的分段阈值。

取最近整数只是把连续值确定性地变成整数，外层上下界保证 K 合法。这个线性
映射不是最优 K 的理论推导，也没有论文直接证明；其合理性必须由手动 K 覆盖率、
实际成功数、PAR-2 和跨数据集实验支撑。

## 4. 文献逐条核查卡

下面给出导师下载论文后可以直接定位的位置。短原句只用于 Ctrl+F 核验；论文中
引用时仍应结合上下文，不应只摘一句话。

### 4.1 LP 联合约束更强但更贵

**论文：** K. Bestuzheva, A. Chmiela, B. Müller, F. Serrano,
S. Vigerske, and F. Wegscheider, “Global optimization of mixed-integer
nonlinear programs with SCIP 8,” *Journal of Global Optimization*, 91,
287–310, 2025（online 2023）。

- DOI：[10.1007/s10898-023-01345-1](https://doi.org/10.1007/s10898-023-01345-1)
- 官方全文：[Springer PDF](https://link.springer.com/content/pdf/10.1007/s10898-023-01345-1.pdf)
- 准确位置：Section 2.7 “Optimization-based bound tightening”，期刊
  p. 299，PDF 第 13 页，第一、二段。
- Ctrl+F：`OBBT considers`；`two LPs that minimize and maximize`
- 短原句：`OBBT considers (a relaxation of) all constraints together`
- **直接支撑：** OBBT 联合约束，可能比逐个传播得到更紧界，但开销更高；每个
  候选变量需要最小化和最大化 LP。
- **不支撑：** 高 Jaccard 必然使 LP 更快或更有效。

### 4.2 多行推理、共享变量及运行开销

**论文：** P. Gemander, W.-K. Chen, D. Weninger, L. Gottwald,
A. Gleixner, and A. Martin, “Two-row and two-column mixed-integer presolve
using hashing-based pairing methods,” *EURO Journal on Computational
Optimization*, 8, 205–240, 2020。

- DOI：[10.1007/s13675-020-00129-6](https://doi.org/10.1007/s13675-020-00129-6)
- 官方全文：[Springer PDF](https://link.springer.com/content/pdf/10.1007/s13675-020-00129-6.pdf)
- 位置 A：Section 1，p. 206，讨论单行局部信息、多行强度和运行开销。
- 位置 B：Section 3.3，Lemma 3.1 及其后解释，p. 213。
- 位置 C：Section 4.1，p. 228；Section 4.2，p. 229。
- Ctrl+F：`limited to local information`；`variables shared by the constraints`
- 短原句 A：`limited to local information and thus often yields weak reductions`
- 短原句 B：`variables shared by the constraints have coefficients with opposite sign`
- **直接支撑：** 多行方法可利用单行没有的跨约束信息；共享变量及其系数符号对
  特定 two-row 方法有实际意义；更广的联合处理伴随额外开销。
- **不支撑：** 无符号 Mean-Jaccard 是任意 LP 收益的充分条件。该论文反而说明
  符号是本公式遗漏的重要因素。

### 4.3 Jaccard 系数的首次定义

**论文：** P. Jaccard, “Distribution de la flore alpine dans le Bassin des
Dranses et dans quelques régions voisines,” *Bulletin de la Société Vaudoise
des Sciences Naturelles*, 37(140), 241–272, 1901。

- DOI：[10.5169/seals-266440](https://doi.org/10.5169/seals-266440)
- 原始扫描：[ETH/E-Periodica PDF](https://www.e-periodica.ch/cntmng?pid=bsv-002%3A1901%3A37%3A%3A745)
- 准确位置：p. 249，脚注 1。
- Ctrl+F/OCR：`Pour évaluer la proportion d'espèces communes`
- 原文数例：共同 295 种，两表合计去重后 525 种，即 $295/525\approx56\%$，
  对应 $|A\cap B|/|A\cup B|$。
- **直接支撑：** 交集除以并集的集合共同度定义。
- **不支撑：** 约束支持集、LP 适用性、约束对平均或 K 映射。

**引用纠错：**经常被引用的同年论文 “Étude comparative de la distribution
florale dans une portion des Alpes et du Jura,” pp. 547–579，DOI
`10.5169/seals-266450` 使用了该系数，但不是首次定义。导师核验公式时应下载上面
241–272 页的 “Distribution ...” 论文。

可用 J. Podani, “The wonder of the Jaccard coefficient: from alpine floras to
bipartite networks,” *Flora Mediterranea*, 31, 105–123, 2021，
[DOI 10.7320/FlMedit31SI.105](https://doi.org/10.7320/FlMedit31SI.105)，
p. 108，交叉核验这段出版史。

### 4.4 同型的支持量收缩函数

**论文：** Y. Koren, “Factorization Meets the Neighborhood: a Multifaceted
Collaborative Filtering Model,” *KDD 2008*, 426–434。

- DOI：[10.1145/1401890.1401944](https://doi.org/10.1145/1401890.1401944)
- 可下载全文：[Cornell PDF](https://www.cs.cornell.edu/courses/cs6241/2020sp/readings/Koren-2008-Factorization.pdf)
- 准确位置：Section 2.2 “Neighborhood models”，p. 427，PDF 第 2 页，
  Equation (2)。
- Ctrl+F：`greater user support are more reliable`；
  `shrunk correlation coefficient`
- 短原句：`similarities based on a greater user support are more reliable`
- 原公式：

$$
s_{ij}=\frac{n_{ij}}{n_{ij}+\lambda_2}\rho_{ij}.
$$

- **直接支撑：** 支持量较少时将相似度向 0 收缩，以及
  $n/(n+\lambda)$ 的函数形式。
- **不支撑：** 约束对数量 P、$\lambda=1$ 或 LP/K。Koren 的
  $n_{ij}$ 是共同评分用户数，$\lambda_2$ 由交叉验证确定，文中典型值为 100。

### 4.5 小支持量相似度降权的实证结果

**论文：** J. L. Herlocker, J. A. Konstan, and J. Riedl,
“An Empirical Analysis of Design Choices in Neighborhood-Based Collaborative
Filtering Algorithms,” *Information Retrieval*, 5(4), 287–310, 2002。

- DOI：[10.1023/A:1020443909834](https://doi.org/10.1023/A:1020443909834)
- 可核验全文：[PDF 镜像](https://ptacts.uspto.gov/ptacts/public-informations/petitions/1522255/download-documents?artifactId=EIVCWJzbInP3JWTjTo8m_9QD-tqSbOXAJwNi7Lg1Db6PZ844E5M_EwI)
- 位置 A：Table 2，p. 293，列出 `n/50 weighting`。
- 位置 B：Section 5.2 “Significance weighting”，pp. 295–297；具体
  $n/50$ 构造在 p. 296，Figure 2 结果在 p. 297。
- 位置 C：Section 8 总结及 Table 8，p. 308。
- Ctrl+F：`based on a very small number of co-rated items`；
  `It is important to use a significance weight`
- 短原句：`devalue similarity weights that were based on a small number of co-rated items`
- **直接支撑：** 小量共同观测产生的高相似度可能不可靠，应按支持量降权。
- **不支撑：** $P/(P+1)$。该文采用的是阈值截断的线性权重
  $\min(n/50,1)$，应用域也不是约束系统。

### 4.6 二次正则化的一般数学来源（可选引用）

**论文：** A. N. Tikhonov, “On the Solution of Ill-Posed Problems and the
Method of Regularization,” *Doklady Akademii Nauk SSSR*, 151(3),
501–504, 1963。

- 原文入口：[Math-Net](https://www.mathnet.ru/eng/dan28329)
- 准确位置：Section 2，p. 502，Equation (2)，使用“数据误差项 + 正则项”的
  一般形式 $M^\alpha=N+\alpha\Omega$。
- **直接支撑：** 通过附加惩罚项选择稳定解的正则化范式。
- **不支撑：** 本文具体的 $d^2$ 单位权重、$P/(P+1)$、$\lambda=1$ 或 LP/K。
  本文的闭式系数来自第 3.5 节写出的具体目标函数及其求导，而不是从 Tikhonov
  论文直接复制。

## 5. 证据边界总表

|论断|性质|支撑程度及出处|
|---|---|---|
|LP 联合考虑约束，可能得到更紧的界但更贵|文献事实|直接：Bestuzheva Sec. 2.7, p. 299|
|单行只含局部信息；多行可产生更强削减|文献事实|直接：Gemander Sec. 1, p. 206|
|共享变量为特定多行推理提供作用通道|文献事实 + 机制推理|较直接：Gemander Lemma 3.1, pp. 213, 228–229|
|交集/并集是 Jaccard 共同度|已有定义|直接：Jaccard p. 249 脚注 1|
|Mean-Jaccard 能预测 EDPLLSim 中 LP 的净收益|本文假设|无论文直接证明；需实验|
|低支持量的相似度应保守降权|跨领域方法先例|直接用于相似度：Koren Eq. (2)；Herlocker Sec. 5.2|
|数据拟合项可配合正则惩罚项|一般数学范式|直接：Tikhonov Sec. 2, Eq. (2)；具体惩罚仍为本文选择|
|用 $P=m_0(m_0-1)/2$ 表示正则支持量|本文设计|无直接论文；不是独立样本量|
|$P/(P+1)$ 和单位强度 $\lambda=1$|本文设计|由本文二次正则目标推出；文献只提供同型先例|
|$K=n_0(1-J_{\mathrm{reg}})$ 的线性映射|本文设计|无直接论文；最简单单调边界映射|
|公式在本求解器上有效|实验结论|由本文消融、手动 K 和跨数据集实验支撑|

最稳妥的论文表述是：

> 本方法依据“共享变量为 LP 跨约束推理提供机会”这一机制，用平均 Jaccard
> 构造归一化结构代理量，并通过约束对数正则项保守处理极小约束系统。具体正则
> 系数和 K 映射属于本文启发式，其合理性由消融和跨数据集实验验证，而非由文献
> 宣称为最优。

## 6. 代表性测例

### 6.1 Random：11_2_11.in

该测例有 $n_0=11$、$m_0=2$、$P=1$、$J=9/11$：

$$
J_{\mathrm{reg}}=\frac12\cdot\frac9{11}=\frac9{22},
\qquad
11\left(1-\frac9{22}\right)=6.5.
$$

中点取较大整数得到 K=7，而未正则化的 Jaccard 给出 K=2。现有手动成功 K
包括 6、8、10、12、14、15、16、18、20。必须准确写成：K=7 位于最小与最大
已测试成功 K 的包络内，但 K=7 本身没有经过该组手动定值实验，不能称为“精确
命中一个已测试成功 K”。

这个测例解释了正则项的目标：只有一个高重叠约束对时，不让该单对相似度直接
产生极端小 K；它不能证明所有两约束测例都不适合 LP。

### 6.2 STN：STN_T1_13.in

该测例有 $n_0=13$、$m_0=156$，所以 $P=12090$。未正则化的
$J=47/465\approx0.1010753$，正则系数为 $12090/12091$，因此

$$
J_{\mathrm{reg}}\approx0.1010669,
$$

K 仍为 12。本机全简化技术验证约 0.02 秒完成。该例说明当约束对很多时正则项
几乎不改变原 Jaccard；STN 的主要改善来自按每对约束自身并集归一化，而不是
$P/(P+1)$。

同时要承认，Mean-Jaccard 并不能完整刻画 STN 的链式全局连通性；这里 K=12
恰好让根分量 $N=13>K$，使 LP 分支介入，是实验结果而非一般定理。

### 6.3 Thin

Thin 的 180 个测例离线重算后仍全部为 K=1。这是跨数据集稳定性的实验依据，
不是公式本身的数学保证。

## 7. 实验解释中必须避免的说法

- 不要写“论文证明了 $P/(P+1)$ 最优”。
- 不要写“约束少就不适合 LP”。
- 不要把 P 称为独立样本量或把 $J_{\mathrm{reg}}$ 称为置信概率。
- 不要写“Jaccard 越高，LP 一定越快”；它只表示联合推理机会。
- 不要把“位于最小—最大成功 K 之间”写成“该 K 已成功”。
- 不要仅在设计公式所使用的 Random 测例上报告结果；应同时给出 STN、Thin、
  Application、MK，并将公式设计集与最终评估的角色说明清楚。
- 所有时间与超时成功数比较必须使用相同的超时、线程策略和 `MAX_JOBS`。

## 8. 实现

```text
-adaptive-k=1
```

自适应 K 现在只保留这一种公式：

- 只读取原始输入的非零支持集；
- 不执行 SB/RIR 根探测；
- 用精确有理数计算 $J$、$J_{\mathrm{reg}}$ 和 K；
- 只选择一次固定 K，求解过程中不再变化；
- 正式搜索中的全部简化技术仍按固定 K 正常执行。

日志中的 `JConf` 对应本文建议记为 $J_{\mathrm{reg}}$ 的同一个数：

```text
[INFO] Adaptive K: n0=... m0=... J=... P=... JConf=... K=...
```
