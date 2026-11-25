#!/usr/bin/env python3
"""Gate 2D component-cache bypass tests for exact MC."""

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


def run(solver: Path, instance: Path, *options: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(solver), "-no-td", "-mode=0", "-inprocess", *options, str(instance)],
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
    legacy = run(solver, instance, "-simp-cache=legacy", *options)
    bypass = run(solver, instance, "-simp-cache=bypass", "-cs=1", *options)
    values = (count(legacy, "legacy"), count(bypass, "bypass"))
    if values != (expected, expected):
        raise AssertionError(f"{instance.name}: counts={values}, oracle={expected}")
    if "c o Cache mode            = legacy" not in legacy.stdout:
        raise AssertionError("legacy mode was not reported")
    if "c o Cache mode            = bypass" not in bypass.stdout:
        raise AssertionError("bypass mode was not reported")
    for name in ("Cache lookup", "Cache hit", "Cache insert", "Cache reduce"):
        if stat(bypass.stdout, name, "bypass") != 0:
            raise AssertionError(f"bypass performed a real {name.lower()}")


def random_opb(rng: random.Random, index: int) -> str:
    variable_count = rng.randint(2, 6)
    constraints = []
    for _ in range(rng.randint(1, 7)):
        variables = rng.sample(
            range(1, variable_count + 1), rng.randint(1, variable_count)
        )
        terms = []
        for variable in variables:
            coefficient = rng.choice((-4, -3, -2, -1, 1, 2, 3, 4))
            literal = f"{'~' if rng.choice((False, True)) else ''}x{variable}"
            terms.append(f"{coefficient:+d} {literal}")
        comparator = "=" if rng.randrange(4) == 0 else ">="
        rhs = rng.choice((-5, -4, -3, -2, -1, 1, 2, 3, 4, 5))
        constraints.append(f"{' '.join(terms)} {comparator} {rhs} ;")
    return (
        f"* #variable= {variable_count} #constraint= {len(constraints)}\n"
        f"* deterministic random Gate 2D case {index}\n"
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

    rng = random.Random(0x2D2026)
    accepted = 0
    attempts = 0
    with tempfile.TemporaryDirectory(prefix="pbmc-gate2d-") as directory:
        root = Path(directory)
        while accepted < 20 and attempts < 500:
            attempts += 1
            instance = root / f"random_{accepted:02d}.opb"
            instance.write_text(random_opb(rng, attempts))
            expected = count_models(instance)
            if expected == 0:
                continue
            check_case(solver, instance, "-vs=0")
            accepted += 1
    if accepted != 20:
        raise AssertionError("could not generate enough random exact-MC cases")

    rejected = subprocess.run(
        [
            str(solver),
            "-no-td",
            "-mode=0",
            "-no-inprocess",
            "-simp-cache=bypass",
            str(cases / "or_ge.opb"),
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if rejected.returncode == 0 or "requires -inprocess" not in rejected.stdout:
        raise AssertionError("bypass was accepted outside the exact-MC framework")

    print("Gate 2D cache bypass ownership and exact-MC tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
