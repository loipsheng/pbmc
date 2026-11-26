#!/usr/bin/env python3
"""Gate 4C generalized RSR exact-MC integration tests."""

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
    require_proposal: bool | None = True,
) -> None:
    expected = count_models(instance)
    empty = run(solver, instance, cache, "1:2:3:4:5:6:7:8:9:10:11")
    rsr = run(solver, instance, cache, "1:2:3:4:5:6:7:8:10:11")
    values = (count(empty, "empty"), count(rsr, "RSR"))
    if values != (expected, expected):
        raise AssertionError(f"{instance.name}: counts={values}, oracle={expected}")
    if cache == "legacy" and stable(empty.stdout, "empty") != stable(
        rsr.stdout, "RSR"
    ):
        raise AssertionError(f"{instance.name}: legacy RSR changed the search")
    if (
        "c o simplification ablation = Basic, BICP, CS, GCD, Cardinality, SC, RIR, RPR, RSR, RUP, SB-LP"
        not in empty.stdout
    ):
        raise AssertionError("empty simplifier ablation was not reported")
    if (
        "c o simplification ablation = Basic, BICP, CS, GCD, Cardinality, SC, RIR, RPR, RUP, SB-LP"
        not in rsr.stdout
    ):
        raise AssertionError("RSR-only ablation was not reported")
    if stat(empty.stdout, "RSR calls", "empty") != 0:
        raise AssertionError("ablated RSR was called")
    if stat(rsr.stdout, "RSR calls", "RSR") == 0:
        raise AssertionError("enabled RSR was not called")
    proposals = stat(rsr.stdout, "RSR proposals", "RSR")
    revalidations = stat(rsr.stdout, "RSR revalidations", "RSR")
    commits = stat(rsr.stdout, "RSR commits", "RSR")
    removed = stat(rsr.stdout, "RSR removed rows", "RSR")
    if stat(rsr.stdout, "RSR budgets", "RSR") != 0:
        raise AssertionError(f"{instance.name}: small RSR case exhausted its budget")
    if require_proposal is True and proposals == 0:
        raise AssertionError(
            f"{instance.name}/{cache}: RSR did not prove the redundant row"
        )
    if require_proposal is False and proposals != 0:
        raise AssertionError(f"{instance.name}/{cache}: RSR false deletion proposal")
    if cache == "legacy" and (revalidations != 0 or commits != 0 or removed != 0):
        raise AssertionError("legacy RSR published an inactive overlay")
    if cache != "legacy" and require_proposal is True:
        if commits == 0 or removed != commits or revalidations != commits:
            raise AssertionError(f"{cache}: RSR commit/revalidation mismatch")


def redundant_opb(rng: random.Random, index: int) -> str:
    variable_count = rng.randint(5, 8)
    helper_count = rng.randint(3, variable_count - 1)
    variables = list(range(1, variable_count + 1))
    rng.shuffle(variables)
    helper_variables = variables[:helper_count]
    extra_variables = variables[helper_count:]
    helper_literals = [
        f"{'~' if rng.choice((False, True)) else ''}x{variable}"
        for variable in helper_variables
    ]
    extra_literals = [
        f"{'~' if rng.choice((False, True)) else ''}x{variable}"
        for variable in extra_variables
    ]
    threshold = rng.randint(2, helper_count - 1)
    helper = " ".join(f"+1 {literal}" for literal in helper_literals)
    target_literals = helper_literals + extra_literals
    rng.shuffle(target_literals)
    target = " ".join(f"+1 {literal}" for literal in target_literals)
    return (
        f"* #variable= {variable_count} #constraint= 2\n"
        f"* deterministic redundant Gate 4C case {index}\n"
        f"{helper} >= {threshold} ;\n"
        f"{target} >= {threshold} ;\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--solver", required=True, type=Path)
    args = parser.parse_args()
    solver = args.solver.resolve()

    with tempfile.TemporaryDirectory(prefix="pbmc-gate4c-") as directory:
        root = Path(directory)
        formulas = {
            "positive.opb": (
                "* #variable= 4 #constraint= 2\n"
                "+1 x1 +1 x2 +1 x3 >= 2 ;\n"
                "+1 x1 +1 x2 +1 x3 +1 x4 >= 2 ;\n"
            ),
            "negative.opb": (
                "* #variable= 4 #constraint= 2\n"
                "+1 x1 +1 x2 +1 x3 >= 2 ;\n"
                "+1 x1 +1 x2 +1 x3 +1 ~x4 >= 2 ;\n"
            ),
            "coefficients.opb": (
                "* #variable= 4 #constraint= 2\n"
                "+2 x1 +1 x2 +1 x3 >= 2 ;\n"
                "+2 x1 +2 x2 +2 x3 +1 x4 >= 2 ;\n"
            ),
            "less_equal.opb": (
                "* #variable= 4 #constraint= 2\n"
                "+1 x1 +1 x2 +1 x3 <= 1 ;\n"
                "+1 x1 +1 x2 +1 x3 +1 x4 <= 2 ;\n"
            ),
            "different_rhs.opb": (
                "* #variable= 4 #constraint= 2\n"
                "+1 x1 +1 x2 +1 x3 >= 2 ;\n"
                "+1 x1 +1 x2 +1 x3 +1 x4 >= 1 ;\n"
            ),
            "equality.opb": (
                "* #variable= 4 #constraint= 2\n"
                "+1 x1 +1 x2 +1 x3 >= 2 ;\n"
                "+1 x1 +1 x2 +1 x3 +1 x4 = 2 ;\n"
            ),
            "coefficient_counterexample.opb": (
                "* #variable= 3 #constraint= 2\n"
                "+2 x1 +1 x2 +1 x3 >= 2 ;\n"
                "+1 x1 +2 x2 +1 x3 >= 2 ;\n"
            ),
            "polarity_counterexample.opb": (
                "* #variable= 3 #constraint= 2\n"
                "+1 x1 +1 x2 +1 x3 >= 2 ;\n"
                "+1 ~x1 +1 x2 +1 x3 >= 2 ;\n"
            ),
            "bridge.opb": (
                "* #variable= 5 #constraint= 3\n"
                "+1 x1 +1 x2 +1 x3 >= 2 ;\n"
                "+1 x1 +1 x2 +1 x3 +1 x4 >= 2 ;\n"
                "+1 x4 +1 x5 >= 1 ;\n"
            ),
        }
        for filename, formula in formulas.items():
            (root / filename).write_text(formula)

        for filename in (
            "positive.opb",
            "negative.opb",
            "coefficients.opb",
            "less_equal.opb",
            "different_rhs.opb",
            "equality.opb",
            "bridge.opb",
        ):
            check_case(solver, root / filename)
        for filename in (
            "coefficient_counterexample.opb",
            "polarity_counterexample.opb",
        ):
            check_case(solver, root / filename, require_proposal=False)

        for cache in ("legacy", "bypass", "residual"):
            check_case(solver, root / "bridge.opb", cache=cache)

        rng = random.Random(0x4C2026)
        for index in range(20):
            instance = root / f"random_{index:02d}.opb"
            instance.write_text(redundant_opb(rng, index))
            check_case(solver, instance)

    print("Gate 4C generalized RSR exactness tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
