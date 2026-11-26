#!/usr/bin/env python3
"""Gate 2B view-only BICP integration tests."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
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


def run(solver: Path, instance: Path, *options: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(solver), "-no-td", *options, str(instance)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def count(result: subprocess.CompletedProcess, label: str) -> int:
    if result.returncode != 0:
        raise AssertionError(f"{label}: exited {result.returncode}\n{result.stdout}")
    match = _EXACT_COUNT.search(result.stdout)
    if match is None:
        raise AssertionError(f"{label}: exact count missing\n{result.stdout}")
    return int(match.group(1))


def stats(output: str, label: str) -> dict[str, str]:
    result = {}
    for name in _STABLE_STATS:
        match = re.search(
            rf"^c o {re.escape(name)}\s+=\s+(.+?)\s*$", output, re.MULTILINE
        )
        if match is None:
            raise AssertionError(f"{label}: missing {name}\n{output}")
        result[name] = match.group(1)
    return result


def stat(output: str, name: str, label: str) -> int:
    match = re.search(
        rf"^c o {re.escape(name)}\s+=\s+([0-9]+)\s*$", output, re.MULTILINE
    )
    if match is None:
        raise AssertionError(f"{label}: missing {name}\n{output}")
    return int(match.group(1))


def check_case(
    solver: Path, instance: Path, require_probe: bool = False
) -> subprocess.CompletedProcess:
    expected = count_models(instance)
    off = run(solver, instance, "-mode=0", "-no-inprocess", "-vs=0")
    empty = run(
        solver, instance, "-mode=0", "-inprocess", "-ablation=1:2:3:4:5:6", "-vs=0"
    )
    bicp = run(solver, instance, "-mode=0", "-inprocess", "-ablation=1:3:4:5", "-vs=0")
    values = (count(off, "off"), count(empty, "empty"), count(bicp, "BICP"))
    if any(value != expected for value in values):
        raise AssertionError(f"{instance.name}: counts={values}, oracle={expected}")
    baseline = stats(off.stdout, "off")
    if stats(empty.stdout, "empty") != baseline:
        raise AssertionError(f"{instance.name}: empty framework changed search statistics")
    if stat(bicp.stdout, "BICP calls", "BICP") == 0:
        raise AssertionError(f"{instance.name}: BICP was not called")
    if require_probe and stat(bicp.stdout, "BICP probes", "BICP") == 0:
        raise AssertionError(f"{instance.name}: BICP did not probe")
    if stat(empty.stdout, "BICP calls", "empty") != 0:
        raise AssertionError(f"{instance.name}: ablated BICP ran")
    return bicp


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--solver", required=True, type=Path)
    args = parser.parse_args()
    solver = args.solver.resolve()
    cases = Path(__file__).resolve().parent / "cases"

    for filename in (
        "or_ge.opb",
        "less_equal.opb",
        "equality_one.opb",
        "signed_equality.opb",
        "two_components.opb",
        "backjump_without_lit.opb",
    ):
        check_case(solver, cases / filename)

    with tempfile.TemporaryDirectory(prefix="pbmc-gate2b-") as directory:
        root = Path(directory)
        failed = root / "failed.opb"
        failed.write_text(
            "* #variable= 2 #constraint= 2\n"
            "+1 x1 +1 x2 >= 1 ;\n"
            "+1 x1 +1 ~x2 >= 1 ;\n"
        )
        failed_result = check_case(solver, failed, require_probe=True)
        if stat(failed_result.stdout, "BICP failed", "failed literal") == 0:
            raise AssertionError("failed-literal candidate was not detected")
        if stat(failed_result.stdout, "BICP implied", "failed literal") == 0:
            raise AssertionError("failed-literal implication was not submitted")

        common = root / "common.opb"
        common.write_text(
            "* #variable= 4 #constraint= 3\n"
            "+1 x1 +1 x2 >= 1 ;\n"
            "+1 ~x1 +1 x2 +1 x3 >= 2 ;\n"
            "+1 ~x2 +1 x4 >= 1 ;\n"
        )
        common_result = check_case(solver, common, require_probe=True)
        if stat(common_result.stdout, "BICP common", "common implication") < 2:
            raise AssertionError("common implications y,t were not detected")
        if stat(common_result.stdout, "BICP implied", "common implication") == 0:
            raise AssertionError("common implication was not submitted")

    print("Gate 2B/2C BICP rollback, guarded Reason, and exact-MC tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
