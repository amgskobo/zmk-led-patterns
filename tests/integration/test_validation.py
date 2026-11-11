"""Check the real validator's source admission before ELF inspection.

Synthetic artifacts and a stopping strings command test this path boundary;
they do not count as firmware validation.
"""

import os
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class ValidationSourceAdmission(unittest.TestCase):
    def run_source_case(self, source):
        with tempfile.TemporaryDirectory() as temp:
            output = pathlib.Path(temp)
            build = output / "build"
            zephyr = build / "zephyr"
            zephyr.mkdir(parents=True)
            for name in ("zmk.elf", "zmk.uf2", "zephyr.dts"):
                (zephyr / name).write_text("synthetic source-admission fixture\n")
            (zephyr / ".config").write_text(
                "CONFIG_ZMK_LED_PATTERNS=y\n"
                "CONFIG_ZMK_LED_PATTERNS_BATTERY_ADC_OFFSET=y\n"
                "CONFIG_ZMK_LOW_PRIORITY_WORK_QUEUE=y\n"
            )
            (build / "compile_commands.json").write_text(str(source) + "\n")
            tools = output / "tools"
            tools.mkdir()
            strings = tools / "strings"
            strings.write_text(
                '#!/bin/sh\nprintf "called\\n" > "$VALIDATION_STRINGS_MARKER"\nexit 47\n'
            )
            strings.chmod(0o755)
            marker = output / "strings-called"
            environment = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"],
                               VALIDATION_STRINGS_MARKER=str(marker))
            result = subprocess.run(
                ["bash", str(ROOT / "tests/integration/validate.sh"), str(build),
                 "upstream", "standalone", str(output / "validation")],
                env=environment, capture_output=True, text=True, check=False,
            )
            return result, marker.exists()

    def test_actual_checkout_source_reaches_elf_inspection(self):
        result, inspected = self.run_source_case(ROOT / "src/behavior_led_pattern.c")
        self.assertEqual(result.returncode, 47, result.stdout + result.stderr)
        self.assertTrue(inspected)

    def test_unrelated_checkout_source_rejects_before_elf_inspection(self):
        result, inspected = self.run_source_case(
            pathlib.Path("/synthetic-other-owner/src/behavior_led_pattern.c")
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertFalse(inspected)


if __name__ == "__main__":
    unittest.main()
