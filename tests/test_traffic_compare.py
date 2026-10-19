#!/usr/bin/env python3
"""Corrupt identical traffic traces; the actual owner comparator must reject them."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


if not __debug__:
    raise RuntimeError("Assertions must remain enabled")


def mutate(raw, name):
    lines = raw.decode().splitlines()
    scenario = -1
    candidates = []
    for index, line in enumerate(lines):
        fields = line.split()
        if fields and fields[0] == "scenario":
            scenario = int(fields[1])
        if scenario == 1 and fields and fields[0] == "apply":
            candidates.append((index, fields))
    assert candidates
    index, fields = next((i, f) for i, f in candidates if f[2] == "0")
    if name == "apply-value":
        fields[7] = str((int(fields[7]) + 17) % 101)
        lines[index] = " ".join(fields)
    elif name == "early-apply":
        fields[1] = str(int(fields[4]) - 10)
        lines[index] = " ".join(fields)
    elif name == "late-apply":
        fields[1] = str(int(fields[4]) + 50)
        lines[index] = " ".join(fields)
    elif name == "apply-order":
        other, _ = next((i, f) for i, f in candidates if f[2] == "0" and i > index)
        lines[index], lines[other] = lines[other], lines[index]
    elif name == "missing-peer-apply":
        index, _ = next((i, f) for i, f in candidates if f[2] == "1")
        del lines[index]
        summary = next(i for i, line in enumerate(lines) if line.startswith("summary 1 "))
        fields = lines[summary].split()
        fields[8] = str(int(fields[8]) - 1)
        lines[summary] = " ".join(fields)
    elif name == "reserved-intent":
        first = next(i for i, line in enumerate(lines) if line.startswith("scenario 1 "))
        index = next(i for i in range(first + 1, len(lines)) if lines[i].startswith("reserved "))
        fields = lines[index].split()
        sequence, deadline = fields[4], fields[5]
        fields[8] = str((int(fields[8]) + 17) % 101)
        lines[index] = " ".join(fields)
        for index, fields in candidates:
            if fields[3:5] == [sequence, deadline]:
                fields[7] = str((int(fields[7]) + 17) % 101)
                lines[index] = " ".join(fields)
    elif name == "final-value":
        index = next(i for i, line in enumerate(lines) if line.startswith("final 1 "))
        fields = lines[index].split()
        fields[4] = str((int(fields[4]) + 17) % 101)
        lines[index] = " ".join(fields)
    elif name == "missing-recovery":
        scenario = -1
        kept = []
        removed = [0, 0]
        for line in lines:
            fields = line.split()
            if fields and fields[0] == "scenario":
                scenario = int(fields[1])
            if scenario == 2 and fields and fields[0] == "apply" and int(fields[1]) >= 26000:
                removed[int(fields[2])] += 1
                continue
            if fields and fields[:2] == ["summary", "2"]:
                fields[7] = str(int(fields[7]) - removed[0])
                fields[8] = str(int(fields[8]) - removed[1])
                line = " ".join(fields)
            kept.append(line)
        assert all(removed)
        lines = kept
    else:
        raise AssertionError(name)
    result = ("\n".join(lines) + "\n").encode()
    assert result != raw
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--apply-only", action="store_true", help="Focused old-shape before-control")
    args = parser.parse_args()
    if args.output.exists() or args.output.is_symlink():
        raise SystemExit("fresh output required")
    args.output.mkdir(parents=True)
    comparator = Path(__file__).with_name("traffic_compare.py")
    source = comparator.read_bytes()
    original = [(args.candidate / f"repeat-{i}.log").read_bytes() for i in range(1, 4)]
    assert original[0] == original[1] == original[2]
    names = ["apply-value"] if args.apply_only else [
        "apply-value", "reserved-intent", "early-apply", "late-apply", "apply-order",
        "missing-peer-apply", "final-value", "missing-recovery",
    ]
    results = []
    for name in ["unchanged", *names]:
        folder = args.output / name
        folder.mkdir()
        candidate = args.candidate
        if name != "unchanged":
            candidate = folder / "inputs"
            candidate.mkdir()
            changed = mutate(original[0], name)
            for repeat in range(1, 4):
                (candidate / f"repeat-{repeat}.log").write_bytes(changed)
        command = [sys.executable, str(comparator), str(args.baseline), str(candidate),
                   str(folder / "comparison")]
        run = subprocess.run(command, capture_output=True)
        (folder / "stdout.log").write_bytes(run.stdout)
        (folder / "stderr.log").write_bytes(run.stderr)
        exits = [run.returncode]
        expected = run.returncode == 0 if name == "unchanged" else run.returncode != 0
        row = {"name": name, "classification": "positive" if name == "unchanged" else "fixture-only corrupted trace",
               "commands": [command], "exits": exits, "expected": expected,
               "identical_input_sha256": hashlib.sha256((candidate / "repeat-1.log").read_bytes()).hexdigest()}
        results.append(row)
        (folder / "result.json").write_text(json.dumps(row, indent=2))
        print(f"traffic comparator {name}: {'PASS' if expected else 'FAIL'} {exits}", flush=True)
    assert comparator.read_bytes() == source
    (args.output / "results.json").write_text(json.dumps({
        "comparator_sha256": hashlib.sha256(source).hexdigest(),
        "fixture_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "results": results, "all_expected": all(row["expected"] for row in results),
        "parser_executions_per_case": 1,
        "claim": "Trace assertion sensitivity only; these are not production source mutations or physical transport tests.",
    }, indent=2))
    return 0 if all(row["expected"] for row in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
