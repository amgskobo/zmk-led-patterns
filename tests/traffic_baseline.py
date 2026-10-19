#!/usr/bin/env python3
"""Materialize byte-exact audited prior headers for the owner baseline entry.

No algorithm is patched or translated. The current fixture drives these exact
retained version3 sources, independently of the current version4 candidate.
"""
import hashlib
import json
import pathlib
import sys

if not __debug__:
    raise RuntimeError("Assertions must remain enabled")

archive, output = map(pathlib.Path, sys.argv[1:])
if output.exists() or output.is_symlink():
    raise SystemExit("fresh baseline output required")
root = pathlib.Path(__file__).resolve().parents[1]
expected = {
    "src/led_sync_protocol.h": "3bd90cbd4e8bd2d70b4c189daebc50d616997d87b29d588a89a58c41972a7213",
    "src/led_sync_wire.h": "8386d62a24e4aaaa561451fc48638ae00af4803076b52781f93f80be5e752ea5",
    "src/led_sync_clock.h": "59b2f28f39498d73a5a4ecc6b31fa51d39337afd536ebea875a5b55fd55bb9ce",
}
selected = {row["path"]: row for row in json.loads(archive.read_text())["sources"]}
data = {}
for name, pin in expected.items():
    content = selected[name]["utf8"].encode() if name in selected else (root / name).read_bytes()
    assert hashlib.sha256(content).hexdigest() == pin, name
    data[pathlib.Path(name).name] = content
output.mkdir(parents=True)
for name, content in data.items():
    (output / name).write_bytes(content)
(output / "identities.json").write_text(json.dumps({"archive": str(archive),
    "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(), "headers": expected}, indent=2))
print("exact periodic version3 baseline headers: PASS")
