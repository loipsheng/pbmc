#!/usr/bin/env python3
"""Gate 4A scoped RIR exact-MC integration tests."""

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
    require_proposal: bool = False,
) -> None:
    expected = count_models(instance)
    empty = run(solver, instance, cache, "1:2:3:4:5:6:7:8:9:10:11")
    rir = run(solver, instance, cache, "1:2:3:4:5:6:8:9:10:11")
    values = (count(empty, "empty"), count(rir, "RIR"))
    if values != (expected, expected):
        raise AssertionError(f"{instance.name}: counts={values}, oracle={expected}")
    if stable(empty.stdout, "empty") != stable(rir.stdout, "RIR"):
        raise AssertionError(f"{instance.name}: scoped RIR changed the search topology")
    if (
        "c o simplification ablation = Basic, BICP, CS, GCD, Cardinality, SC, RIR, RPR, RSR, RUP, SB-LP"
        not in empty.stdout
    ):
        raise AssertionError("empty simplifier ablation was not reported")
    if (
        "c o simplification ablation = Basic, BICP, CS, GCD, Cardinality, SC, RPR, RSR, RUP, SB-LP"
        not in rir.stdout
    ):
        raise AssertionError("RIR-only ablation was not reported")
    if stat(empty.stdout, "RIR calls", "empty") != 0:
        raise AssertionError("ablated RIR was called")
    if stat(rir.stdout, "RIR calls", "RIR") == 0:
        raise AssertionError("enabled RIR was not called")
    proposals = stat(rir.stdout, "RIR proposals", "RIR")
    commits = stat(rir.stdout, "RIR commits", "RIR")
    removed = stat(rir.stdout, "RIR removed rows", "RIR")
    if require_proposal and proposals == 0:
        raise AssertionError(f"{cache}: RIR did not find the required satisfied row")
    if cache == "legacy" and (commits != 0 or removed != 0):
        raise AssertionError("legacy RIR published an inactive overlay")
    if cache != "legacy" and require_proposal and (commits == 0 or removed == 0):
        raise AssertionError(f"{cache}: RIR did not commit the required active mask")
    if removed < commits:
        raise AssertionError("RIR committed without removing a row direction")


def random_opb(rng: random.Random, index: int) -> str:
    variable_count = rng.randint(2, 7)
    constraints = []
    for _ in range(rng.randint(2, 8)):
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
        f"* deterministic random Gate 4A case {index}\n"
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

    with tempfile.TemporaryDirectory(prefix="pbmc-gate4a-") as directory:
        root = Path(directory)
        formulas = {
            "positive.opb": (
                "* #variable= 3 #constraint= 1\n"
                "+2 x1 +1 x2 +1 x3 >= 2 ;\n"
            ),
            "negative.opb": (
                "* #variable= 3 #constraint= 1\n"
                "+2 ~x1 +1 x2 +1 x3 >= 2 ;\n"
            ),
            "less_equal.opb": (
                "* #variable= 3 #constraint= 1\n"
                "+2 x1 +1 x2 +1 x3 <= 2 ;\n"
            ),
            "equality.opb": (
                "* #variable= 2 #constraint= 1\n"
                "+1 x1 +1 x2 = 1 ;\n"
            ),
            "bridge.opb": (
                "* #variable= 5 #constraint= 3\n"
                "+2 x1 +1 x2 +1 x3 >= 2 ;\n"
                "+1 x2 +1 x4 >= 1 ;\n"
                "+1 x3 +1 x5 >= 1 ;\n"
            ),
        }
        for filename, formula in formulas.items():
            instance = root / filename
            instance.write_text(formula)
            check_case(solver, instance, require_proposal=True)

        for cache in ("legacy", "bypass", "residual"):
            check_case(
                solver,
                root / "positive.opb",
                cache=cache,
                require_proposal=True,
            )

        rng = random.Random(0x4A2026)
        accepted = 0
        attempts = 0
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

    print("Gate 4A scoped RIR exactness tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
