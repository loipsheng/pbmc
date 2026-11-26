#!/usr/bin/env python3
"""Gate 5-LP-1 SB-LP candidate/checker exact-MC tests."""

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
_OFF = "1:2:3:4:5:6:7:8:9:10:11"
_SBLP = "1:2:3:4:5:6:7:8:9:10"


def run(solver: Path, instance: Path, cache: str, ablation: str) -> subprocess.CompletedProcess:
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


def check_case(
    solver: Path,
    instance: Path,
    cache: str = "residual",
    require_candidate: bool = True,
) -> None:
    expected = count_models(instance)
    off = run(solver, instance, cache, _OFF)
    sblp = run(solver, instance, cache, _SBLP)
    values = (count(off, "off"), count(sblp, "SB-LP"))
    if values != (expected, expected):
        raise AssertionError(f"{instance.name}: counts={values}, oracle={expected}")
    if stat(off.stdout, "SB-LP calls", "off") != 0:
        raise AssertionError("ablated SB-LP was called")
    if stat(sblp.stdout, "SB-LP calls", "SB-LP") == 0:
        raise AssertionError("enabled SB-LP was not called")
    if require_candidate:
        if stat(sblp.stdout, "SB-LP candidates", "SB-LP") == 0:
            raise AssertionError(f"{instance.name}: GLPK produced no candidate")
        if stat(sblp.stdout, "SB-LP exact passed", "SB-LP") == 0:
            raise AssertionError(f"{instance.name}: exact checker proved no candidate")
        if stat(sblp.stdout, "SB-LP implied", "SB-LP") == 0:
            raise AssertionError(f"{instance.name}: no implication was committed")


def backbone_formula(target_negated: bool, swap: bool, free_variables: int) -> str:
    target = "~x1" if target_negated else "x1"
    a, b = (("x2", "x3") if not swap else ("~x2", "x3"))
    opposite_a = "~x2" if a == "x2" else "x2"
    rows = (
        f"+1 {target} +1 {a} +1 {b} >= 1 ;",
        f"+1 {target} +1 {a} +1 ~x3 >= 1 ;",
        f"+1 {target} +1 {opposite_a} +1 {b} >= 1 ;",
        f"+1 {target} +1 {opposite_a} +1 ~x3 >= 1 ;",
    )
    count = 3 + free_variables
    return f"* #variable= {count} #constraint= 4\n" + "\n".join(rows) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--solver", required=True, type=Path)
    args = parser.parse_args()
    solver = args.solver.resolve()

    with tempfile.TemporaryDirectory(prefix="pbmc-gate5-lp1-") as directory:
        root = Path(directory)
        formulas = {
            "positive.opb": backbone_formula(False, False, 0),
            "negative.opb": backbone_formula(True, False, 0),
            "less_equal.opb": (
                "* #variable= 3 #constraint= 4\n"
                "+1 ~x1 +1 ~x2 +1 ~x3 <= 2 ;\n"
                "+1 ~x1 +1 ~x2 +1 x3 <= 2 ;\n"
                "+1 ~x1 +1 x2 +1 ~x3 <= 2 ;\n"
                "+1 ~x1 +1 x2 +1 x3 <= 2 ;\n"
            ),
            "equality.opb": (
                "* #variable= 4 #constraint= 5\n"
                "+1 x1 +1 x2 = 1 ;\n"
                "+1 ~x2 +1 x3 +1 x4 >= 1 ;\n"
                "+1 ~x2 +1 x3 +1 ~x4 >= 1 ;\n"
                "+1 ~x2 +1 ~x3 +1 x4 >= 1 ;\n"
                "+1 ~x2 +1 ~x3 +1 ~x4 >= 1 ;\n"
            ),
            "large_exact.opb": (
                "* #variable= 6 #constraint= 6\n"
                + backbone_formula(False, False, 0).split("\n", 1)[1]
                + "+3000000000 x4 +2999999999 x5 +1 x6 >= 3000000000 ;\n"
                + "+1 x5 >= 1 ;\n"
            ),
        }
        for filename, formula in formulas.items():
            (root / filename).write_text(formula)

        for filename in ("positive.opb", "negative.opb", "less_equal.opb", "equality.opb", "large_exact.opb"):
            check_case(solver, root / filename)
        for cache in ("legacy", "bypass", "residual"):
            check_case(solver, root / "positive.opb", cache=cache)

        rng = random.Random(0x51B1)
        for index in range(12):
            instance = root / f"random_{index:02d}.opb"
            instance.write_text(
                backbone_formula(
                    target_negated=bool(rng.getrandbits(1)),
                    swap=bool(rng.getrandbits(1)),
                    free_variables=rng.randrange(0, 4),
                )
            )
            check_case(solver, instance)

    print("Gate 5-LP-1 SB-LP exactness tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
