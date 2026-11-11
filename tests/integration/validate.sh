#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# All build-result assertions, also usable on an already retained binary.
set -euo pipefail
repo_root="$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd -P)"
test "$#" -ge 4 && test "$#" -le 5 || {
    echo 'usage: validate.sh BUILD_ROOT upstream|dya ROLE FRESH_OUTPUT [SETTINGS_MODULE]' >&2
    exit 2
}
build_root="$1"
variant="$2"
role="$3"
output="$4"
selected_settings="${5:-}"
case "$variant" in upstream|dya) ;; *) exit 2;; esac
case "$role" in standalone|central|peripheral) ;; *) exit 2;; esac
case "$output" in /*) ;; *) echo 'output must be absolute' >&2; exit 2;; esac
if [ -e "$output" ] || [ -L "$output" ]; then
    echo "evidence output already exists: $output" >&2; exit 2
fi
mkdir -p -- "$(dirname -- "$output")"
mkdir -- "$output"
printf 'build=%s variant=%s role=%s settings=%s\n' "$build_root" "$variant" "$role" \
    "$selected_settings" > "$output/command.txt"
sha256sum "$build_root/zephyr/zmk.elf" "$build_root/zephyr/zmk.uf2" \
    "$build_root/zephyr/.config" "$build_root/zephyr/zephyr.dts" > "$output/input-sha256.txt"
test -f "$build_root/zephyr/zmk.uf2"
grep -q '^CONFIG_ZMK_LED_PATTERNS=y' "$build_root/zephyr/.config"
grep -q '^CONFIG_ZMK_LED_PATTERNS_BATTERY_ADC_OFFSET=y' "$build_root/zephyr/.config"
grep -q '^CONFIG_ZMK_LOW_PRIORITY_WORK_QUEUE=y' "$build_root/zephyr/.config"
grep -Fq "$repo_root/src/behavior_led_pattern.c" "$build_root/compile_commands.json"
if grep -q '^CONFIG_ZMK_BACKLIGHT=y' "$build_root/zephyr/.config"; then
    echo 'ZMK backlight unexpectedly enabled as a second LED owner' >&2; exit 1
fi
strings "$build_root/zephyr/zmk.elf" > "$output/strings.txt"
grep -Fxq led_pattern "$output/strings.txt"
grep -Fxq led_brightness "$output/strings.txt"
grep -Fxq led_speed "$output/strings.txt"
grep -q '^CONFIG_ZMK_BATTERY_REPORTING=y' "$build_root/zephyr/.config"
grep -q '^CONFIG_ZMK_BATTERY_REPORTING_FETCH_MODE_STATE_OF_CHARGE=y' "$build_root/zephyr/.config"
grep -Eq '^[[:space:]]*zmk,battery = &led_patterns_test_battery;' "$build_root/zephyr/zephyr.dts"
grep -Fxq led_patterns_test_battery "$output/strings.txt"
grep -q '^CONFIG_PM_DEVICE=y' "$build_root/zephyr/.config"
nm --defined-only "$build_root/zephyr/zmk.elf" > "$output/defined-symbols.txt"
python3 "$(dirname -- "$0")/registration.py" "$output/defined-symbols.txt"
if [ "$variant" = upstream ] || [ "$role" = peripheral ]; then
    if grep -q '^CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS=y' "$build_root/zephyr/.config"; then
        echo 'custom settings unexpectedly enabled for this core/role' >&2; exit 1
    fi
else
    grep -q '^CONFIG_ZMK_LED_PATTERNS_CUSTOM_SETTINGS=y' "$build_root/zephyr/.config"
    grep -Fxq amgskobo__led "$output/strings.txt"
    for transport in usb ble; do
        for field in pattern speed brightness idle_off; do
            grep -Fxq "led.${transport}_${field}" "$output/strings.txt" || {
                echo "missing Studio setting: led.${transport}_${field}" >&2; exit 1;
            }
        done
    done
    grep -Fxq led.adv_blink "$output/strings.txt"
    test -n "$selected_settings"
    selected_settings="$(CDPATH='' cd -- "$selected_settings" && pwd -P)"
    grep -Fq "$selected_settings/src/custom_settings.c" "$build_root/compile_commands.json"
fi
if [ "$role" != standalone ]; then
    grep -q '^CONFIG_ZMK_SPLIT=y' "$build_root/zephyr/.config"
    grep -q '^CONFIG_ZMK_SPLIT_RELAY_EVENT=y' "$build_root/zephyr/.config"
    grep -Fq "$repo_root/src/led_scheduled_sync.c" "$build_root/compile_commands.json"
    for symbol in led_scheduled_start led_scheduled_render_begin led_scheduled_render_end \
        led_scheduled_pm_prepare led_scheduled_request zmk_event_zmk_led_scheduled_wire \
        zmk_listener_led_scheduled_sync zmk_listener_zmk_led_scheduled_wire_relay_handle; do
        grep -Eq "[[:space:]]${symbol}$" "$output/defined-symbols.txt"
    done
    # Receipt-phase implementation was archived and replaced by scheduled
    # clock/session/admission/complete-state confirmation assertions.
    if grep -Eq '[[:space:]](mirror_work_handler|led_pattern_ack_listener|led_pattern_mirror_listener)$' \
        "$output/defined-symbols.txt"; then
        echo 'retired receipt-phase LED implementation unexpectedly linked' >&2; exit 1
    fi
    if [ "$role" = central ]; then
        grep -q '^CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y' "$build_root/zephyr/.config"
        grep -Eq '[[:space:]]zmk_split_central_relay_event_listener_zmk_led_scheduled_wire_cb$' \
            "$output/defined-symbols.txt"
    else
        if grep -q '^CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y' "$build_root/zephyr/.config"; then
            echo 'peripheral unexpectedly built as central' >&2; exit 1
        fi
        grep -Eq '[[:space:]]zmk_split_peripheral_relay_event_listener_zmk_led_scheduled_wire_cb$' \
            "$output/defined-symbols.txt"
        grep -Eq '[[:space:]]zmk_listener_led_scheduled_link$' "$output/defined-symbols.txt"
    fi
else
    if grep -Fq "$repo_root/src/led_scheduled_sync.c" "$build_root/compile_commands.json"; then
        echo 'split scheduled adapter unexpectedly compiled for standalone' >&2; exit 1
    fi
fi
echo "$variant ZMK firmware fixture ($role): PASS"
