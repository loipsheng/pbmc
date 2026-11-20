## Introduction

This is the source code of the exact MCILC counter `EDPLLSim` proposed in the paper
"An Exhaustive DPLL Approach to Model Counting over Integer Linear Constraints with Simplification Techniques" and the benchmark sets.

## Build

System requirements: `Ubuntu` (tested on `Ubuntu 22.04`).

Library requirements: `GMP` and `GLPK`.

PaToH is included as a prebuilt static library in `./lib/libpatoh.a` with header `./src/patoh.h` (no extra install needed on Ubuntu).

Open a terminal in this directory:

`./build_utils.sh` - install `GMP` and `GLPK` (may take some time). Please make sure that the full path to this code package contains no spaces. If you encounter a `Permission denied` error, grant execute permission to the script first:

```bash
  chmod +x build_utils.sh
  ./build_utils.sh
```

`make` - build both executables: our counter `EDPLLSim`, and the preprocessor based on our simplification procedure `Sim`.

`make clean` - clean both EDPLLSim and Sim build outputs

`make test-adaptive-k` - build and run the adaptive-K unit tests

## Benchmarks

Original benchmarks are in `./benchmarks/origin`. 
Benchmarks preprocessed by `Sim` are written to `./benchmarks/preprocessing`.
See `./benchmarks/README.md` for the benchmark information and input format.

## Usage

### Basic usage:

```bash
./EDPLLSim <benchmark_file> [options]
```
The result is printed to stdout.

### Run EDPLLSim (in best configuration) to solve instances:
```bash
./EDPLLSim benchmarks/origin/random/10_7_5.in -heur=2 -varlimit=14 -ablation=5:7
./EDPLLSim benchmarks/origin/thin/thin_3_1.in -heur=2 -varlimit=6 -ablation=5:7
./EDPLLSim benchmarks/origin/STN/STN_T1_3.in -heur=2 -varlimit=14 -ablation=5:7
./EDPLLSim benchmarks/origin/multiple_knapsack/mk_3_1.in -heur=2 -varlimit=14 -ablation=5:7
./EDPLLSim benchmarks/origin/application/cubature_1.in -heur=2 -varlimit=14 -ablation=5:7
```

### Select one input-specific K at the root and keep it fixed during solving:

```bash
./EDPLLSim benchmarks/origin/thin/thin_3_1.in -heur=2 -adaptive-k=1 -sb-iterate=1 -ablation=5:7
```

Adaptive K has one implementation: Jaccard-Confidence. It computes K solely
from the original coefficient matrix and does not run an SB/RIR root probe.
The selected threshold is fixed for the complete solve and satisfies
`1 <= K <= n0` whenever `n0 > 0`.

### Run EDPLL with selected simplification techniques. For example, retain RV and the K-selected RIR/RIR-LP stage:
```bash
./EDPLLSim benchmarks/origin/random/10_7_5.in -heur=2 -varlimit=14 -ablation=2:3:4:5:6:7
```

### Run EDPLLSim without some simplification techniques. For example, EDPLLSim without SC, SB-LP and RIR-LP:
```bash
./EDPLLSim benchmarks/origin/random/10_7_5.in -heur=2 -varlimit=20 -ablation=3:6:7
```

### Run EDPLLSim with all simplifications and `k=10`:
```bash
./EDPLLSim benchmarks/origin/random/10_7_5.in -heur=2 -varlimit=10 -ablation=
```

### Run EDPLLSim with different heuristics. For example, EDPLLSim with BC:
```bash
./EDPLLSim benchmarks/origin/random/10_7_5.in -heur=4 -varlimit=14 -ablation=5:7
```

### Options

```text
-varlimit=K          select SB and RIR when N<=K; select SB-LP and RIR-LP when N>K; default 14
-adaptive-k=0|1      choose one input-specific Jaccard-Confidence K; default 0
-timing=0|1          record adaptive-K decision and search-simplification time; CLI default 0, experiment scripts default 1
-sb-iterate=0|1      run SB once (0) or to a fixed point (1), default 1
-heur=ID             branching heuristic, default DHP-P
-ablation=IDS        disable simplification techniques, e.g. -ablation=5:7
-cache-limit=K       backward-compatible alias for -cache-admission-max-cells
-cache-admission-max-cells=K  cache components with N*M <= K; 0 disables cache, default 20
-cache-max-age-inserts=N  default 51200; batch-clean limit is 20*N
```

For original row supports `S_i`, define:

```text
P     = m0*(m0-1)/2
J     = (1/P) * sum_{i<r} |S_i intersection S_r| / |S_i union S_r|
Jconf = J * P/(P+1)
K     = nearest_integer_half_up(n0*(1-Jconf)), clamped to [1,n0]
```

An empty-union pair contributes zero. The score and threshold use exact
rational arithmetic through positive half-up rounding. With timing enabled,
each run reports adaptive-K decision time separately from simplification time
accumulated during the DPLL search; K selection is not counted as a
simplification pass.

### Component cache

EDPLLSim now has one exact component-cache implementation. Its internal
pipeline is implemented in the unified `src/cache.h` and `src/cache.cc` files:

```text
current component
  -> normalize constraints and domains into a NormalizedComponent
  -> serialize each normalized coefficient row for RowTemplatePool lookup
  -> replace every row structure with a shared TemplateID while retaining RHS
  -> serialize domains + sorted (TemplateID, RHS) pairs as the final key
  -> query or insert ComponentCache
```

The final key contains the complete domains, normalized RHS values, and
delta-coded row-template IDs. A row template contains local variable positions
and arbitrary-precision coefficients. Hashes only select a bucket; a hit still
requires exact comparison of the complete serialized key.

Cache management follows Cachet's insertion-age policy. Hits do not renew an
entry. Lookup and insertion lazily remove stale suffixes only from the touched
bucket. The batch-clean threshold is `20 * max_age_inserts`; with the default
age 51,200, cleanup starts at 1,024,000 resident entries and retains the newest
quarter. There is no internal byte budget; experiment memory limits are
enforced externally.

The following exact counters are always printed:

```text
Cache lookups
Cache hits / misses
Cache hit rate
Peak number of cache entries
Number of cache cleanup
Number of deleted entries
Row template requests / reuses / reuse rate
```

`Peak RSS (MB)` is the operating system's process-wide peak resident-memory
measurement. There is no cache-statistics level option. Use
`-cache-admission-max-cells=0` to disable the component cache; statistics are
still printed and remain zero.

### Component-cache admission experiment

The random-dataset runner compares cache-disabled and different N*M admission
thresholds while using the single final cache implementation:

```bash
# cache disabled
./scripts/vm_random_cache_compare.sh 0

# cache components with N*M <= 20
./scripts/vm_random_cache_compare.sh 20

# cache components with N*M <= 100
./scripts/vm_random_cache_compare.sh 100
```

For a short smoke test:

```bash
INPUT_LIMIT=5 MAX_JOBS=1 TIMEOUT_SECS=60 \
  ./scripts/vm_random_cache_compare.sh 20
```

Run the quick exact-count and cache-statistics regression with:

```bash
./tests/cache_regression/run_cache_regression.sh
```

Run the normalization and final component-cache unit tests with:

```bash
make test-row-normalizer
make test-cache-key
```

Heuristic IDs:

| ID | Heuristic |
| --- | --- |
| 1 | DHP |
| 2 | DHP-P |
| 3 | SHP |
| 4 | BC |
| 5 | BC-P |
| 6 | D/W |
| 7 | D/W-P |

Ablation IDs:

| ID | Disabled |
| --- | --- |
| 1 | RV |
| 2 | SB |
| 3 | SC |
| 4 | RSR |
| 5 | RPR |
| 6 | SB-LP |
| 7 | RIR-LP |

RIR and RIR-LP are the two implementations of the individual-row-removal
stage. The same `-varlimit=K` threshold controls both paired stages: a current
component with `N<=K` uses SB followed by RIR, while `N>K` uses SB-LP followed
by RIR-LP. Because RV runs between the two stages, the RIR decision uses the
current number of variables after fixed variables have been removed. Ablation
ID 7 disables the optional RIR-LP enhancement. In that configuration the
individual-row-removal stage falls back to interval-based RIR even when
`N>K`, so RIR remains the mandatory baseline technique. RIR-LP computes both
a minimum (conflict detection) and a maximum (redundant-row detection).

## Run Sim

`Sim` takes `.in` benchmarks as input and outputs simplified benchmarks. 
See `./sim/README.md` for the full usage.

Example:

```bash
./Sim -varlimit 14 benchmarks/origin/random/10_7_5.in -output benchmarks/preprocessing/random/10_7_5.in
```

## Source Files

Main code of EDPLLSim is in `./src`:

```text
src/main.cc                command-line entry point
src/solver.cc              Algorithm 2 : main search algorithm
src/linear_constraint.cc   decomposition function and simplification procedure
src/heuristic.cc           variable selecting heuristics
```

Main code of Sim is in `./sim/src`. It reuses `linear_constraint.*`, `big_int.*`, and
`heuristic.*` from `./src`.
