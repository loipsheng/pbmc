# PBMC 文件与目录逐项说明

本文是一张面向后续开发的 PBMC 源码地图。它回答两个最容易混淆的问题：

1. 一个文件是做什么的；
2. 它现在是否真的参与 PBMC 的编译和运行。

本文中的“每个文件”是指仓库自有源码、构建脚本、文档和第三方组件的源码文件。以下内容不会逐个列出：`.git/` 内部对象、`build/` 生成的每个 CMake 临时文件、Python 的 `__pycache__/`，以及 `benchmarks/` 中成千上万个同类测例；这些目录会解释其整体结构和命名规则。

## 1. 阅读文件表之前先分清五种身份

| 标记 | 含义 | 是否应直接修改 |
|---|---|---|
| **活动源码** | 顶层 CMake 当前实际编译，或者运行时会调用 | 是，日常开发主要修改这里 |
| **覆盖文件** | `pbmc-mod/`、`roundingsat-mod/` 中用于重建仓库的补丁副本 | 不直接参与编译；活动源码验证后要同步到这里 |
| **旧代码/可选代码** | 从 GPMC、Glucose 或其他项目保留下来，但 PB 主路径不一定调用 | 先确认调用关系再改 |
| **第三方代码** | FlowCutter、RoundingSat、MiniSat 模板库、MPFR 包装器等 | 必要时才改，并保留许可证 |
| **生成物/数据** | 可执行文件、编译缓存、论文、测试数据 | 不作为算法源码修改 |

还要记住 C++ 文件的一般规则：

- `.h`、`.hpp`：声明类型、字段、函数接口和模板；
- `.cc`、`.cpp`：实现非模板函数；
- 某些模板为了让编译器看到完整定义，会全部写在 `.h/.hpp` 中。

## 2. PBMC 是怎样由几个代码来源拼起来的

当前工作目录不是一个只含 PBMC 自有文件的“小型源码树”，而是一个组装后的完整工程：

```text
GPMC 上游完整源码
    + pbmc-mod/ 中的 PBMC 覆盖文件
    + RoundingSat 上游完整源码
    + roundingsat-mod/ 中的适配覆盖文件
    + FlowCutter、MPFR C++ 包装等依赖
    = 当前根目录下可编译的 core/、preprocessor/、roundingsat/……
```

`setup-complete-code.sh` 会先复制 `*-mod/`，再用上游文件补齐缺少的文件。因此：

- `core/Counter.cc` 是当前真正编译的活动文件；
- `pbmc-mod/core/Counter.cc` 是重建工程时用的持久覆盖副本；
- 两份文件长期不一致时，重新执行 setup 可能丢失只存在于活动源码中的修改。

日常推荐流程是：修改活动源码、编译测试、确认正确，再同步到相应的 `*-mod/` 相对路径。

## 3. 仓库根目录

### 3.1 根目录文件

| 文件 | 身份 | 作用 |
|---|---|---|
| `.gitignore` | 工程配置 | 告诉 Git 忽略数据集、编译目录、可执行文件、临时上游克隆等大文件或生成物。它不影响编译。 |
| `CMakeLists.txt` | **活动构建入口** | 定义 `pbmc` 静态库和最终可执行文件，列出参与编译的 `.cc` 文件，并链接 RoundingSat、FlowCutter、zlib、GMP、MPFR 等库。判断“某个 `.cc` 是否被编译”时先看这里。 |
| `Makefile` | **日常构建入口** | 对现有脚本进行易记封装，提供 `make`、`make debug`、`make static`、`make clean`、`make rebuild`、`make run`、`make test` 等目标。它最终仍会调用 CMake/脚本。 |
| `build.sh` | **实际构建脚本** | 创建构建目录、运行 CMake，再调用底层 `make`。参数 `r`/`d`/`rs` 分别用于 release、debug、release static 等构建。 |
| `clean.sh` | 日常清理脚本 | 删除 `build/`、`bin/` 及其 debug/static 变体等编译产物。改代码后通常不需要先清理，直接 `make` 可增量编译；构建状态异常时才清理。 |
| `clean-up.sh` | **完整源码清理脚本** | 为重新组装工程而删除 `core/`、`roundingsat/`、`preprocessor/` 等完整源码和临时克隆。它远比 `clean.sh` 激进，不应作为日常 `make clean` 使用。 |
| `setup-complete-code.sh` | **工程组装脚本** | 克隆固定版本的 GPMC 与 RoundingSat，把 `pbmc-mod/`、`roundingsat-mod/` 先覆盖进去，再补齐上游文件，形成可编译源码树。只在首次安装或需要重建完整源码时运行。 |
| `README.md` | 用户文档 | 中文构建、运行、参数、数据和开发入门说明。 |
| `PBMC_CURRENT_ARCHITECTURE_CN.md` | 架构文档 | 重点解释 PBMC 与 RoundingSat/GPMC 的连接方式、递归计数位置、树分解与 VCIS 的组合关系。 |
| `PBMC_CODE.md` | 扩展文档 | 围绕递归简化、预处理器、启发式和缓存四个计划修改点讲解代码。 |
| `PBMC_SIMPLIFICATION.md` | 技术文档 | 解释负文字、PB 约束正规化、等式、简化正确性、冲突学习以及简化技术落地注意事项。 |
| `PBMC_FILE_AND_DIRECTORY_GUIDE_CN.md` | 本文档 | 逐目录、逐文件说明整个仓库。 |
| `LICENSE` | 法律文件 | PBMC 顶层代码的许可证声明；重分发时不可随意删除。 |

### 3.2 根目录文件夹

| 目录 | 身份 | 作用 |
|---|---|---|
| `core/` | **PBMC 活动核心** | 主程序、OPB 实例、top-down 精确计数、分量分解、缓存和启发式选择。后续研究改动最集中的目录。 |
| `roundingsat/` | **活动第三方核心** | PB 传播、冲突检测、PB 冲突分析、学习约束、回跳和变量 activity。PBMC 的 `PBcounter` 继承它的 `Solver`。 |
| `preprocessor/` | 混合目录 | 包含 GPMC 遗留的 CNF 预处理器，也包含 PBMC 当前会使用的树分解与 FlowCutter 接口。不能把整个目录都理解成“PBMC 当前预处理器”。 |
| `pbmc-mod/` | **覆盖文件** | PBMC 相对 GPMC 修改过的文件快照，供 setup 组装使用，本身不由顶层 CMake直接编译。 |
| `roundingsat-mod/` | **覆盖文件** | PBMC 相对 RoundingSat 修改过的文件快照，作用同上。 |
| `ddnnf/` | 可选功能 | d-DNNF 决策图/枚举实验代码。当前主要计数流程不依赖 Python 枚举器。 |
| `flow-cutter-pace17/` | 第三方库 | 用 FlowCutter 计算图分隔和树分解，为树分解启发式提供结构信息。 |
| `mtl/` | 第三方基础库 | MiniSat Template Library，提供向量、堆、区域分配器等底层容器，主要服务于 GPMC/Glucose 旧代码。 |
| `utils/` | 活动公共工具 | 命令行选项、流式解析、CPU 时间和内存统计。 |
| `mpfr/` | 第三方头文件 | MPFR 的 C++ 包装 `mpreal`，用于需要高精度实数的加权计数类型。 |
| `benchmarks/` | 数据 | 论文实验用 OPB 数据、转换后的 CNF 数据、压缩归档等。 |
| `paper/` | 论文 | PBMC 论文 PDF。 |
| `scripts/` | 辅助脚本 | 当前包含数据集统计脚本，不参与求解器编译。 |
| `AAAI_EDPLLSim/` | 独立参考项目 | 另一个 EDPLLSim 工程，可作为简化、缓存、启发式设计参考，但不链接进 PBMC。 |
| `temp-source-clones/` | 上游参考/临时目录 | setup 下载的 GPMC 与 RoundingSat 原始版本，用于补齐源码或比较差异。不是顶层编译入口。 |
| `build/` | **生成物** | CMake release 构建树、对象文件、库和临时可执行文件。不要手工改。 |
| `bin/` | **生成物** | 面向用户的最终 `pbmc` 可执行文件。 |
| `.vscode/` | 编辑器配置 | 其中 `settings.json` 只关闭 Git 大仓库文件数警告，不是算法代码。 |
| `.git/` | 版本库内部数据 | Git 对象、索引、分支与远端信息。不要手工编辑。 |
| `.agents/`、`.codex/` | 工具元数据 | 本地协作/开发工具配置，不参与 PBMC 构建与算法。 |

## 4. `core/`：PBMC 的核心活动源码

这是首先应该掌握的目录。

| 文件 | 当前作用 |
|---|---|
| `Main.cc` | 程序入口。解析命令行和资源限制，选择计数数值类型，调用 `main_pbmc`，读取实例、执行根层处理、设置树分解/VCIS，并启动 `PBcounter::countModels()`，最后输出计数结果。 |
| `Counter.h` | 声明模板类 `PBcounter<T>`。它继承 `rs::Solver`，把 RoundingSat 的 PB 传播/学习能力与 GPMC 风格的分量递归、计数和缓存连接起来。还声明计数状态、递归栈、分支与简化接口。 |
| `Counter.cc` | **top-down 计数主循环**。处理决定、传播、冲突分析和回跳，再搜索连通分量、查缓存、选分支变量、汇总子问题计数。`countModels()` 在 `on_simp` 等条件成立时会走到 `simplify()` 调用点，但当前实现只返回 `true`，所以没有实际简化效果。它是以后集成递归简化的候选入口，但不能只填函数而不维护 trail、约束数据库、组件图、缓存键和模型数补偿。 |
| `Instance.h` | 声明旧 CNF `Instance<T>` 和当前 PB 路径的 `PBInstance<T>`；保存变量数、PB 约束、投影/权重信息、VCIS 数据和预处理映射。 |
| `Instance.cc` | OPB 实例前处理。解析或承接约束，进行文字/系数正规化、根层简化、变量重编号、相位与 VCIS 信息计算，并可把处理后的实例重新序列化为 OPB 交给 RoundingSat 解析。这里是构建“新根预处理器”的首要位置之一。 |
| `pbconstraint.h` | 定义 PBMC 前端使用的轻量 `PBconstraint` 数据结构，保存系数、文字和界；它与 RoundingSat 内部用于传播/学习的 `Constr` 体系不是同一个对象。 |
| `roundingsatInclude.h` | RoundingSat 头文件桥接层，集中包含其辅助函数、全局状态、解析与运行接口，避免在 PBMC 文件中散落大量 include。 |
| `Component.h` | 定义计数器的核心结构：分支决定 `Decision`、连通分量 `Component`、可哈希压缩分量 `PackedComponent`、缓存项 `CachedComponent`，以及组件管理器/缓存管理器的接口。 |
| `ComponentManager.cc` | 构造变量—约束关联图，在当前赋值下扫描活动约束并寻找独立连通分量；负责 `searchComponent()`、分量拆分、动态频率/间隙信息、缓存查询入口和候选分支变量。简化改变约束后必须保证这里看到的是最新结构。 |
| `ComponentCache.cc` | 实现分量缓存：压缩键的哈希与相等判断、查找、插入、命中/缺失统计、内存上限和淘汰，以及父子缓存项关系。修改递归简化或变量消去时必须重新审查缓存键是否仍唯一代表剩余子问题。 |
| `Config.h` | 定义配置结构、枚举和选项字段，例如计数模式、树分解开关、缓存/资源限制等。 |
| `Config.cc` | 注册并解析 GPMC/PBMC 命令行选项，把命令行值写入配置对象。要新增用户可调简化开关，通常会同时修改此文件和 `Config.h`。 |
| `Solver.h` | 声明 GPMC 原有的 Glucose 风格 CNF SAT 求解器。注意：当前 `PBcounter` 继承的是 `rs::Solver`，不是这个类。该文件仍为 CNF 预处理/测试求解器等遗留路径服务。 |
| `Solver.cc` | 上述 Glucose CNF 求解器实现，包括布尔传播、CNF 冲突分析、重启、子句数据库维护等。它不是当前 PB 计数主循环的冲突学习后端。 |
| `SolverTypes.h` | Glucose/MiniSat 风格的 `Lit`、`lbool`、`Clause`、`ClauseAllocator`、`Watcher` 等基础类型，主要服务 CNF 遗留代码，也有部分组件代码依赖这些通用定义。不要与 `roundingsat/typedefs.hpp` 的 PB 类型混淆。 |
| `Dimacs.h` | GPMC 遗留的 DIMACS CNF、投影变量与权重解析代码。当前 `.opb` 主路径主要经 `PBInstance` 和 `roundingsat/parsing.cpp`，不是由这里读取。 |
| `BoundedQueue.h` | Glucose 使用的定长统计队列，例如保存近期 LBD/trail 数据以辅助重启判断。主要属于 CNF 旧求解器基础设施。 |
| `Constants.h` | Glucose CNF 求解器的编译期常量、宏和调参默认值。不是 PBMC 启发式的集中配置文件。 |

### 4.1 `core/` 中最容易混淆的三组对象

```text
PBInstance / PBconstraint
    前端输入、正规化、根预处理使用

rs::Solver / rs::Constr / rs::ConstrExp
    RoundingSat 内部传播、冲突分析、学习使用

Component / PackedComponent / ComponentCache
    PBMC 分解、递归计数和子问题复用使用
```

以后把简化放进递归时，必须明确简化修改的是哪一层，并同步其他两层需要观察的状态；不能只改 `PBInstance` 中最初的约束副本，却期待 RoundingSat watcher 和组件图自动变化。

## 5. `preprocessor/`：名字叫预处理器，但只有一部分服务当前 PB 路径

| 文件 | 当前作用 |
|---|---|
| `Preprocessor.h` | 声明 GPMC 的 **CNF 预处理器**，包括等价识别、失败文字、子句强化、变量消去、包含消去等接口。输入对象主要是 CNF 子句，不是一般 PB 约束。 |
| `Preprocessor.cc` | 实现上述 CNF 技术。它会维护模型计数所需的变量/权重映射，但当前 PBMC `.opb` 主流程没有直接调用整套 CNF `Preprocessor`。不能因为目录名相同就认为这些技术已经用于 PB 实例。 |
| `TestSolver.h` | 声明供 CNF 预处理器探测使用的小型/派生 Glucose 求解器，例如在假设下测试文字是否失败。 |
| `TestSolver.cc` | 实现 CNF 探测求解流程，供失败文字、隐含关系等预处理步骤调用。它不是 `PBcounter` 的 PB 冲突学习器。 |
| `TreeDecomposition.h` | 定义图和树分解数据结构、从 CNF/PB 约束构造 primal graph 的接口，以及树分解中心、距离和变量评分等函数。 |
| `TreeDecomposition.cc` | 实现树分解构造后的评分与查询逻辑。这里是当前 PBMC 启发式路径的一部分，会与 VCIS 分数组合。 |
| `IFlowCutter.h` | 声明 PBMC/GPMC 与 FlowCutter 第三方库之间的适配接口。 |
| `IFlowCutter.cc` | 把内部 primal graph 转成 FlowCutter 图，运行分隔/排序算法，再构造 PBMC 使用的树分解。 |

因此，更准确的理解是：

```text
preprocessor/Preprocessor.*     = GPMC CNF 预处理器遗留/可复用参考
preprocessor/TestSolver.*      = 上述 CNF 预处理器的探测工具
preprocessor/TreeDecomposition.*
preprocessor/IFlowCutter.*     = 当前 PBMC 启发式会使用的图结构模块
```

### 5.1 `preprocessor/lib_sharpsat_td/`

这是来自 SharpSAT-TD 的第三方预处理辅助代码。

| 文件 | 作用 |
|---|---|
| `LICENSE` | 该第三方代码的许可证。 |
| `README.txt` | 来源和使用说明。 |
| `bitset.hpp` | 面向子句/变量集合运算的紧凑 bitset。 |
| `subsumer.hpp` | 声明子句包含消去等结构与接口。 |
| `subsumer.cpp` | 实现 subsumption/相关 CNF 简化；顶层 CMake 会编译此文件，但这不等于 PB 主路径已经调用它。 |
| `utils.hpp` | 该小库内部通用辅助函数。 |

## 6. `roundingsat/`：PB 传播和约束学习后端

这是当前 PBMC 精确计数所依赖的 PB 求解核心。

| 文件 | 当前作用 |
|---|---|
| `CMakeLists.txt` | 把 RoundingSat 源码编译成可供 PBMC 链接的库，并配置 GMP、许可证代码和可选功能。 |
| `typedefs.hpp` | 定义 RoundingSat 的基本类型：变量 `Var`、文字 `Lit`、不同宽度系数/大整数、约束 ID、约束来源 `Origin` 及常用容器别名。研究负文字的内部编码应从这里和相关辅助函数开始。 |
| `SolverStructs.hpp` | 声明约束引用 `CRef`、watch 记录、约束分配器、变量顺序堆和赋值辅助结构。 |
| `SolverStructs.cpp` | 实现上述非模板结构的方法。 |
| `Solver.hpp` | 声明 `rs::Solver`：赋值数组、trail、decision level、watch lists、约束数据库、activity、传播、冲突分析、学习、回跳和清理接口。`PBcounter` 直接继承它。 |
| `Solver.cpp` | 实现 PB 传播和约束数据库维护，是理解“赋值后何时发现冲突、学习约束如何挂回 watcher、如何 backjump”的核心文件。 |
| `Constr.hpp` | 声明用于高效传播的紧凑 PB 约束体系及不同实现，例如 clause、cardinality、counting/watched/safe 等形式。 |
| `Constr.cpp` | 实现约束的挂接、watch 更新、传播、冲突解释、resolve 支持及与展开形式转换。 |
| `ConstrExp.hpp` | 声明可修改的展开 PB 约束 `ConstrExp`，用于输入正规化和冲突分析中的代数操作。 |
| `ConstrExp.cpp` | 实现 saturation、GCD 化简、weakening、division/rounding、resolution、冲突约束后处理及证明日志相关操作。要研究 PB 冲突学习数学过程，此文件非常关键。 |
| `ConstrSimple.hpp` | 声明拥有自身系数数据、便于保存和搬运的简单 PB 约束表示。 |
| `ConstrSimple.cpp` | 实现简单约束与展开/传播表示之间的转换；学习约束暂存等场景会使用。 |
| `parsing.hpp` | 声明 OPB、CNF/WCNF 等输入解析接口。 |
| `parsing.cpp` | 实现实际语法读取并调用 `Solver` 添加约束。PBMC 会把处理后的内存 OPB 送入这里，建立真正用于传播的 RoundingSat 约束。 |
| `Options.hpp` | RoundingSat 自身的选项层次和默认值，例如传播、学习、削弱和数据库策略。它与 `core/Config.*`、`utils/Options.*` 是不同层次。 |
| `Stats.hpp` | RoundingSat 传播、冲突、学习等统计计数器及打印逻辑。 |
| `globals.hpp` | 声明 RoundingSat 全局 `options`、`stats`、异步中断标志等。 |
| `roundingsat.cpp` | 定义上述全局对象和信号相关内容；原独立 RoundingSat `main` 的相关代码也在此演化而来。PBMC 把它作为库的一部分链接。 |
| `run.hpp` | 声明 RoundingSat 独立求解/优化运行流程。 |
| `run.cpp` | 独立求解器的优化、core-guided 等高层流程。PBMC 的 top-down 模型计数不会简单调用完整的独立优化循环，但会复用底层 Solver。 |
| `LpSolver.hpp` | 声明可选的 LP/SoPlex 松弛和割平面接口。 |
| `LpSolver.cpp` | 实现可选 LP 功能；只有对应编译宏和依赖启用时才真正工作，普通 PBMC 构建通常不以它为主。 |
| `Logger.hpp` | PB proof 日志/证明记录的状态和接口。 |
| `IntSet.hpp` | 针对整数、变量或文字集合的紧凑集合工具，用于 active/assumption 等集合。 |
| `auxiliary.hpp` | 数值转换、文字操作、数学、计时、向量处理等通用辅助函数。 |
| `quit.hpp` | 声明 RoundingSat 的结果、错误和退出辅助接口。 |
| `quit.cpp` | 实现结果/解/错误输出与退出处理。作为库接入 PBMC 时并非每种独立求解器输出都会使用。 |

### 6.1 `roundingsat/used_licenses/`

这些文件用于在程序中保留/显示第三方许可证，不是求解算法：

| 文件组 | 作用 |
|---|---|
| `MIT.txt`、`boost.txt`、`gpl-3.0.txt`、`lgpl-3.0.txt`、`roundingsat.txt`、`zib_academic.txt` | 各许可证的原始文本。 |
| `MIT.hpp`、`MIT.cpp`、`boost.hpp`、`boost.cpp`、`gpl_3_0.hpp`、`gpl_3_0.cpp`、`lgpl_3_0.hpp`、`lgpl_3_0.cpp`、`roundingsat.hpp`、`roundingsat.cpp`、`zib_academic.hpp`、`zib_academic.cpp` | 把对应许可证嵌入 C++ 字符串。 |
| `licenses.hpp`、`licenses.cpp` | 聚合和输出所有许可证。 |
| `add_license.py` | 从许可证文本生成相应 C++ 文件的辅助脚本。 |

## 7. `pbmc-mod/`：PBMC 覆盖 GPMC 的文件

这个目录本身不参与顶层编译。里面的文件会在 `setup-complete-code.sh` 运行时复制到活动源码目录。

| 覆盖文件 | 对应活动文件和意义 |
|---|---|
| `core/Main.cc` | 对应 `core/Main.cc`；PBMC 入口和 PB 计数流程改造。 |
| `core/Counter.h`、`core/Counter.cc` | 对应计数器；把 GPMC top-down 框架接到 RoundingSat。 |
| `core/Instance.h`、`core/Instance.cc` | 对应 PB 输入、正规化、投影/权重和 VCIS 处理。 |
| `core/pbconstraint.h` | 对应 PB 前端约束结构。 |
| `core/roundingsatInclude.h` | 对应 RoundingSat 桥接头。 |
| `core/Component.h` | 对应分量/缓存结构，包含 PBMC 所需改造。 |
| `core/ComponentManager.cc` | 对应 PB 分量搜索和启发式改造。 |
| `core/ComponentCache.cc` | 对应 PB 分量缓存改造。 |
| `core/Config.cc` | 对应 PBMC/GPMC 命令行选项改造。 |
| `core/Solver.h` | 对应 GPMC Solver 头的兼容性修改。 |
| `preprocessor/TreeDecomposition.h`、`preprocessor/TreeDecomposition.cc` | 对应 PB primal graph 和树分解评分改造。 |
| `utils/Options.h`、`utils/Options.cc` | 对应选项系统修改，例如隐藏选项类别/显示方式。 |

这里没有出现的活动文件，通常是 setup 从 GPMC 上游原样补进来的。例如 `core/BoundedQueue.h` 不在 `pbmc-mod/` 中，说明它不是 PBMC 最初覆盖的文件。

## 8. `roundingsat-mod/`：PBMC 覆盖 RoundingSat 的文件

同样，这里是 setup 用的覆盖源，不是独立编译目录。

| 覆盖文件 | 修改层次 |
|---|---|
| `CMakeLists.txt` | 把 RoundingSat 调整为 PBMC 可链接的构建形式。 |
| `typedefs.hpp` | 基础类型/兼容性修改。 |
| `Solver.hpp`、`Solver.cpp` | 暴露或调整 PBMC 继承/复用所需的 Solver 状态和行为。 |
| `SolverStructs.hpp`、`SolverStructs.cpp` | watcher、约束分配器和结构兼容改造。 |
| `Constr.hpp`、`Constr.cpp` | 传播约束接口/实现改造。 |
| `Options.hpp` | RoundingSat 选项的 PBMC 适配。 |
| `auxiliary.hpp` | 辅助函数或兼容性改造。 |
| `roundingsat.cpp` | 独立入口/全局对象改造成可作为库链接的版本。 |
| `quit.cpp` | 避免库模式下不合适的直接退出或调整输出行为。 |

如果以后修改了 `roundingsat/ConstrExp.cpp`，但它原本不在 `roundingsat-mod/` 中，那么测试正确后应新建同相对路径 `roundingsat-mod/ConstrExp.cpp`；否则重跑 setup 时该改动不会保留。

## 9. `utils/`：公共工具

| 文件 | 作用 |
|---|---|
| `Options.h` | 声明 MiniSat/GPMC 风格的命令行选项基类及布尔、整数、浮点、字符串等选项类型。 |
| `Options.cc` | 实现选项注册、解析、帮助输出和隐藏选项处理。所谓“隐藏布尔参数”主要是帮助页默认不展示，并非求解器暗中增加另一类布尔变量。 |
| `ParseUtils.h` | 流式字符读取、跳过空白、解析整数/字符串等通用文本解析模板。 |
| `System.h` | 声明 CPU 时间、墙钟时间、内存使用和资源限制查询。 |
| `System.cc` | 针对操作系统实现上述统计。 |
| `Makefile` | 上游遗留的 utils 独立构建文件；完整 PBMC 工程主要使用顶层 CMake。 |

## 10. `ddnnf/`：可选 d-DNNF 表示与枚举器

| 文件 | 作用 |
|---|---|
| `DecisionTree.h` | 声明 d-DNNF/决策图节点、AND/OR/decision/don't-care 等节点类型和管理接口。 |
| `DecisionTree.cc` | 实现节点创建、共享、计数、求值和可能的 d-DNNF 输出。顶层 CMake 会编译它，但是否在一次运行中使用取决于配置和代码路径。 |
| `enumerator/README.md` | Python d-DNNF 枚举实验的说明。 |
| `enumerator/ddnnf.py` | 读取/表示 d-DNNF 并枚举或处理模型的 Python 模块。 |
| `enumerator/main.py` | 上述 Python 工具的命令行入口。 |
| `enumerator/test.cnf` | 枚举器示例 CNF 输入。 |
| `enumerator/test.nnf` | 对应示例 d-DNNF/NNF 文件。 |

## 11. `flow-cutter-pace17/`：树分解第三方库

### 11.1 顶层文件

| 文件 | 作用 |
|---|---|
| `CMakeLists.txt` | 供 PBMC 把 FlowCutter 编译为库。 |
| `Makefile` | 第三方项目原有的独立构建入口。 |
| `build.sh` | 第三方项目独立构建脚本。 |
| `README.md` | 算法、PACE 输入输出和使用说明。 |
| `LICENSE` | FlowCutter 许可证。 |

### 11.2 `flow-cutter-pace17/src/`

这些文件大多是模板化图算法基础设施。后续研究 VCIS 时通常只需要先读 `IFlowCutter.*`、`flow_cutter.h` 和 `tree_decomposition.*`，不必从所有容器开始。

| 文件 | 作用 |
|---|---|
| `flow_cutter.h` | FlowCutter 图割/分隔算法的主要模板实现。 |
| `flow_cutter_config.h` | FlowCutter 参数和策略配置。 |
| `node_flow_cutter.h` | 以节点分隔为目标的 FlowCutter 封装。 |
| `tree_decomposition.h`、`tree_decomposition.cpp` | 从消去序/分隔结构构建和处理树分解。 |
| `greedy_order.h`、`greedy_order.cpp` | 计算贪心节点消去/排序。 |
| `tree_node_ranking.h` | 对树节点进行排名，支持分解中心和顺序相关计算。 |
| `separator.h` | 图分隔集的数据结构与辅助算法。 |
| `cell.h`、`cell.cpp` | 分隔递归产生的 cell/区域结构。 |
| `list_graph.h`、`list_graph.cpp` | 邻接表图表示。 |
| `contraction_graph.h` | 图收缩数据结构。 |
| `connected_components.h` | 连通分量算法。 |
| `union_find.h` | 并查集。 |
| `heap.h` | 图算法用堆。 |
| `tree.h` | 通用树结构/遍历辅助。 |
| `chain.h` | 链式结构辅助。 |
| `back_arc.h` | 构建或查询反向弧。 |
| `multi_arc.h` | 多重弧相关结构。 |
| `sort_arc.h` | 对弧按端点等键排序。 |
| `cch_order.h` | CCH/嵌套分解相关排序辅助。 |
| `preorder.h` | 图或树的先序编号。 |
| `permutation.h` | 排列、逆排列和重编号辅助。 |
| `id_sort.h` | 按 ID/键排序。 |
| `id_func.h` | 以整数 ID 索引的函数/数组容器。 |
| `tiny_id_func.h` | 更轻量的小型 ID 映射。 |
| `id_multi_func.h` | 一个 ID 对应多个值的映射。 |
| `id_func_traits.h` | 上述 ID 容器的类型特征。 |
| `array_id_func.h` | 固定数组形式的 ID 映射。 |
| `filter.h` | 根据谓词过滤范围/图元素。 |
| `range.h` | 轻量迭代范围工具。 |
| `count_range.h` | 生成连续整数范围。 |
| `histogram.h` | 直方图/频数统计辅助。 |
| `min_max.h` | 最小值/最大值辅助。 |
| `io_helper.h` | 图和 PACE 格式输入输出辅助。 |
| `pace.cpp` | FlowCutter 原项目参加 PACE 的独立命令行入口；PBMC 作为库使用时不以它作为主程序。 |

## 12. `mtl/`：MiniSat Template Library

| 文件 | 作用 |
|---|---|
| `Vec.h` | MiniSat 风格动态数组 `vec<T>`。 |
| `Alloc.h` | 区域分配器，CNF ClauseAllocator 等会使用。 |
| `Heap.h` | 支持 update 的二叉堆，常用于变量 activity 排序。 |
| `Queue.h` | 队列容器。 |
| `Map.h` | 映射容器。 |
| `Alg.h` | 基础算法辅助。 |
| `Sort.h` | 排序算法。 |
| `IntTypes.h` | 固定宽度整数和平台兼容宏。 |
| `XAlloc.h` | 内存分配封装与内存不足异常。 |
| `config.mk` | MiniSat 风格 Make 构建配置。 |
| `template.mk` | MiniSat 风格通用 Make 模板。 |

## 13. `mpfr/`：高精度实数包装

| 文件 | 作用 |
|---|---|
| `mpreal.h` | MPFR C API 的 C++ 类包装，用于高精度浮点/实数计数。 |
| `README.md` | 包装库使用说明。 |
| `changelog` | 版本变更记录。 |
| `copying.txt` | 许可证文本。 |
| `example/example.cpp` | `mpreal` 使用示例。 |
| `example/makefile` | 编译示例的独立 Makefile。 |

## 14. `benchmarks/`：论文实验数据

数据目录不参与编译，而是运行时输入。

| 路径/文件 | 作用 |
|---|---|
| `description.txt` | 数据集来源、类别或转换方式说明。 |
| `opb/auction/` | 拍卖类原生伪布尔实例。 |
| `opb/knapsack/` | 背包类原生伪布尔实例。 |
| `opb/graph/` | 图问题类原生伪布尔实例。 |
| `cnf/` | 把 PB 问题编码为 CNF 后供 CNF 计数器比较的实例，通常按编码器/问题类型再分目录。 |
| `cost-graph/` | cost/graph 相关 OPB 实例；`.mtx.opb` 表示名称中保留了矩阵来源标记，但最终格式仍是 OPB。 |
| `ZIP/` | 原始压缩数据归档；本地保留即可，通常不提交 Git。 |

`.opb.xz` 的 `.xz` 只是单文件的 XZ 压缩后缀，解压后才是文本 `.opb`。当前仓库已经把需要查看的 OPB 数据解压时，可以直接用编辑器或 `less` 查看。是否能直接把 `.xz` 交给 PBMC 取决于启动脚本是否先解压；求解器本身的普通文件流不应默认当作透明 XZ 解压器。

具体实例文件通常只是同一类输入，不需要在源码地图中逐个解释。OPB 常见内容为：

```text
* #variable= ... #constraint= ...
+3 x1 -2 ~x2 >= 1;
```

其中 `x2` 是正文字，`~x2` 是负文字，末尾关系和右端值组成约束界。

## 15. `paper/` 和 `scripts/`

| 文件 | 作用 |
|---|---|
| `paper/PBMC_On Top-Down Pseudo-Boolean Model Counting.pdf` | 项目对应论文，算法、VCIS、实验配置和数据集说明的第一手依据。 |
| `scripts/summarize_pb_datasets.py` | 扫描 OPB 数据并汇总变量、约束、关系符号、负文字等信息的研究辅助脚本；不进入求解器编译。 |

## 16. `AAAI_EDPLLSim/`：独立参考项目，不属于 PBMC 可执行文件

该目录可用于移植思路，但它有自己的 Makefile、源文件和可执行文件。PBMC 顶层 `CMakeLists.txt` 不会编译这里。

### 16.1 EDPLLSim 核心文件

| 文件 | 作用 |
|---|---|
| `Makefile` | EDPLLSim 自己的构建入口。 |
| `README.md` | EDPLLSim 使用和算法说明。 |
| `.gitignore` | 忽略 EDPLLSim 的对象文件、可执行文件、构建/结果目录、下载的依赖和编辑器缓存。 |
| `.vscode/settings.json` | EDPLLSim 的本地 VS Code/CMake 路径设置；其中路径与本机目录有关，不是算法配置。 |
| `.DS_Store`、`sim/.DS_Store` | macOS Finder 自动生成的目录元数据，应忽略，不是源码。 |
| `src/main.cc` | EDPLLSim 命令行入口。 |
| `src/solver.h`、`src/solver.cc` | EDPLL 风格递归求解/计数主流程。 |
| `src/linear_constraint.h`、`src/linear_constraint.cc` | 线性/PB 约束表示和操作。 |
| `src/heuristic.h`、`src/heuristic.cc` | EDPLLSim 分支/结构启发式。 |
| `src/cache.h`、`src/cache.cc` | 子问题缓存。 |
| `src/adaptive_k.h`、`src/adaptive_k.cc` | 自适应 K 选择或简化强度控制。 |
| `src/big_int.h`、`src/big_int.cc` | 大整数计数封装。 |
| `src/patoh.h` | PaToH 超图划分库接口。 |
| `EDPLLSim` | 已编译的 EDPLLSim 可执行文件，属于生成物。 |
| `lib/libpatoh.a` | PaToH 第三方静态库。 |
| `build_utils.sh` | 构建/准备依赖的辅助脚本。 |

### 16.2 模拟器、测试和实验脚本

| 文件 | 作用 |
|---|---|
| `sim/src/main.cc` | 独立模拟器入口。 |
| `sim/src/sim.h`、`sim/src/sim.cc` | 模拟器实现。 |
| `sim/README.md` | 模拟器说明。 |
| `Sim` | 已编译模拟器可执行文件。 |
| `tests/adaptive_k/adaptive_k_test.cc` | adaptive-k 单元/实验测试。 |
| `tests/rir_lp/rir_lp_test.cc` | RIR/LP 相关测试。 |
| `scripts/vm_common.sh` | 多组虚拟机实验共享函数和参数。 |
| `scripts/vm_STN.sh` | STN 数据实验脚本。 |
| `scripts/vm_application.sh` | application 数据实验脚本。 |
| `scripts/vm_multiple_knapsack.sh` | multiple-knapsack 数据实验脚本。 |
| `scripts/vm_random.sh` | random 数据实验脚本。 |
| `scripts/vm_thin.sh` | thin 数据实验脚本。 |
| `scripts/merge_vm_results.py` | 合并多台机器的实验结果。 |
| `scripts/split_random_by_runtime.py` | 按运行时间拆分随机实例。 |

`scripts/__pycache__/*.pyc` 是 Python 自动生成的字节码缓存，不应阅读、修改或提交。

### 16.3 EDPLLSim 文档

| 文件 | 作用 |
|---|---|
| `Adaptive_K_Ablation_Commands.md` | adaptive-k 消融实验命令。 |
| `Adaptive_K_Jaccard_Confidence_Formula.md` | Jaccard/confidence 公式说明。 |
| `Adaptive_K_Jaccard_Confidence_Simple.md` | 上述方法的简化说明。 |
| `EDPLLSim_Cache_AdaptiveK_to_PBMC_Guide.md` | 把 EDPLLSim 缓存/adaptive-k 思路迁移到 PBMC 的指南。 |
| `EDPLLSim_Component_Cache_Memory_Optimization_Plan.md` | 分量缓存内存优化计划。 |
| `K_Simplification_Commands.md` | K 简化相关实验命令。 |

其余 `build/`、结果目录、压缩依赖包和 `.DS_Store` 分别是生成物、实验输出、依赖归档和 macOS 元数据，不是核心源码。

## 17. `temp-source-clones/`、`build/` 和 `bin/`

| 路径 | 作用 |
|---|---|
| `temp-source-clones/GPMC/` | setup 获取的固定 GPMC 上游快照，用于补齐活动源码和比较 PBMC 改动。 |
| `temp-source-clones/roundingsat/` | setup 获取的固定 RoundingSat 上游快照。 |
| `build/CMakeCache.txt` | CMake 配置缓存。若切换编译器/依赖后出现奇怪配置，可 `make clean` 后重建。 |
| `build/Makefile` | CMake 生成的底层 Makefile，不要手工编辑。 |
| `build/cmake_install.cmake` | CMake 自动生成的安装规则。 |
| `build/libpbmc.a` | PBMC 自有源码编成的静态库。 |
| `build/pbmc` | 构建树中的可执行文件。 |
| `bin/pbmc` | 日常直接运行的最终可执行文件。 |

对象文件、依赖文件以及 `CMakeFiles/` 内部内容均由构建系统管理；源码有改动时让 `make` 重新生成即可。

## 18. 顶层 CMake 当前实际编译哪些 PBMC 文件

当前顶层构建直接编译的 PBMC/GPMC 文件可以概括为：

```text
core/ComponentCache.cc
core/ComponentManager.cc
core/Counter.cc
core/Config.cc
core/Instance.cc
core/Solver.cc
ddnnf/DecisionTree.cc
utils/Options.cc
utils/System.cc
preprocessor/Preprocessor.cc
preprocessor/TestSolver.cc
preprocessor/lib_sharpsat_td/subsumer.cpp
preprocessor/TreeDecomposition.cc
preprocessor/IFlowCutter.cc
core/Main.cc                    # 最终程序入口
```

此外会链接 `roundingsat/` 和 `flow-cutter-pace17/` 构建出的库。

“被编译”不等于“每次运行都会调用”。例如 `Preprocessor.cc` 被放进静态库，但普通 PB 主路径若没有引用它的函数，链接器和运行控制流不会让 CNF 预处理器自动作用在 OPB 约束上。

## 19. 针对计划修改的最快阅读顺序

### 19.1 把简化技术放进递归

```text
core/Counter.h
→ core/Counter.cc
→ roundingsat/Solver.hpp、Solver.cpp
→ roundingsat/Constr.hpp、Constr.cpp
→ roundingsat/ConstrExp.hpp、ConstrExp.cpp
→ core/Component.h、ComponentManager.cc
→ core/ComponentCache.cc
```

重点是找准一次递归中的顺序：当前赋值/传播、冲突学习与回跳、简化、组件重新发现、缓存查找、分支和计数合并。是否在学习后执行简化，取决于回跳后的约束状态和简化的可撤销性，不能仅凭函数名字接入。

### 19.2 形成新的根预处理器

```text
core/pbconstraint.h
→ core/Instance.h、Instance.cc
→ roundingsat/parsing.cpp
```

`preprocessor/Preprocessor.*` 可借鉴技术组织方式，但它处理的是 CNF；PB 技术必须重新定义对系数、负文字、等式、模型计数补偿和投影变量的处理。

### 19.3 修改 VCIS 和分支启发式

```text
core/Instance.cc
→ preprocessor/TreeDecomposition.*
→ preprocessor/IFlowCutter.*
→ core/ComponentManager.cc
→ core/Counter.cc
→ roundingsat/Solver.*          # activity 来源
```

当前不是“VCIS 和树分解二选一”：树分解会给出多个结构分数/距离信息，VCIS 再对这些结构分数施加 PB 约束系数影响；局部分量频率和 RoundingSat activity 还会参与后备选择。

### 19.4 修改缓存

```text
core/Component.h
→ core/ComponentManager.cc
→ core/ComponentCache.cc
→ core/Counter.cc
```

缓存设计必须与简化后的规范形式一起设计。两个语义相同但表示不同的子问题若得到不同 key，会降低命中率；两个语义不同的子问题若错误共用 key，则会直接产生错误精确计数。

## 20. 日常修改时到底改哪一份

例如要修改计数循环：

```bash
# 1. 修改活动源码
vim core/Counter.cc

# 2. 增量构建和测试
make
make test

# 3. 确认正确后，同步持久覆盖副本
cp core/Counter.cc pbmc-mod/core/Counter.cc
```

例如修改 RoundingSat：

```bash
vim roundingsat/Solver.cpp
make
make test
cp roundingsat/Solver.cpp roundingsat-mod/Solver.cpp
```

如果只是持续在当前工作树开发，不重跑 `setup-complete-code.sh`，活动源码当然仍在；同步 `*-mod/` 的意义在于保证将来清理并重新组装后，PBMC 的自有改动仍能恢复。

## 21. 一句话定位常见名字

- `pbmc-mod`：PBMC 对 GPMC 的**覆盖补丁源**，不是当前编译目录。
- `roundingsat-mod`：PBMC 对 RoundingSat 的**覆盖补丁源**。
- `preprocessor`：一半是旧 GPMC CNF 预处理，一半是当前会用的树分解接口，不能整体等同于 PB 预处理器。
- `core`：PBMC 计数框架和后续研究的主要修改区。
- `roundingsat`：传播、PB 冲突分析、约束学习和回跳后端。
- `ComponentManager`：找独立子问题并辅助选变量。
- `ComponentCache`：记住已经精确计数过的子问题。
- `TreeDecomposition`：给变量提供全局图结构位置/评分。
- `VCIS`：在结构评分中体现 PB 约束系数影响，不是另一个完整求解器。
- `build`、`bin`：编译生成物，删掉可重建。
