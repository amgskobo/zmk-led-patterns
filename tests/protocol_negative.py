#!/usr/bin/env python3
"""Identified protocol mutations in fresh test artifacts, never source/dependencies.

Each operator changes one exact production expression. Compilation failure is
an error, not a killed mutant. The same resulting binary is run three times.
"""
import hashlib
import json
import pathlib
import subprocess
import sys

from mutation_result import is_c_assertion_abort

root = pathlib.Path(__file__).resolve().parents[1]
out = pathlib.Path(sys.argv[1])
if out.exists() or out.is_symlink():
    raise SystemExit("fresh output required")
out.mkdir(parents=True)
source = root / "src/led_sync_protocol.h"
original = source.read_bytes()
text = original.decode()
operators = {
    "early-reservation": ("now >= e->pending.local_deadline", "true"),
    "reserved-byte": ("p->data.bytes[i] != 0", "false && p->data.bytes[i] != 0"),
    "false-applied": ("if (e->have_active && e->active_rendered)", "if (e->active.valid)"),
    "lost-reset-intent": ("e->reset_phase |= e->central && p.reset_phase;", "(void)p.reset_phase;"),
    "missing-demand-fit": ("e->clock_due = now;\n            /* A stale-to-fresh", "e->clock_due = LED_SYNC_NEVER;\n            /* A stale-to-fresh"),
    "reused-challenge": ("e->candidate_peer_epoch = ++e->challenge_counter;", "e->candidate_peer_epoch = e->epoch + 1;"),
    "late-completion": ("now - e->active.local_deadline > LED_SYNC_LATE_MS", "now - e->active.local_deadline > 2U * LED_SYNC_LATE_MS"),
    "early-synced": ("bool confirmed = e->active_rendered && (!e->central || e->active.confirmed);", "bool confirmed = (!e->central || e->active.confirmed);"),
}
results = []
for name, pair in [("unchanged", None), *operators.items()]:
    folder = out / name
    folder.mkdir()
    changed = text
    if pair:
        old, new = pair
        if text.count(old) != 1:
            raise SystemExit(f"operator no longer identifies exactly one expression: {name}")
        changed = text.replace(old, new)
    (folder / source.name).write_text(changed, encoding="utf-8")
    binary = folder / "test"
    command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
               "-I" + str(folder), "-I" + str(root / "src"),
               str(root / "tests/test_led_sync_protocol.c"), "-o", str(binary)]
    compiled = subprocess.run(command, capture_output=True, text=True)
    (folder / "compile.stdout.log").write_text(compiled.stdout)
    (folder / "compile.stderr.log").write_text(compiled.stderr)
    if compiled.returncode:
        raise SystemExit(f"mutation compile failure is not a passing control: {name}")
    codes = []
    assertion_failures = []
    for repeat in range(1, 4):
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        (folder / f"run-{repeat}.stdout.log").write_text(run.stdout)
        (folder / f"run-{repeat}.stderr.log").write_text(run.stderr)
        codes.append(run.returncode)
        assertion_failures.append(is_c_assertion_abort(run.returncode, run.stderr))
    expected = all(code == 0 for code in codes) if not pair else all(assertion_failures)
    row = {"name": name, "operator": pair, "command": command, "exits": codes, "assertion_failures": assertion_failures,
           "sha256": hashlib.sha256(changed.encode()).hexdigest(), "expected": expected}
    results.append(row)
    (folder / "result.json").write_text(json.dumps(row, indent=2))
    print(f"protocol {name}: {'PASS' if expected else 'FAIL'} {codes}", flush=True)
    if not expected:
        raise SystemExit(f"unexpected identified control result: {name}")
if source.read_bytes() != original:
    raise SystemExit("production source changed during controls")
(out / "results.json").write_text(json.dumps({"source_sha256": hashlib.sha256(original).hexdigest(),
                                              "results": results}, indent=2))
