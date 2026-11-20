#!/usr/bin/env bash
# Formal experiment runner for the random dataset.

VM_CALLER=$0
VM_DATASET_NAME="random"
VM_DATA_DIR_REL="benchmarks/origin/random"
VM_RESULT_FILE_REL="result/Oct/only/one_random.txt"

wrapper_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$wrapper_dir/vm_common.sh" "$@"
