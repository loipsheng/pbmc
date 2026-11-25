#!/usr/bin/env python3
"""Independent brute-force oracle for small unweighted OPB instances."""

from __future__ import annotations

import argparse
import itertools
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Sequence, Tuple


_LITERAL = re.compile(r"(~?)x([1-9][0-9]*)$")
_HEADER = re.compile(r"#variable=\s*([0-9]+)")


@dataclass(frozen=True)
class Term:
    coefficient: int
    variable: int
    negated: bool


@dataclass(frozen=True)
class Constraint:
    terms: Tuple[Term, ...]
    comparator: str
    rhs: int

    def accepts(self, assignment: Sequence[int]) -> bool:
        lhs = 0
        for term in self.terms:
            value = assignment[term.variable - 1]
            if term.negated:
                value = 1 - value
            lhs += term.coefficient * value
        if self.comparator == ">=":
            return lhs >= self.rhs
        if self.comparator == "<=":
            return lhs <= self.rhs
        return lhs == self.rhs


def _parse_terms(tokens: Sequence[str]) -> Tuple[Term, ...]:
    terms: List[Term] = []
    index = 0
    while index < len(tokens):
        literal_match = _LITERAL.fullmatch(tokens[index])
        if literal_match is not None:
            coefficient = 1
            index += 1
        else:
            coefficient = int(tokens[index])
            index += 1
            if index >= len(tokens):
                raise ValueError("coefficient without a literal")
            literal_match = _LITERAL.fullmatch(tokens[index])
            if literal_match is None:
                raise ValueError(f"invalid literal: {tokens[index]}")
            index += 1
        terms.append(
            Term(
                coefficient=coefficient,
                variable=int(literal_match.group(2)),
                negated=bool(literal_match.group(1)),
            )
        )
    return tuple(terms)


def parse_opb(path: Path) -> Tuple[int, Tuple[Constraint, ...]]:
    declared_variables = None
    constraints: List[Constraint] = []
    maximum_variable = 0
    for line_number, raw_line in enumerate(path.read_text().splitlines(), 1):
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("*"):
            match = _HEADER.search(line)
            if match is not None:
                declared_variables = int(match.group(1))
            continue
        if not line.endswith(";"):
            raise ValueError(f"{path}:{line_number}: missing ';'")
        tokens = line[:-1].split()
        comparator_positions = [
            index for index, token in enumerate(tokens)
            if token in (">=", "<=", "=")
        ]
        if len(comparator_positions) != 1:
            raise ValueError(f"{path}:{line_number}: invalid comparator")
        comparator_index = comparator_positions[0]
        if comparator_index + 2 != len(tokens):
            raise ValueError(f"{path}:{line_number}: invalid right-hand side")
        terms = _parse_terms(tokens[:comparator_index])
        for term in terms:
            maximum_variable = max(maximum_variable, term.variable)
        constraints.append(
            Constraint(terms, tokens[comparator_index], int(tokens[-1]))
        )

    variable_count = maximum_variable if declared_variables is None else declared_variables
    if maximum_variable > variable_count:
        raise ValueError(f"{path}: literal exceeds declared variable count")
    return variable_count, tuple(constraints)


def satisfying_assignments(path: Path) -> Iterable[Tuple[int, ...]]:
    variable_count, constraints = parse_opb(path)
    for assignment in itertools.product((0, 1), repeat=variable_count):
        if all(constraint.accepts(assignment) for constraint in constraints):
            yield assignment


def count_models(path: Path) -> int:
    return sum(1 for _ in satisfying_assignments(path))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("instance", type=Path)
    parser.add_argument("--models", action="store_true")
    args = parser.parse_args()
    models = list(satisfying_assignments(args.instance))
    if args.models:
        for model in models:
            print("".join(map(str, model)))
    print(len(models))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
