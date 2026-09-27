#!/usr/bin/env bash
# Copyright (c) 2026 amgskobo
# SPDX-License-Identifier: MIT
set -euo pipefail

export MSYS_NO_PATHCONV=1
repo_root="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
image="${ZMK_TEST_IMAGE:-zmkfirmware/zmk-build-arm:stable}"

docker run --rm --volume "$repo_root:/src:ro" "$image" /bin/bash -lc '
    set -euo pipefail
    warnings=(-std=c11 -Wall -Wextra -Werror -pedantic -Wconversion -Wsign-conversion -Wshadow)
    for flags in "-O2" "-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"; do
        read -r -a args <<< "$flags"
        cc "${warnings[@]}" "${args[@]}" -I/src/src -I/src/include \
            /src/tests/test_usb_pending.c -o /tmp/test_usb_pending
        ASAN_OPTIONS=detect_leaks=0 /tmp/test_usb_pending
        cc "${warnings[@]}" "${args[@]}" -I/src/include \
            /src/tests/test_battery_adc_offset.c -o /tmp/test_battery_adc_offset
        ASAN_OPTIONS=detect_leaks=0 /tmp/test_battery_adc_offset
    done
    cc "${warnings[@]}" -O0 --coverage -I/src/src -I/src/include \
        /src/tests/test_usb_pending.c -o /tmp/test_usb_pending_cov
    /tmp/test_usb_pending_cov
    cd /tmp
    coverage=$(gcov -b -c -o /tmp/test_usb_pending_cov-test_usb_pending.gcno \
        /src/tests/test_usb_pending.c)
    printf "%s\n" "$coverage"
    helper=$(printf "%s\n" "$coverage" | grep -F -A4 "/src/src/usb_pending.h")
    printf "%s\n" "$helper" | grep -Fq "Lines executed:100.00%"
    printf "%s\n" "$helper" | grep -Fq "Taken at least once:100.00%"
    cc "${warnings[@]}" -O0 --coverage -I/src/include \
        /src/tests/test_battery_adc_offset.c -o /tmp/test_battery_adc_offset_cov
    /tmp/test_battery_adc_offset_cov
    battery_coverage=$(gcov -b -c -o /tmp/test_battery_adc_offset_cov-test_battery_adc_offset.gcno \
        /src/tests/test_battery_adc_offset.c)
    printf "%s\n" "$battery_coverage"
    battery_helper=$(printf "%s\n" "$battery_coverage" | grep -F -A4 "/src/include/zmk-led-patterns/battery_adc_offset.h")
    printf "%s\n" "$battery_helper" | grep -Fq "Lines executed:100.00%"
    printf "%s\n" "$battery_helper" | grep -Fq "Taken at least once:100.00%"
    python3 /src/tests/runtime/run.py
'
