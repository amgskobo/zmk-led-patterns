# Copyright (c) 2026 amgskobo
# SPDX-License-Identifier: MIT

"""Exercise all production-driver scenarios in quick or full host mode.

Quick runs every fixture under ASan/UBSan. Full additionally executes the
optimized and measured coverage variants and retains the coverage gate below.

Each fixture names a source file, the functions whose lines and branches it
gates, and any further functions it only needs in order to compile. They are
lifted out of the source, under #line markers that keep their real file and
line numbers, into tests/runtime/<fixture>_harness.c in place of its
/* DRIVER_FUNCTIONS */ marker. The harness supplies stand-ins for Zephyr and
ZMK and drives the functions. Every line and branch inside a gated function
must run, measured from gcov's per-line report rather than its per-file total,
so a support function is compiled in without being held to it.

A name may end in ":2" to lift the second of two definitions -- the other
side of an #if -- and a name starting with "#define " lifts that macro.
"""

import pathlib
import re
import subprocess
import tempfile
import sys

import os

mode = os.environ.get("ZMK_TEST_MODE", "quick")
if mode not in ("quick", "full"):
    raise SystemExit("ZMK_TEST_MODE must be quick or full")
variants = (('optimized', ['-O2']), ('sanitized', ['-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']), ('coverage', ['-O0', '--coverage']))
if mode == "quick":
    variants = tuple(v for v in variants if v[0] == "sanitized")

root = pathlib.Path(__file__).resolve().parents[2]
source_dir = root / "src"
links = ()
# Sources with a coverage gate of their own.
elsewhere = ("led_scheduled_sync.c",)  # Actual adapter functions: native/run_scheduled.sh.
# Curves are shared; the standalone-only clock helpers are excluded below.
curves = (
    "segment_sample",
    "table_sample",
    "ramp_hold",
    "triangular_sample",
    "heartbeat_sample",
    "blink_sample",
    "fast_blink_sample",
    "triple_flash_sample",
    "sos_sample",
    "candle_sample",
    "sawtooth_sample",
    "beacon_sample",
    "slow_breathe_sample",
    "strobe_sample",
    "double_beacon_sample",
    "long_flash_sample",
    "sparkle_sample",
    "countdown_sample",
    "ripple_sample",
    "fade_blink_sample",
    "pattern_sample_at",
    "pattern_clock",
    "hold_to_wall_ms",
)
# The rest of what every role builds. The standalone fixture gates all of it;
# the split fixtures gate the parts whose code differs by role.
shared = (
    "schedule_pattern_work",
    "elapsed_since",
    "write_led",
    "set_led_brightness",
    "pattern_work_handler",
    "refresh_pattern_output",
    "apply_activity",
    "led_pattern_activity_listener",
    "masking_indicator",
    "led_pattern_get_state",
    "led_pattern_get_brightness",
    "led_pattern_applied_brightness",
    "notify_state_changed",
    "led_pattern_set_brightness",
    "set_presentation",
    "led_pattern_set_state",
    "led_pattern_set_presentation",
    "led_pattern_ready",
    "led_pattern_field_value",
    "apply_request",
    "led_pattern_request",
    "on_pattern_pressed",
    "on_pattern_released",
    "behavior_led_pattern_init",
    "led_pattern_pm_action",
    "led_pattern_pm_init",
)
host = (
    "should_show_advertising_blink",
    "refresh_connection_state",
    "ble_profile_changed_listener",
    "led_pattern_endpoint_listener",
)
split = ("split_status_sample",)
# Shared functions with role-specific code, gated again in the split roles.
role_specific = ("schedule_pattern_work", "pattern_work_handler", "masking_indicator",
                 "apply_activity", "led_pattern_get_state", "led_pattern_get_brightness",
                 "led_pattern_set_brightness", "set_presentation", "led_pattern_set_state", "apply_request",
                 "behavior_led_pattern_init", "led_pattern_pm_action")


def without(names, *excluded):
    return tuple(name for name in names if name not in excluded)


fixtures = (
    # No split, BLE and custom settings on.
    ("standalone", "behavior_led_pattern.c", curves + shared + host, ()),
    # No split and no BLE: there is no advertising to show.
    ("usb_only", "behavior_led_pattern.c", ("should_show_advertising_blink",),
     without(curves + shared + host, "should_show_advertising_blink")),
    ("central", "behavior_led_pattern.c", split + role_specific + ("refresh_connection_state",),
     without(curves + shared + host, *role_specific, "refresh_connection_state",
             "pattern_clock", "hold_to_wall_ms", "set_led_brightness")),
    ("peripheral", "behavior_led_pattern.c", split + role_specific,
     without(curves + shared, *role_specific,
             "pattern_clock", "hold_to_wall_ms", "set_led_brightness")),
    ("brightness_behavior", "behavior_led_brightness.c", (
        "step_brightness",
        "on_brightness_pressed",
        "on_brightness_released",
    ), ()),
    ("speed_behavior", "behavior_led_speed.c", (
        "step_speed",
        "on_speed_pressed",
        "on_speed_released",
    ), ()),
    ("settings", "behavior_led_pattern_custom_settings.c", (
        "led_patterns_namespace_handler",
        "active_transport",
        "read_int32",
        "read_bool",
        "led_pattern_apply_settings",
        "led_pattern_settings_event_cb",
        "usb_studio_port_closed",
        "write_led_setting",
        "save_led_setting",
        "field_setting",
        "usb_coalesce_work_handler",
        "persist_work_handler",
        "led_pattern_settings_store",
    ), ()),
    ("settings_ble_only", "behavior_led_pattern_custom_settings.c", (
        "active_transport",
        "usb_studio_port_closed",
    ), ()),
    ("settings_interleaving", "behavior_led_pattern_custom_settings.c", (), (
        "active_transport", "read_int32", "read_bool", "led_pattern_apply_settings",
        "led_pattern_settings_event_cb",
    )),
    ("settings_presentation", "behavior_led_pattern_custom_settings.c", (), (
        "active_transport", "read_int32", "read_bool", "led_pattern_apply_settings",
        "led_pattern_settings_event_cb",
    )),
    ("battery", "battery_adc_offset.c", (
        "led_battery_channel_supported",
        "led_battery_sample_fetch",
        "led_battery_channel_get",
        "led_battery_init",
    ), ()),
)

# Exercise the same assertions through the original pinned registry API too.
settings_fixture = next(row for row in fixtures if row[0] == "settings")
fixtures += (("settings_legacy", *settings_fixture[1:]),)


def definition(source, name):
    """Return (first line, last line, text) of name's definition."""
    if name.startswith("#define "):
        start = source.index(name)
        end = source.index("\n\n", start) + 1
        first = source.count("\n", 0, start) + 1
        return first, first + source.count("\n", start, end - 1), source[start:end]
    name, _, nth = name.partition(":")
    skip = int(nth or 1) - 1
    for match in re.finditer(r"\b" + name + r"\s*\(", source):
        closing = match.end()
        depth = 1
        while depth:
            if source[closing] == "(":
                depth += 1
            elif source[closing] == ")":
                depth -= 1
            closing += 1
        closing -= 1
        if not source[closing + 1:].lstrip().startswith("{"):
            continue  # a call or a prototype
        start = source.rfind("\n", 0, match.start()) + 1
        if start == match.start():
            start = source.rfind("\n", 0, start - 1) + 1  # return type on its own line
        head = source[start:match.start()]
        if not head or head[0].isspace() or head.startswith("#"):
            continue  # a call inside a body or a macro
        if skip:
            skip -= 1
            continue
        end = body_end(source, source.index("{", closing))
        first = source.count("\n", 0, start) + 1
        return first, first + source.count("\n", start, end - 1), source[start:end]
    raise RuntimeError(f"missing definition: {name}")


def body_end(source, opening):
    """Index just past the brace that closes the one at opening.

    Braces inside comments and string or character literals do not count.
    """
    depth = 0
    i = opening
    while True:
        if source.startswith("/*", i):
            i = source.index("*/", i) + 2
            continue
        if source.startswith("//", i):
            i = source.index("\n", i)
            continue
        char = source[i]
        if char in "\"'":
            i += 1
            while source[i] != char:
                i += 2 if source[i] == "\\" else 1
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1


def misses(gcov_text, ranges):
    """Count lines and branches inside ranges; list what never ran."""
    lines = executed = branches = taken = 0
    missed = []
    current = None
    for row in gcov_text.splitlines():
        if row.startswith("branch"):
            if current is None:
                continue
            branches += 1
            if re.search(r"taken [1-9]", row):
                taken += 1
            else:
                missed.append(f"  {current[1]}: {current[2].strip()}\n    {row}")
            continue
        found = re.match(r"\s*([^:]+):\s*(\d+):(.*)", row)
        if not found:
            continue
        count, number, text = found.group(1).strip(), int(found.group(2)), found.group(3)
        inside = any(first <= number <= last for first, last in ranges)
        current = (count, number, text) if inside else None
        if not inside or count == "-":
            continue
        lines += 1
        if count.startswith(("#####", "=====")):
            missed.append(f"  {number}: {text.strip()}  (never ran)")
        else:
            executed += 1
    return lines, executed, branches, taken, missed


def ungated_functions():
    """Functions defined in the sources that no fixture gates.

    A file named in `elsewhere` has a gate of its own and is skipped, so a
    function added to any other source without a test fails this run.
    """
    gated = {}
    for _fixture, file_name, names, _support in fixtures:
        gated.setdefault(file_name, set()).update(name.split(":")[0] for name in names)
    definition_pattern = re.compile(
        r"^(?:static\s+|__weak\s+)?(?:inline\s+)?(?:const\s+)?[a-z_0-9]+(?:\s+[a-z_0-9]+)?"
        r"[\s*]+([a-z_0-9]+)\([^;]*?\)\s*\{", re.M)
    missing = []
    for path in sorted(source_dir.glob("*.c")):
        if path.name in elsewhere:
            continue
        defined = set(definition_pattern.findall(path.read_text(encoding="utf-8")))
        missing += [f"{path.name}: {name}" for name in sorted(defined - gated.get(path.name, set()))]
    return missing


def render_fixture(file_name, gated, support, harness_path, marker="/* DRIVER_FUNCTIONS */"):
    """Use the same unmodified production definitions for host and kernel fixtures."""
    source_path = source_dir / file_name
    source = source_path.read_text(encoding="utf-8")
    functions = []
    ranges = []
    for name in (*support, *gated):
        first, last, text = definition(source, name)
        if name in gated:
            ranges.append((first, last))
        elif not name.startswith("#"):
            # A harness need not call every function it only compiles.
            text = "__attribute__((unused)) " + text
        functions.append(f'#line {first} "{source_path}"\n{text}')

    harness = harness_path.read_text(encoding="utf-8")
    resume = harness.count("\n", 0, harness.index(marker)) + 2
    return harness.replace(marker, "\n".join(functions) +
                           f'\n#line {resume} "{harness_path}"'), ranges



def presentation_adapter(unit, harness_path):
    """Actual adapter metadata admission; kernel calls remain fixture boundaries."""
    marker = "/* DRIVER_ADAPTER_FUNCTIONS */"
    source_path = source_dir / "led_scheduled_sync.c"
    source = source_path.read_text(encoding="utf-8")
    names = ("led_scheduled_get_desired", "effective_desired", "publish_locked",
             "led_scheduled_set_brightness", "set_state_locked", "led_scheduled_set_state",
             "led_scheduled_set_presentation", "led_scheduled_request")
    definitions = []
    for name in names:
        first, _last, text = definition(source, name)
        definitions.append(f'#line {first} "{source_path}"\n{text}')
    harness = harness_path.read_text(encoding="utf-8")
    resume = harness.count("\n", 0, harness.index(marker)) + 2
    return unit.replace(marker, "\n".join(definitions) + f'\n#line {resume} "{harness_path}"')

if sys.argv[1:]:
    if len(sys.argv) != 3 or sys.argv[1] not in ("--settings-interleaving", "--native-scheduled", "--settings-presentation"):
        raise SystemExit("usage: run.py [--native-scheduled|--settings-interleaving|--settings-presentation NEW_SOURCE_OUTPUT]")
    output = pathlib.Path(sys.argv[2])
    if output.exists() or output.is_symlink():
        raise SystemExit(f"source output already exists: {output}")
    if sys.argv[1] == "--native-scheduled":
        adapter_functions = (
            "schedule_locked", "report_stage_locked", "tx_handler", "apply_handler",
            "receive_listener", "link_changed", "peer_connection", "connected", "disconnected", "peripheral_status_listener",
            "led_scheduled_render_queue", "effective_desired", "led_scheduled_start", "led_scheduled_get_desired",
            "publish_locked", "led_scheduled_set_brightness", "set_state_locked",
            "led_scheduled_set_state", "led_scheduled_set_presentation", "led_scheduled_request",
            "led_scheduled_activity", "led_scheduled_endpoint", "led_scheduled_pm_prepare", "led_scheduled_pm",
            "led_scheduled_render_begin", "led_scheduled_render_end",
        )
        harness_path = root / "tests/native/scheduled_harness.c"
        unit, _ = render_fixture("led_scheduled_sync.c", adapter_functions, (), harness_path)
        controller_source = (source_dir / "behavior_led_pattern.c").read_text(encoding="utf-8")
        controller = []
        for name in ("schedule_pattern_work", "elapsed_since", "split_status_sample",
                     *without(curves, "pattern_clock", "hold_to_wall_ms"), "write_led",
                     "pattern_work_handler", "refresh_pattern_output", "led_pattern_pm_action"):
            first, _last, text = definition(controller_source, name)
            controller.append(f'#line {first} "{source_dir / "behavior_led_pattern.c"}"\n'
                              f'__attribute__((unused)) {text}')
        marker = "/* DRIVER_CONTROLLER_FUNCTIONS */"
        resume = unit.count("\n", 0, unit.index(marker)) + 2
        unit = unit.replace(marker, "\n".join(controller) +
                            f'\n#line {resume} "{harness_path}"')
        output.write_text(unit, encoding="utf-8")
        raise SystemExit(0)
    fixture_name = "settings_presentation" if sys.argv[1] == "--settings-presentation" else "settings_interleaving"
    _, file_name, gated, support = next(row for row in fixtures if row[0] == fixture_name)
    harness_path = root / "tests/runtime" / f"{fixture_name}_harness.c"
    unit, _ = render_fixture(file_name, gated, support, harness_path)
    if fixture_name == "settings_presentation":
        unit = presentation_adapter(unit, harness_path)
    output.write_text(unit, encoding="utf-8")
    raise SystemExit(0)

missing_functions = ungated_functions()
if missing_functions:
    raise RuntimeError("functions no fixture gates:\n  " + "\n  ".join(missing_functions))

for fixture, file_name, gated, support in fixtures:
    harness_name = "settings" if fixture == "settings_legacy" else fixture
    harness_path = root / "tests/runtime" / f"{harness_name}_harness.c"
    generated, ranges = render_fixture(file_name, gated, support, harness_path)
    if fixture == "settings_presentation":
        generated = presentation_adapter(generated, harness_path)
    with tempfile.TemporaryDirectory(prefix=f"{fixture}-runtime-") as folder:
        work = pathlib.Path(folder)
        unit = work / "test.c"
        unit.write_text(generated, encoding="utf-8")
        for variant, flags in variants:
            binary = work / variant
            api_flags = ["-DLED_SETTINGS_LEGACY_API=1"] if fixture == "settings_legacy" else []
            if fixture == "settings_interleaving":
                api_flags += ["-pthread"]
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
                            *api_flags,
                            "-I" + str(root / "include"), "-I" + str(source_dir),
                            "-I" + str(root / "tests/runtime"),
                            str(unit), *map(str, links), "-o", str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True)
        if mode == "quick":
            print(f"{fixture}: PASS (all scenarios, ASan/UBSan)")
            continue
        subprocess.run(["gcov", "-b", "-c", "-o", str(work / "coverage-test.gcno"), str(unit)],
                       cwd=work, check=True, capture_output=True, text=True)
        report = (work / f"{file_name}.gcov").read_text(encoding="utf-8")
        lines, executed, branches, taken, missed = misses(report, ranges)
        count = sum(not name.startswith("#") for name in gated)
        print(f"{fixture}: {count} functions of {file_name}: "
              f"lines {executed}/{lines}, branches {taken}/{branches}")
        if missed:
            print("\n".join(missed))
            raise RuntimeError(f"{fixture} fell below 100% coverage")
