#!/usr/bin/env python3
# Copyright (c) 2026 amgskobo
# SPDX-License-Identifier: MIT
"""Run the canonical integration entry with failing dependency/build tools."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASH = shutil.which("bash")
if os.name == "nt" and Path("C:/Program Files/Git/bin/bash.exe").is_file():
    BASH = "C:/Program Files/Git/bin/bash.exe"


@unittest.skipUnless(BASH, "Bash is required for the integration entry")
class IntegrationLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="led-lifecycle-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        tools = self.work / "bin"
        tools.mkdir()
        west = tools / "west"
        west.write_text(
            '#!/usr/bin/env bash\n'
            'pwd > "$ZMK_WORK_CAPTURE"\n'
            'if [ "$1" = update ] && [ "$ZMK_FAIL_STAGE" = update ]; then exit 41; fi\n'
            'if [ "$1" = manifest ]; then\n'
            '  if [ "$#" -ne 3 ] || [ "$2" != --freeze ] || [ "$3" != --active-only ]; then exit 44; fi\n'
            '  if [ "$ZMK_FAIL_STAGE" = manifest ]; then exit 43; fi\n'
            '  printf "manifest: {projects: [{name: zmk, revision: mock-exact-sha}]}\\n"\n'
            '  exit 0\n'
            'fi\n'
            'if [ "$1" = build ]; then\n'
            '  shift; while [ "$#" -gt 0 ]; do\n'
            '    if [ "$1" = -d ]; then shift; printf partial > "$1/mock-build.txt"; break; fi\n'
            '    shift\n'
            '  done\n'
            '  exit 42\n'
            'fi\n', encoding="utf-8",
        )
        west.chmod(0o755)
        self.output = self.work / "new-output"
        self.capture = self.work / "workspace.txt"
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith("ZMK_TEST_")}
        self.env.update({
            "PATH": str(tools) + os.pathsep + self.env.get("PATH", ""),
            "ZMK_WORK_CAPTURE": self.capture.as_posix(),
            "ZMK_TEST_OUTPUT": self.output.as_posix(),
            "ZMK_FAIL_STAGE": "update",
        })

    def run_entry(self, stage: str = "update") -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [BASH, str(ROOT / "tests/integration/run.sh"), "upstream"],
            env=self.env | {"ZMK_FAIL_STAGE": stage}, text=True, encoding="utf-8",
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15,
        )

    def assert_temporary_workspace_removed(self) -> None:
        workspace = self.capture.read_text(encoding="utf-8").strip()
        # Let the same Bash map its path on Windows as well as Linux.
        check = subprocess.run([BASH, "-c", 'test ! -e "$1"', "check", workspace], timeout=15)
        self.assertEqual(check.returncode, 0, workspace)

    def test_dependency_failure_keeps_fresh_evidence_and_cleans_workspace(self) -> None:
        result = self.run_entry()
        self.assertEqual(result.returncode, 41, result.stderr)
        self.assertTrue(self.output.is_dir(), "Explicit results were removed or ignored")
        self.assertIn("variant=upstream role=standalone", (self.output / "invocation.txt").read_text())
        self.assert_temporary_workspace_removed()

    def test_build_failure_keeps_artifacts_and_real_exit(self) -> None:
        result = self.run_entry("build")
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertTrue(self.output.is_dir(), "Explicit build artifacts were removed or ignored")
        self.assertEqual((self.output / "mock-build.txt").read_text(), "partial")
        self.assertIn("variant=upstream role=standalone", (self.output / "fixture.txt").read_text())
        resolved = self.output / "resolved-west.yml"
        self.assertTrue(resolved.is_file(), "Actual frozen dependency graph was not retained")
        self.assertIn("revision: mock-exact-sha", resolved.read_text())
        self.assert_temporary_workspace_removed()

    def test_manifest_failure_preserves_exit_and_never_claims_frozen_identity(self) -> None:
        result = self.run_entry("manifest")
        self.assertEqual(result.returncode, 43, result.stderr)
        self.assertTrue(self.output.is_dir())
        self.assertFalse((self.output / "resolved-west.yml").exists())
        self.assertFalse((self.output / "mock-build.txt").exists())
        self.assert_temporary_workspace_removed()

    def test_existing_output_rejects_before_dependency_tools(self) -> None:
        self.output.mkdir()
        marker = self.output / "keep"
        marker.write_text("keep", encoding="utf-8")
        result = self.run_entry()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse(self.capture.exists())
        self.assertEqual(marker.read_text(), "keep")


if __name__ == "__main__":
    unittest.main(verbosity=2)
