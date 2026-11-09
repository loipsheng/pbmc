# PBMC 中文使用与开发指南

PBMC 是一个精确的伪布尔模型计数器（Pseudo-Boolean Model Counter），实现论文 *On Top-Down Pseudo-Boolean Model Counting* 中的 top-down 计数方法。它建立在 GPMC 的分量分解与缓存框架和 RoundingSat 的伪布尔传播、冲突分析能力之上，并加入了面向 PB 约束的 VCIS 变量启发式与缓存表示。

本仓库最初只保存相对于 GPMC 和 RoundingSat 修改过的文件。第一次构建前，需要先下载指定版本的上游源码并进行拼装。

## 1. 环境依赖

构建需要支持 C++17 的编译器，以及 Git、Make、CMake、GMP、MPFR、Boost iostreams 和 zlib。

Ubuntu/Debian 上可参考：

```bash
sudo apt install build-essential cmake git \
  libgmp-dev libmpfr-dev libboost-iostreams-dev zlib1g-dev
```

## 2. 第一次安装

只在首次克隆、完整源码尚未拼装时运行：

```bash
make setup
```

它会执行 `setup-complete-code.sh`：

1. 下载固定提交的 GPMC；
2. 下载固定提交的 RoundingSat；
3. 将 `pbmc-mod/` 中的 PBMC 修改覆盖到 GPMC 源码；
4. 将 `roundingsat-mod/` 中的修改覆盖到 RoundingSat 源码；
5. 生成 `core/`、`roundingsat/`、`preprocessor/` 等完整源码目录。

正常修改和重新编译不需要再次运行 `make setup`。

注意：`setup-complete-code.sh` 中的 `set -e` 当前被注释，网络下载失败后脚本可能继续执行。因此 setup 后必须运行 `make`，以编译结果确认源码是否完整。

## 3. 编译

默认编译 Release 动态链接版本：

```bash
make
```

等价于：

```bash
make release
./build.sh r
```

最终程序为：

```text
bin/pbmc
```

其他构建命令：

```bash
make debug       # Debug 版本：bin-debug/pbmc
make static      # 尝试构建静态链接版本：bin/pbmc
make clean       # 只删除编译产物
make rebuild     # clean 后重新编译 Release
make help        # 查看 Makefile 帮助
```

`make` 支持增量编译。修改一个 `.cc` 或 `.h` 后直接再次运行 `make` 即可，通常不需要先执行 `make clean`。

`make static` 和 `make release` 都会写入 `bin/pbmc`，后执行的版本会覆盖前一个版本。

## 4. 清理命令的区别

### `make clean` / `clean.sh`

只删除：

```text
build/
bin/
build-debug/
bin-debug/
```

不会删除源码和 benchmark，可用于日常开发。

### `clean-up.sh`

会删除 setup 拼装出来的完整源码目录：

```text
core/ ddnnf/ flow-cutter-pace17/ mpfr/ mtl/
preprocessor/ roundingsat/ utils/ temp-source-clones/
```

它不是普通的编译清理命令。修改源码后不要运行它，否则尚未同步回 `pbmc-mod/` 或 `roundingsat-mod/` 的改动可能丢失。

## 5. 运行普通精确模型计数

运行一个未压缩的 OPB 文件：

```bash
./bin/pbmc input.opb
```

也可以通过 Makefile 编译并运行：

```bash
make run INPUT=input.opb
```

传递额外参数：

```bash
make run INPUT=input.opb ARGS="-mode=2"
```

运行仓库中的冒烟测试：

```bash
make test
```

默认测试输入是：

```text
benchmarks/opb/knapsack/k-dim-knapsack-bench-1-104-4.opb
```

## 6. OPB 输入格式

示例：

```opb
* #variable= 3 #constraint= 2
*
+2 x1 -3 x2 +4 ~x3 >= 4;
+1 x1 +1 x2 = 1;
```

含义：

- 变量为 `x1`、`x2`、`x3`；
- `~x3` 表示非 `x3`；
- 每一行以分号结束；
- 支持 `>=` 和 `=` 约束。

变量编号必须与头部声明一致。例如 `#variable= 3` 时，只能使用 `x1` 到 `x3`。

## 7. 计数模式

通过 `-mode=N` 选择：

| 模式 | 名称 | 含义 |
|---:|---|---|
| 0 | MC | 普通精确模型计数，默认值 |
| 1 | WMC | 加权模型计数 |
| 2 | PMC | 投影模型计数 |
| 3 | WPMC | 加权投影模型计数 |

投影变量可在 OPB 中写为：

```opb
* p show 3 5 8 0
```

表示只统计 `x3`、`x5`、`x8` 上能够扩展成完整满足解的不同赋值。运行：

```bash
./bin/pbmc -mode=2 input.opb
```

当前解析器只支持一行投影集合，不能把投影变量拆成多行 `* p show`。

## 8. 输出解释

典型输出：

```text
s SATISFIABLE
c s type mc
c s exact arb int 5028
c s log10-estimate 3.70139526901392
```

解释：

- `s SATISFIABLE`：公式至少存在一个满足赋值；
- `type mc`：本次执行的是普通模型计数；
- `exact`：结果是精确值，不是近似值；
- `arb int`：使用任意精度整数保存结果；
- `5028`：精确模型数量；
- `log10-estimate`：模型数量的十进制对数，便于观察大整数数量级。

## 9. Benchmark

当前 OPB 数据已解压为普通文本文件，可直接交给 PBMC：

```text
benchmarks/opb/auction/      1000 个 Auction 实例
benchmarks/opb/knapsack/     1000 个多维背包实例
benchmarks/opb/graph/        1500 个 Sensor placement 实例
benchmarks/cost-graph/       1500 个 cost-aware Sensor placement 实例
```

例如：

```bash
./bin/pbmc \
  benchmarks/opb/knapsack/k-dim-knapsack-bench-1-104-4.opb
```

`benchmarks/cnf/` 是使用 PBLib 将 PB 约束编码成 CNF 后的数据，主要供 CNF 模型计数器使用，不是 PBMC 的原生输入。

## 10. 参数和树分解

部分实验性参数被作者从帮助信息中隐藏，但解析器仍可识别。布尔选项的写法是：

```bash
-td          # 开启树分解
-no-td       # 关闭树分解
```

不是 `-td=false`。

默认启发式不是单独的 GPMC 树分解启发式，而是：

```text
树分解分数 + PBMC 的 VCIS 系数影响分数
→ 当前分量内变量频率
→ RoundingSat activity
```

树分解只影响变量顺序和运行性能，不改变精确计数结果。对于变量太少、图太密或树宽过大的实例，PBMC 会自动输出 `skip td` 或 `ignore td`。

当前实现中，命令行使用 `-no-td` 还会跳过 VCIS 分数的合并，因此它并不只是单纯关闭树分解；做启发式消融实验前应检查源码控制流。

## 11. 源码结构与修改位置

主要目录：

```text
core/                   PBMC 输入、计数、分量缓存和主程序
roundingsat/            PB 传播、冲突分析和学习约束
preprocessor/           GPMC 预处理遗留代码与树分解接口
flow-cutter-pace17/     FlowCutter 树分解库
ddnnf/                  可选 d-DNNF 功能
mtl/                    MiniSat 风格基础容器
utils/                  参数解析和系统工具
pbmc-mod/               Git 跟踪的 PBMC 修改文件
roundingsat-mod/        Git 跟踪的 RoundingSat 修改文件
```

核心调用顺序：

```text
core/Main.cc
  → core/Instance.cc                 读取、正规化、预处理、VCIS
  → roundingsat/parsing.cpp          建立 RoundingSat 约束
  → core/Counter.cc                  top-down 计数主循环
  → core/ComponentManager.cc         分量分解与分支变量选择
  → core/ComponentCache.cc           分量缓存
```

日常开发可以先修改根目录下参与编译的完整源码，例如：

```text
core/Counter.cc
roundingsat/Solver.cpp
```

验证完成后，需要把 PBMC 修改同步回对应的持久目录：

```text
core/Counter.cc          → pbmc-mod/core/Counter.cc
roundingsat/Solver.cpp   → roundingsat-mod/Solver.cpp
```

如果修改的上游文件原来不在 `*-mod/` 中，也需要按相同相对路径加入相应的 mod 目录，否则重新 setup 后无法恢复该修改。

## 12. 推荐开发流程

```bash
# 1. 修改源码
vim core/Counter.cc

# 2. 增量编译
make

# 3. 小实例验证
make test

# 4. 指定实例运行
make run INPUT=benchmarks/opb/knapsack/某个实例.opb

# 5. 需要调试时
make debug
gdb --args ./bin-debug/pbmc input.opb
```

在开始实现递归内简化、预处理器或新启发式之前，建议先为若干小实例保存基准精确计数，以便每次修改后进行回归验证。

## 许可证

PBMC 及所使用的上游组件分别遵循仓库中声明的许可证。分发或重构第三方代码时，应保留对应的许可证和来源说明。
