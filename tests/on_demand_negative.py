#!/usr/bin/env python3
"""Identified on-demand engine mutations; unchanged owner scenarios, no source edits."""
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
    "periodic-background": ("e->transaction_active = e->pending.valid || e->dirty;",
                            "e->transaction_active = e->pending.valid || e->dirty; e->clock_due = e->fit.reference_ms + 3000U;"),
    "automatic-failed-retry": ("return promote;", "if (!promote) { e->clock_attempts = 0; e->clock_due = 0; } return promote;"),
    "old-coasting-floor": ("e->last_logical = 0;\n            }\n            e->phase_floor = previous;", "(void)0;\n            }\n            e->phase_floor = previous;"),
    "no-prior-rollback": ("e->stage = LED_SYNC_LINKED;\n        if (e->previous.valid) {",
                          "e->stage = LED_SYNC_LINKED;\n        if (false && e->previous.valid) {"),
    "lost-latest-intent": ("!confirmed_latest;", "false && !confirmed_latest;"),
    "duplicate-completion-refill": ("e->active.sequence <= e->completed_sequence",
                                    "(false && e->active.sequence <= e->completed_sequence)"),
    "late-request-stage": ("e->resume_after_sequence = e->sequence;",
                           "e->resume_after_sequence = 0;"),
    "late-fit-stage": ("else\n                led_sync_presentation_stage(e, now);", "else\n                (void)now;"),
    "skip-confirmation-wait": ("if (now < e->confirmation_due)", "if (false && now < e->confirmation_due)"),
    "resume-hint-replay": ("p->peer_epoch <= seen ||", "(false && p->peer_epoch <= seen) ||"),
    "unacknowledged-candidate-ready": (
        "e->clock_attempts++;\n        e->clock_ready = false;\n        e->t1 = now;",
        "e->clock_attempts++;\n        e->t1 = now;"),
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
               str(root / "tests/test_led_sync_on_demand.c"), "-o", str(folder / "test")]
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
    print(f"on-demand {name}: PASS {codes}", flush=True)
assert source.read_bytes() == original
(output / "results.json").write_text(json.dumps(results, indent=2))
