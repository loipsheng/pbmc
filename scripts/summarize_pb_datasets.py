#!/usr/bin/env python3
"""Summarize variable, constraint, row-length, and coefficient statistics.

The input is expected to use the OPB pseudo-Boolean format.  Statistics use
the declarations in the OPB header for n and m when they are available.  The
parser independently counts variables and constraints and reports mismatches.

For one instance, l is the maximum number of nonzero terms as written in any
constraint.  Coefficient ranges exclude the right-hand side and zero-valued
terms.  The paper-style summary reports absolute coefficient magnitudes.
"""

from __future__ import annotations

import argparse
import csv
import gzip
import os
import re
import sys
from concurrent.futures import ProcessPoolExecutor
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Iterator, Optional, TextIO


DEFAULT_DATASETS = (
    ("Auction", Path("benchmarks/opb/auction")),
    ("Knapsack", Path("benchmarks/opb/knapsack")),
    ("Graph", Path("benchmarks/opb/graph")),
    ("CostGraph", Path("benchmarks/cost-graph")),
)

HEADER_VARIABLE_RE = re.compile(r"#variable\s*=\s*(\d+)", re.IGNORECASE)
HEADER_CONSTRAINT_RE = re.compile(r"#constraint\s*=\s*(\d+)", re.IGNORECASE)
RELATION_RE = re.compile(r">=|<=|=")
LITERAL_RE = re.compile(r"~?[A-Za-z_][A-Za-z0-9_.$\[\]]*\Z")
TERM_RE = re.compile(
    r"(?<!\S)([+-]?\s*\d+)\s*\*?\s*"
    r"(~?[A-Za-z_][A-Za-z0-9_.$\[\]]*)"
)


class ParseError(RuntimeError):
    """Raised when an OPB file cannot be parsed safely."""


@dataclass
class InstanceStats:
    dataset: str
    path: str
    variables: int
    constraints: int
    max_nonzeros: int
    coefficient_abs_min: Optional[int]
    coefficient_abs_max: Optional[int]
    coefficient_signed_min: Optional[int]
    coefficient_signed_max: Optional[int]
    declared_variables: Optional[int]
    observed_variables: int
    declared_constraints: Optional[int]
    parsed_constraints: int
    equalities: int


@dataclass
class DatasetSummary:
    dataset: str
    instances: int
    variables_min: int
    variables_max: int
    constraints_min: int
    constraints_max: int
    max_nonzeros_min: int
    max_nonzeros_max: int
    coefficient_abs_min: Optional[int]
    coefficient_abs_max: Optional[int]


def open_text(path: Path) -> TextIO:
    if path.name.endswith(".gz"):
        return gzip.open(path, mode="rt", encoding="utf-8", errors="strict")
    return path.open(mode="rt", encoding="utf-8", errors="strict")


def iter_opb_files(path: Path) -> list[Path]:
    if path.is_file():
        if path.name.endswith((".opb", ".opb.gz")):
            return [path]
        raise ValueError(f"not an OPB file: {path}")
    if not path.is_dir():
        raise ValueError(f"dataset path does not exist: {path}")
    return sorted(
        candidate
        for candidate in path.rglob("*")
        if candidate.is_file()
        and candidate.name.endswith((".opb", ".opb.gz"))
    )


def update_optional_min(current: Optional[int], value: int) -> int:
    return value if current is None else min(current, value)


def update_optional_max(current: Optional[int], value: int) -> int:
    return value if current is None else max(current, value)


def parse_constraint(statement: str, source: Path) -> Optional[tuple[int, list[int], set[str], bool]]:
    stripped = statement.strip()
    if not stripped:
        return None
    if stripped.lower().startswith(("min:", "max:")):
        return None

    relation = RELATION_RE.search(stripped)
    if relation is None:
        raise ParseError(f"{source}: statement has no relation: {stripped[:120]!r}")

    lhs = stripped[: relation.start()]
    coefficients: list[int] = []
    variables: set[str] = set()

    # Generated benchmark files overwhelmingly use alternating integer and
    # literal tokens.  This path is much faster on multi-gigabyte datasets.
    # Keep the regex parser below as a compatibility fallback for forms such
    # as "+ 1 x1" and "1*x1".
    tokens = lhs.split()
    fast_path = len(tokens) % 2 == 0
    if fast_path:
        try:
            for index in range(0, len(tokens), 2):
                coefficient = int(tokens[index])
                literal = tokens[index + 1]
                if LITERAL_RE.fullmatch(literal) is None:
                    fast_path = False
                    break
                variables.add(
                    literal[1:] if literal.startswith("~") else literal
                )
                if coefficient != 0:
                    coefficients.append(coefficient)
        except ValueError:
            fast_path = False

    if not fast_path:
        coefficients.clear()
        variables.clear()
        for match in TERM_RE.finditer(lhs):
            coefficient = int(match.group(1).replace(" ", ""))
            literal = match.group(2)
            variables.add(
                literal[1:] if literal.startswith("~") else literal
            )
            if coefficient != 0:
                coefficients.append(coefficient)

        unparsed = TERM_RE.sub("", lhs).strip()
        if unparsed:
            raise ParseError(
                f"{source}: unsupported text on constraint left-hand side: "
                f"{unparsed[:120]!r}"
            )

    return len(coefficients), coefficients, variables, relation.group() == "="


def scan_instance(dataset: str, path: Path, expand_equalities: bool) -> tuple[InstanceStats, list[str]]:
    declared_variables: Optional[int] = None
    declared_constraints: Optional[int] = None
    observed_variables: set[str] = set()
    parsed_constraints = 0
    equalities = 0
    max_nonzeros = 0
    coefficient_abs_min: Optional[int] = None
    coefficient_abs_max: Optional[int] = None
    coefficient_signed_min: Optional[int] = None
    coefficient_signed_max: Optional[int] = None
    pending = ""

    with open_text(path) as source:
        for raw_line in source:
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith("*"):
                variable_match = HEADER_VARIABLE_RE.search(line)
                constraint_match = HEADER_CONSTRAINT_RE.search(line)
                if variable_match:
                    declared_variables = int(variable_match.group(1))
                if constraint_match:
                    declared_constraints = int(constraint_match.group(1))
                continue

            pending = f"{pending} {line}" if pending else line
            while ";" in pending:
                statement, pending = pending.split(";", 1)
                parsed = parse_constraint(statement, path)
                if parsed is None:
                    continue
                nonzeros, coefficients, variables, is_equality = parsed
                parsed_constraints += 1
                equalities += int(is_equality)
                max_nonzeros = max(max_nonzeros, nonzeros)
                observed_variables.update(variables)
                for coefficient in coefficients:
                    magnitude = abs(coefficient)
                    coefficient_abs_min = update_optional_min(
                        coefficient_abs_min, magnitude
                    )
                    coefficient_abs_max = update_optional_max(
                        coefficient_abs_max, magnitude
                    )
                    coefficient_signed_min = update_optional_min(
                        coefficient_signed_min, coefficient
                    )
                    coefficient_signed_max = update_optional_max(
                        coefficient_signed_max, coefficient
                    )

    if pending.strip():
        raise ParseError(f"{path}: unterminated statement at end of file")

    warnings: list[str] = []
    observed_count = len(observed_variables)
    if declared_variables is not None and observed_count > declared_variables:
        warnings.append(
            f"declares {declared_variables} variables but uses {observed_count}"
        )
    if (
        declared_constraints is not None
        and parsed_constraints != declared_constraints
    ):
        warnings.append(
            f"declares {declared_constraints} constraints but contains "
            f"{parsed_constraints}"
        )

    variables = (
        declared_variables if declared_variables is not None else observed_count
    )
    constraints = parsed_constraints + (equalities if expand_equalities else 0)
    return (
        InstanceStats(
            dataset=dataset,
            path=str(path),
            variables=variables,
            constraints=constraints,
            max_nonzeros=max_nonzeros,
            coefficient_abs_min=coefficient_abs_min,
            coefficient_abs_max=coefficient_abs_max,
            coefficient_signed_min=coefficient_signed_min,
            coefficient_signed_max=coefficient_signed_max,
            declared_variables=declared_variables,
            observed_variables=observed_count,
            declared_constraints=declared_constraints,
            parsed_constraints=parsed_constraints,
            equalities=equalities,
        ),
        warnings,
    )


def scan_job(
    job: tuple[str, Path, bool],
) -> tuple[InstanceStats, list[str]]:
    """Pickle-friendly adapter for ProcessPoolExecutor."""
    return scan_instance(*job)


def summarize(dataset: str, instances: list[InstanceStats]) -> DatasetSummary:
    if not instances:
        raise ValueError(f"dataset {dataset!r} contains no OPB instances")
    abs_min_values = [
        item.coefficient_abs_min
        for item in instances
        if item.coefficient_abs_min is not None
    ]
    abs_max_values = [
        item.coefficient_abs_max
        for item in instances
        if item.coefficient_abs_max is not None
    ]
    return DatasetSummary(
        dataset=dataset,
        instances=len(instances),
        variables_min=min(item.variables for item in instances),
        variables_max=max(item.variables for item in instances),
        constraints_min=min(item.constraints for item in instances),
        constraints_max=max(item.constraints for item in instances),
        max_nonzeros_min=min(item.max_nonzeros for item in instances),
        max_nonzeros_max=max(item.max_nonzeros for item in instances),
        coefficient_abs_min=min(abs_min_values) if abs_min_values else None,
        coefficient_abs_max=max(abs_max_values) if abs_max_values else None,
    )


def range_text(lower: Optional[int], upper: Optional[int]) -> str:
    if lower is None or upper is None:
        return "N/A"
    return f"[{lower}, {upper}]"


def markdown_table(summaries: Iterable[DatasetSummary]) -> str:
    lines = [
        "| Benchmark | Instances | Variables n | Constraints m | Max nonzeros l | Coefficients |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for item in summaries:
        lines.append(
            f"| {item.dataset} | {item.instances:,} | "
            f"{range_text(item.variables_min, item.variables_max)} | "
            f"{range_text(item.constraints_min, item.constraints_max)} | "
            f"{range_text(item.max_nonzeros_min, item.max_nonzeros_max)} | "
            f"{range_text(item.coefficient_abs_min, item.coefficient_abs_max)} |"
        )
    return "\n".join(lines) + "\n"


def latex_escape(text: str) -> str:
    return (
        text.replace("\\", r"\textbackslash{}")
        .replace("_", r"\_")
        .replace("&", r"\&")
        .replace("%", r"\%")
        .replace("#", r"\#")
    )


def latex_table(summaries: Iterable[DatasetSummary]) -> str:
    lines = [
        r"\begin{tabular}{lrrrrr}",
        r"\hline",
        r"Benchmark & Instances & Variables $n$ & Constraints $m$ & Max nnz $\ell$ & Coefficients $|a|$ \\",
        r"\hline",
    ]
    for item in summaries:
        lines.append(
            f"{latex_escape(item.dataset)} & {item.instances:,} & "
            f"{range_text(item.variables_min, item.variables_max)} & "
            f"{range_text(item.constraints_min, item.constraints_max)} & "
            f"{range_text(item.max_nonzeros_min, item.max_nonzeros_max)} & "
            f"{range_text(item.coefficient_abs_min, item.coefficient_abs_max)} "
            r"\\"
        )
    lines.extend((r"\hline", r"\end{tabular}"))
    return "\n".join(lines) + "\n"


def write_csv(path: Path, rows: Iterable[object]) -> None:
    materialized = list(rows)
    if not materialized:
        return
    fieldnames = list(asdict(materialized[0]).keys())
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        for row in materialized:
            writer.writerow(asdict(row))


def parse_dataset_argument(value: str) -> tuple[str, Path]:
    if "=" in value:
        name, raw_path = value.split("=", 1)
        if not name or not raw_path:
            raise argparse.ArgumentTypeError(
                f"expected NAME=PATH, received {value!r}"
            )
        return name, Path(raw_path)
    path = Path(value)
    return path.name or str(path), path


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Summarize OPB dataset sizes, maximum row nonzeros, and "
            "coefficient ranges."
        )
    )
    parser.add_argument(
        "datasets",
        nargs="*",
        type=parse_dataset_argument,
        metavar="[NAME=]PATH",
        help=(
            "dataset directory or OPB file; when omitted, scan Auction, "
            "Knapsack, Graph, and CostGraph under benchmarks/"
        ),
    )
    parser.add_argument(
        "--expand-equalities",
        action="store_true",
        help="count each equality as two inequalities, as PBMC does internally",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(8, os.cpu_count() or 1),
        metavar="N",
        help="number of files to scan in parallel (default: %(default)s)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help=(
            "write instance_stats.csv, dataset_summary.csv, summary.md, "
            "and summary.tex to this directory"
        ),
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="fail if parsed counts disagree with an OPB header",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="suppress progress messages on stderr",
    )
    return parser


def main() -> int:
    args = build_argument_parser().parse_args()
    if args.jobs < 1:
        print("error: --jobs must be at least 1", file=sys.stderr)
        return 2
    datasets = args.datasets or list(DEFAULT_DATASETS)
    all_instances: list[InstanceStats] = []
    summaries: list[DatasetSummary] = []

    try:
        for dataset, dataset_path in datasets:
            files = iter_opb_files(dataset_path)
            if not files:
                raise ValueError(f"no .opb or .opb.gz files under {dataset_path}")
            if not args.quiet:
                print(
                    f"Scanning {dataset}: {len(files)} instance(s) from "
                    f"{dataset_path} with {args.jobs} job(s)",
                    file=sys.stderr,
                    flush=True,
                )

            dataset_instances: list[InstanceStats] = []
            jobs = (
                (dataset, path, args.expand_equalities) for path in files
            )
            if args.jobs == 1:
                results: Iterable[tuple[InstanceStats, list[str]]] = map(
                    scan_job, jobs
                )
                executor = None
            else:
                executor = ProcessPoolExecutor(max_workers=args.jobs)
                results = executor.map(scan_job, jobs, chunksize=1)

            for index, (stats, warnings) in enumerate(results, start=1):
                for warning in warnings:
                    message = f"warning: {stats.path}: {warning}"
                    if args.strict:
                        raise ParseError(message)
                    print(message, file=sys.stderr)
                dataset_instances.append(stats)
                if not args.quiet and index % 100 == 0:
                    print(
                        f"  {dataset}: {index}/{len(files)}",
                        file=sys.stderr,
                        flush=True,
                    )
            if executor is not None:
                executor.shutdown()

            all_instances.extend(dataset_instances)
            summaries.append(summarize(dataset, dataset_instances))
    except (OSError, UnicodeError, ValueError, ParseError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    table = markdown_table(summaries)
    print(table, end="")

    if args.output_dir is not None:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        write_csv(args.output_dir / "instance_stats.csv", all_instances)
        write_csv(args.output_dir / "dataset_summary.csv", summaries)
        (args.output_dir / "summary.md").write_text(table, encoding="utf-8")
        (args.output_dir / "summary.tex").write_text(
            latex_table(summaries), encoding="utf-8"
        )
        if not args.quiet:
            print(f"Wrote results to {args.output_dir}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
