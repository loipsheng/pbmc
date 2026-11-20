#!/usr/bin/env bash
# Shared runner for the five formal EDPLLSim datasets. Dataset-specific
# vm_*.sh files only select paths; experiment controls and result parsing live
# here so all five datasets use exactly the same protocol.

set +m
set -uo pipefail

die() {
  echo "ERROR: $*" >&2
  exit 1
}

simplification_name() {
  case "$1" in
    0) echo "RIR" ;;
    1) echo "RV" ;;
    2) echo "SB" ;;
    3) echo "SC" ;;
    4) echo "RSR" ;;
    5) echo "RPR" ;;
    6) echo "SB-LP" ;;
    7) echo "RIR-LP" ;;
  esac
}

is_simplification_disabled() {
  local id="$1"
  [[ ":$ABLATION_IDS:" == *":$id:"* ]]
}

format_simplifications() {
  local mode="$1"
  local id name result=""

  # ID 0 is the always-available interval RIR branch.  K normally selects
  # SB/RIR for N<=K and SB-LP/RIR-LP for N>K.  If ID 7 disables RIR-LP,
  # the mandatory row-removal stage falls back to RIR for every component.
  for id in 1 2 6 3 0 7 5 4; do
    if [[ "$id" == 0 ]]; then
      [[ "$mode" == "enabled" ]] || continue
      name="RIR"
    elif [[ "$mode" == "enabled" ]]; then
      is_simplification_disabled "$id" && continue
      name=$(simplification_name "$id")
    else
      is_simplification_disabled "$id" || continue
      name=$(simplification_name "$id")
    fi

    [[ -n "$result" ]] && result+=", "
    result+="$name"
  done

  printf '%s\n' "${result:-none}"
}

for required_name in VM_DATASET_NAME VM_DATA_DIR_REL VM_RESULT_FILE_REL; do
  [[ -n "${!required_name:-}" ]] || die "missing wrapper setting: $required_name"
done

if [[ $# -ne 1 || ! "$1" =~ ^[1-9]$|^10$ ]]; then
  echo "Usage: ${VM_CALLER:-$0} <heur_id: 1-10>" >&2
  exit 1
fi

HEUR=$1
# K controls both paired stages: SB/RIR for N<=K and SB-LP/RIR-LP for N>K.
VAR_LIMIT=${VAR_LIMIT:-999}
ADAPTIVE_K=${ADAPTIVE_K:-0}
TIMING=${TIMING:-1}
SB_ITERATE=${SB_ITERATE:-1}
# K selects RIR for N<=K and RIR-LP for N>K.  ID 7 disables the optional
# RIR-LP enhancement and makes the row-removal stage fall back to RIR.
ABLATION_IDS=${ABLATION_IDS-"1:2:3:5:6:7"}
CACHE_ADMISSION_MAX_CELLS=${CACHE_ADMISSION_MAX_CELLS:-20}
CACHE_MAX_AGE_INSERTS=${CACHE_MAX_AGE_INSERTS:-51200}

MAX_JOBS=${MAX_JOBS:-10}
PER_PROCESS_MEM_GB=${PER_PROCESS_MEM_GB:-10}
TIMEOUT_SECS=${TIMEOUT_SECS:-3600}
INPUT_LIMIT=${INPUT_LIMIT:-0}
INPUT_LIST=${INPUT_LIST:-}
FAILURE_TAIL_LINES=${FAILURE_TAIL_LINES:-20}

[[ "$VAR_LIMIT" =~ ^[0-9]+$ ]] || die "VAR_LIMIT must be a non-negative integer"
[[ "$ADAPTIVE_K" =~ ^[01]$ ]] || die "ADAPTIVE_K must be 0 or 1"
[[ "$TIMING" =~ ^[01]$ ]] || die "TIMING must be 0 or 1"
[[ "$SB_ITERATE" =~ ^[01]$ ]] || die "SB_ITERATE must be 0 or 1"
[[ "$CACHE_ADMISSION_MAX_CELLS" =~ ^[0-9]+$ ]] || \
  die "CACHE_ADMISSION_MAX_CELLS must be a non-negative integer"
[[ "$CACHE_MAX_AGE_INSERTS" =~ ^[0-9]+$ ]] || \
  die "CACHE_MAX_AGE_INSERTS must be a non-negative integer"
[[ "$MAX_JOBS" =~ ^[1-9][0-9]*$ ]] || die "MAX_JOBS must be positive"
[[ "$PER_PROCESS_MEM_GB" =~ ^[1-9][0-9]*$ ]] || \
  die "PER_PROCESS_MEM_GB must be positive"
[[ "$TIMEOUT_SECS" =~ ^[1-9][0-9]*$ ]] || die "TIMEOUT_SECS must be positive"
[[ "$INPUT_LIMIT" =~ ^[0-9]+$ ]] || die "INPUT_LIMIT must be non-negative"
[[ "$FAILURE_TAIL_LINES" =~ ^[1-9][0-9]*$ ]] || \
  die "FAILURE_TAIL_LINES must be positive"

if [[ -n "$ABLATION_IDS" ]]; then
  IFS=':' read -r -a ablation_id_list <<<"$ABLATION_IDS"
  for ablation_id in "${ablation_id_list[@]}"; do
    [[ "$ablation_id" =~ ^[1-7]$ ]] || \
      die "ABLATION_IDS must contain only IDs 1-7 separated by ':'"
  done
fi

ENABLED_SIMPLIFICATIONS=$(format_simplifications enabled)
DISABLED_SIMPLIFICATIONS=$(format_simplifications disabled)

VM_COMMON_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$VM_COMMON_DIR/.." && pwd)
EXE=${EXE:-"$REPO_ROOT/EDPLLSim"}
DATA_DIR=${DATA_DIR:-"$REPO_ROOT/$VM_DATA_DIR_REL"}
RESULT_FILE=${RESULT_FILE:-"$REPO_ROOT/$VM_RESULT_FILE_REL"}

[[ -x "$EXE" ]] || die "missing executable: $EXE (run make EDPLLSim first)"
[[ -d "$DATA_DIR" ]] || die "missing dataset directory: $DATA_DIR"
if [[ -n "$INPUT_LIST" ]]; then
  [[ -f "$INPUT_LIST" ]] || die "missing input list: $INPUT_LIST"
fi

mkdir -p "$(dirname -- "$RESULT_FILE")"
RUN_TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/edpllsim_${VM_DATASET_NAME}.XXXXXX")
VMEM_LIMIT_KB=$((PER_PROCESS_MEM_GB * 1024 * 1024))

cleanup() {
  jobs -pr | xargs -r kill 2>/dev/null || true
  rm -rf "$RUN_TMP_DIR"
}
trap cleanup EXIT INT TERM

append_result() {
  (
    flock -x 200
    cat >&200
  ) 200>>"$RESULT_FILE"
}

kb_to_mb() {
  local kb="${1:-0}"
  awk -v kb="$kb" 'BEGIN{printf "%.2f", kb / 1024.0}'
}

extract_field() {
  local output="$1"
  local label="$2"
  printf '%s\n' "$output" | awk -F': ' -v label="$label" \
    '$1 == label {print $2; exit}'
}

get_process_tree_pids() {
  local root_pid="$1"
  local queue=("$root_pid")
  local all=()
  local pid child children

  while (( ${#queue[@]} > 0 )); do
    pid="${queue[0]}"
    queue=("${queue[@]:1}")
    if ! kill -0 "$pid" 2>/dev/null; then
      continue
    fi
    all+=("$pid")
    children=$(pgrep -P "$pid" 2>/dev/null || true)
    for child in $children; do
      queue+=("$child")
    done
  done
  printf '%s\n' "${all[@]}"
}

# Updates max_ext_rss_kb and max_ext_vmem_kb in run_single_experiment.
sample_process_tree_memory() {
  local root_pid="$1"
  local pid hwm vpeak
  local sample_rss_kb=0
  local sample_vmem_kb=0

  while read -r pid; do
    [[ -z "$pid" || ! -r "/proc/$pid/status" ]] && continue
    read -r hwm vpeak < <(awk '
      /^VmHWM:/  { hwm=$2 }
      /^VmPeak:/ { vpeak=$2 }
      END { printf "%d %d\n", hwm+0, vpeak+0 }
    ' "/proc/$pid/status" 2>/dev/null)
    [[ "$hwm" =~ ^[0-9]+$ ]] || hwm=0
    [[ "$vpeak" =~ ^[0-9]+$ ]] || vpeak=0
    sample_rss_kb=$((sample_rss_kb + hwm))
    sample_vmem_kb=$((sample_vmem_kb + vpeak))
  done < <(get_process_tree_pids "$root_pid")

  if (( sample_rss_kb > max_ext_rss_kb )); then
    max_ext_rss_kb=$sample_rss_kb
  fi
  if (( sample_vmem_kb > max_ext_vmem_kb )); then
    max_ext_vmem_kb=$sample_vmem_kb
  fi
}

write_failure() {
  local filepath="$1"
  local result_code="$2"
  local status="$3"
  local exit_code="$4"
  local signal="$5"
  local max_ext_rss_mb="$6"
  local max_ext_vmem_mb="$7"
  local stdout_log="$8"
  local stderr_log="$9"
  local stat_log="${10}"
  local decision_time
  decision_time=$(awk -F': ' \
    '$1 == "[INFO] Adaptive K decision time (s)" {print $2; exit}' \
    "$stderr_log")
  [[ -n "$decision_time" ]] || decision_time=0

  {
    echo "*************************************"
    echo "Input constraints: $filepath $result_code"
    echo "Failure category: $status"
    echo "Exit code: $exit_code"
    echo "Signal: $signal"
    echo "External peak RSS (MB): $max_ext_rss_mb"
    echo "External peak virtual memory (MB): $max_ext_vmem_mb"
    # Keep these structured fields outside the stderr tail so merged summaries
    # can recover them even when later diagnostics push them out of view.
    echo "Adaptive K decision time (s): $decision_time"
    echo "stderr tail (last $FAILURE_TAIL_LINES lines):"
    tail -n "$FAILURE_TAIL_LINES" "$stderr_log"
    echo "stdout tail (last $FAILURE_TAIL_LINES lines):"
    tail -n "$FAILURE_TAIL_LINES" "$stdout_log"
    echo "*************************************"
  } | append_result

  printf '%s 0 0 %s %s 0 0 0 0 0 0 0 0 %s %s %s 0 0 0 %s\n' \
    "$status" "$max_ext_rss_mb" "$max_ext_vmem_mb" \
    "$exit_code" "$signal" 0 "$decision_time" >"$stat_log"
}

run_single_experiment() {
  local filepath="$1"
  local log_prefix stat_log stdout_log stderr_log
  log_prefix=$(basename "$filepath")
  log_prefix=${log_prefix%.*}
  stat_log="$RUN_TMP_DIR/${log_prefix}_stat.log"
  stdout_log="$RUN_TMP_DIR/${log_prefix}_stdout.log"
  stderr_log="$RUN_TMP_DIR/${log_prefix}_stderr.log"
  : >"$stdout_log"
  : >"$stderr_log"

  local cmd=(
    "$EXE" "$filepath"
    "-heur=$HEUR"
    "-varlimit=$VAR_LIMIT"
    "-adaptive-k=$ADAPTIVE_K"
    "-timing=$TIMING"
    "-sb-iterate=$SB_ITERATE"
    "-ablation=$ABLATION_IDS"
    "-cache-admission-max-cells=$CACHE_ADMISSION_MAX_CELLS"
    "-cache-max-age-inserts=$CACHE_MAX_AGE_INSERTS"
  )

  local max_ext_rss_kb=0
  local max_ext_vmem_kb=0
  (
    ulimit -v "$VMEM_LIMIT_KB"
    LC_ALL=C timeout --verbose -k 10s "$TIMEOUT_SECS" "${cmd[@]}"
    child_exit_code=$?
    exit "$child_exit_code"
  ) >"$stdout_log" 2>"$stderr_log" &
  local runner_pid=$!

  while kill -0 "$runner_pid" 2>/dev/null; do
    sample_process_tree_memory "$runner_pid"
    sleep 0.1
  done

  local exit_code=0
  wait "$runner_pid" 2>/dev/null || exit_code=$?

  local output stderr_output combined_output
  local max_ext_rss_mb max_ext_vmem_mb signal=0
  output=$(<"$stdout_log")
  stderr_output=$(<"$stderr_log")
  combined_output=$(printf '%s\n%s\n' "$stderr_output" "$output")
  max_ext_rss_mb=$(kb_to_mb "$max_ext_rss_kb")
  max_ext_vmem_mb=$(kb_to_mb "$max_ext_vmem_kb")

  # Shells encode signal S as 128+S. Values above 192 may instead be a
  # negative program return value, e.g. EDPLLSim -2 becomes 254.
  if (( exit_code > 128 && exit_code <= 192 )); then
    signal=$((exit_code - 128))
  fi

  if [[ $exit_code -ne 0 ]]; then
    local status="EXIT_ERROR"
    local result_code=-3
    if [[ $exit_code -eq 124 ]] || \
       grep -Eq '^timeout: sending signal (TERM|KILL)' "$stderr_log"; then
      status="TIMEOUT"
      result_code=-1
    elif printf '%s\n' "$combined_output" | grep -Eqi \
      "Cannot allocate memory|std::bad_alloc|bad_alloc|out of memory|memory exhausted|malloc\(\) failed|mmap\(\) failed|GLPK.*out of memory|GNU MP.*Cannot allocate memory"; then
      status="OOM"
      result_code=-2
    elif (( signal > 0 )); then
      status="SIGNAL"
    fi
    write_failure "$filepath" "$result_code" "$status" "$exit_code" \
      "$signal" "$max_ext_rss_mb" "$max_ext_vmem_mb" \
      "$stdout_log" "$stderr_log" "$stat_log"
    return
  fi

  local solution_count runtime peak_rss lookups hits misses
  local entries_peak cleanups deleted_entries template_requests template_reuses
  local decision_time search_simplification_time
  local total_simplification_time
  local simplification_time_ratio
  solution_count=$(extract_field "$output" "Number of solutions")
  runtime=$(extract_field "$output" "Run time")
  peak_rss=$(extract_field "$output" "Peak RSS (MB)")
  lookups=$(extract_field "$output" "Cache lookups")
  hits=$(extract_field "$output" "Cache hits")
  misses=$(extract_field "$output" "Cache misses")
  entries_peak=$(extract_field "$output" "Peak number of cache entries")
  cleanups=$(extract_field "$output" "Number of cache cleanup")
  deleted_entries=$(extract_field "$output" "Number of deleted entries")
  template_requests=$(extract_field "$output" "Row template requests")
  template_reuses=$(extract_field "$output" "Row template reuses")
  decision_time=$(extract_field "$output" "Adaptive K decision time (s)")
  search_simplification_time=$(extract_field "$output" "Search simplification time (s)")
  total_simplification_time=$(extract_field "$output" "Total simplification time (s)")
  simplification_time_ratio=$(extract_field "$output" "Simplification time ratio (%)")

  # Exit code 0 is not enough for a valid experiment record. Reject truncated
  # or stale solver output before it silently becomes a row of zeros.
  if [[ -z "$solution_count" || -z "$runtime" || -z "$peak_rss" ||
        -z "$lookups" || -z "$hits" || -z "$misses" ||
        -z "$entries_peak" || -z "$cleanups" || -z "$deleted_entries" ||
        -z "$template_requests" || -z "$template_reuses" ||
        -z "$decision_time" ||
        -z "$search_simplification_time" ||
        -z "$total_simplification_time" ||
        -z "$simplification_time_ratio" ]]; then
    write_failure "$filepath" -3 "OUTPUT_ERROR" 0 0 \
      "$max_ext_rss_mb" "$max_ext_vmem_mb" \
      "$stdout_log" "$stderr_log" "$stat_log"
    return
  fi

  # Preserve all current solver output and append independently sampled memory
  # inside the final delimiter block.
  { cat "$stderr_log"; cat "$stdout_log"; } | awk \
    -v ext_rss_mb="$max_ext_rss_mb" \
    -v ext_vmem_mb="$max_ext_vmem_mb" '
    { lines[NR] = $0 }
    END {
      if (NR > 0 && lines[NR] ~ /^\*+$/) {
        for (i = 1; i < NR; ++i) print lines[i];
        print "External peak RSS (MB): " ext_rss_mb;
        print "External peak virtual memory (MB): " ext_vmem_mb;
        print lines[NR];
      } else {
        for (i = 1; i <= NR; ++i) print lines[i];
        print "External peak RSS (MB): " ext_rss_mb;
        print "External peak virtual memory (MB): " ext_vmem_mb;
      }
    }' | append_result

  printf 'OK %s %s %s %s %s %s %s %s %s %s %s %s 0 0 %s %s %s %s %s\n' \
    "$runtime" "$peak_rss" "$max_ext_rss_mb" "$max_ext_vmem_mb" \
    "$lookups" "$hits" "$misses" "$entries_peak" "$cleanups" \
    "$deleted_entries" "$template_requests" "$template_reuses" \
    0 "$search_simplification_time" \
    "$total_simplification_time" "$simplification_time_ratio" \
    "$decision_time" >"$stat_log"
}

load_inputs() {
  if [[ -n "$INPUT_LIST" ]]; then
    INPUT_FILES=()
    local input_name input_path
    while IFS= read -r input_name || [[ -n "$input_name" ]]; do
      input_name=${input_name%$'\r'}
      [[ -n "$input_name" && "$input_name" != \#* ]] || continue
      [[ "$input_name" != /* ]] || \
        die "input-list entries must be relative to DATA_DIR: $input_name"
      input_path="$DATA_DIR/$input_name"
      [[ -f "$input_path" ]] || die "missing listed input: $input_path"
      INPUT_FILES+=("$input_path")
    done <"$INPUT_LIST"
  else
    # Run every current .in file. The sort only makes job submission and
    # INPUT_LIMIT smoke tests reproducible; parallel results are intentionally
    # appended in completion order and may be sorted later for Excel analysis.
    mapfile -t INPUT_FILES < <(
      find "$DATA_DIR" -maxdepth 1 -type f -name '*.in' -print | sort -V
    )
  fi

  if (( INPUT_LIMIT > 0 && INPUT_LIMIT < ${#INPUT_FILES[@]} )); then
    INPUT_FILES=("${INPUT_FILES[@]:0:INPUT_LIMIT}")
  fi
  (( ${#INPUT_FILES[@]} > 0 )) || die "no .in inputs found in $DATA_DIR"
}

write_summary() {
  local stat_files=("$RUN_TMP_DIR"/*_stat.log)
  local total_files=${#stat_files[@]}
  local success_files timeout_files oom_files signal_files exit_error_files
  local output_error_files fail_total
  success_files=$(grep -l '^OK' "${stat_files[@]}" 2>/dev/null | wc -l)
  timeout_files=$(grep -l '^TIMEOUT' "${stat_files[@]}" 2>/dev/null | wc -l)
  oom_files=$(grep -l '^OOM' "${stat_files[@]}" 2>/dev/null | wc -l)
  signal_files=$(grep -l '^SIGNAL' "${stat_files[@]}" 2>/dev/null | wc -l)
  exit_error_files=$(grep -l '^EXIT_ERROR' "${stat_files[@]}" 2>/dev/null | wc -l)
  output_error_files=$(grep -l '^OUTPUT_ERROR' "${stat_files[@]}" 2>/dev/null | wc -l)
  fail_total=$((timeout_files + oom_files + signal_files + exit_error_files + output_error_files))

  local avg_runtime avg_peak_rss avg_ext_rss avg_ext_vmem
  local total_lookups total_hits total_misses cache_hit_rate
  local peak_entries total_cleanups total_deleted_entries
  local template_requests template_reuses template_reuse_rate
  local avg_decision_time avg_search_simplification_time
  local avg_total_simplification_time aggregate_simplification_ratio
  local failure_exit_codes failure_signals
  avg_runtime=$(awk '/^OK/{sum+=$2; n++} END{if(n) printf "%.9f",sum/n; else print 0}' "${stat_files[@]}")
  avg_peak_rss=$(awk '/^OK/{sum+=$3; n++} END{if(n) printf "%.2f",sum/n; else print 0}' "${stat_files[@]}")
  avg_ext_rss=$(awk '/^OK/{sum+=$4; n++} END{if(n) printf "%.2f",sum/n; else print 0}' "${stat_files[@]}")
  avg_ext_vmem=$(awk '/^OK/{sum+=$5; n++} END{if(n) printf "%.2f",sum/n; else print 0}' "${stat_files[@]}")
  total_lookups=$(awk '/^OK/{sum+=$6} END{printf "%.0f",sum+0}' "${stat_files[@]}")
  total_hits=$(awk '/^OK/{sum+=$7} END{printf "%.0f",sum+0}' "${stat_files[@]}")
  total_misses=$(awk '/^OK/{sum+=$8} END{printf "%.0f",sum+0}' "${stat_files[@]}")
  cache_hit_rate=$(awk '/^OK/{hits+=$7; misses+=$8} END{if(hits+misses) printf "%.2f%%",100*hits/(hits+misses); else print "N/A"}' "${stat_files[@]}")
  peak_entries=$(awk '/^OK/ && $9>max{max=$9} END{printf "%.0f",max+0}' "${stat_files[@]}")
  total_cleanups=$(awk '/^OK/{sum+=$10} END{printf "%.0f",sum+0}' "${stat_files[@]}")
  total_deleted_entries=$(awk '/^OK/{sum+=$11} END{printf "%.0f",sum+0}' "${stat_files[@]}")
  template_requests=$(awk '/^OK/{sum+=$12} END{printf "%.0f",sum+0}' "${stat_files[@]}")
  template_reuses=$(awk '/^OK/{sum+=$13} END{printf "%.0f",sum+0}' "${stat_files[@]}")
  template_reuse_rate=$(awk '/^OK/{requests+=$12; reuses+=$13} END{if(requests) printf "%.2f%%",100*reuses/requests; else print "N/A"}' "${stat_files[@]}")
  avg_decision_time=$(awk '{sum+=$20; n++} END{if(n) printf "%.9f",sum/n; else print 0}' "${stat_files[@]}")
  avg_search_simplification_time=$(awk '/^OK/{sum+=$17; n++} END{if(n) printf "%.9f",sum/n; else print 0}' "${stat_files[@]}")
  avg_total_simplification_time=$(awk '/^OK/{sum+=$18; n++} END{if(n) printf "%.9f",sum/n; else print 0}' "${stat_files[@]}")
  aggregate_simplification_ratio=$(awk '/^OK/{runtime+=$2; simplification+=$18} END{if(runtime) printf "%.2f%%",100*simplification/runtime; else print "N/A"}' "${stat_files[@]}")
  failure_exit_codes=$(awk '$1!="OK"{count[$14]++} END{for(code in count) printf "%s%s:%d",sep,code,count[code],sep=","}' "${stat_files[@]}")
  failure_signals=$(awk '$1!="OK" && $15!=0{count[$15]++} END{for(sig in count) printf "%s%s:%d",sep,sig,count[sig],sep=","}' "${stat_files[@]}")
  [[ -n "$failure_exit_codes" ]] || failure_exit_codes=none
  [[ -n "$failure_signals" ]] || failure_signals=none

  {
    echo "========== SUMMARY =========="
    echo "Dataset: $VM_DATASET_NAME"
    echo "Total inputs: $total_files"
    echo "Successful inputs: $success_files"
    echo "Failed inputs: $fail_total"
    echo "Timeout inputs: $timeout_files"
    echo "OOM inputs: $oom_files"
    echo "Signal inputs: $signal_files"
    echo "Exit-error inputs: $exit_error_files"
    echo "Output-error inputs: $output_error_files"
    echo "Failure exit-code histogram: $failure_exit_codes"
    echo "Failure signal histogram: $failure_signals"
    echo "Average runtime (s): $avg_runtime"
    echo "Average adaptive K decision time, all inputs (s): $avg_decision_time"
    echo "Average search simplification time, successful inputs (s): $avg_search_simplification_time"
    echo "Average total simplification time, successful inputs (s): $avg_total_simplification_time"
    echo "Aggregate simplification time ratio, successful inputs: $aggregate_simplification_ratio"
    echo "Average Peak RSS (MB): $avg_peak_rss"
    echo "Average external peak RSS (MB): $avg_ext_rss"
    echo "Average external peak virtual memory (MB): $avg_ext_vmem"
    echo "Total cache lookups: $total_lookups"
    echo "Total cache hits: $total_hits"
    echo "Total cache misses: $total_misses"
    echo "Aggregate cache hit rate: $cache_hit_rate"
    echo "Peak number of cache entries: $peak_entries"
    echo "Total number of cache cleanup: $total_cleanups"
    echo "Total number of deleted entries: $total_deleted_entries"
    echo "Total row-template requests: $template_requests"
    echo "Total row-template reuses: $template_reuses"
    echo "Aggregate row-template reuse rate: $template_reuse_rate"
    echo "============================="
  } | append_result
}

load_inputs
: >"$RESULT_FILE"

{
  echo "========== EXPERIMENT CONFIG =========="
  echo "Dataset: $VM_DATASET_NAME"
  echo "Inputs: ${#INPUT_FILES[@]}"
  if [[ -n "$INPUT_LIST" ]]; then
    echo "Input list: $INPUT_LIST"
  fi
  echo "Command flags: -heur=$HEUR -varlimit=$VAR_LIMIT -adaptive-k=$ADAPTIVE_K -timing=$TIMING -sb-iterate=$SB_ITERATE -ablation=$ABLATION_IDS -cache-admission-max-cells=$CACHE_ADMISSION_MAX_CELLS -cache-max-age-inserts=$CACHE_MAX_AGE_INSERTS"
  echo "Timing instrumentation: $([[ $TIMING == 1 ]] && echo enabled || echo disabled)"
  if [[ "$SB_ITERATE" == 1 ]]; then
    echo "SB iteration mode: fixed-point"
  else
    echo "SB iteration mode: single-pass"
  fi
  echo "Enabled simplifications: $ENABLED_SIMPLIFICATIONS"
  echo "Disabled simplifications: $DISABLED_SIMPLIFICATIONS"
  echo "Memory limit per process: ${PER_PROCESS_MEM_GB}GB virtual memory"
  echo "Timeout per input: ${TIMEOUT_SECS}s"
  echo "Max concurrent jobs: $MAX_JOBS"
  echo "Output: $RESULT_FILE"
  echo "======================================="
} | tee -a "$RESULT_FILE"

for filepath in "${INPUT_FILES[@]}"; do
  while (( $(jobs -rp | wc -l) >= MAX_JOBS )); do
    sleep 1
  done
  run_single_experiment "$filepath" &
done
wait

write_summary
echo "Experiment complete: $RESULT_FILE"
