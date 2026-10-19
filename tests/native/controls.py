#!/usr/bin/env python3
# Copyright (c) 2026 amgskobo
# SPDX-License-Identifier: MIT
"""Canonical scheduled-native sensitivity selection and retained-output validation.

Validation reads prior compiler/run evidence; it does not execute a binary or
establish physical BLE/PWM/power acceptance. Unit parser fixtures are synthetic.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import subprocess
import sys

OWNER = Path(__file__).resolve().parents[2]
RUNNER_COMMAND = ("bash", str(OWNER / "tests/native/run_scheduled.sh"))
KERNEL_PIN = "10ba6d0cb38bc3d258775d27982f707599320085"
BACKEND = "native_sim/native/64"
SOURCE_PATHS = (
    "src/led_sync_clock.h", "src/led_sync_protocol.h", "src/led_sync_wire.h",
    "src/led_scheduled_sync.c", "src/led_scheduled_sync.h", "src/behavior_led_pattern.c",
    "tests/runtime/led_stubs.h", "tests/runtime/run.py", "tests/native/scheduled_harness.c",
    "tests/native/run_scheduled.sh", "tests/native/CMakeLists.txt", "tests/native/prj.conf",
    "include/zmk-led-patterns/led_pattern.h", "include/dt-bindings/zmk/led_pattern.h",
)
COMMON_CASES = (
    "blocked_tx_does_not_delay_reserved_draw", "codec_rejects_malformed_without_mutation",
    "stage_waits_for_local_render_completion", "render_failure_never_reports_synced",
    "due_commit_precedes_queued_animation", "frozen_mapping_keeps_accepted_deadline",
    "pm_darkens_before_blocked_tx_and_fences_resume", "unilateral_resume_completes_new_session",
    "unchanged_coast_has_no_transport_or_worker_spin",
    "animation_continues_after_clock_expiry_without_tx",
    "long_coast_refit_rebases_only_at_future_edge", "failed_transaction_goes_silent_until_real_trigger",
    "invalid_request_fields_preserve_state_and_work",
    "disconnect_retires_retry_and_preserves_redraw",
    "retired_tx_accepts_new_work_while_old_send_is_blocked",
)
ROLE_CASES = {
    "central": COMMON_CASES + ("concurrent_requests_preserve_independent_fields",
                              "disabled_idle_policy_activity_and_endpoint_are_radio_noops"),
    "peripheral": COMMON_CASES + ("ignored_relay_never_replays_pending_desired",),
}


@dataclass(frozen=True)
class Control:
    name: str
    roles: tuple[str, ...]
    flag: str
    target: str


CONTROLS = (
    Control("share-render-tx-queue", ("peripheral",), "LED_NATIVE_SHARE_TX_QUEUE",
            "blocked_tx_does_not_delay_reserved_draw"),
    Control("old-draw", ("central",), "LED_NATIVE_OLD_DRAW", "due_commit_precedes_queued_animation"),
    Control("old-request", ("central",), "LED_NATIVE_OLD_REQUEST",
            "concurrent_requests_preserve_independent_fields"),
    Control("early-sync-stage", ("central", "peripheral"), "LED_NATIVE_EARLY_SYNC_STAGE",
            "stage_waits_for_local_render_completion"),
    Control("old-rx-disposition", ("peripheral",), "LED_NATIVE_OLD_RX_DISPOSITION",
            "ignored_relay_never_replays_pending_desired"),
    Control("accept-noncanonical-wire", ("peripheral",), "LED_NATIVE_ACCEPT_NONCANONICAL_WIRE",
            "codec_rejects_malformed_without_mutation"),
    Control("coast-wakeup", ("central", "peripheral"), "LED_NATIVE_COAST_WAKEUP",
            "unchanged_coast_has_no_transport_or_worker_spin"),
    Control("retry-exhausted", ("central",), "LED_NATIVE_RETRY_EXHAUSTED",
            "failed_transaction_goes_silent_until_real_trigger"),
    Control("no-coast-rebase", ("peripheral",), "LED_NATIVE_NO_COAST_REBASE",
            "long_coast_refit_rebases_only_at_future_edge"),
    Control("idle-off-activity", ("central",), "LED_NATIVE_IDLE_OFF_ACTIVITY",
            "disabled_idle_policy_activity_and_endpoint_are_radio_noops"),
    Control("no-render-rollback", ("central", "peripheral"), "LED_NATIVE_NO_RENDER_ROLLBACK",
            "render_failure_never_reports_synced"),
    Control("skip-replay", ("central", "peripheral"), "LED_NATIVE_SKIP_REPLAY",
            "render_failure_never_reports_synced"),
    Control("cancel-running-tx", ("central", "peripheral"), "LED_NATIVE_CANCEL_RUNNING_TX",
            "retired_tx_accepts_new_work_while_old_send_is_blocked"),
)


class InvalidEvidence(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise InvalidEvidence(message)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def data(path: Path) -> bytes:
    require(path.is_file() and not path.is_symlink(), f"missing/nonregular evidence: {path}")
    return path.read_bytes()


def text(path: Path) -> str:
    return data(path).decode("utf-8")


def source_hashes(owner: Path) -> dict[str, str]:
    return {name: digest(data(owner / name)) for name in SOURCE_PATHS}


def hash_rows(path: Path) -> list[tuple[str, str]]:
    rows = []
    for line in text(path).splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        require(match is not None, f"malformed SHA256 row: {path}")
        rows.append((match[1], match[2]))
    require(bool(rows), f"empty SHA256 evidence: {path}")
    return rows


def source_guard(path: Path, current: dict[str, str]) -> tuple[dict[str, str], str]:
    result: dict[str, str] = {}
    prefixes = set()
    for sha, recorded in hash_rows(path):
        require(".." not in PurePosixPath(recorded).parts, f"escaped source guard: {path}")
        candidates = [name for name in SOURCE_PATHS if recorded.endswith("/" + name)]
        require(len(candidates) == 1, f"unexpected source guard path: {recorded}")
        name = candidates[0]
        prefix = recorded[:-len("/" + name)]
        require(PurePosixPath(prefix).is_absolute(), f"nonabsolute source prefix: {recorded}")
        require(name not in result, f"duplicate source guard: {name}")
        result[name] = sha
        prefixes.add(prefix)
    require(set(result) == set(SOURCE_PATHS), f"incomplete {len(SOURCE_PATHS)}-input source guard: {path}")
    require(len(prefixes) == 1, f"mixed owner prefixes: {path}")
    require(result == current, f"source guard does not match current owner bytes: {path}")
    return result, next(iter(prefixes))


def expected_flags(control: Control, role: str) -> list[str]:
    return [f"-D{control.flag}=1"] + (["-DLED_NATIVE_PERIPHERAL=1"] if role == "peripheral" else [])


def validate_log(path: Path, role: str, target: str) -> dict:
    raw = text(path)
    require("PROJECT EXECUTION FAILED" in raw and "PROJECT EXECUTION SUCCESSFUL" not in raw,
            f"missing control execution failure: {path}")
    starts = list(re.finditer(r"^START - test_(\w+)\s*$", raw, re.M))
    require(Counter(m[1] for m in starts) == Counter(ROLE_CASES[role]),
            f"missing/duplicate native test starts: {path}")
    outcomes = []
    target_block = None
    for index, match in enumerate(starts):
        end = starts[index + 1].start() if index + 1 < len(starts) else len(raw)
        block = raw[match.end():end]
        found = re.findall(r"^\s*(PASS|FAIL) - test_(\w+) in ([^\n]+)$", block, re.M)
        require(len(found) == 1 and found[0][1] == match[1],
                f"incomplete/mismatched test outcome: {path}: {match[1]}")
        outcomes.append({"name": match[1], "outcome": found[0][0], "duration": found[0][2]})
        if match[1] == target:
            require(found[0][0] == "FAIL", f"intended control target passed: {path}: {target}")
            require("Assertion failed at " in block,
                    f"intended target has no assertion failure: {path}: {target}")
            target_block = block
    require(target_block is not None, f"intended target absent: {path}")
    return {
        "raw_log": str(path), "sha256": digest(data(path)), "ordered_outcomes": outcomes,
        "target_assertion_block": target_block.strip(),
    }


def validate_control(root: Path, control: Control, current: dict[str, str]) -> dict:
    folder = root / control.name
    require(text(root / f"{control.name}.outer.exit").strip() == "1",
            f"unexpected outer exit: {control.name}")
    require(text(folder / "exit-code").strip() == "1", f"native entry exit: {control.name}")
    before, prefix = source_guard(folder / "source-before.sha256", current)
    after, after_prefix = source_guard(folder / "source-after.sha256", current)
    require(before == after and prefix == after_prefix, f"source drift: {control.name}")
    dependencies = text(folder / "dependencies.txt").splitlines()
    require(len(dependencies) == 3, f"dependency descriptor shape: {control.name}")
    kernel = re.fullmatch(r"Zephyr (/[^\n]+) " + KERNEL_PIN, dependencies[0])
    require(kernel is not None and dependencies[1] == "backend " + BACKEND and
            dependencies[2] == "mode --" + control.name, f"dependency/mode mismatch: {control.name}")
    command = shlex.split(text(folder / "command.txt"))
    require(len(command) == 5 and command[:2] == ["bash", "tests/native/run_scheduled.sh"] and
            command[4] == "--" + control.name, f"command/mode mismatch: {control.name}")
    recorded_root = command[3]
    require(PurePosixPath(recorded_root).is_absolute() and
            ".." not in PurePosixPath(recorded_root).parts,
            f"invalid recorded output path: {control.name}")
    require(kernel[1] == command[2].rstrip("/") + "/zephyr",
            f"workspace/kernel mismatch: {control.name}")
    require({r for r in ("central", "peripheral") if (folder / r).exists()} == set(control.roles),
            f"missing/unexpected role: {control.name}")
    emitted = digest(data(folder / "scheduled.c"))
    roles = []
    for role in control.roles:
        output = folder / role
        variant = text(output / "variant.txt").splitlines()
        require(len(variant) == 2 and variant[0] == "role=" + role and
                variant[1].startswith("flags=") and
                Counter(shlex.split(variant[1][6:])) == Counter(expected_flags(control, role)),
                f"role/control flag mismatch: {control.name}/{role}")
        require(Counter(text(output / "required-tests.txt").splitlines()) == Counter(ROLE_CASES[role]),
                f"native scenario selection mismatch: {control.name}/{role}")
        binary_path = output / "build/zephyr/zephyr.exe"
        binary = data(binary_path)
        require(binary.startswith(b"\x7fELF"), f"missing ELF compiler output: {binary_path}")
        binary_sha = digest(binary)
        hashes = hash_rows(output / "sha256.txt")
        require(len(hashes) == 2 and len({p for _, p in hashes}) == 2,
                f"binary/source hash descriptor shape: {control.name}/{role}")
        require(dict((p, h) for h, p in hashes) == {
            recorded_root + "/" + role + "/build/zephyr/zephyr.exe": binary_sha,
            recorded_root + "/scheduled.c": emitted,
        }, f"recorded binary/source hash mismatch: {control.name}/{role}")
        build = text(output / "build.log")
        # The guarded set-e runner records hashes/repeats only after compilation.
        # Require that output chain, without depending on build progress wording.
        require(not re.search(r"(^FAILED:|^.*fatal error:)", build, re.M),
                f"native build did not complete: {control.name}/{role}")
        commands_path = output / "build/compile_commands.json"
        commands = json.loads(text(commands_path))
        source_commands = [item for item in commands if item.get("file") == recorded_root + "/scheduled.c"]
        require(len(source_commands) == 1, f"compiled generated source missing: {control.name}/{role}")
        item = source_commands[0]
        tokens = item.get("arguments") or shlex.split(item["command"])
        native_flags = [flag for flag in tokens if flag.startswith("-DLED_NATIVE_")]
        require(Counter(native_flags) == Counter(expected_flags(control, role)),
                f"compiled control/role flags mismatch: {control.name}/{role}")
        require({p.name for p in output.glob("repeat-*.log")} ==
                {f"repeat-{n}.log" for n in (1, 2, 3)} and
                {p.name for p in output.glob("repeat-*.exit")} ==
                {f"repeat-{n}.exit" for n in (1, 2, 3)},
                f"missing/extra repeat: {control.name}/{role}")
        runs = []
        for n in (1, 2, 3):
            require(text(output / f"repeat-{n}.exit").strip() == "1",
                    f"repeat exit is not assertion failure (timeout/setup): {control.name}/{role}/{n}")
            row = validate_log(output / f"repeat-{n}.log", role, control.target)
            row["repeat"] = n
            row["exit"] = 1
            runs.append(row)
        roles.append({
            "role": role, "binary_sha256": binary_sha,
            "compile_commands_sha256": digest(data(commands_path)), "runs": runs,
        })
    return {"mode": control.name, "expected_target": control.target,
            "source_prefix": prefix, "source_hashes": before,
            "workspace": command[2], "emitted_source_sha256": emitted, "roles": roles}


def validate_suite(existing: Path, owner: Path = OWNER) -> dict:
    current = source_hashes(owner)
    rows = [validate_control(existing, control, current) for control in CONTROLS]
    require(len({row["source_prefix"] for row in rows}) == 1, "cross-mode source prefix mismatch")
    require(len({row["workspace"] for row in rows}) == 1, "cross-mode workspace mismatch")
    require(len({row["emitted_source_sha256"] for row in rows}) == 1,
            "cross-mode generated source mismatch")
    require(source_hashes(owner) == current, "owner inputs changed during validation")
    return {"status": "PASS", "control_modes": len(rows),
            "role_variants": sum(len(row["roles"]) for row in rows),
            "expected_assertion_failure_runs": sum(len(role["runs"]) for row in rows for role in row["roles"]),
            "native_input_hashes": current, "controls": rows}


def fresh_output(path: Path) -> None:
    require(path.is_absolute(), "output must be absolute")
    require(not os.path.lexists(path), f"evidence output already exists: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.mkdir()


def write_report(output: Path, result: dict, command: list[str]) -> None:
    result["command"] = command
    result["action"] = command[2]
    result["validator_sha256"] = digest(Path(__file__).read_bytes())
    result["limits"] = "Compiler/kernel control evidence does not establish physical radio/PWM/power acceptance."
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


def validate_entry(existing: Path, output: Path, owner: Path = OWNER) -> int:
    fresh_output(output)
    command = ["python3", "tests/native/controls.py", "validate", str(existing), str(output)]
    try:
        result = validate_suite(existing, owner)
        result["existing_run"] = str(existing)
        result["execution"] = "Retained control output validation; no binaries executed"
        code = 0
    except (InvalidEvidence, OSError, ValueError, KeyError, TypeError) as error:
        result = {"status": "FAIL", "existing_run": str(existing), "error": str(error)}
        code = 1
    write_report(output, result, command)
    return code


def run_entry(workspace: Path, output: Path, owner: Path = OWNER) -> int:
    fresh_output(output)
    entry_command = ["python3", "tests/native/controls.py", "run", str(workspace), str(output)]
    if not workspace.is_absolute() or not workspace.is_dir():
        write_report(output, {"status": "FAIL", "error": "workspace must be an existing absolute directory"}, entry_command)
        return 2
    try:
        initial = source_hashes(owner)
    except (InvalidEvidence, OSError) as error:
        write_report(output, {"status": "FAIL", "phase": "source-preflight",
                              "error": str(error)}, entry_command)
        return 2
    for control in CONTROLS:
        command = [*RUNNER_COMMAND, str(workspace), str(output / control.name), "--" + control.name]
        try:
            with (output / f"{control.name}.outer.log").open("xb") as log:
                process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=False)
        except OSError as error:
            write_report(output, {"status": "FAIL", "phase": "launch", "mode": control.name,
                                  "runner_started": False, "runner_exit": None,
                                  "runner_command": command, "error": str(error)}, entry_command)
            return 2
        code = process.returncode
        (output / f"{control.name}.outer.exit").write_text(str(code) + "\n", encoding="utf-8")
        if code != 1:
            write_report(output, {"status": "FAIL", "mode": control.name, "runner_exit": code,
                                  "error": "control runner must exit1 after intended assertions"}, entry_command)
            return (code if code > 0 else 128 - code) if code != 0 else 1
    try:
        require(source_hashes(owner) == initial, "owner inputs changed during control run")
        result = validate_suite(output, owner)
        result["execution"] = "Canonical native control runner, each compiled binary three times"
        code = 0
    except (InvalidEvidence, OSError, ValueError, KeyError, TypeError) as error:
        result = {"status": "FAIL", "error": str(error)}
        code = 1
    write_report(output, result, entry_command)
    return code


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    for name, first in (("run", "workspace"), ("validate", "existing_run")):
        command = sub.add_parser(name)
        command.add_argument(first, type=Path)
        command.add_argument("fresh_output", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.action == "run":
            return run_entry(args.workspace, args.fresh_output)
        return validate_entry(args.existing_run, args.fresh_output)
    except (InvalidEvidence, OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
