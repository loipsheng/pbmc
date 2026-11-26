#!/usr/bin/env python3
"""Gate 3A coefficient-saturation residual-commit integration tests."""

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
_STABLE_STATS = (
    "conflicts",
    "decisions",
    "Components",
    "Split",
    "Cache lookup",
    "Cache hit",
    "Cache insert",
    "Cache reduce",
    "backjumps",
    "chronological backs",
)


def run(
    solver: Path, instance: Path, cache: str, ablation: str
) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            str(solver),
            "-no-td",
            "-mode=0",
            "-inprocess",
            f"-simp-cache={cache}",
            f"-ablation={ablation}",
            "-vs=0",
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
        rf"^c o {re.escape(name)}\s+=\s+([0-9]+)", output, re.MULTILINE
    )
    if match is None:
        raise AssertionError(f"{label}: missing statistic {name}\n{output}")
    return int(match.group(1))


def stable(output: str, label: str) -> dict[str, str]:
    result = {}
    for name in _STABLE_STATS:
        match = re.search(
            rf"^c o {re.escape(name)}\s+=\s+(.+?)\s*$", output, re.MULTILINE
        )
        if match is None:
            raise AssertionError(f"{label}: missing statistic {name}\n{output}")
        result[name] = match.group(1)
    return result


def check_case(
    solver: Path,
    instance: Path,
    cache: str = "residual",
    require_commit: bool = False,
) -> None:
    expected = count_models(instance)
    empty = run(solver, instance, cache, "1:2:3:4:5:6:7:8:9:10:11")
    cs = run(solver, instance, cache, "1:2:4:5:6:7:8:9:10:11")
    values = (count(empty, "empty"), count(cs, "CS"))
    if values != (expected, expected):
        raise AssertionError(f"{instance.name}: counts={values}, oracle={expected}")
    if cache == "legacy" and stable(empty.stdout, "empty") != stable(cs.stdout, "CS"):
        raise AssertionError(f"{instance.name}: legacy view-only CS changed the search")
    if "c o simplification ablation = Basic, BICP, CS, GCD, Cardinality, SC, RIR, RPR, RSR, RUP, SB-LP" not in empty.stdout:
        raise AssertionError("empty simplifier ablation was not reported")
    if "c o simplification ablation = Basic, BICP, GCD, Cardinality, SC, RIR, RPR, RSR, RUP, SB-LP" not in cs.stdout:
        raise AssertionError("CS-only ablation was not reported")
    if stat(empty.stdout, "CS calls", "empty") != 0:
        raise AssertionError("ablated CS was called")
    if stat(cs.stdout, "CS calls", "CS") == 0:
        raise AssertionError("enabled CS was not called")
    commits = stat(cs.stdout, "CS commits", "CS")
    if cache == "legacy" and commits != 0:
        raise AssertionError("legacy CS published an overlay")
    if cache != "legacy" and require_commit and commits == 0:
        raise AssertionError(f"{cache}: CS did not publish the required residual row")


def random_opb(rng: random.Random, index: int) -> str:
    variable_count = rng.randint(2, 7)
    constraints = []
    for _ in range(rng.randint(1, 8)):
        variables = rng.sample(
            range(1, variable_count + 1), rng.randint(1, variable_count)
        )
        terms = []
        for variable in variables:
            coefficient = rng.choice((-8, -5, -3, -1, 1, 3, 5, 8))
            literal = f"{'~' if rng.choice((False, True)) else ''}x{variable}"
            terms.append(f"{coefficient:+d} {literal}")
        comparator = rng.choice((">=", ">=", "<=", "="))
        rhs = rng.choice((-9, -6, -3, -1, 1, 3, 6, 9))
        constraints.append(f"{' '.join(terms)} {comparator} {rhs} ;")
    return (
        f"* #variable= {variable_count} #constraint= {len(constraints)}\n"
        f"* deterministic random Gate 3A case {index}\n"
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
        check_case(solver, cases / filename)

    for cache in ("legacy", "bypass", "residual"):
        check_case(solver, cases / "two_components.opb", cache)

    rng = random.Random(0x3A2026)
    accepted = 0
    attempts = 0
    with tempfile.TemporaryDirectory(prefix="pbmc-gate3a-") as directory:
        root = Path(directory)
        commit = root / "commit.opb"
        commit.write_text(
            "* #variable= 3 #constraint= 1\n"
            "+6 x1 +5 x2 +4 x3 >= 10 ;\n"
        )
        check_case(solver, commit, "legacy")
        check_case(solver, commit, "bypass", require_commit=True)
        check_case(solver, commit, "residual", require_commit=True)
        while accepted < 20 and attempts < 600:
            attempts += 1
            instance = root / f"random_{accepted:02d}.opb"
            instance.write_text(random_opb(rng, attempts))
            if count_models(instance) == 0:
                continue
            check_case(solver, instance)
            accepted += 1
    if accepted != 20:
        raise AssertionError("could not generate enough random exact-MC cases")

    print("Gate 3A CS residual-commit exactness tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
