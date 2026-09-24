"""Host fault-injection test of the actual LED battery sensor callbacks."""

import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source_path = root / "src/battery_adc_offset.c"
source = source_path.read_text(encoding="utf-8")
names = ("led_battery_sample_fetch", "led_battery_channel_get", "led_battery_init")
functions = []
for name in names:
    match = re.search(r"static\s+int\s+" + name + r"\([^;{}]+?\)\s*\{", source)
    if match is None:
        raise RuntimeError(f"sensor callback missing: {name}")
    end = source.index("\n}", match.end()) + 2
    line = source.count("\n", 0, match.start()) + 1
    functions.append(f'#line {line} "{source_path}"\n' + source[match.start():end])

harness = (root / "tests/battery_driver/harness.c").read_text(encoding="utf-8")
with tempfile.TemporaryDirectory(prefix="led-battery-") as folder:
    unit = pathlib.Path(folder) / "test.c"
    unit.write_text(harness.replace("/* DRIVER_FUNCTIONS */",
                                    "\n".join(functions) + f'\n#line 1 "{unit}"\n'),
                    encoding="utf-8")
    for variant, flags in (
        ("optimized", ["-O2"]),
        ("sanitized", ["-O1", "-g", "-fno-omit-frame-pointer",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]),
        ("coverage", ["-O0", "--coverage"]),
    ):
        binary = pathlib.Path(folder) / variant
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(root / "include"), str(unit), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    result = subprocess.run(
        ["gcov", "-b", "-c", "-o", str(pathlib.Path(folder) / "coverage-test.gcno"),
         str(unit)], cwd=folder, check=True, capture_output=True, text=True)
    print(result.stdout, end="")
    source_report = result.stdout.split(f"File '{source_path}'", 1)[1]
    source_report = source_report.split("Creating ", 1)[0]
    if ("Lines executed:100.00%" not in source_report or
            "Taken at least once:100.00%" not in source_report):
        raise RuntimeError("LED battery sensor callback coverage fell below 100%")
