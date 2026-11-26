#!/usr/bin/env python3
"""Gate 1 empty recursive-simplification integration tests."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True

from mc_oracle import count_models


_EXACT_COUNT = re.compile(r"^c s exact arb int ([0-9]+)$", re.MULTILINE)
_STABLE_STATS = (
    "equality groups",
    "conflicts",
    "decisions",
    "Components",
    "Split",
    "Cache lookup",
    "Cache hit",
    "Cache reduce",
    "SAT calls",
    "SAT starts",
    "backjumps",
    "chronological backs",
)


def run_solver(solver: Path, instance: Path, *options: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(solver), "-no-td", *options, str(instance)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def exact_count(result: subprocess.CompletedProcess, label: str) -> int:
    if result.returncode != 0:
        raise AssertionError(f"{label}: solver exited {result.returncode}\n{result.stdout}")
    match = _EXACT_COUNT.search(result.stdout)
    if match is None:
        raise AssertionError(f"{label}: exact MC result not found\n{result.stdout}")
    return int(match.group(1))


def stable_stats(output: str, label: str) -> dict[str, str]:
    result = {}
    for key in _STABLE_STATS:
        match = re.search(
            rf"^c o {re.escape(key)}\s+=\s+(.+?)\s*$", output, re.MULTILINE
        )
        if match is None:
            raise AssertionError(f"{label}: missing statistic {key}\n{output}")
        result[key] = match.group(1)
    return result


def simplification_stats(output: str, label: str) -> None:
    values = {}
    for key in ("calls", "changed", "conflict", "budget"):
        match = re.search(
            rf"^c o simplification {key}\s+=\s+([0-9]+)\s*$",
            output,
            re.MULTILINE,
        )
        if match is None:
            raise AssertionError(f"{label}: missing simplification {key}\n{output}")
        values[key] = int(match.group(1))
    if values["calls"] == 0:
        raise AssertionError(f"{label}: empty framework was not called")
    if any(values[key] != 0 for key in ("changed", "conflict", "budget")):
        raise AssertionError(f"{label}: empty framework changed state: {values}")
    if "c o Cache mode            = legacy" not in output:
        raise AssertionError(f"{label}: legacy cache mode was not reported\n{output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--solver", required=True, type=Path)
    args = parser.parse_args()
    solver = args.solver.resolve()
    cases = Path(__file__).resolve().parent / "cases"

    filenames = (
        "or_ge.opb",
        "less_equal.opb",
        "equality_one.opb",
        "signed_equality.opb",
        "two_components.opb",
        "conflict_unique.opb",
        "backjump_without_lit.opb",
    )
    for filename in filenames:
        instance = cases / filename
        disabled = run_solver(
            solver, instance, "-mode=0", "-no-inprocess", "-vs=0"
        )
        enabled = run_solver(
            solver, instance, "-mode=0", "-inprocess", "-ablation=1:2:3:4:5:6", "-vs=0"
        )
        oracle = count_models(instance)
        off_count = exact_count(disabled, f"{filename} disabled")
        on_count = exact_count(enabled, f"{filename} enabled")
        if off_count != oracle or on_count != oracle:
            raise AssertionError(
                f"{filename}: disabled={off_count}, enabled={on_count}, oracle={oracle}"
            )
        off_stats = stable_stats(disabled.stdout, f"{filename} disabled")
        on_stats = stable_stats(enabled.stdout, f"{filename} enabled")
        if off_stats != on_stats:
            raise AssertionError(
                f"{filename}: search statistics changed\n"
                f"disabled={off_stats}\nenabled={on_stats}"
            )
        if "c o simplification calls" in disabled.stdout:
            raise AssertionError(f"{filename}: disabled run printed simplification stats")
        simplification_stats(enabled.stdout, f"{filename} enabled")

    base = cases / "or_ge.opb"
    for option in ("-rmvsatcl", "-no-rmvsatcl", "-rmvsatclthd=2"):
        result = run_solver(solver, base, "-mode=0", option)
        if result.returncode == 0 or "Unknown flag" not in result.stdout:
            raise AssertionError(f"legacy option {option} was not rejected\n{result.stdout}")

    print(
        "Gate 1 empty framework (-ablation=1:2:3:4:5:6) counts and search statistics match; "
        "legacy rmvsatcl options are rejected"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
