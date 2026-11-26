#!/usr/bin/env python3
"""Gate 3D committed pairwise SC integration tests."""

from __future__ import annotations

import argparse
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

from mc_oracle import count_models, satisfying_assignments


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
    proposal: bool | None = None,
    require_commit: bool = False,
) -> None:
    expected = count_models(instance)
    empty = run(solver, instance, cache, "1:2:3:4:5:6")
    sc = run(solver, instance, cache, "1:2:3:5:6")
    values = (count(empty, "empty"), count(sc, "SC"))
    if values != (expected, expected):
        raise AssertionError(f"{instance.name}: counts={values}, oracle={expected}")
    if cache == "legacy" and stable(empty.stdout, "empty") != stable(sc.stdout, "SC"):
        raise AssertionError(f"{instance.name}: legacy view-only SC changed the search")
    if "c o simplification ablation = Basic, BICP, CS, GCD, Cardinality, SC" not in empty.stdout:
        raise AssertionError("empty simplifier ablation was not reported")
    if "c o simplification ablation = Basic, BICP, CS, GCD, Cardinality" not in sc.stdout:
        raise AssertionError("SC-only ablation was not reported")
    if stat(empty.stdout, "SC calls", "empty") != 0:
        raise AssertionError("ablated SC was called")
    if stat(sc.stdout, "SC calls", "SC") == 0:
        raise AssertionError("enabled SC was not called")
    if stat(sc.stdout, "SC budgets", "SC") != 0:
        raise AssertionError("small SC case exhausted its budget")
    commits = stat(sc.stdout, "SC commits", "SC")
    if cache == "legacy" and commits != 0:
        raise AssertionError("legacy SC published an overlay")
    if cache != "legacy" and require_commit and commits == 0:
        raise AssertionError(f"{cache}: SC did not publish the required residual row")
    if stat(sc.stdout, "SC revalidations", "SC") < commits:
        raise AssertionError("SC committed without a latest-row revalidation")
    if proposal is not None:
        found = stat(sc.stdout, "SC proposals", "SC") > 0
        if found != proposal:
            raise AssertionError(
                f"{instance.name}: SC proposal={found}, expected={proposal}"
            )


def check_model_set(root: Path, name: str, before: str, after: str) -> None:
    old = root / f"{name}_old.opb"
    new = root / f"{name}_new.opb"
    old.write_text(before)
    new.write_text(after)
    old_models = set(satisfying_assignments(old))
    new_models = set(satisfying_assignments(new))
    if old_models != new_models:
        raise AssertionError(
            f"{name}: rewrite changed the complete model set "
            f"({len(old_models)} != {len(new_models)})"
        )


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
        f"* deterministic random Gate 3D case {index}\n"
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

    with tempfile.TemporaryDirectory(prefix="pbmc-gate3d-") as directory:
        root = Path(directory)
        formulas = {
            "positive.opb": (
                "* #variable= 3 #constraint= 2\n"
                "+8 x1 +3 x2 +2 x3 >= 5 ;\n"
                "+1 x2 +1 x3 >= 1 ;\n"
            ),
            "negative.opb": (
                "* #variable= 3 #constraint= 2\n"
                "+8 ~x1 +3 x2 +2 ~x3 >= 5 ;\n"
                "+1 x2 +1 ~x3 >= 1 ;\n"
            ),
            "no_bound.opb": (
                "* #variable= 4 #constraint= 2\n"
                "+8 x1 +3 x2 +2 x3 >= 5 ;\n"
                "+1 x2 +1 x4 >= 1 ;\n"
            ),
            "one_row.opb": (
                "* #variable= 3 #constraint= 1\n"
                "+8 x1 +3 x2 +2 x3 >= 5 ;\n"
            ),
            "different_support.opb": (
                "* #variable= 5 #constraint= 2\n"
                "+8 x1 +4 x2 +3 x3 +2 x4 >= 6 ;\n"
                "+2 x2 +2 x3 +1 x5 >= 3 ;\n"
            ),
            "difference_bound.opb": (
                "* #variable= 4 #constraint= 2\n"
                "+10 x1 +3 x2 +2 x3 +4 x4 >= 7 ;\n"
                "+1 x2 +1 x3 +1 x4 >= 2 ;\n"
            ),
            "batch_guard.opb": (
                "* #variable= 4 #constraint= 2\n"
                "+5 x1 +5 x2 +5 x3 +1 x4 >= 7 ;\n"
                "+1 x1 +1 x2 +1 x3 >= 2 ;\n"
            ),
            "equality.opb": (
                "* #variable= 3 #constraint= 2\n"
                "+5 x1 +3 x2 +2 x3 = 5 ;\n"
                "+1 x2 +1 x3 >= 1 ;\n"
            ),
        }
        for filename, formula in formulas.items():
            instance = root / filename
            instance.write_text(formula)
            check_case(
                solver,
                instance,
                proposal=filename not in ("no_bound.opb", "one_row.opb"),
                require_commit=filename not in ("no_bound.opb", "one_row.opb"),
            )

        for cache in ("legacy", "bypass", "residual"):
            check_case(
                solver,
                root / "positive.opb",
                cache,
                proposal=True,
                require_commit=cache != "legacy",
            )

        check_model_set(
            root,
            "common_literals",
            formulas["positive.opb"],
            "* #variable= 3 #constraint= 2\n"
            "+3 x1 +3 x2 +2 x3 >= 5 ;\n"
            "+1 x2 +1 x3 >= 1 ;\n",
        )
        check_model_set(
            root,
            "opposite_literal",
            formulas["negative.opb"],
            "* #variable= 3 #constraint= 2\n"
            "+3 ~x1 +3 x2 +2 ~x3 >= 5 ;\n"
            "+1 x2 +1 ~x3 >= 1 ;\n",
        )
        check_model_set(
            root,
            "different_support",
            formulas["different_support.opb"],
            "* #variable= 5 #constraint= 2\n"
            "+3 x1 +4 x2 +3 x3 +2 x4 >= 6 ;\n"
            "+2 x2 +2 x3 +1 x5 >= 3 ;\n",
        )
        check_model_set(
            root,
            "difference_bound",
            formulas["difference_bound.opb"],
            "* #variable= 4 #constraint= 2\n"
            "+2 x1 +3 x2 +2 x3 +4 x4 >= 7 ;\n"
            "+1 x2 +1 x3 +1 x4 >= 2 ;\n",
        )
        check_model_set(
            root,
            "batch_single_commit",
            formulas["batch_guard.opb"],
            "* #variable= 4 #constraint= 2\n"
            "+2 x1 +5 x2 +5 x3 +1 x4 >= 7 ;\n"
            "+1 x1 +1 x2 +1 x3 >= 2 ;\n",
        )
        bad_batch = root / "batch_bad.opb"
        bad_batch.write_text(
            "* #variable= 4 #constraint= 2\n"
            "+2 x1 +2 x2 +2 x3 +1 x4 >= 7 ;\n"
            "+1 x1 +1 x2 +1 x3 >= 2 ;\n"
        )
        if set(satisfying_assignments(root / "batch_guard.opb")) == set(
            satisfying_assignments(bad_batch)
        ):
            raise AssertionError("batch counterexample no longer detects unsafe bulk commit")
        check_model_set(
            root,
            "equality_direction",
            formulas["equality.opb"],
            "* #variable= 3 #constraint= 3\n"
            "+3 x1 +3 x2 +2 x3 >= 5 ;\n"
            "+5 ~x1 +3 ~x2 +2 ~x3 >= 5 ;\n"
            "+1 x2 +1 x3 >= 1 ;\n",
        )

        rng = random.Random(0x3D2026)
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

    print("Gate 3D committed pairwise SC exactness tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
