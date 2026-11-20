# Preprocessor Sim

`Sim` is the standalone preprocessor based on our simplification procedure. 
The default configuration sets `k=14`.

Build from the repository root:

```bash
make Sim
```

Preprocessing one instance:

```bash
./Sim -varlimit 14 benchmarks/origin/random/10_7_5.in -output benchmarks/preprocessing/random/10_7_5.in
```

Preprocessing all benchmark sets:

```bash
./Sim -varlimit 14 benchmarks/origin/random -output benchmarks/preprocessing/random
./Sim -varlimit 6 benchmarks/origin/thin -output benchmarks/preprocessing/thin
./Sim -varlimit 14 benchmarks/origin/STN -output benchmarks/preprocessing/STN
./Sim -varlimit 14 benchmarks/origin/multiple_knapsack -output benchmarks/preprocessing/multiple_knapsack
./Sim -varlimit 14 benchmarks/origin/application -output benchmarks/preprocessing/application
```
Options:

```text
-varlimit K       variable threshold, default 14
-output PATH      output file or directory
```

Some instances can be directly solved by Sim. In this case, the output file contains the result like:

```text
# known_count 123
```
