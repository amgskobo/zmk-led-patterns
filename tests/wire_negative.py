#!/usr/bin/env python3
"""Compile identified exact codec faults into artifacts; never edit production."""
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
source = root / "src/led_sync_wire.h"
original = source.read_bytes()
text = original.decode()
operators = {
    "lost-indicator": ("((s->flags & 8U) << 4)", "((s->flags & 0U) << 4)"),
    "admit-state-tail": ("if (bytes[i])", "if (false && bytes[i])"),
    "truncated-time": ("led_sync_wire_put(out + 28, s->apply_at, 8);",
                       "led_sync_wire_put(out + 28, (uint32_t)s->apply_at, 8);"),
    "wrong-endian": ("value |= (uint64_t)p[i] << (8U * i);",
                     "value |= (uint64_t)p[i] << (8U * (count - 1U - i));"),
    "admit-old-version": ("p->version != LED_SYNC_WIRE_VERSION", "p->version > LED_SYNC_WIRE_VERSION"),
    "coswapped-clock-fields": (
        ("led_sync_wire_put(out + 32, p->data.clock.t2, 8);", "led_sync_wire_put(out + 40, p->data.clock.t2, 8);"),
        ("led_sync_wire_put(out + 40, p->data.clock.t3, 8);", "led_sync_wire_put(out + 32, p->data.clock.t3, 8);"),
        ("p.data.clock.t2 = led_sync_wire_get(bytes + 32, 8);", "p.data.clock.t2 = led_sync_wire_get(bytes + 40, 8);"),
        ("p.data.clock.t3 = led_sync_wire_get(bytes + 40, 8);", "p.data.clock.t3 = led_sync_wire_get(bytes + 32, 8);"),
    ),
}
results = []
for name, operator in [("unchanged", None), *operators.items()]:
    folder = output / name
    folder.mkdir()
    changed = text
    if operator:
        pairs = [operator] if isinstance(operator[0], str) else operator
        for old, new in pairs:
            assert changed.count(old) == 1, (name, changed.count(old))
            changed = changed.replace(old, new)
    (folder / source.name).write_text(changed)
    command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
               "-I" + str(folder), "-I" + str(root / "src"),
               str(root / "tests/test_led_sync_wire.c"), "-o", str(folder / "test")]
    compile_result = subprocess.run(command, capture_output=True, text=True)
    (folder / "compile.stdout.log").write_text(compile_result.stdout)
    (folder / "compile.stderr.log").write_text(compile_result.stderr)
    assert compile_result.returncode == 0, (name, compile_result.stderr)
    codes = []
    assertion_failures = []
    for repeat in range(1, 4):
        run = subprocess.run([str(folder / "test")], capture_output=True, text=True)
        (folder / f"repeat-{repeat}.stdout.log").write_text(run.stdout)
        (folder / f"repeat-{repeat}.stderr.log").write_text(run.stderr)
        codes.append(run.returncode)
        assertion_failures.append(is_c_assertion_abort(run.returncode, run.stderr))
    expected = [0, 0, 0] if operator is None else [-6, -6, -6]
    assert codes == expected, (name, codes)
    if operator is not None and not all(assertion_failures):
        raise SystemExit(f"mutation did not fail a C assertion: {name}")
    results.append({"name": name, "operator": operator, "command": command,
                    "exits": codes, "assertion_failures": assertion_failures, "source_sha256": hashlib.sha256(changed.encode()).hexdigest()})
    print(f"wire {name}: PASS {codes}", flush=True)
assert source.read_bytes() == original
(output / "results.json").write_text(json.dumps(results, indent=2))
