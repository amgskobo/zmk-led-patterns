#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Canonical CLI admission with a recording compiler, not real C fault evidence."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

OWNER = Path(__file__).resolve().parents[1]
MUTATION_ENTRIES = ("wire_negative.py", "on_demand_negative.py", "traffic_negative.py")
ENTRIES = MUTATION_ENTRIES + ("test_traffic_compare.py", "traffic_baseline.py")
GUARD_MESSAGE = "Assertions must remain enabled"


@unittest.skipUnless(os.name == "posix", "Canonical recording-compiler fixture uses the POSIX SDK")
class PythonAssertionAdmission(unittest.TestCase):
    def run_entry(self, folder, script, optimized, nonassertion_abort=False):
        tools = folder / "tools"
        tools.mkdir()
        record = folder / "compiler.jsonl"
        compiler = tools / "cc"
        compiler.write_text(
            "#!" + sys.executable + "\n"
            "import json, os, sys\n"
            "from pathlib import Path\n"
            "with Path(os.environ['LED_RECORD_COMPILER']).open('a') as log:\n"
            "    log.write(json.dumps(sys.argv[1:]) + '\\n')\n"
            "out = Path(sys.argv[sys.argv.index('-o') + 1])\n"
            "body = '#!/bin/sh\\nexit 0\\n'\n"
            "if out.parent.name != 'unchanged':\n"
            "    body = os.environ.get('LED_RECORD_MUTANT_PROGRAM', body)\n"
            "out.write_text(body)\n"
            "out.chmod(0o755)\n",
            encoding="utf-8",
        )
        compiler.chmod(0o755)
        output = folder / "output"
        arguments = [str(output)]
        if script == "test_traffic_compare.py":
            arguments = [str(folder / "missing-baseline"), str(folder / "missing-candidate"), str(output)]
        elif script == "traffic_baseline.py":
            arguments = [str(folder / "missing-archive.json"), str(output)]
        env = dict(os.environ)
        env.pop("PYTHONOPTIMIZE", None)
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        env["PATH"] = str(tools) + os.pathsep + env.get("PATH", "")
        env["LED_RECORD_COMPILER"] = str(record)
        env.pop("LED_RECORD_MUTANT_PROGRAM", None)
        if nonassertion_abort:
            env["LED_RECORD_MUTANT_PROGRAM"] = (
                "#!" + sys.executable + "\n"
                "import os, resource, signal, sys\n"
                "resource.setrlimit(resource.RLIMIT_CORE, (0, 0))\n"
                "sys.stderr.write('controlled nonassertion abort\\n')\n"
                "sys.stderr.flush()\n"
                "os.kill(os.getpid(), signal.SIGABRT)\n"
            )
        flags = []
        if optimized == "environment":
            env["PYTHONOPTIMIZE"] = "1"
        elif optimized == "flag":
            flags = ["-O"]
        command = [sys.executable, *flags, str(OWNER / "tests" / script), *arguments]
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=20)
        calls = [json.loads(line) for line in record.read_text().splitlines()] if record.exists() else []
        detail = (f"command={command!r}\nexit={result.returncode} compiler_calls={len(calls)}\n"
                  f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        recorded_stderr = {str(path.relative_to(output)): path.read_text()
                           for path in output.rglob("*.stderr.log")} if output.exists() else {}
        detail += f"\nrecorded mutant stderr={recorded_stderr!r}"
        return result, output, calls, detail

    def check_optimized(self, how):
        with tempfile.TemporaryDirectory(prefix="led-python-admission-") as temporary:
            root = Path(temporary)
            for script in ENTRIES:
                with self.subTest(script=script, optimization=how):
                    folder = root / script
                    folder.mkdir()
                    result, output, calls, detail = self.run_entry(folder, script, how)
                    self.assertNotEqual(result.returncode, 0, detail)
                    self.assertIn(GUARD_MESSAGE, result.stderr, detail)
                    self.assertEqual(calls, [], detail)
                    self.assertFalse(os.path.lexists(output), detail)

    def test_environment_optimization_rejects_before_work(self):
        self.check_optimized("environment")

    def test_explicit_optimization_rejects_before_work(self):
        self.check_optimized("flag")

    def test_unoptimized_mutation_entries_reject_surviving_binaries(self):
        with tempfile.TemporaryDirectory(prefix="led-python-survivor-") as temporary:
            root = Path(temporary)
            for script in MUTATION_ENTRIES:
                with self.subTest(script=script):
                    folder = root / script
                    folder.mkdir()
                    result, output, calls, detail = self.run_entry(folder, script, None)
                    self.assertNotEqual(result.returncode, 0, detail)
                    self.assertIn("AssertionError", result.stderr, detail)
                    self.assertGreaterEqual(len(calls), 2, detail)
                    self.assertTrue(output.is_dir(), detail)


    def test_nonassertion_abort_does_not_kill_mutants(self):
        with tempfile.TemporaryDirectory(prefix="led-assertion-result-") as temporary:
            root = Path(temporary)
            for script in MUTATION_ENTRIES + ("protocol_negative.py",):
                with self.subTest(script=script):
                    folder = root / script
                    folder.mkdir()
                    result, output, calls, detail = self.run_entry(folder, script, None, True)
                    self.assertNotEqual(result.returncode, 0, detail)
                    self.assertGreaterEqual(len(calls), 2, detail)
                    errors = "".join(path.read_text() for path in output.rglob("*.stderr.log"))
                    self.assertIn("controlled nonassertion abort", errors, detail)


    def test_c_assertion_diagnostic_requires_the_abort_signal(self):
        import signal
        from mutation_result import is_c_assertion_abort

        diagnostic = "program: fixture.c:12: check: Assertion `value == expected' failed.\n"
        self.assertTrue(is_c_assertion_abort(-signal.SIGABRT, diagnostic))
        for code in (0, 1, -signal.SIGSEGV):
            with self.subTest(returncode=code):
                self.assertFalse(is_c_assertion_abort(code, diagnostic))
        self.assertFalse(is_c_assertion_abort(-signal.SIGABRT, "controlled nonassertion abort\n"))
        self.assertFalse(is_c_assertion_abort(-signal.SIGABRT, "Assertion mentioned without a C diagnostic\n"))


if __name__ == "__main__":
    unittest.main()
