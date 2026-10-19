#!/usr/bin/env python3
"""Check linked early/late LED PM registrations using exact defined symbols."""
import argparse
from pathlib import Path
import re

REQUIRED = frozenset(
    prefix + device
    for device in ("led_pattern_pm", "led_pattern_pm_late")
    for prefix in ("__device_", "__init___device_", "__pm_device_")
)


def validate(text):
    defined = set(re.findall(r"^[0-9a-fA-F]+\s+[A-TV-Za-z]\s+(\S+)\s*$", text, re.MULTILINE))
    missing = REQUIRED - defined
    if missing:
        raise ValueError("Missing linked PM registration symbols: " + ", ".join(sorted(missing)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("symbols", type=Path)
    args = parser.parse_args()
    validate(args.symbols.read_text(encoding="utf-8"))
    print("PASS: exact early/late LED device, init and PM registrations")
