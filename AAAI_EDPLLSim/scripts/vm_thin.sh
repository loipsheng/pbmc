#!/usr/bin/env bash
# Formal experiment runner for the thin dataset.

VM_CALLER=$0
VM_DATASET_NAME="thin"
VM_DATA_DIR_REL="benchmarks/origin/thin"
VM_RESULT_FILE_REL="result/Oct/only/one_thin.txt"

wrapper_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$wrapper_dir/vm_common.sh" "$@"
