#!/usr/bin/env python3
"""Gate 2E residual component-cache exactness and lifecycle tests."""

from __future__ import annotations

import argparse
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

from mc_oracle import count_models


_EXACT_COUNT = re.compile(r"^c s exact arb int ([0-9]+)$", re.MULTILINE)


def run(solver: Path, instance: Path, mode: str, *options: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            str(solver),
            "-no-td",
            "-mode=0",
            "-inprocess",
            f"-simp-cache={mode}",
            *options,
            str(instance),
        ],
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


def stat(output: str, name: str, label: str) -> int:
    match = re.search(
        rf"^c o {re.escape(name)}\s+=\s+([0-9]+)\s*$", output, re.MULTILINE
    )
    if match is None:
        raise AssertionError(f"{label}: missing {name}\n{output}")
    return int(match.group(1))


def check_case(solver: Path, instance: Path, *options: str) -> None:
    expected = count_models(instance)
    results = {
        mode: run(solver, instance, mode, *options)
        for mode in ("legacy", "bypass", "residual")
    }
    values = tuple(count(results[mode], mode) for mode in results)
    if values != (expected, expected, expected):
        raise AssertionError(f"{instance.name}: counts={values}, oracle={expected}")
    for mode, result in results.items():
        if f"c o Cache mode            = {mode}" not in result.stdout:
            raise AssertionError(f"{mode}: cache mode was not reported")
    residual = results["residual"].stdout
    for name in (
        "Cache lookup",
        "Cache hit",
        "Cache insert",
        "Cache reduce",
        "Template request",
        "Template reuse",
        "Template wake",
        "Template reclaim",
    ):
        stat(residual, name, "residual")


def random_opb(rng: random.Random, index: int) -> str:
    variable_count = rng.randint(2, 7)
    constraints = []
    for _ in range(rng.randint(1, 8)):
        variables = rng.sample(
            range(1, variable_count + 1), rng.randint(1, variable_count)
        )
        terms = []
        for variable in variables:
            coefficient = rng.choice((-7, -4, -2, -1, 1, 2, 4, 7))
            literal = f"{'~' if rng.choice((False, True)) else ''}x{variable}"
            terms.append(f"{coefficient:+d} {literal}")
        comparator = rng.choice((">=", ">=", "<=", "="))
        rhs = rng.choice((-8, -5, -3, -1, 1, 3, 5, 8))
        constraints.append(f"{' '.join(terms)} {comparator} {rhs} ;")
    return (
        f"* #variable= {variable_count} #constraint= {len(constraints)}\n"
        f"* deterministic random Gate 2E case {index}\n"
        + "\n".join(constraints)
        + "\n"
    )


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
        check_case(solver, cases / filename, "-vs=0")

    check_case(
        solver,
        cases / "backjump_without_lit.opb",
        "-ablation=1:2",
        "-vs=0",
        "-bjthd=0.99",
    )
    check_case(
        solver,
        cases / "backjump_without_lit.opb",
        "-ablation=1:2",
        "-vs=0",
        "-no-bj",
    )

    rng = random.Random(0x2E2026)
    accepted = 0
    attempts = 0
    with tempfile.TemporaryDirectory(prefix="pbmc-gate2e-") as directory:
        root = Path(directory)
        while accepted < 30 and attempts < 800:
            attempts += 1
            instance = root / f"random_{accepted:02d}.opb"
            instance.write_text(random_opb(rng, attempts))
            expected = count_models(instance)
            if expected == 0:
                continue
            check_case(solver, instance, "-vs=0")
            accepted += 1
    if accepted != 30:
        raise AssertionError("could not generate enough random exact-MC cases")

    two_components = run(
        solver, cases / "two_components.opb", "residual", "-cs=1", "-vs=0"
    )
    if stat(two_components.stdout, "Cache insert", "two-components") == 0:
        raise AssertionError("residual cache did not store a component")
    if stat(two_components.stdout, "Template reuse", "two-components") == 0:
        raise AssertionError("residual cache did not reuse a row template")

    print("Gate 2E residual component-cache exactness tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
