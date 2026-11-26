#!/usr/bin/env python3
"""Gate 2A Basic residual-view and ablation integration tests."""

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
        raise AssertionError(f"{label}: exact count not found\n{result.stdout}")
    return int(match.group(1))


def stable_stats(output: str, label: str) -> dict[str, str]:
    values = {}
    for key in _STABLE_STATS:
        match = re.search(
            rf"^c o {re.escape(key)}\s+=\s+(.+?)\s*$", output, re.MULTILINE
        )
        if match is None:
            raise AssertionError(f"{label}: missing statistic {key}\n{output}")
        values[key] = match.group(1)
    return values


def stat(output: str, name: str, label: str) -> int:
    match = re.search(rf"^c o {re.escape(name)}\s+=\s+([0-9]+)\s*$", output, re.MULTILINE)
    if match is None:
        raise AssertionError(f"{label}: missing statistic {name}\n{output}")
    return int(match.group(1))


def require_failure(result: subprocess.CompletedProcess, text: str, label: str) -> None:
    if result.returncode == 0 or text not in result.stdout:
        raise AssertionError(f"{label}: expected failure containing {text!r}\n{result.stdout}")


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
        off = run_solver(solver, instance, "-mode=0", "-no-inprocess", "-vs=0")
        ablated = run_solver(
            solver, instance, "-mode=0", "-inprocess", "-ablation=1:2:3:4:5:6", "-vs=0"
        )
        enabled = run_solver(
            solver, instance, "-mode=0", "-inprocess", "-ablation=2:3:4:5", "-vs=0"
        )
        expected = count_models(instance)
        counts = (
            exact_count(off, f"{filename} off"),
            exact_count(ablated, f"{filename} ablated"),
            exact_count(enabled, f"{filename} enabled"),
        )
        if any(value != expected for value in counts):
            raise AssertionError(f"{filename}: counts={counts}, oracle={expected}")

        baseline = stable_stats(off.stdout, f"{filename} off")
        if stable_stats(ablated.stdout, f"{filename} ablated") != baseline:
            raise AssertionError(f"{filename}: ablated Basic changed search statistics")
        if stable_stats(enabled.stdout, f"{filename} enabled") != baseline:
            raise AssertionError(f"{filename}: read-only Basic changed search statistics")

        if "c o simplification ablation = Basic, BICP, CS, GCD, Cardinality, SC" not in ablated.stdout:
            raise AssertionError(f"{filename}: empty framework ablation was not reported")
        if stat(ablated.stdout, "Basic calls", f"{filename} ablated") != 0:
            raise AssertionError(f"{filename}: ablated Basic was called")
        calls = stat(enabled.stdout, "Basic calls", f"{filename} enabled")
        rows = stat(enabled.stdout, "Basic rows", f"{filename} enabled")
        opened = stat(enabled.stdout, "Basic open", f"{filename} enabled")
        satisfied = stat(enabled.stdout, "Basic satisfied", f"{filename} enabled")
        if calls == 0 or rows != opened + satisfied:
            raise AssertionError(
                f"{filename}: inconsistent Basic stats "
                f"calls={calls}, rows={rows}, open={opened}, satisfied={satisfied}"
            )

    base = cases / "or_ge.opb"
    require_failure(
        run_solver(solver, base, "-mode=0", "-ablation=1"),
        "-ablation requires -inprocess",
        "ablation without framework",
    )
    for value, message in (
        ("7", "unknown -ablation rule ID"),
        ("x", "invalid -ablation rule ID"),
        ("1:", "empty rule ID"),
        (":1", "empty rule ID"),
    ):
        require_failure(
            run_solver(solver, base, "-mode=0", "-inprocess", f"-ablation={value}"),
            message,
            f"invalid ablation {value!r}",
        )

    duplicate = run_solver(
        solver, base, "-mode=0", "-inprocess", "-ablation=1:1:2", "-vs=0"
    )
    if exact_count(duplicate, "duplicate ablation") != count_models(base):
        raise AssertionError("duplicate ablation ID changed the count")

    print("Gate 2A Basic, native Reason/GC checks, and EDPLLSim-style ablation passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
