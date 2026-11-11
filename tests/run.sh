#!/usr/bin/env bash
# Copyright (c) 2026 amgskobo
# SPDX-License-Identifier: MIT
set -euo pipefail
mode="${ZMK_TEST_MODE:-quick}"
case "$mode" in
    quick|full) ;;
    *) echo "ZMK_TEST_MODE must be quick or full" >&2; exit 2 ;;
esac
export ZMK_TEST_MODE="$mode"
repo_root="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/zmk-led-patterns-test.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
warnings=(-std=c11 -Wall -Wextra -Werror -pedantic -Wconversion -Wsign-conversion -Wshadow)
variants=("-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all")
if [ "$mode" = full ]; then
    variants=("-O2" "${variants[@]}" "-O0 --coverage")
fi
export ASAN_OPTIONS=detect_leaks=0
export UBSAN_OPTIONS=print_stacktrace=1
for test in usb_pending battery_adc_offset led_sync_clock led_sync_protocol led_sync_wire led_sync_traffic led_sync_on_demand; do
    for index in "${!variants[@]}"; do
        read -r -a flags <<< "${variants[$index]}"
        binary="$build_dir/test_${test}_${index}"
        cc "${warnings[@]}" "${flags[@]}" -I"$repo_root/src" -I"$repo_root/include" \
            "$repo_root/tests/test_${test}.c" -o "$binary"
        if [ "$test" = led_sync_traffic ]; then
            "$binary" | tee "$build_dir/traffic-$index.log"
        else
            "$binary"
        fi
    done
done
python3 "$repo_root/tests/traffic_fixture.py" "$build_dir/traffic-baseline"
mkdir "$build_dir/traffic-candidate"
cp "$build_dir/traffic-0.log" "$build_dir/traffic-candidate/repeat-1.log"
if [ "$mode" = full ]; then
    # Reuse the already compiled optimized binary for two genuine extra repeats.
    "$build_dir/test_led_sync_traffic_0" > "$build_dir/traffic-candidate/repeat-2.log"
    "$build_dir/test_led_sync_traffic_0" > "$build_dir/traffic-candidate/repeat-3.log"
else
    # Quick executes the engine once; these copies are only parser test inputs.
    cp "$build_dir/traffic-0.log" "$build_dir/traffic-candidate/repeat-2.log"
    cp "$build_dir/traffic-0.log" "$build_dir/traffic-candidate/repeat-3.log"
fi
python3 "$repo_root/tests/test_traffic_compare.py" "$build_dir/traffic-baseline" \
    "$build_dir/traffic-candidate" "$build_dir/traffic-comparator"
if [ "$mode" = full ]; then
    cd "$build_dir"
    for spec in 'usb_pending:src/usb_pending.h' 'battery_adc_offset:include/zmk-led-patterns/battery_adc_offset.h' 'led_sync_clock:src/led_sync_clock.h'; do
        test="${spec%%:*}"
        report=$(gcov -b -c -o "test_${test}_2-test_${test}.gcno" "$repo_root/tests/test_${test}.c")
        printf '%s\n' "$report"
        helper=$(printf '%s\n' "$report" | grep -F -A4 "$repo_root/${spec#*:}")
        printf '%s\n' "$helper" | grep -Fq 'Lines executed:100.00%'
        printf '%s\n' "$helper" | grep -Fq 'Taken at least once:100.00%'
    done
    gcov -b -c -o "test_led_sync_protocol_2-test_led_sync_protocol.gcno" \
        "$repo_root/tests/test_led_sync_protocol.c"
    python3 "$repo_root/tests/protocol_negative.py" "$build_dir/protocol-negative"
    python3 "$repo_root/tests/wire_negative.py" "$build_dir/wire-negative"
    python3 "$repo_root/tests/on_demand_negative.py" "$build_dir/on-demand-negative"
    python3 "$repo_root/tests/traffic_negative.py" "$build_dir/traffic-negative"
fi
python3 "$repo_root/tests/runtime/run.py"
timeout 10s bash "$repo_root/tests/native/run.sh" --test-admission "$build_dir/native-runner-guards"
python3 -m unittest discover -v -s "$repo_root/tests/integration" -p 'test_*.py'
python3 -m unittest discover -v -s "$repo_root/tests" -p 'test_integration_*.py'
python3 -m unittest discover -v -s "$repo_root/tests/native" -p test_controls.py
python3 -m unittest discover -v -s "$repo_root/tests" -p test_python_assertion_admission.py
