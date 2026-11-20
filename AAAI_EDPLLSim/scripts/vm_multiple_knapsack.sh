#!/usr/bin/env bash
# Formal experiment runner for the multiple-knapsack dataset.

VM_CALLER=$0
VM_DATASET_NAME="multiple_knapsack"
VM_DATA_DIR_REL="benchmarks/origin/multiple_knapsack"
VM_RESULT_FILE_REL="result/Oct/only/one_multiple_knapsack.txt"

wrapper_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$wrapper_dir/vm_common.sh" "$@"
