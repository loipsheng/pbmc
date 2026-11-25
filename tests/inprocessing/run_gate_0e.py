#!/usr/bin/env python3
"""Gate 0E operator/equality contract and exact-MC oracle tests."""

from __future__ import annotations

import argparse
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

from mc_oracle import count_models, parse_opb


_EXACT_COUNT = re.compile(r"^c s exact arb int ([0-9]+)$", re.MULTILINE)
_EQ_STATS = re.compile(
    r"equality groups\s+=\s+(\d+)\s+"
    r"\(attached dirs (\d+), root-true dirs (\d+)\)"
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


def check_count(solver: Path, instance: Path, *options: str) -> subprocess.CompletedProcess:
    oracle = count_models(instance)
    result = run_solver(solver, instance, "-mode=0", *options)
    actual = exact_count(result, f"{instance.name} {' '.join(options)}")
    if actual != oracle:
        raise AssertionError(
            f"{instance.name}: PBMC {actual}, oracle {oracle}\n{instance.read_text()}"
        )
    return result


def check_stats(
    result: subprocess.CompletedProcess,
    expected: tuple[int, int, int],
    label: str,
) -> None:
    match = _EQ_STATS.search(result.stdout)
    if match is None:
        raise AssertionError(f"{label}: equality statistics not found\n{result.stdout}")
    actual = tuple(int(value) for value in match.groups())
    if actual != expected:
        raise AssertionError(f"{label}: equality statistics {actual}, expected {expected}")


def random_opb(rng: random.Random, index: int) -> str:
    variable_count = rng.randint(2, 5)
    constraints = []
    for _ in range(rng.randint(1, 7)):
        terms = []
        for _ in range(rng.randint(1, 7)):
            variable = rng.randint(1, variable_count)
            coefficient = rng.choice((-4, -3, -2, -1, 0, 1, 2, 3, 4))
            literal = f"{'~' if rng.choice((False, True)) else ''}x{variable}"
            terms.append(f"{coefficient:+d} {literal}")
        comparator = rng.choice((">=", "<=", "="))
        rhs = rng.randint(-6, 6)
        constraints.append(f"{' '.join(terms)} {comparator} {rhs} ;")
    return (
        f"* #variable= {variable_count} #constraint= {len(constraints)}\n"
        f"* deterministic random Gate 0E case {index}\n"
        + "\n".join(constraints)
        + "\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--solver", required=True, type=Path)
    args = parser.parse_args()
    solver = args.solver.resolve()
    cases = Path(__file__).resolve().parent / "cases"

    fixed_cases = (
        "or_ge.opb",
        "less_equal.opb",
        "signed_less_equal.opb",
        "equality_one.opb",
        "signed_equality.opb",
        "equality_one_root_true.opb",
        "equality_complement.opb",
        "equality_two_root_true.opb",
        "equality_unsat.opb",
        "constant_true.opb",
        "constant_false.opb",
        "negative_coefficient.opb",
        "two_components.opb",
    )
    results = {}
    for filename in fixed_cases:
        instance = cases / filename
        for switch in ("-no-inprocess", "-inprocess"):
            result = check_count(solver, instance, switch)
            if switch == "-inprocess":
                results[filename] = result

    check_stats(results["equality_one.opb"], (1, 2, 0), "two attached directions")
    check_stats(
        results["equality_one_root_true.opb"],
        (1, 1, 1),
        "one root-true direction",
    )
    check_stats(
        results["equality_complement.opb"],
        (1, 0, 2),
        "complementary-literal equality",
    )
    check_stats(
        results["equality_two_root_true.opb"],
        (1, 0, 2),
        "two root-true directions",
    )

    overflow = run_solver(
        solver, cases / "overflow_less_equal.opb", "-mode=0", "-inprocess"
    )
    if overflow.returncode == 0 or "integer overflow while canonicalizing <=" not in overflow.stdout:
        raise AssertionError(f"<= overflow was not rejected precisely\n{overflow.stdout}")

    rng = random.Random(0x0E2026)
    accepted = 0
    attempts = 0
    with tempfile.TemporaryDirectory(prefix="pbmc-gate0e-") as directory:
        directory_path = Path(directory)
        malformed = directory_path / "malformed.opb"
        malformed.write_text("* #variable= 1 #constraint= 1\n+1 x1 0 ;\n")
        malformed_result = run_solver(
            solver, malformed, "-mode=0", "-inprocess"
        )
        if malformed_result.returncode == 0 or "malformed PB row" not in malformed_result.stdout:
            raise AssertionError(
                f"missing comparator was not rejected precisely\n{malformed_result.stdout}"
            )
        while accepted < 36 and attempts < 1000:
            attempts += 1
            instance = directory_path / f"random_{accepted:02d}.opb"
            instance.write_text(random_opb(rng, attempts))
            oracle = count_models(instance)
            variable_count, _ = parse_opb(instance)
            if oracle in (0, 1 << variable_count):
                continue
            result = run_solver(
                solver, instance, "-mode=0", "-inprocess", "-vs=0"
            )
            actual = exact_count(result, f"random Gate 0E case {accepted}")
            if actual != oracle:
                raise AssertionError(
                    f"random case {accepted}: PBMC {actual}, oracle {oracle}\n"
                    + instance.read_text()
                )
            accepted += 1
    if accepted != 36:
        raise AssertionError("could not generate enough nontrivial random Gate 0E cases")

    print(
        "Gate 0E >=/<=/= contract, equality metadata states, overflow guard, "
        "and 36 random exact-MC oracle tests passed"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
