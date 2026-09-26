#!/usr/bin/env bash
# Copyright (c) 2026 amgskobo
# SPDX-License-Identifier: MIT

set -euo pipefail

variant="${1:-upstream}"
case "$variant" in
upstream | dya) ;;
*)
    echo "unknown integration variant '$variant': expected upstream or dya" >&2
    exit 2
    ;;
esac

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/zmk-led-patterns-integration.XXXXXX")"
cleanup() {
    rm -rf "$work_dir"
}
trap cleanup EXIT HUP INT TERM

cp -R "/src/tests/integration/config/$variant" "$work_dir/config"
cd "$work_dir"

west init -l config
west update --narrow --fetch-opt=--depth=1
west zephyr-export

export ZEPHYR_BASE="$work_dir/zephyr"

cmake_args=()
if [ "$variant" = dya ]; then
    cmake_args+=("-DEXTRA_CONF_FILE=/src/tests/integration/custom-settings.conf")
fi

west build -s "$work_dir/zmk/app" -d "$work_dir/build" -b xiao_ble/nrf52840/zmk -- \
    -DZMK_EXTRA_MODULES="/src;/src/tests/integration/firmware" \
    -DSHIELD=led_patterns_test \
    "${cmake_args[@]}"

test -f "$work_dir/build/zephyr/zmk.uf2"
grep -q '^CONFIG_ZMK_LED_PATTERNS=y' "$work_dir/build/zephyr/.config"
grep -q '^CONFIG_ZMK_LED_PATTERNS_BATTERY_ADC_OFFSET=y' "$work_dir/build/zephyr/.config"
grep -q '^CONFIG_ZMK_LOW_PRIORITY_WORK_QUEUE=y' "$work_dir/build/zephyr/.config"
if grep -q '^CONFIG_ZMK_BACKLIGHT=y' "$work_dir/build/zephyr/.config"; then
    echo "ZMK backlight unexpectedly enabled as a second LED owner" >&2
    exit 1
fi

strings "$work_dir/build/zephyr/zmk.elf" >"$work_dir/build/zephyr/strings.txt"
grep -Fxq led_pattern "$work_dir/build/zephyr/strings.txt"
grep -Fxq led_brightness "$work_dir/build/zephyr/strings.txt"
grep -Fxq led_speed "$work_dir/build/zephyr/strings.txt"
# ZMK's battery reporting reads DT_CHOSEN(zmk_battery), and it must be the LED
# wrapper, linked, in the state-of-charge fetch mode the wrapper recomputes.
grep -q '^CONFIG_ZMK_BATTERY_REPORTING=y' "$work_dir/build/zephyr/.config"
grep -q '^CONFIG_ZMK_BATTERY_REPORTING_FETCH_MODE_STATE_OF_CHARGE=y' "$work_dir/build/zephyr/.config"
grep -Eq '^[[:space:]]*zmk,battery = &led_patterns_test_battery;' "$work_dir/build/zephyr/zephyr.dts"
grep -Fxq led_patterns_test_battery "$work_dir/build/zephyr/strings.txt"
# The fixture turns device power management on, so both power-off devices
# that write the LED dark before the SoC goes off are built.
grep -q '^CONFIG_PM_DEVICE=y' "$work_dir/build/zephyr/.config"
grep -Fxq led_pattern_pm "$work_dir/build/zephyr/strings.txt"
grep -Fxq led_pattern_pm_late "$work_dir/build/zephyr/strings.txt"

if [ "$variant" = upstream ]; then
    if grep -q '^CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS=y' "$work_dir/build/zephyr/.config"; then
        echo "custom settings unexpectedly enabled" >&2
        exit 1
    fi
else
    grep -q '^CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS=y' "$work_dir/build/zephyr/.config"
    grep -Fxq amgskobo__led "$work_dir/build/zephyr/strings.txt"
    for transport in usb ble; do
        for field in pattern speed brightness idle_off; do
            grep -Fxq "led.${transport}_${field}" "$work_dir/build/zephyr/strings.txt" || {
                echo "missing Studio setting: led.${transport}_${field}" >&2
                exit 1
            }
        done
    done
    grep -Fxq led.adv_blink "$work_dir/build/zephyr/strings.txt"
fi

echo "$variant ZMK firmware fixture: PASS"
