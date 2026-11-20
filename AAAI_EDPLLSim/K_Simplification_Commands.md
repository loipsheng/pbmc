# Jaccard-Confidence 自适应 K 实验指令

自适应 K 只保留 Jaccard-Confidence 公式。启用方式为：

```text
-adaptive-k=1
```

不再提供 `-adaptive-k-formula` 参数。公式只读取原始输入矩阵，不执行额外的
SB/RIR 根探测；搜索中的 RV、SB、SB-LP、SC、RIR、RIR-LP、RPR、RSR 仍全部
启用。

## 1. 编译

```bash
cd /home/experimenter/Desktop/lyc/EDPLLSim/AAAI_EDPLLSim
make -j"$(nproc)" EDPLLSim
```

## 2. Random 四台机器

四台机器分别执行一段。四份清单合计 2840 个测例。

### 机器1

```bash
cd /home/experimenter/Desktop/lyc/EDPLLSim/AAAI_EDPLLSim

ADAPTIVE_K=1 \
TIMING=1 \
VAR_LIMIT=999 \
SB_ITERATE=1 \
ABLATION_IDS="" \
DATA_DIR="$PWD/benchmarks/origin/random" \
INPUT_LIST="$PWD/benchmarks/input_lists/random_jaccard_confidence_4way/part1.txt" \
RESULT_FILE="$PWD/result/adaptive_k/final_jaccard_confidence/random_part1.txt" \
MAX_JOBS=15 \
TIMEOUT_SECS=3600 \
./scripts/vm_random.sh 2
```

### 机器2

```bash
cd /home/experimenter/Desktop/lyc/EDPLLSim/AAAI_EDPLLSim

ADAPTIVE_K=1 \
TIMING=1 \
VAR_LIMIT=999 \
SB_ITERATE=1 \
ABLATION_IDS="" \
DATA_DIR="$PWD/benchmarks/origin/random" \
INPUT_LIST="$PWD/benchmarks/input_lists/random_jaccard_confidence_4way/part2.txt" \
RESULT_FILE="$PWD/result/adaptive_k/final_jaccard_confidence/random_part2.txt" \
MAX_JOBS=15 \
TIMEOUT_SECS=3600 \
./scripts/vm_random.sh 2
```

### 机器3

```bash
cd /home/experimenter/Desktop/lyc/EDPLLSim/AAAI_EDPLLSim

ADAPTIVE_K=1 \
TIMING=1 \
VAR_LIMIT=999 \
SB_ITERATE=1 \
ABLATION_IDS="" \
DATA_DIR="$PWD/benchmarks/origin/random" \
INPUT_LIST="$PWD/benchmarks/input_lists/random_jaccard_confidence_4way/part3.txt" \
RESULT_FILE="$PWD/result/adaptive_k/final_jaccard_confidence/random_part3.txt" \
MAX_JOBS=15 \
TIMEOUT_SECS=3600 \
./scripts/vm_random.sh 2
```

### 机器4

```bash
cd /home/experimenter/Desktop/lyc/EDPLLSim/AAAI_EDPLLSim

ADAPTIVE_K=1 \
TIMING=1 \
VAR_LIMIT=999 \
SB_ITERATE=1 \
ABLATION_IDS="" \
DATA_DIR="$PWD/benchmarks/origin/random" \
INPUT_LIST="$PWD/benchmarks/input_lists/random_jaccard_confidence_4way/part4.txt" \
RESULT_FILE="$PWD/result/adaptive_k/final_jaccard_confidence/random_part4.txt" \
MAX_JOBS=15 \
TIMEOUT_SECS=3600 \
./scripts/vm_random.sh 2
```

## 3. 合并 Random

```bash
cd /home/experimenter/Desktop/lyc/EDPLLSim/AAAI_EDPLLSim

python3 scripts/merge_vm_results.py \
  --output result/adaptive_k/final_jaccard_confidence/random.txt \
  result/adaptive_k/final_jaccard_confidence/random_part1.txt \
  result/adaptive_k/final_jaccard_confidence/random_part2.txt \
  result/adaptive_k/final_jaccard_confidence/random_part3.txt \
  result/adaptive_k/final_jaccard_confidence/random_part4.txt

rg -c '^Input constraints:' \
  result/adaptive_k/final_jaccard_confidence/random.txt
```

最后应输出 `2840`。

## 4. Thin、Application、Multiple Knapsack、STN

```bash
cd /home/experimenter/Desktop/lyc/EDPLLSim/AAAI_EDPLLSim

datasets=(thin application multiple_knapsack STN)
runners=(vm_thin.sh vm_application.sh vm_multiple_knapsack.sh vm_STN.sh)

for index in "${!datasets[@]}"; do
  dataset="${datasets[$index]}"
  runner="${runners[$index]}"

  ADAPTIVE_K=1 \
  TIMING=1 \
  VAR_LIMIT=999 \
  SB_ITERATE=1 \
  ABLATION_IDS="" \
  DATA_DIR="$PWD/benchmarks/origin/$dataset" \
  RESULT_FILE="$PWD/result/adaptive_k/final_jaccard_confidence/$dataset.txt" \
  MAX_JOBS=15 \
  TIMEOUT_SECS=3600 \
  "./scripts/$runner" 2 || exit 1
done
```

## 5. 重新生成四份 Random 清单

只有在需要重新平衡四台机器工作量时才执行：

```bash
python3 scripts/split_random_by_runtime.py
```

默认读取最终 Jaccard-Confidence Random 结果，并写入：

```text
benchmarks/input_lists/random_jaccard_confidence_4way/
```
