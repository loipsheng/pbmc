#!/usr/bin/env python3
"""Gate 4B RPR exact-MC integration tests."""

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
    rpr = run(solver, instance, cache, "1:2:3:4:5:6:7:9:10:11")
    values = (count(empty, "empty"), count(rpr, "RPR"))
    if values != (expected, expected):
        raise AssertionError(f"{instance.name}: counts={values}, oracle={expected}")
    if cache == "legacy" and stable(empty.stdout, "empty") != stable(
        rpr.stdout, "RPR"
    ):
        raise AssertionError(f"{instance.name}: legacy RPR changed the search")
    if (
        "c o simplification ablation = Basic, BICP, CS, GCD, Cardinality, SC, RIR, RPR, RSR, RUP, SB-LP"
        not in empty.stdout
    ):
        raise AssertionError("empty simplifier ablation was not reported")
    if (
        "c o simplification ablation = Basic, BICP, CS, GCD, Cardinality, SC, RIR, RSR, RUP, SB-LP"
        not in rpr.stdout
    ):
        raise AssertionError("RPR-only ablation was not reported")
    if stat(empty.stdout, "RPR calls", "empty") != 0:
        raise AssertionError("ablated RPR was called")
    if stat(rpr.stdout, "RPR calls", "RPR") == 0:
        raise AssertionError("enabled RPR was not called")
    proposals = stat(rpr.stdout, "RPR proposals", "RPR")
    revalidations = stat(rpr.stdout, "RPR revalidations", "RPR")
    commits = stat(rpr.stdout, "RPR commits", "RPR")
    removed = stat(rpr.stdout, "RPR removed rows", "RPR")
    if require_proposal is True and proposals == 0:
        raise AssertionError(
            f"{instance.name}/{cache}: RPR did not find the parallel row"
        )
    if require_proposal is False and proposals != 0:
        raise AssertionError(f"{cache}: RPR falsely treated a hash bucket as proof")
    if cache == "legacy" and (revalidations != 0 or commits != 0 or removed != 0):
        raise AssertionError("legacy RPR published an inactive overlay")
    if cache != "legacy" and require_proposal is True:
        if commits == 0 or removed != commits or revalidations != commits:
            raise AssertionError(f"{cache}: RPR commit/revalidation mismatch")


def duplicate_opb(rng: random.Random, index: int) -> str:
    variable_count = rng.randint(4, 7)
    literals = [
        f"{'~' if rng.choice((False, True)) else ''}x{variable}"
        for variable in range(1, variable_count + 1)
    ]
    first = " ".join(f"+1 {literal}" for literal in literals)
    shuffled = list(literals)
    rng.shuffle(shuffled)
    second = " ".join(f"+1 {literal}" for literal in shuffled)
    threshold = rng.randint(2, variable_count - 2)
    return (
        f"* #variable= {variable_count} #constraint= 2\n"
        f"* deterministic duplicate Gate 4B case {index}\n"
        f"{first} >= {threshold} ;\n"
        f"{second} >= {threshold} ;\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--solver", required=True, type=Path)
    args = parser.parse_args()
    solver = args.solver.resolve()

    with tempfile.TemporaryDirectory(prefix="pbmc-gate4b-") as directory:
        root = Path(directory)
        formulas = {
            "rhs_chain.opb": (
                "* #variable= 4 #constraint= 3\n"
                "+1 x1 +1 x2 +1 x3 +1 x4 >= 1 ;\n"
                "+1 x1 +1 x2 +1 x3 +1 x4 >= 2 ;\n"
                "+1 x1 +1 x2 +1 x3 +1 x4 >= 3 ;\n"
            ),
            "proportional_negative.opb": (
                "* #variable= 5 #constraint= 2\n"
                "+2 ~x1 +2 x2 +2 ~x3 +2 x4 +2 x5 >= 5 ;\n"
                "+3 x5 +3 x4 +3 ~x3 +3 x2 +3 ~x1 >= 10 ;\n"
            ),
            "less_equal.opb": (
                "* #variable= 4 #constraint= 2\n"
                "+1 x1 +1 x2 +1 x3 +1 x4 <= 2 ;\n"
                "+2 x4 +2 x3 +2 x2 +2 x1 <= 4 ;\n"
            ),
            "equality.opb": (
                "* #variable= 4 #constraint= 2\n"
                "+1 x1 +1 x2 +1 x3 +1 x4 = 2 ;\n"
                "+1 x4 +1 x3 +1 x2 +1 x1 = 2 ;\n"
            ),
            "hash_bucket_only.opb": (
                "* #variable= 4 #constraint= 2\n"
                "+2 x1 +1 x2 +1 x3 +1 x4 >= 2 ;\n"
                "+1 x1 +2 x2 +1 x3 +1 x4 >= 2 ;\n"
            ),
            "scoped.opb": (
                "* #variable= 3 #constraint= 2\n"
                "+1 x1 +1 x2 +1 x3 >= 1 ;\n"
                "+1 x2 +1 x3 >= 1 ;\n"
            ),
        }
        for filename, formula in formulas.items():
            (root / filename).write_text(formula)

        for filename in (
            "rhs_chain.opb",
            "proportional_negative.opb",
            "less_equal.opb",
            "equality.opb",
        ):
            check_case(solver, root / filename)
        check_case(solver, root / "scoped.opb", require_proposal=None)
        check_case(
            solver, root / "hash_bucket_only.opb", require_proposal=False
        )

        for cache in ("legacy", "bypass", "residual"):
            check_case(solver, root / "rhs_chain.opb", cache=cache)

        rng = random.Random(0x4B2026)
        for index in range(20):
            instance = root / f"random_{index:02d}.opb"
            instance.write_text(duplicate_opb(rng, index))
            check_case(solver, instance)

    print("Gate 4B RPR exactness tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
