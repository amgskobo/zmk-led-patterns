# Copyright (c) 2026 amgskobo
# SPDX-License-Identifier: MIT
"""Synthetic parser/dispatcher fixtures only; fake ELF/ztest bytes are never executed."""
from contextlib import redirect_stderr
import io
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import controls as ctl


def put(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value if isinstance(value, bytes) else value.encode())


def synthetic_log(role, failed, assertion=True):
    lines = ["SYNTHETIC PARSER FIXTURE: no kernel or hardware execution"]
    for name in sorted(ctl.ROLE_CASES[role]):
        lines.append("START - test_" + name)
        if name == failed:
            if assertion:
                lines.append("    Assertion failed at synthetic.c:1: helper: parser fixture")
            lines.append(" FAIL - test_" + name + " in 0.000 seconds")
        else:
            lines.append(" PASS - test_" + name + " in 0.000 seconds")
    return "\n".join(lines + ["PROJECT EXECUTION FAILED"]) + "\n"


def make_control(root, control, hashes, workspace="/synthetic/workspace"):
    folder = root / control.name
    folder.mkdir()
    recorded = folder.resolve().as_posix()
    guard = "".join(sha + "  /synthetic/owner/" + name + "\n" for name, sha in hashes.items())
    for name in ("source-before.sha256", "source-after.sha256"):
        put(folder / name, guard)
    put(folder / "scheduled.c", "/* SYNTHETIC parser data; no production algorithm. */\n")
    emitted = ctl.digest((folder / "scheduled.c").read_bytes())
    put(folder / "exit-code", "1\n")
    put(root / (control.name + ".outer.exit"), "1\n")
    put(root / (control.name + ".outer.log"), "SYNTHETIC recording runner\n")
    put(folder / "dependencies.txt", "Zephyr " + workspace + "/zephyr " + ctl.KERNEL_PIN +
        "\nbackend " + ctl.BACKEND + "\nmode --" + control.name + "\n")
    put(folder / "command.txt", shlex.join(
        ["bash", "tests/native/run_scheduled.sh", workspace, recorded, "--" + control.name]) + "\n")
    for role in control.roles:
        output = folder / role
        flags = ctl.expected_flags(control, role)
        put(output / "variant.txt", "role=" + role + "\nflags=" + " ".join(flags) + "\n")
        put(output / "required-tests.txt", "\n".join(ctl.ROLE_CASES[role]) + "\n")
        binary = b"\x7fELF SYNTHETIC parser fixture, not executable: " + (control.name + role).encode()
        put(output / "build/zephyr/zephyr.exe", binary)
        put(output / "build.log",
            "SYNTHETIC successful compiler output; progress formatting is not an interface\n")
        put(output / "sha256.txt", ctl.digest(binary) + "  " + recorded + "/" + role +
            "/build/zephyr/zephyr.exe\n" + emitted + "  " + recorded + "/scheduled.c\n")
        put(output / "build/compile_commands.json", json.dumps([{
            "file": recorded + "/scheduled.c",
            "arguments": ["synthetic-parser-compiler", *flags, "-c", recorded + "/scheduled.c"],
        }]))
        for n in (1, 2, 3):
            put(output / f"repeat-{n}.exit", "1\n")
            put(output / f"repeat-{n}.log", synthetic_log(role, control.target))
    return folder


class NativeControlParserTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="native-control-parser-")
        self.addCleanup(temp.cleanup)
        self.base = Path(temp.name)
        self.existing = self.base / "SYNTHETIC-existing"
        self.existing.mkdir()
        self.hashes = ctl.source_hashes(ctl.OWNER)
        self.control = ctl.CONTROLS[0]
        make_control(self.existing, self.control, self.hashes)
        self.folder = self.existing / self.control.name
        self.role = self.control.roles[0]
        self.role_output = self.folder / self.role
        self.report_number = 0

    def report(self):
        self.report_number += 1
        return self.base / ("report-" + str(self.report_number))

    def assert_rejected(self):
        # A local corruption must fail here, not at a later missing mode.
        with self.assertRaises(ctl.InvalidEvidence):
            ctl.validate_control(self.existing, self.control, self.hashes)

    def complete_suite(self):
        for control in ctl.CONTROLS[1:]:
            make_control(self.existing, control, self.hashes)

    def test_complete_synthetic_parser_fixture(self):
        self.complete_suite()
        # Progress text is not the successful compiler-output contract.
        put(self.role_output / "build.log", "SYNTHETIC successful build with neutral progress text\n")
        output = self.report()
        self.assertEqual(ctl.validate_entry(self.existing, output), 0)
        record = json.loads((output / "results.json").read_text())
        self.assertEqual((record["control_modes"], record["role_variants"],
                          record["expected_assertion_failure_runs"]), (13, 18, 54))
        self.assertEqual(len(record["native_input_hashes"]), 14)
        self.assertEqual(record["action"], "validate")
        self.assertIn("no binaries executed", record["execution"])

    def test_wrong_target_failure_rejected(self):
        wrong = next(n for n in ctl.ROLE_CASES[self.role] if n != self.control.target)
        put(self.role_output / "repeat-1.log", synthetic_log(self.role, wrong))
        self.assert_rejected()
        # Keep one complete-suite CLI failure/report assertion.
        self.complete_suite()
        output = self.report()
        self.assertEqual(ctl.validate_entry(self.existing, output), 1)
        record = json.loads((output / "results.json").read_text())
        self.assertEqual(record["status"], "FAIL")
        self.assertTrue(record["error"])

    def test_unrelated_assertion_cannot_satisfy_target(self):
        raw = synthetic_log(self.role, self.control.target, assertion=False)
        put(self.role_output / "repeat-1.log",
            "Assertion failed at unrelated.c:1: other: unrelated\n" + raw)
        self.assert_rejected()

    def test_timeout_exit_is_not_assertion_failure(self):
        put(self.role_output / "repeat-1.exit", "124\n")
        self.assert_rejected()

    def test_missing_binary_rejected(self):
        (self.role_output / "build/zephyr/zephyr.exe").unlink()
        self.assert_rejected()

    def test_missing_binary_hash_rejected(self):
        (self.role_output / "sha256.txt").unlink()
        self.assert_rejected()

    def test_binary_hash_mismatch_rejected(self):
        put(self.role_output / "build/zephyr/zephyr.exe", b"\x7fELF changed synthetic bytes")
        self.assert_rejected()

    def test_generated_source_hash_mismatch_rejected(self):
        put(self.folder / "scheduled.c", "changed synthetic parser source\n")
        self.assert_rejected()

    def test_missing_repeat_rejected(self):
        (self.role_output / "repeat-3.log").unlink()
        self.assert_rejected()

    def test_transitive_pattern_binding_is_required_and_bound(self):
        # A synthetic owner models a real transitive include without mutating
        # the checkout. The source guard must bind its bytes, not just its parent.
        binding = "include/dt-bindings/zmk/led_pattern.h"
        owner = self.base / "SYNTHETIC-owner"
        for name in (*ctl.SOURCE_PATHS, binding):
            put(owner / name, "SYNTHETIC owner input: " + name + "\n")
        hashes = ctl.source_hashes(owner)
        existing = self.base / "SYNTHETIC-transitive-existing"
        existing.mkdir()
        folder = make_control(existing, self.control, hashes)
        ctl.validate_control(existing, self.control, hashes)
        original = (owner / binding).read_bytes()
        put(owner / binding, original + b"changed pattern constant\n")
        with self.assertRaises(ctl.InvalidEvidence):
            ctl.validate_control(existing, self.control, ctl.source_hashes(owner))
        self.assertIn(binding, hashes)
        put(owner / binding, original)
        before = (folder / "source-before.sha256").read_text()
        after = (folder / "source-after.sha256").read_text()
        for guard in ("source-before.sha256", "source-after.sha256"):
            with self.subTest(guard=guard, fault="omitted transitive include"):
                rows = (folder / guard).read_text().splitlines(keepends=True)
                put(folder / guard, "".join(row for row in rows if not row.rstrip().endswith(binding)))
                with self.assertRaises(ctl.InvalidEvidence):
                    ctl.validate_control(existing, self.control, hashes)
                put(folder / "source-before.sha256", before)
                put(folder / "source-after.sha256", after)
        put(folder / "source-after.sha256", after.replace(hashes[binding], "0" * 64))
        with self.assertRaises(ctl.InvalidEvidence):
            ctl.validate_control(existing, self.control, hashes)
        (owner / binding).unlink()
        with self.assertRaises(ctl.InvalidEvidence):
            ctl.source_hashes(owner)

    def test_source_guards_fail_closed(self):
        original = (self.folder / "source-before.sha256").read_text()
        lines = original.splitlines(keepends=True)
        changes = {
            "empty": ("", ""),
            "equal-subset": ("".join(lines[:-1]), "".join(lines[:-1])),
            "duplicate": (original + lines[0], original + lines[0]),
            "unknown": (original.replace(ctl.SOURCE_PATHS[0], "unknown/input.h"), original),
            "escape": (original.replace("/synthetic/owner/", "/synthetic/../owner/"), original),
            "current-mismatch": (original.replace(lines[0][:64], "0" * 64), original),
            "after-drift": (original, original.replace(lines[0][:64], "0" * 64)),
        }
        for label, (before, after) in changes.items():
            with self.subTest(label=label):
                put(self.folder / "source-before.sha256", before)
                put(self.folder / "source-after.sha256", after)
                self.assert_rejected()
        put(self.folder / "source-before.sha256", original)
        (self.folder / "source-after.sha256").unlink()
        self.assert_rejected()

    def test_mode_role_backend_flags_are_bound(self):
        paths = {"dependencies": self.folder / "dependencies.txt",
                 "variant": self.role_output / "variant.txt"}
        originals = {k: p.read_text() for k, p in paths.items()}
        changes = (
            ("mode", "dependencies", originals["dependencies"].replace(
                "--" + self.control.name, "--skip-replay")),
            ("pin", "dependencies", originals["dependencies"].replace(ctl.KERNEL_PIN, "0" * 40)),
            ("backend", "dependencies", originals["dependencies"].replace(ctl.BACKEND, "other")),
            ("role", "variant", originals["variant"].replace("role=" + self.role, "role=central")),
            ("flag", "variant", originals["variant"].replace(self.control.flag, "LED_NATIVE_SKIP_REPLAY")),
        )
        for label, key, value in changes:
            with self.subTest(label=label):
                put(paths[key], value)
                self.assert_rejected()
                put(paths[key], originals[key])

    def test_build_and_compile_metadata_fail_closed(self):
        build = self.role_output / "build.log"
        original = build.read_text()
        put(build, original + "FAILED: synthetic compiler failure\n")
        self.assert_rejected()
        put(build, original)
        commands = self.role_output / "build/compile_commands.json"
        original_commands = commands.read_text()
        put(commands, "[]")
        self.assert_rejected()
        changed = json.loads(original_commands)
        changed[0]["arguments"] = ["synthetic-parser-compiler", "-DLED_NATIVE_SKIP_REPLAY=1"]
        put(commands, json.dumps(changed))
        self.assert_rejected()

    def test_required_scenario_cannot_be_dropped(self):
        put(self.role_output / "required-tests.txt", self.control.target + "\n")
        self.assert_rejected()

    def test_fresh_report_rejects_existing_and_symlinks(self):
        retained = self.base / "retained"
        retained.mkdir()
        put(retained / "sentinel", "retain")
        link, dangling = self.base / "link", self.base / "dangling"
        link.symlink_to(retained, target_is_directory=True)
        dangling.symlink_to(self.base / "missing", target_is_directory=True)
        for output in (retained, link, dangling):
            with self.subTest(output=output.name), redirect_stderr(io.StringIO()):
                self.assertEqual(ctl.main(["validate", str(self.existing), str(output)]), 2)
        self.assertEqual((retained / "sentinel").read_text(), "retain")
        self.assertFalse((self.base / "missing").exists())

    def test_run_dispatches_exact_modes_outputs_argv(self):
        output, workspace = self.base / "SYNTHETIC-dispatch", self.base / "workspace"
        workspace.mkdir()
        calls = []

        def recording_runner(command, stdout, stderr, check):
            # A controlled recording runner; creates parser data, not kernel evidence.
            control = ctl.CONTROLS[len(calls)]
            self.assertEqual(command, ["bash", str(ctl.OWNER / "tests/native/run_scheduled.sh"), str(workspace),
                                      str(output / control.name), "--" + control.name])
            self.assertEqual(stderr, subprocess.STDOUT)
            self.assertFalse(check)
            calls.append(command)
            make_control(output, control, self.hashes, workspace.resolve().as_posix())
            stdout.write(b"SYNTHETIC recording runner only\n")
            return subprocess.CompletedProcess(command, 1)

        with mock.patch.object(ctl.subprocess, "run", side_effect=recording_runner):
            self.assertEqual(ctl.run_entry(workspace, output), 0)
        self.assertEqual(len(calls), 13)
        record = json.loads((output / "results.json").read_text())
        self.assertEqual(record["status"], "PASS")
        self.assertNotIn("not a new execution", record["limits"])
        self.assertEqual(record["action"], "run")

    def test_run_records_and_propagates_real_outer_exit(self):
        output, workspace = self.base / "real-exit-bookkeeping", self.base / "workspace"
        workspace.mkdir()
        # A real Python process tests only exit bookkeeping, never a kernel run.
        command = (sys.executable, "-c", "import sys; print('bookkeeping only'); sys.exit(41)")
        with mock.patch.object(ctl, "RUNNER_COMMAND", command):
            self.assertEqual(ctl.run_entry(workspace, output), 41)
        self.assertEqual((output / (ctl.CONTROLS[0].name + ".outer.exit")).read_text().strip(), "41")
        record = json.loads((output / "results.json").read_text())
        self.assertEqual((record["status"], record["runner_exit"]), ("FAIL", 41))
        self.assertFalse((output / ctl.CONTROLS[1].name).exists())

    def test_run_reports_setup_and_launch_failures(self):
        workspace = self.base / "workspace"
        workspace.mkdir()
        for name, error in (("missing-runner", FileNotFoundError(2, "runner missing")),
                            ("denied-runner", PermissionError(13, "runner denied"))):
            with self.subTest(name=name):
                output = self.base / name
                with mock.patch.object(ctl.subprocess, "run", side_effect=error):
                    self.assertEqual(ctl.run_entry(workspace, output), 2)
                record = json.loads((output / "results.json").read_text())
                self.assertEqual((record["status"], record["phase"]), ("FAIL", "launch"))
                self.assertFalse(record["runner_started"])
                self.assertIsNone(record["runner_exit"])
                self.assertEqual(record["runner_command"],
                                 [*ctl.RUNNER_COMMAND, str(workspace),
                                  str(output / ctl.CONTROLS[0].name),
                                  "--" + ctl.CONTROLS[0].name])
                self.assertFalse((output / (ctl.CONTROLS[0].name + ".outer.exit")).exists())
        output = self.base / "missing-source"
        with mock.patch.object(ctl.subprocess, "run") as invoke:
            self.assertEqual(ctl.run_entry(workspace, output, owner=self.base / "missing-owner"), 2)
            invoke.assert_not_called()
        record = json.loads((output / "results.json").read_text())
        self.assertEqual((record["status"], record["phase"]), ("FAIL", "source-preflight"))

    def test_run_rejects_existing_before_invoking_runner(self):
        workspace = self.base / "workspace"
        workspace.mkdir()
        with mock.patch.object(ctl.subprocess, "run") as invoke:
            with self.assertRaises(ctl.InvalidEvidence):
                ctl.run_entry(workspace, self.existing)
            invoke.assert_not_called()


if __name__ == "__main__":
    unittest.main()
