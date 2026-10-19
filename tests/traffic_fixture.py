#!/usr/bin/env python3
"""Admit the pinned historical output fixture, never an old algorithm copy."""
import hashlib
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parent
source = root / "fixtures/led-sync-periodic-v3.log"
metadata = json.loads((root / "fixtures/led-sync-periodic-v3.json").read_text())
raw = source.read_bytes()
if metadata["wire_version"] != 3 or metadata["trace_sha256"] != hashlib.sha256(raw).hexdigest():
    raise SystemExit("historical fixture identity mismatch")
output = Path(sys.argv[1])
if output.exists() or output.is_symlink():
    raise SystemExit("fresh output required")
output.mkdir(parents=True)
for repeat in range(1, 4):
    (output / f"repeat-{repeat}.log").write_bytes(raw)
(output / "fixture-input.json").write_text(json.dumps({
    "source": str(source), "sha256": metadata["trace_sha256"],
    "classification": "Three identical historical fixture copies for parser input, not binary reruns.",
    "provenance": metadata,
}, indent=2))
print("periodic output fixture: PASS (pinned input copies; not binary reruns)")
