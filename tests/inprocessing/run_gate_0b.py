#!/usr/bin/env python3
"""Gate 0B regression tests for exact MC and startup guards."""

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
_BACKJUMPS = re.compile(
    r"backjumps\s+=\s+(\d+)\s+"
    r"\(with-lit (\d+), without-lit (\d+), fallback (\d+)\)"
)
_CHRONOLOGICAL = re.compile(r"chronological backs\s+=\s+(\d+)")


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


def expect_rejected(result: subprocess.CompletedProcess, text: str, label: str) -> None:
    if result.returncode == 0 or text not in result.stdout:
        raise AssertionError(
            f"{label}: expected non-zero exit containing {text!r}\n{result.stdout}"
        )


def random_opb(rng: random.Random, index: int) -> str:
    variable_count = rng.randint(2, 5)
    constraints = []
    for _ in range(rng.randint(1, 6)):
        variables = rng.sample(
            range(1, variable_count + 1), rng.randint(1, variable_count)
        )
        terms = []
        for variable in variables:
            coefficient = rng.choice((-3, -2, -1, 1, 2, 3))
            literal = f"{'~' if rng.choice((False, True)) else ''}x{variable}"
            terms.append(f"{coefficient:+d} {literal}")
        comparator = "=" if rng.randrange(5) == 0 else ">="
        rhs = rng.choice((-4, -3, -2, -1, 1, 2, 3, 4))
        constraints.append(f"{' '.join(terms)} {comparator} {rhs} ;")
    return (
        f"* #variable= {variable_count} #constraint= {len(constraints)}\n"
        f"* deterministic random Gate 0B case {index}\n"
        + "\n".join(constraints)
        + "\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--solver", required=True, type=Path)
    args = parser.parse_args()
    solver = args.solver.resolve()
    cases = Path(__file__).resolve().parent / "cases"

    expected_counts = {
        "or_ge.opb": 3,
        "equality_one.opb": 2,
        "conflict_unique.opb": 1,
        "two_components.opb": 9,
        "negative_coefficient.opb": 3,
        "backjump_without_lit.opb": 19,
    }
    for filename, expected in expected_counts.items():
        instance = cases / filename
        oracle = count_models(instance)
        if oracle != expected:
            raise AssertionError(f"{filename}: oracle {oracle}, expected {expected}")
        for switch in ("-no-inprocess", "-inprocess"):
            result = run_solver(solver, instance, "-mode=0", switch)
            actual = exact_count(result, f"{filename} {switch}")
            if actual != oracle:
                raise AssertionError(
                    f"{filename} {switch}: PBMC {actual}, oracle {oracle}"
                )

    le_instance = cases / "less_equal.opb"
    if count_models(le_instance) != 3:
        raise AssertionError("less_equal.opb: oracle regression")
    le_result = run_solver(solver, le_instance, "-mode=0", "-inprocess")
    if exact_count(le_result, "<= canonicalization") != 3:
        raise AssertionError("less_equal.opb: exact <= canonicalization regression")

    base = cases / "or_ge.opb"
    for mode in (1, 2, 3):
        expect_rejected(
            run_solver(solver, base, f"-mode={mode}", "-inprocess"),
            "-inprocess currently supports only exact MC",
            f"mode {mode} rejection",
        )
    expect_rejected(
        run_solver(solver, base, "-mode=0", "-inprocess", "-ddnnf"),
        "-inprocess currently supports only exact MC",
        "d-DNNF rejection",
    )

    backjump_case = cases / "backjump_without_lit.opb"
    backjump_result = run_solver(
        solver, backjump_case, "-mode=0", "-inprocess", "-ablation=1:2:3:4:5:6",
        "-vs=0", "-bjthd=0.99"
    )
    if exact_count(backjump_result, "ResolveWithoutLit backjump") != 19:
        raise AssertionError("backjump case: wrong exact count")
    backjump_stats = _BACKJUMPS.search(backjump_result.stdout)
    chronological_stats = _CHRONOLOGICAL.search(backjump_result.stdout)
    if backjump_stats is None or int(backjump_stats.group(3)) == 0:
        raise AssertionError(
            "backjump case did not execute ResolveWithoutLit -> bjResolve\n"
            + backjump_result.stdout
        )
    if chronological_stats is None or int(chronological_stats.group(1)) == 0:
        raise AssertionError("backjump case did not execute ordinary backtrack")

    no_backjump_result = run_solver(
        solver, backjump_case, "-mode=0", "-inprocess", "-ablation=1:2:3:4:5:6",
        "-vs=0", "-no-bj"
    )
    if exact_count(no_backjump_result, "backjump disabled") != 19:
        raise AssertionError("-no-bj changed the exact count")

    rng = random.Random(0x0B2026)
    accepted_random_cases = 0
    attempts = 0
    with tempfile.TemporaryDirectory(prefix="pbmc-gate0b-") as directory:
        while accepted_random_cases < 24 and attempts < 500:
            attempts += 1
            instance = Path(directory) / f"random_{accepted_random_cases:02d}.opb"
            instance.write_text(random_opb(rng, attempts))
            oracle = count_models(instance)
            variable_count, _ = parse_opb(instance)
            if oracle == 0 or oracle == 1 << variable_count:
                continue
            result = run_solver(solver, instance, "-mode=0", "-inprocess", "-vs=0")
            actual = exact_count(result, f"random case {accepted_random_cases}")
            if actual != oracle:
                raise AssertionError(
                    f"random case {accepted_random_cases}: PBMC {actual}, oracle {oracle}\n"
                    + instance.read_text()
                )
            accepted_random_cases += 1
    if accepted_random_cases != 24:
        raise AssertionError("could not generate enough nontrivial random MC cases")

    print(
        "Gate 0B exact-MC, comparator, mode-guard, backjump, "
        "and 24 random oracle tests passed"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
