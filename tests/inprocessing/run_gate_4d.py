#!/usr/bin/env python3
"""Gate 4D RUP exact-MC integration tests."""

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
    require_proof: bool | None = True,
) -> None:
    expected = count_models(instance)
    empty = run(solver, instance, cache, "1:2:3:4:5:6:7:8:9:10:11")
    rup = run(solver, instance, cache, "1:2:3:4:5:6:7:8:9:11")
    values = (count(empty, "empty"), count(rup, "RUP"))
    if values != (expected, expected):
        raise AssertionError(f"{instance.name}: counts={values}, oracle={expected}")
    if cache == "legacy" and stable(empty.stdout, "empty") != stable(
        rup.stdout, "RUP"
    ):
        raise AssertionError(f"{instance.name}: legacy RUP changed the search")
    if (
        "c o simplification ablation = Basic, BICP, CS, GCD, Cardinality, SC, RIR, RPR, RSR, RUP, SB-LP"
        not in empty.stdout
    ):
        raise AssertionError("empty simplifier ablation was not reported")
    if (
        "c o simplification ablation = Basic, BICP, CS, GCD, Cardinality, SC, RIR, RPR, RSR, SB-LP"
        not in rup.stdout
    ):
        raise AssertionError("RUP-only ablation was not reported")
    if stat(empty.stdout, "RUP calls", "empty") != 0:
        raise AssertionError("ablated RUP was called")
    if stat(rup.stdout, "RUP calls", "RUP") == 0:
        raise AssertionError("enabled RUP was not called")
    proofs = stat(rup.stdout, "RUP proofs", "RUP")
    revalidations = stat(rup.stdout, "RUP revalidations", "RUP")
    commits = stat(rup.stdout, "RUP commits", "RUP")
    removed = stat(rup.stdout, "RUP removed rows", "RUP")
    if stat(rup.stdout, "RUP budgets", "RUP") != 0:
        raise AssertionError(f"{instance.name}: small RUP case exhausted its budget")
    if require_proof is True and proofs == 0:
        raise AssertionError(f"{instance.name}/{cache}: RUP found no proof")
    if require_proof is False and proofs != 0:
        raise AssertionError(f"{instance.name}/{cache}: RUP false deletion proof")
    if cache == "legacy" and (revalidations != 0 or commits != 0 or removed != 0):
        raise AssertionError("legacy RUP published an inactive overlay")
    if cache != "legacy" and require_proof is True:
        if commits == 0 or removed != commits or revalidations != commits:
            raise AssertionError(f"{cache}: RUP commit/revalidation mismatch")


def redundant_opb(rng: random.Random, index: int) -> str:
    force_negated = rng.choice((False, True))
    helper_negated = rng.choice((False, True))
    extra_negated = rng.choice((False, True))
    forced = f"{'~' if force_negated else ''}x1"
    helper = f"{'~' if helper_negated else ''}x2"
    opposite = f"{'~' if not helper_negated else ''}x2"
    extra = f"{'~' if extra_negated else ''}x3"
    return (
        "* #variable= 4 #constraint= 3\n"
        f"* deterministic RUP case {index}\n"
        f"+1 {forced} +1 {extra} >= 1 ;\n"
        f"+1 {forced} +1 {helper} >= 1 ;\n"
        f"+1 {forced} +1 {opposite} >= 1 ;\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--solver", required=True, type=Path)
    args = parser.parse_args()
    solver = args.solver.resolve()

    with tempfile.TemporaryDirectory(prefix="pbmc-gate4d-") as directory:
        root = Path(directory)
        formulas = {
            "positive.opb": (
                "* #variable= 4 #constraint= 3\n"
                "+1 x1 +1 x3 >= 1 ;\n"
                "+1 x1 +1 x2 >= 1 ;\n"
                "+1 x1 +1 ~x2 >= 1 ;\n"
            ),
            "negative.opb": (
                "* #variable= 4 #constraint= 3\n"
                "+1 ~x1 +1 x3 >= 1 ;\n"
                "+1 ~x1 +1 x2 >= 1 ;\n"
                "+1 ~x1 +1 ~x2 >= 1 ;\n"
            ),
            "coefficients.opb": (
                "* #variable= 4 #constraint= 3\n"
                "+2 x1 +1 x3 +1 x4 >= 2 ;\n"
                "+1 x1 +1 x2 >= 1 ;\n"
                "+1 x1 +1 ~x2 >= 1 ;\n"
            ),
            "less_equal.opb": (
                "* #variable= 3 #constraint= 3\n"
                "+1 x1 +1 x3 <= 1 ;\n"
                "+1 x1 +1 x2 <= 1 ;\n"
                "+1 x1 +1 ~x2 <= 1 ;\n"
            ),
            "equality.opb": (
                "* #variable= 3 #constraint= 3\n"
                "+1 x1 +1 x3 = 1 ;\n"
                "+1 x1 +1 x2 >= 1 ;\n"
                "+1 x1 +1 ~x2 >= 1 ;\n"
            ),
            "branch.opb": (
                "* #variable= 5 #constraint= 3\n"
                "+1 x1 +1 x3 >= 1 ;\n"
                "+1 x1 +1 x2 >= 1 ;\n"
                "+1 x1 +1 ~x2 +1 x4 >= 1 ;\n"
            ),
            "no_proof.opb": (
                "* #variable= 3 #constraint= 2\n"
                "+1 x1 +1 x3 >= 1 ;\n"
                "+1 x1 +1 x2 >= 1 ;\n"
            ),
        }
        for filename, formula in formulas.items():
            (root / filename).write_text(formula)

        for filename in (
            "positive.opb",
            "negative.opb",
            "coefficients.opb",
            "less_equal.opb",
            "equality.opb",
        ):
            check_case(solver, root / filename)
        check_case(solver, root / "branch.opb", require_proof=None)
        check_case(solver, root / "no_proof.opb", require_proof=False)

        for cache in ("legacy", "bypass", "residual"):
            check_case(solver, root / "positive.opb", cache=cache)

        rng = random.Random(0x4D2026)
        for index in range(12):
            instance = root / f"random_{index:02d}.opb"
            instance.write_text(redundant_opb(rng, index))
            check_case(solver, instance)

    print("Gate 4D RUP exactness tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
