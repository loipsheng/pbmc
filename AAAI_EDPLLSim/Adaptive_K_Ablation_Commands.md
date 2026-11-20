# 自适应 K 与 SB 循环配置下的简化技术消融实验指令

## 1. 统一实验配置

本实验固定使用以下配置：

| 配置项 | 取值 |
|---|---|
| 启发式 | DHP-P（`-heur=2`） |
| K | Jaccard-Confidence 自适应 K |
| SB | 循环到不动点（`SB_ITERATE=1`） |
| Timing | 开启（`TIMING=1`） |
| 缓存准入阈值 | `N*M <= 20` |
| 缓存插入年龄 | 51200 |
| 单测例超时 | 3600 秒 |
| 单进程虚拟内存上限 | 10 GB |
| 最大并行任务数 | **13** |

RIR 是必要的基础兜底技术，不参与消融。RIR-LP 是可选增强技术；当 ID 7
禁止 RIR-LP 时，不论当前分量是否满足 `N>K`，单行删除阶段都会回退执行
RIR。

消融 ID 如下：

| ID | 被消融技术 | 消融后的行为 |
|---:|---|---|
| 1 | RV | 跳过 RV |
| 2 | SB | `N<=K` 时跳过 SB；`N>K` 时仍可执行 SB-LP |
| 3 | SC | 跳过 SC |
| 4 | RSR | 跳过 RSR |
| 5 | RPR | 跳过 RPR |
| 6 | SB-LP | `N>K` 时跳过 SB-LP，不回退 SB |
| 7 | RIR-LP | 禁止 RIR-LP，并对所有分量回退使用 RIR |

## 2. 编译与结果目录

在项目根目录执行：

```bash
cd /home/experimenter/Desktop/lyc/EDPLLSim/AAAI_EDPLLSim

make -j"$(nproc)" EDPLLSim

mkdir -p result/ablation
```

## 3. Random 完整数据集

以下命令均设置 `INPUT_LIST=""` 和 `INPUT_LIMIT=0`，因此直接运行
`benchmarks/origin/random` 中的全部测例，不使用 Random 四机拆分清单。

### 3.1 完整技术配置基线

如果已有结果不是在完全相同的程序版本、机器和资源限制下生成，建议重新运行
该基线。

```bash
ADAPTIVE_K=1 \
TIMING=1 \
VAR_LIMIT=999 \
SB_ITERATE=1 \
ABLATION_IDS="" \
DATA_DIR="$PWD/benchmarks/origin/random" \
INPUT_LIST="" \
INPUT_LIMIT=0 \
RESULT_FILE="$PWD/result/ablation/random_full.txt" \
CACHE_ADMISSION_MAX_CELLS=20 \
CACHE_MAX_AGE_INSERTS=51200 \
MAX_JOBS=13 \
PER_PROCESS_MEM_GB=10 \
TIMEOUT_SECS=3600 \
./scripts/vm_random.sh 2
```

### 3.2 消融 RV（ID 1）

```bash
ADAPTIVE_K=1 \
TIMING=1 \
VAR_LIMIT=999 \
SB_ITERATE=1 \
ABLATION_IDS="1" \
DATA_DIR="$PWD/benchmarks/origin/random" \
INPUT_LIST="" \
INPUT_LIMIT=0 \
RESULT_FILE="$PWD/result/ablation/random_without_rv.txt" \
CACHE_ADMISSION_MAX_CELLS=20 \
CACHE_MAX_AGE_INSERTS=51200 \
MAX_JOBS=13 \
PER_PROCESS_MEM_GB=10 \
TIMEOUT_SECS=3600 \
./scripts/vm_random.sh 2
```

### 3.3 消融 SB（ID 2）

```bash
ADAPTIVE_K=1 \
TIMING=1 \
VAR_LIMIT=999 \
SB_ITERATE=1 \
ABLATION_IDS="2" \
DATA_DIR="$PWD/benchmarks/origin/random" \
INPUT_LIST="" \
INPUT_LIMIT=0 \
RESULT_FILE="$PWD/result/ablation/random_without_sb.txt" \
CACHE_ADMISSION_MAX_CELLS=20 \
CACHE_MAX_AGE_INSERTS=51200 \
MAX_JOBS=13 \
PER_PROCESS_MEM_GB=10 \
TIMEOUT_SECS=3600 \
./scripts/vm_random.sh 2
```

### 3.4 消融 SC（ID 3）

```bash
ADAPTIVE_K=1 \
TIMING=1 \
VAR_LIMIT=999 \
SB_ITERATE=1 \
ABLATION_IDS="3" \
DATA_DIR="$PWD/benchmarks/origin/random" \
INPUT_LIST="" \
INPUT_LIMIT=0 \
RESULT_FILE="$PWD/result/ablation/random_without_sc.txt" \
CACHE_ADMISSION_MAX_CELLS=20 \
CACHE_MAX_AGE_INSERTS=51200 \
MAX_JOBS=13 \
PER_PROCESS_MEM_GB=10 \
TIMEOUT_SECS=3600 \
./scripts/vm_random.sh 2
```

### 3.5 消融 RSR（ID 4）

```bash
ADAPTIVE_K=1 \
TIMING=1 \
VAR_LIMIT=999 \
SB_ITERATE=1 \
ABLATION_IDS="4" \
DATA_DIR="$PWD/benchmarks/origin/random" \
INPUT_LIST="" \
INPUT_LIMIT=0 \
RESULT_FILE="$PWD/result/ablation/random_without_rsr.txt" \
CACHE_ADMISSION_MAX_CELLS=20 \
CACHE_MAX_AGE_INSERTS=51200 \
MAX_JOBS=13 \
PER_PROCESS_MEM_GB=10 \
TIMEOUT_SECS=3600 \
./scripts/vm_random.sh 2
```

### 3.6 消融 RPR（ID 5）

```bash
ADAPTIVE_K=1 \
TIMING=1 \
VAR_LIMIT=999 \
SB_ITERATE=1 \
ABLATION_IDS="5" \
DATA_DIR="$PWD/benchmarks/origin/random" \
INPUT_LIST="" \
INPUT_LIMIT=0 \
RESULT_FILE="$PWD/result/ablation/random_without_rpr.txt" \
CACHE_ADMISSION_MAX_CELLS=20 \
CACHE_MAX_AGE_INSERTS=51200 \
MAX_JOBS=13 \
PER_PROCESS_MEM_GB=10 \
TIMEOUT_SECS=3600 \
./scripts/vm_random.sh 2
```

### 3.7 消融 SB-LP（ID 6）

该配置在 `N>K` 时跳过 SB-LP，不回退到 SB。

```bash
ADAPTIVE_K=1 \
TIMING=1 \
VAR_LIMIT=999 \
SB_ITERATE=1 \
ABLATION_IDS="6" \
DATA_DIR="$PWD/benchmarks/origin/random" \
INPUT_LIST="" \
INPUT_LIMIT=0 \
RESULT_FILE="$PWD/result/ablation/random_without_sb_lp.txt" \
CACHE_ADMISSION_MAX_CELLS=20 \
CACHE_MAX_AGE_INSERTS=51200 \
MAX_JOBS=13 \
PER_PROCESS_MEM_GB=10 \
TIMEOUT_SECS=3600 \
./scripts/vm_random.sh 2
```

### 3.8 消融 RIR-LP 并回退 RIR（ID 7）

该配置不再使用 RIR-LP，所有分量均使用基础 RIR。

```bash
ADAPTIVE_K=1 \
TIMING=1 \
VAR_LIMIT=999 \
SB_ITERATE=1 \
ABLATION_IDS="7" \
DATA_DIR="$PWD/benchmarks/origin/random" \
INPUT_LIST="" \
INPUT_LIMIT=0 \
RESULT_FILE="$PWD/result/ablation/random_without_rir_lp_fallback_rir.txt" \
CACHE_ADMISSION_MAX_CELLS=20 \
CACHE_MAX_AGE_INSERTS=51200 \
MAX_JOBS=13 \
PER_PROCESS_MEM_GB=10 \
TIMEOUT_SECS=3600 \
./scripts/vm_random.sh 2
```

## 4. Multiple Knapsack、Application、STN 和 Thin 循环消融

下面的双层循环依次运行 7 个单项消融和 4 个数据集，共运行 28 组实验。
不同配置顺序执行；每组实验内部最多同时运行 13 个测例。

```bash
cd /home/experimenter/Desktop/lyc/EDPLLSim/AAAI_EDPLLSim

mkdir -p result/ablation

datasets=(
  multiple_knapsack
  application
  STN
  thin
)

runners=(
  vm_multiple_knapsack.sh
  vm_application.sh
  vm_STN.sh
  vm_thin.sh
)

ablation_ids=(
  1
  2
  3
  4
  5
  6
  7
)

ablation_names=(
  rv
  sb
  sc
  rsr
  rpr
  sb_lp
  rir_lp_fallback_rir
)

for ablation_index in "${!ablation_ids[@]}"; do
  ablation_id="${ablation_ids[$ablation_index]}"
  ablation_name="${ablation_names[$ablation_index]}"

  for dataset_index in "${!datasets[@]}"; do
    dataset="${datasets[$dataset_index]}"
    runner="${runners[$dataset_index]}"

    echo "Running dataset=$dataset, without=$ablation_name"

    ADAPTIVE_K=1 \
    TIMING=1 \
    VAR_LIMIT=999 \
    SB_ITERATE=1 \
    ABLATION_IDS="$ablation_id" \
    DATA_DIR="$PWD/benchmarks/origin/$dataset" \
    INPUT_LIST="" \
    INPUT_LIMIT=0 \
    RESULT_FILE="$PWD/result/ablation/${dataset}_without_${ablation_name}.txt" \
    CACHE_ADMISSION_MAX_CELLS=20 \
    CACHE_MAX_AGE_INSERTS=51200 \
    MAX_JOBS=13 \
    PER_PROCESS_MEM_GB=10 \
    TIMEOUT_SECS=3600 \
    "./scripts/$runner" 2 || exit 1
  done
done
```

## 5. 四个快速数据集的完整配置基线

如果需要在同一环境下重新生成完整技术配置基线，执行：

```bash
cd /home/experimenter/Desktop/lyc/EDPLLSim/AAAI_EDPLLSim

datasets=(
  multiple_knapsack
  application
  STN
  thin
)

runners=(
  vm_multiple_knapsack.sh
  vm_application.sh
  vm_STN.sh
  vm_thin.sh
)

for dataset_index in "${!datasets[@]}"; do
  dataset="${datasets[$dataset_index]}"
  runner="${runners[$dataset_index]}"

  echo "Running full configuration: dataset=$dataset"

  ADAPTIVE_K=1 \
  TIMING=1 \
  VAR_LIMIT=999 \
  SB_ITERATE=1 \
  ABLATION_IDS="" \
  DATA_DIR="$PWD/benchmarks/origin/$dataset" \
  INPUT_LIST="" \
  INPUT_LIMIT=0 \
  RESULT_FILE="$PWD/result/ablation/${dataset}_full.txt" \
  CACHE_ADMISSION_MAX_CELLS=20 \
  CACHE_MAX_AGE_INSERTS=51200 \
  MAX_JOBS=13 \
  PER_PROCESS_MEM_GB=10 \
  TIMEOUT_SECS=3600 \
  "./scripts/$runner" 2 || exit 1
done
```

## 6. 结果文件

所有结果统一写入：

```text
result/ablation/
```

ID 7 的结果文件使用 `without_rir_lp_fallback_rir` 后缀，强调该实验只移除
RIR-LP 增强版本，RIR 仍作为必要的基础技术运行。
