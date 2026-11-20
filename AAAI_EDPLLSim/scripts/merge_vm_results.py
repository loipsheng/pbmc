#!/usr/bin/env python3
"""Merge vm_common.sh result logs and rebuild their aggregate summary.

By default, duplicate input names are rejected.  ``--prefer-success`` supports
reproducibility reruns: a later successful record replaces an earlier failed
record, while the first record is retained for all other duplicate pairs.
"""

from __future__ import annotations

import argparse
import re
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


CONFIG_END = "======================================="
SUMMARY_START = "========== SUMMARY =========="
INPUT_RE = re.compile(r"^Input constraints:\s+(.+?)(?:\s+-\d+)?\s*$", re.MULTILINE)
RECORD_END_RE = re.compile(r"^\*+$", re.MULTILINE)


@dataclass
class Record:
    name: str
    status: str
    exit_code: int
    signal: int
    runtime: float | None
    decision_time: float
    search_simplification_time: float | None
    total_simplification_time: float | None
    peak_rss: float | None
    external_peak_rss: float | None
    external_peak_vmem: float | None
    cache_lookups: int | None
    cache_hits: int | None
    cache_misses: int | None
    peak_cache_entries: int | None
    cache_cleanups: int | None
    deleted_cache_entries: int | None
    row_template_requests: int | None
    row_template_reuses: int | None


def first_number(text: str, field: str, cast=float):
    match = re.search(
        rf"^(?:\[INFO\]\s+)?{re.escape(field)}:\s*([0-9.eE+-]+)\s*$",
        text,
        re.MULTILINE,
    )
    return cast(match.group(1)) if match else None


def split_log(path: Path):
    text = path.read_text(errors="replace")
    config_end = text.find(CONFIG_END)
    summary_start = text.rfind(SUMMARY_START)
    if config_end < 0 or summary_start < 0:
        raise ValueError(f"invalid result log: {path}")
    config_end += len(CONFIG_END)
    return text[:config_end], text[config_end:summary_start].strip("\n"), text[summary_start:]


def parse_records(body: str, source: Path):
    inputs = list(INPUT_RE.finditer(body))
    records = []
    for index, match in enumerate(inputs):
        end = inputs[index + 1].start() if index + 1 < len(inputs) else len(body)
        segment = body[match.end():end]
        name = Path(match.group(1)).name
        runtime = first_number(segment, "Run time")
        if runtime is not None:
            status = "OK"
            exit_code = 0
            signal = 0
        else:
            status_match = re.search(r"^Failure category:\s*(\S+)", segment, re.MULTILINE)
            if not status_match:
                raise ValueError(f"missing status for {name} in {source}")
            status = status_match.group(1)
            exit_code = first_number(segment, "Exit code", int) or 0
            signal = first_number(segment, "Signal", int) or 0

        decision_time = first_number(segment, "Adaptive K decision time (s)")
        if decision_time is None:
            decision_time = 0.0

        records.append(
            Record(
                name=name,
                status=status,
                exit_code=exit_code,
                signal=signal,
                runtime=runtime,
                decision_time=decision_time,
                search_simplification_time=first_number(
                    segment, "Search simplification time (s)"
                ),
                total_simplification_time=first_number(
                    segment, "Total simplification time (s)"
                ),
                peak_rss=first_number(segment, "Peak RSS (MB)"),
                external_peak_rss=first_number(segment, "External peak RSS (MB)"),
                external_peak_vmem=first_number(
                    segment, "External peak virtual memory (MB)"
                ),
                cache_lookups=first_number(segment, "Cache lookups", int),
                cache_hits=first_number(segment, "Cache hits", int),
                cache_misses=first_number(segment, "Cache misses", int),
                peak_cache_entries=first_number(
                    segment, "Peak number of cache entries", int
                ),
                cache_cleanups=first_number(segment, "Number of cache cleanup", int),
                deleted_cache_entries=first_number(
                    segment, "Number of deleted entries", int
                ),
                row_template_requests=first_number(
                    segment, "Row template requests", int
                ),
                row_template_reuses=first_number(segment, "Row template reuses", int),
            )
        )
    return records


def parse_record_chunks(body: str, source: Path):
    """Return each input record together with the exact text that produced it."""
    inputs = list(INPUT_RE.finditer(body))
    if not inputs:
        if body.strip():
            raise ValueError(f"no input records found in {source}")
        return []

    chunks = []
    start = 0
    for index, input_match in enumerate(inputs):
        if input_match.start() < start:
            raise ValueError(f"overlapping input records in {source}")
        end_match = RECORD_END_RE.search(body, input_match.end())
        if not end_match:
            raise ValueError(
                f"missing record terminator after input {index + 1} in {source}"
            )
        end = end_match.end()
        chunk = body[start:end].strip("\n")
        chunk_records = parse_records(chunk, source)
        if len(chunk_records) != 1:
            raise ValueError(
                f"expected one input in record {index + 1} of {source}, "
                f"found {len(chunk_records)}"
            )
        chunks.append((chunk_records[0], chunk))
        start = end

    if body[start:].strip():
        raise ValueError(f"unexpected text after final input record in {source}")
    return chunks


def average(records, attribute):
    values = [getattr(record, attribute) for record in records]
    if any(value is None for value in values):
        raise ValueError(f"missing {attribute} in a successful record")
    return sum(values) / len(values) if values else 0.0


def total(records, attribute):
    values = [getattr(record, attribute) for record in records]
    if any(value is None for value in values):
        raise ValueError(f"missing {attribute} in a successful record")
    return sum(values)


def histogram(values):
    counts = Counter(values)
    return ",".join(f"{value}:{counts[value]}" for value in sorted(counts)) or "none"


def build_summary(records):
    successful = [record for record in records if record.status == "OK"]
    failed = [record for record in records if record.status != "OK"]
    status_counts = Counter(record.status for record in failed)

    runtime_total = total(successful, "runtime")
    simplification_total = total(successful, "total_simplification_time")
    cache_hits = total(successful, "cache_hits")
    cache_misses = total(successful, "cache_misses")
    template_requests = total(successful, "row_template_requests")
    template_reuses = total(successful, "row_template_reuses")

    cache_hit_rate = (
        f"{100.0 * cache_hits / (cache_hits + cache_misses):.2f}%"
        if cache_hits + cache_misses
        else "N/A"
    )
    template_reuse_rate = (
        f"{100.0 * template_reuses / template_requests:.2f}%"
        if template_requests
        else "N/A"
    )
    simplification_ratio = (
        f"{100.0 * simplification_total / runtime_total:.2f}%"
        if runtime_total
        else "N/A"
    )

    lines = [
        SUMMARY_START,
        "Dataset: random",
        f"Total inputs: {len(records)}",
        f"Successful inputs: {len(successful)}",
        f"Failed inputs: {len(failed)}",
        f"Timeout inputs: {status_counts['TIMEOUT']}",
        f"OOM inputs: {status_counts['OOM']}",
        f"Signal inputs: {status_counts['SIGNAL']}",
        f"Exit-error inputs: {status_counts['EXIT_ERROR']}",
        f"Output-error inputs: {status_counts['OUTPUT_ERROR']}",
        f"Failure exit-code histogram: {histogram(r.exit_code for r in failed)}",
        f"Failure signal histogram: {histogram(r.signal for r in failed if r.signal)}",
        f"Average runtime (s): {average(successful, 'runtime'):.9f}",
        "Average adaptive K decision time, all inputs (s): "
        f"{sum(r.decision_time for r in records) / len(records):.9f}",
        "Average search simplification time, successful inputs (s): "
        f"{average(successful, 'search_simplification_time'):.9f}",
        "Average total simplification time, successful inputs (s): "
        f"{average(successful, 'total_simplification_time'):.9f}",
        "Aggregate simplification time ratio, successful inputs: "
        f"{simplification_ratio}",
        f"Average Peak RSS (MB): {average(successful, 'peak_rss'):.2f}",
        "Average external peak RSS (MB): "
        f"{average(successful, 'external_peak_rss'):.2f}",
        "Average external peak virtual memory (MB): "
        f"{average(successful, 'external_peak_vmem'):.2f}",
        f"Total cache lookups: {total(successful, 'cache_lookups')}",
        f"Total cache hits: {cache_hits}",
        f"Total cache misses: {cache_misses}",
        f"Aggregate cache hit rate: {cache_hit_rate}",
        "Peak number of cache entries: "
        f"{max(r.peak_cache_entries for r in successful)}",
        f"Total number of cache cleanup: {total(successful, 'cache_cleanups')}",
        "Total number of deleted entries: "
        f"{total(successful, 'deleted_cache_entries')}",
        f"Total row-template requests: {template_requests}",
        f"Total row-template reuses: {template_reuses}",
        f"Aggregate row-template reuse rate: {template_reuse_rate}",
        "=============================",
    ]
    return "\n".join(lines)


def merged_config(config: str, count: int, output: Path, sources, prefer_success=False):
    config = re.sub(r"^Inputs:\s*\d+$", f"Inputs: {count}", config, flags=re.MULTILINE)
    config = re.sub(
        r"^Input list:.*$",
        "Input lists: merged complete random dataset",
        config,
        flags=re.MULTILINE,
    )
    config = re.sub(r"^Output:.*$", f"Output: {output}", config, flags=re.MULTILINE)
    config = re.sub(r"^Merged sources:.*\n?", "", config, flags=re.MULTILINE)
    config = re.sub(r"^Merge policy:.*\n?", "", config, flags=re.MULTILINE)
    metadata = ["Merged sources: " + ", ".join(str(source) for source in sources)]
    if prefer_success:
        metadata.append(
            "Merge policy: later successful rerun replaces earlier failed record"
        )
    return config.replace(CONFIG_END, "\n".join(metadata) + "\n" + CONFIG_END)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--prefer-success",
        action="store_true",
        help=(
            "allow duplicate inputs and replace an earlier failed record with "
            "a later successful record"
        ),
    )
    parser.add_argument("sources", nargs="+", type=Path)
    args = parser.parse_args()

    configs = []
    record_bodies = []
    records = []
    record_indices = {}
    for source in args.sources:
        config, body, _summary = split_log(source)
        configs.append(config)
        for record, record_body in parse_record_chunks(body, source):
            previous_index = record_indices.get(record.name)
            if previous_index is None:
                record_indices[record.name] = len(records)
                records.append(record)
                record_bodies.append(record_body)
                continue

            if not args.prefer_success:
                raise ValueError(f"duplicate input in {source}: {record.name}")

            previous = records[previous_index]
            if previous.status != "OK" and record.status == "OK":
                records[previous_index] = record
                record_bodies[previous_index] = record_body

    output_text = "\n".join(
        [
            merged_config(
                configs[0],
                len(records),
                args.output,
                args.sources,
                prefer_success=args.prefer_success,
            ),
            *record_bodies,
            build_summary(records),
            "",
        ]
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output_text)
    print(f"merged {len(records)} unique inputs into {args.output}")


if __name__ == "__main__":
    main()
