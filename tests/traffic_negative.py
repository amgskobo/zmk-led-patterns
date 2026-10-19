#!/usr/bin/env python3
"""Ordered traffic sensitivity against exact production faults, never totals alone."""
import hashlib
import json
import pathlib
import resource
import subprocess
import sys

from mutation_result import is_c_assertion_abort

if not __debug__:
    raise RuntimeError("Assertions must remain enabled")

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = pathlib.Path(__file__).resolve().parents[1]
output = pathlib.Path(sys.argv[1])
if output.exists() or output.is_symlink():
    raise SystemExit("fresh output required")
output.mkdir(parents=True)
source = root / "src/led_sync_protocol.h"
original = source.read_bytes()
text = original.decode()
operators = {
    "wrong-visible-value": (
        "e->visible = led_sync_wire_values(&plan->wire);",
        "e->visible = led_sync_wire_values(&plan->wire); e->visible.brightness ^= 1U;"),
    "wrong-phase-epoch": (
        "e->phase_epoch = plan->wire.phase_epoch;", "e->phase_epoch = plan->wire.phase_epoch + 1U;"),
    "early-reservation-consumption": (
        "e->pending.valid && now >= e->pending.local_deadline",
        "e->pending.valid && now + 10U >= e->pending.local_deadline"),
    "delayed-reservation": (
        "e->pending.valid && now >= e->pending.local_deadline",
        "e->pending.valid && now >= e->pending.local_deadline + 50U"),
}
results = []
for name, pair in [("unchanged", None), *operators.items()]:
    folder = output / name
    folder.mkdir()
    changed = text
    if pair:
        old, new = pair
        assert text.count(old) == 1, (name, text.count(old))
        changed = text.replace(old, new)
    (folder / source.name).write_text(changed)
    wire = (root / "src/led_sync_wire.h").read_bytes()
    (folder / "led_sync_wire.h").write_bytes(wire)
    command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
               "-I" + str(folder), "-I" + str(root / "src"),
               str(root / "tests/test_led_sync_traffic.c"), "-o", str(folder / "test")]
    compiled = subprocess.run(command, capture_output=True, text=True)
    (folder / "compile.stdout.log").write_text(compiled.stdout)
    (folder / "compile.stderr.log").write_text(compiled.stderr)
    assert compiled.returncode == 0, (name, compiled.stderr)
    codes = []
    assertion_failures = []
    for repeat in range(1, 4):
        run = subprocess.run([str(folder / "test")], capture_output=True, text=True)
        (folder / f"repeat-{repeat}.stdout.log").write_text(run.stdout)
        (folder / f"repeat-{repeat}.stderr.log").write_text(run.stderr)
        codes.append(run.returncode)
        assertion_failures.append(is_c_assertion_abort(run.returncode, run.stderr))
    expected = [0, 0, 0] if pair is None else [-6, -6, -6]
    row = {"name": name, "operator": pair, "command": command, "exits": codes, "assertion_failures": assertion_failures,
           "protocol_sha256": hashlib.sha256(changed.encode()).hexdigest(),
           "unchanged_wire_sha256": hashlib.sha256(wire).hexdigest()}
    results.append(row)
    (folder / "result.json").write_text(json.dumps(row, indent=2))
    assert codes == expected, (name, codes)
    if pair is not None and not all(assertion_failures):
        raise SystemExit(f"mutation did not fail a C assertion: {name}")
    print(f"traffic {name}: PASS {codes}", flush=True)
assert source.read_bytes() == original
(output / "results.json").write_text(json.dumps(results, indent=2))
