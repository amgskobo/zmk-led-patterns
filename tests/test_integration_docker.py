#!/usr/bin/env python3
# Copyright (c) 2026 amgskobo
# SPDX-License-Identifier: MIT
"""Exercise the real Docker wrapper with a recording Docker executable."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
WRAPPER = ROOT / "tests/run-integration-docker.sh"
BASH = shutil.which("bash")
if os.name == "nt":
    git_bash = Path("C:/Program Files/Git/bin/bash.exe")
    if git_bash.is_file():
        BASH = str(git_bash)


@unittest.skipUnless(BASH, "Bash is required for the canonical Docker wrapper")
class IntegrationDockerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="led-wrapper-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.bin = self.work / "bin"
        self.bin.mkdir()
        self.capture = self.work / "docker-argv"
        docker = self.bin / "docker"
        docker.write_text(
            '#!/usr/bin/env bash\n'
            'printf "%s\\0" "$@" > "$ZMK_DOCKER_CAPTURE"\n'
            'exit "$ZMK_DOCKER_EXIT"\n', encoding="utf-8",
        )
        docker.chmod(0o755)
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith("ZMK_TEST_")}
        self.env.update({
            "PATH": str(self.bin) + os.pathsep + self.env.get("PATH", ""),
            "ZMK_DOCKER_CAPTURE": self.capture.as_posix(),
            "ZMK_DOCKER_EXIT": "0",
        })

    def run_wrapper(self, *args: str, **variables: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [BASH, str(WRAPPER), *args], env=self.env | variables, text=True, encoding="utf-8",
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15,
        )

    def argv(self) -> list[str]:
        return self.capture.read_bytes().decode().rstrip("\0").split("\0")

    def option(self, args: list[str], name: str) -> list[str]:
        return [args[index + 1] for index, value in enumerate(args[:-1]) if value == name]

    def assert_rejected(self, result: subprocess.CompletedProcess[str]) -> None:
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse(self.capture.exists(), "Docker ran before input admission")

    def test_role_is_forwarded(self) -> None:
        for role in ("standalone", "central", "peripheral"):
            with self.subTest(role=role):
                result = self.run_wrapper("dya", ZMK_TEST_LED_ROLE=role)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("ZMK_TEST_LED_ROLE=" + role, self.option(self.argv(), "--env"))

    def test_default_variant_and_read_only_source(self) -> None:
        result = self.run_wrapper()
        self.assertEqual(result.returncode, 0, result.stderr)
        args = self.argv()
        self.assertEqual(args[-3:], ["/bin/bash", "/src/tests/integration/run.sh", "upstream"])
        self.assertEqual(args[:2], ["run", "--rm"])
        self.assertTrue(any(mount.endswith(":/src:ro") for mount in self.option(args, "--volume")))

    def test_explicit_current_settings_is_mounted_read_only(self) -> None:
        module = self.work / "current settings"
        (module / "zephyr").mkdir(parents=True)
        (module / "zephyr/module.yml").write_text("name: settings\n", encoding="utf-8")
        result = self.run_wrapper("dya", ZMK_TEST_CUSTOM_SETTINGS_MODULE=module.as_posix())
        self.assertEqual(result.returncode, 0, result.stderr)
        args = self.argv()
        self.assertIn("ZMK_TEST_CUSTOM_SETTINGS_MODULE=/custom-settings", self.option(args, "--env"))
        mounts = self.option(args, "--volume")
        self.assertTrue(any(mount.endswith("current settings:/custom-settings:ro") for mount in mounts), mounts)
        self.assertNotIn("ZMK_TEST_CUSTOM_SETTINGS_MODULE=" + module.as_posix(), self.option(args, "--env"))

    def test_offline_workspace_results_and_network(self) -> None:
        workspace = self.work / "resolved workspace"
        (workspace / ".west").mkdir(parents=True)
        output = self.work / "new results"
        result = self.run_wrapper(
            "dya", ZMK_TEST_OFFLINE_WORKSPACE=workspace.as_posix(),
            ZMK_TEST_OUTPUT=output.as_posix(), ZMK_TEST_LED_ROLE="central",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        args = self.argv()
        self.assertEqual(self.option(args, "--network"), ["none"])
        self.assertIn("ZMK_TEST_OFFLINE_WORKSPACE=/workspace", self.option(args, "--env"))
        self.assertIn("ZMK_TEST_OUTPUT=/results/new results", self.option(args, "--env"))
        mounts = self.option(args, "--volume")
        self.assertTrue(any(mount.endswith("resolved workspace:/workspace:ro") for mount in mounts), mounts)
        self.assertTrue(any(mount.endswith(":/results:rw") for mount in mounts), mounts)
        self.assertFalse(output.exists(), "Wrapper must leave atomic output admission to the owner runner")

    def test_online_results_are_mapped_without_offline_network_policy(self) -> None:
        output = self.work / "online output"
        result = self.run_wrapper("dya", ZMK_TEST_OUTPUT=output.as_posix())
        self.assertEqual(result.returncode, 0, result.stderr)
        args = self.argv()
        self.assertIn("ZMK_TEST_OUTPUT=/results/online output", self.option(args, "--env"))
        self.assertEqual(self.option(args, "--network"), [])
        self.assertTrue(any(mount.endswith(":/results:rw") for mount in self.option(args, "--volume")))
        self.assertFalse(output.exists())

    def test_existing_output_is_preserved(self) -> None:
        workspace = self.work / "workspace"
        (workspace / ".west").mkdir(parents=True)
        output = self.work / "existing"
        output.mkdir()
        marker = output / "keep"
        marker.write_text("existing evidence", encoding="utf-8")
        self.assert_rejected(self.run_wrapper("dya", ZMK_TEST_OFFLINE_WORKSPACE=workspace.as_posix(), ZMK_TEST_OUTPUT=output.as_posix()))
        self.assertEqual(marker.read_text(encoding="utf-8"), "existing evidence")

    def test_symlink_output_is_rejected(self) -> None:
        workspace = self.work / "workspace"
        (workspace / ".west").mkdir(parents=True)
        for target in (self.work / "missing", workspace):
            with self.subTest(target=target):
                output = self.work / ("link-" + target.name)
                try:
                    output.symlink_to(target, target_is_directory=True)
                except OSError as error:
                    self.skipTest("Host cannot create symlinks: " + str(error))
                self.assert_rejected(self.run_wrapper("dya", ZMK_TEST_OFFLINE_WORKSPACE=workspace.as_posix(), ZMK_TEST_OUTPUT=output.as_posix()))

    def test_offline_requires_fresh_output_and_resolved_workspace(self) -> None:
        workspace = self.work / "workspace"
        workspace.mkdir()
        self.assert_rejected(self.run_wrapper("dya", ZMK_TEST_OFFLINE_WORKSPACE=workspace.as_posix()))
        self.assert_rejected(self.run_wrapper("dya", ZMK_TEST_OFFLINE_WORKSPACE=workspace.as_posix(), ZMK_TEST_OUTPUT=(self.work / "new").as_posix()))

    def test_invalid_variant_role_and_extra_arguments(self) -> None:
        for args, variables in (
            (("unknown",), {}),
            (("dya", "extra"), {}),
            (("dya",), {"ZMK_TEST_LED_ROLE": "sideways"}),
            (("upstream",), {"ZMK_TEST_LED_ROLE": "central"}),
            (("upstream",), {"ZMK_TEST_CUSTOM_SETTINGS_MODULE": self.work.as_posix()}),
            (("dya",), {"ZMK_TEST_CUSTOM_SETTINGS_MODULE": (self.work / "missing").as_posix()}),
        ):
            with self.subTest(args=args, variables=variables):
                self.assert_rejected(self.run_wrapper(*args, **variables))

    def test_docker_exit_is_propagated_without_cleanup(self) -> None:
        marker = self.work / "keep"
        marker.write_text("keep", encoding="utf-8")
        result = self.run_wrapper("dya", ZMK_DOCKER_EXIT="37", ZMK_TEST_IMAGE="test-sdk@sha256:example")
        self.assertEqual(result.returncode, 37, result.stderr)
        self.assertIn("test-sdk@sha256:example", self.argv())
        self.assertEqual(marker.read_text(encoding="utf-8"), "keep")


if __name__ == "__main__":
    unittest.main(verbosity=2)
