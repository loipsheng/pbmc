#!/usr/bin/env python3
"""Split every random input into runtime-balanced, reproducible manifests.

By default, runtimes come from the merged 2840-case log of the latest completed
adaptive-K experiment.  Successful cases use their measured ``Run time`` and
historical timeouts are charged as 3600 seconds.  No case is excluded.

The old XLSX source and an optional exclusion list remain available only for
reproducing earlier partitions.
"""

import argparse
import csv
import re
from pathlib import Path
from xml.etree import ElementTree as ET
from zipfile import ZipFile


ROOT = Path(__file__).resolve().parent.parent
MAIN_NS = "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
REL_NS = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
PKG_REL_NS = "http://schemas.openxmlformats.org/package/2006/relationships"
INPUT_RE = re.compile(
    r"^Input constraints:\s+(.+?)(?:\s+-\d+)?\s*$", re.MULTILINE
)


def column_number(reference):
    letters = "".join(char for char in reference if char.isalpha())
    result = 0
    for letter in letters.upper():
        result = result * 26 + ord(letter) - ord("A") + 1
    return result


def natural_key(name):
    return tuple(
        int(part) if part.isdigit() else part
        for part in re.split(r"(\d+)", name)
    )


def read_worksheet(path, sheet_name):
    main = {"m": MAIN_NS, "r": REL_NS}
    package = {"p": PKG_REL_NS}
    with ZipFile(path) as archive:
        shared_strings = []
        if "xl/sharedStrings.xml" in archive.namelist():
            root = ET.fromstring(archive.read("xl/sharedStrings.xml"))
            for item in root.findall("m:si", main):
                shared_strings.append(
                    "".join(
                        node.text or ""
                        for node in item.iter(f"{{{MAIN_NS}}}t")
                    )
                )

        workbook = ET.fromstring(archive.read("xl/workbook.xml"))
        relationships = ET.fromstring(
            archive.read("xl/_rels/workbook.xml.rels")
        )
        targets = {
            relation.attrib["Id"]: relation.attrib["Target"]
            for relation in relationships.findall("p:Relationship", package)
        }

        target = None
        for sheet in workbook.findall("m:sheets/m:sheet", main):
            if sheet.attrib["name"] == sheet_name:
                relation_id = sheet.attrib[f"{{{REL_NS}}}id"]
                target = targets[relation_id]
                break
        if target is None:
            raise ValueError(f"worksheet not found: {sheet_name}")
        target = target.lstrip("/")
        if not target.startswith("xl/"):
            target = f"xl/{target}"

        worksheet = ET.fromstring(archive.read(target))
        rows = []
        for row in worksheet.findall(".//m:sheetData/m:row", main):
            values = {}
            for cell in row.findall("m:c", main):
                index = column_number(cell.attrib["r"])
                cell_type = cell.attrib.get("t")
                value_node = cell.find("m:v", main)
                if cell_type == "inlineStr":
                    value = "".join(
                        node.text or ""
                        for node in cell.iter(f"{{{MAIN_NS}}}t")
                    )
                elif value_node is None:
                    value = ""
                elif cell_type == "s":
                    value = shared_strings[int(value_node.text)]
                else:
                    value = value_node.text
                values[index] = value
            if values:
                rows.append(
                    [values.get(index, "") for index in range(1, max(values) + 1)]
                )
        return rows


def load_runtime_estimates(path, sheet, runtime_column, timeout_seconds):
    rows = read_worksheet(path, sheet)
    if not rows:
        raise ValueError(f"empty worksheet: {sheet}")
    header = {name: index for index, name in enumerate(rows[0])}
    if "Test Case" not in header or runtime_column not in header:
        raise ValueError(
            f"required columns are missing; found: {', '.join(header)}"
        )

    estimates = {}
    timed_out = {}
    for row in rows[1:]:
        if len(row) <= max(header["Test Case"], header[runtime_column]):
            continue
        case = str(row[header["Test Case"]]).strip()
        raw_runtime = str(row[header[runtime_column]]).strip()
        if not case or not raw_runtime:
            continue
        runtime = float(raw_runtime)
        is_timeout = runtime < 0.0
        estimates[f"{case}.in"] = timeout_seconds if is_timeout else runtime
        timed_out[f"{case}.in"] = is_timeout
    return estimates, timed_out


def load_runtime_estimates_from_log(path, timeout_seconds):
    text = path.read_text(errors="replace")
    inputs = list(INPUT_RE.finditer(text))
    if not inputs:
        raise ValueError(f"no input records found in runtime log: {path}")

    estimates = {}
    timed_out = {}
    for index, match in enumerate(inputs):
        end = inputs[index + 1].start() if index + 1 < len(inputs) else len(text)
        segment = text[match.end():end]
        name = Path(match.group(1)).name
        if name in estimates:
            raise ValueError(f"duplicate input in runtime log: {name}")

        runtime_match = re.search(
            r"^Run time:\s*([0-9.eE+-]+)\s*$", segment, re.MULTILINE
        )
        if runtime_match:
            runtime = float(runtime_match.group(1))
            if runtime < 0.0:
                raise ValueError(f"negative successful runtime for {name}")
            estimates[name] = runtime
            timed_out[name] = False
            continue

        failure_match = re.search(
            r"^Failure category:\s*(\S+)\s*$", segment, re.MULTILINE
        )
        if not failure_match:
            raise ValueError(f"missing runtime or failure category for {name}")
        failure = failure_match.group(1)
        if failure != "TIMEOUT":
            raise ValueError(
                f"unsupported historical failure for {name}: {failure}; "
                "provide an explicit estimate instead of silently treating it "
                "as a timeout"
            )
        estimates[name] = timeout_seconds
        timed_out[name] = True

    return estimates, timed_out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--runtime-log",
        type=Path,
        default=ROOT / "result/adaptive_k/final_jaccard_confidence/random.txt",
        help="merged vm_random result log used by default",
    )
    parser.add_argument(
        "--runtime-xlsx",
        type=Path,
        default=None,
        help="legacy XLSX source; when set, overrides --runtime-log",
    )
    parser.add_argument("--sheet", default="random")
    parser.add_argument("--runtime-column", default="Adaptive_cp_max")
    parser.add_argument(
        "--benchmark-dir",
        type=Path,
        default=ROOT / "benchmarks/origin/random",
    )
    parser.add_argument(
        "--exclude-list",
        type=Path,
        default=None,
        help="optional legacy exclusion manifest; default: exclude nothing",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT /
        "benchmarks/input_lists/random_jaccard_confidence_4way",
    )
    parser.add_argument("--parts", type=int, default=4)
    parser.add_argument("--timeout-seconds", type=float, default=3600.0)
    parser.add_argument(
        "--expected-inputs",
        type=int,
        default=2840,
        help="fail unless this many cases are assigned; use 0 to disable",
    )
    args = parser.parse_args()

    if args.parts <= 0:
        parser.error("--parts must be positive")
    if args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")
    if args.expected_inputs < 0:
        parser.error("--expected-inputs must be nonnegative")

    if args.runtime_xlsx is not None:
        estimates, timed_out = load_runtime_estimates(
            args.runtime_xlsx,
            args.sheet,
            args.runtime_column,
            args.timeout_seconds,
        )
        runtime_source = args.runtime_xlsx
    else:
        estimates, timed_out = load_runtime_estimates_from_log(
            args.runtime_log, args.timeout_seconds
        )
        runtime_source = args.runtime_log
    benchmark_names = {
        path.name for path in args.benchmark_dir.glob("*.in") if path.is_file()
    }
    unexpected_runtimes = estimates.keys() - benchmark_names
    if unexpected_runtimes:
        raise ValueError(
            "runtime records are not present in the benchmark directory: "
            + ", ".join(sorted(unexpected_runtimes, key=natural_key))
        )
    excluded = set()
    if args.exclude_list is not None:
        excluded = {
            line.strip().removesuffix("\r")
            for line in args.exclude_list.read_text().splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }

    missing_exclusions = excluded - benchmark_names
    if missing_exclusions:
        raise ValueError(
            "excluded inputs are missing from the benchmark directory: "
            + ", ".join(sorted(missing_exclusions, key=natural_key))
        )
    selected = benchmark_names - excluded
    if args.expected_inputs and len(selected) != args.expected_inputs:
        raise ValueError(
            f"expected {args.expected_inputs} selected inputs, found "
            f"{len(selected)}"
        )
    missing_runtimes = selected - estimates.keys()
    if missing_runtimes:
        raise ValueError(
            "runtime estimates are missing for: "
            + ", ".join(sorted(missing_runtimes, key=natural_key))
        )

    assignments = [[] for _ in range(args.parts)]
    loads = [0.0 for _ in range(args.parts)]
    timeout_counts = [0 for _ in range(args.parts)]
    ordered = sorted(
        selected,
        key=lambda name: (-estimates[name], natural_key(name)),
    )
    for name in ordered:
        part = min(
            range(args.parts),
            key=lambda index: (loads[index], len(assignments[index]), index),
        )
        assignments[part].append(name)
        loads[part] += estimates[name]
        timeout_counts[part] += int(timed_out[name])

    args.output_dir.mkdir(parents=True, exist_ok=True)
    for index, names in enumerate(assignments, start=1):
        (args.output_dir / f"part{index}.txt").write_text(
            "".join(f"{name}\n" for name in names)
        )

    with (args.output_dir / "assignment.csv").open("w", newline="") as output:
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(
            ["part", "case", "estimated_seconds", "historical_timeout"]
        )
        for index, names in enumerate(assignments, start=1):
            for name in names:
                writer.writerow(
                    [
                        index,
                        name,
                        f"{estimates[name]:.12g}",
                        int(timed_out[name]),
                    ]
                )

    with (args.output_dir / "balance_summary.csv").open(
        "w", newline=""
    ) as output:
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(
            ["part", "inputs", "estimated_seconds", "estimated_hours", "timeouts"]
        )
        for index in range(args.parts):
            writer.writerow(
                [
                    index + 1,
                    len(assignments[index]),
                    f"{loads[index]:.6f}",
                    f"{loads[index] / 3600.0:.6f}",
                    timeout_counts[index],
                ]
            )

    assigned = [name for names in assignments for name in names]
    if len(assigned) != len(set(assigned)) or set(assigned) != selected:
        raise RuntimeError("partition validation failed")
    if args.expected_inputs and len(assigned) != args.expected_inputs:
        raise RuntimeError(
            f"expected {args.expected_inputs} assigned inputs, found "
            f"{len(assigned)}"
        )
    if not excluded and len(selected) % args.parts == 0:
        expected_part_size = len(selected) // args.parts
        if any(len(names) != expected_part_size for names in assignments):
            raise RuntimeError(
                "balanced full split has unequal manifest sizes: "
                + ", ".join(str(len(names)) for names in assignments)
            )

    print(f"runtime source: {runtime_source}")
    print(f"benchmarks: {len(benchmark_names)}")
    print(f"excluded: {len(excluded)}")
    print(f"assigned: {len(assigned)}")
    for index in range(args.parts):
        print(
            f"part{index + 1}: inputs={len(assignments[index])} "
            f"estimated_hours={loads[index] / 3600.0:.6f} "
            f"historical_timeouts={timeout_counts[index]}"
        )


if __name__ == "__main__":
    main()
