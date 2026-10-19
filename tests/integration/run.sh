#!/usr/bin/env bash
# Copyright (c) 2026 amgskobo
# SPDX-License-Identifier: MIT

set -euo pipefail
repo_root="$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)"

variant="${1:-upstream}"
case "$variant" in
upstream | dya) ;;
*)
    echo "unknown integration variant '$variant': expected upstream or dya" >&2
    exit 2
    ;;
esac

role="${ZMK_TEST_LED_ROLE:-standalone}"
case "$role" in
    standalone) ;;
    central|peripheral)
        test "$variant" = dya || {
            echo 'stock split relay is NOT APPLICABLE for this owner' >&2
            exit 2
        }
        ;;
    *) echo "unknown LED role '$role'" >&2; exit 2 ;;
esac

if [ -n "${ZMK_TEST_OFFLINE_WORKSPACE:-}" ]; then
    source "$repo_root/tests/integration/offline.sh"
    prepare_offline_workspace
else
    if [ -n "${ZMK_TEST_OUTPUT:-}" ]; then
        case "$ZMK_TEST_OUTPUT" in /*|[A-Za-z]:/*) ;; *) echo "ZMK_TEST_OUTPUT must be absolute" >&2; exit 2 ;; esac
        if [ -e "$ZMK_TEST_OUTPUT" ] || [ -L "$ZMK_TEST_OUTPUT" ]; then
            echo "evidence output already exists: $ZMK_TEST_OUTPUT" >&2; exit 2
        fi
        mkdir -p -- "$(dirname -- "$ZMK_TEST_OUTPUT")"
        mkdir -- "$ZMK_TEST_OUTPUT"
        build_root="$(CDPATH='' cd -- "$ZMK_TEST_OUTPUT" && pwd)"
        printf 'variant=%s role=%s\n' "$variant" "$role" > "$build_root/invocation.txt"
    fi
    work_dir="$(mktemp -d "${TMPDIR:-/tmp}/zmk-led-patterns-integration.XXXXXX")"
    cleanup() {
        rm -rf "$work_dir"
    }
    trap cleanup EXIT HUP INT TERM
    cp -R "$repo_root/tests/integration/config/$variant" "$work_dir/config"
    cd "$work_dir"
    west init -l config
    west update --narrow --fetch-opt=--depth=1
    build_root="${build_root:-$work_dir/build}"
    mkdir -p -- "$build_root"
    west manifest --freeze --active-only > "$build_root/resolved-west.yml.tmp"
    mv -- "$build_root/resolved-west.yml.tmp" "$build_root/resolved-west.yml"
fi
west zephyr-export

export ZEPHYR_BASE="$work_dir/zephyr"

cmake_args=()
if [ "$variant" = dya ] && [ "$role" != peripheral ]; then
    cmake_args+=("-DEXTRA_CONF_FILE=$repo_root/tests/integration/custom-settings.conf")
fi
if [ "$role" != standalone ]; then
    cmake_args+=("-DCONFIG_ZMK_SPLIT=y")
    if [ "$role" = central ]; then
        cmake_args+=("-DCONFIG_ZMK_SPLIT_ROLE_CENTRAL=y")
    else
        cmake_args+=("-DCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n")
    fi
fi
extra_modules="$repo_root;$repo_root/tests/integration/firmware"
if [ -n "${ZMK_TEST_CUSTOM_SETTINGS_MODULE:-}" ]; then
    test "$variant" = dya || {
        echo 'custom-settings override requires the DYA fixture' >&2; exit 2;
    }
    test -f "$ZMK_TEST_CUSTOM_SETTINGS_MODULE/zephyr/module.yml"
    extra_modules+=";$ZMK_TEST_CUSTOM_SETTINGS_MODULE"
fi
mkdir -p "$build_root"
printf 'variant=%s role=%s modules=%s\n' "$variant" "$role" "$extra_modules" \
    > "$build_root/fixture.txt"
if [ "$variant" = dya ] && [ "$role" != peripheral ]; then
    selected_settings="${ZMK_TEST_CUSTOM_SETTINGS_MODULE:-$work_dir/zmk-feature-custom-settings}"
    test -f "$selected_settings/include/cormoran/zmk/custom_settings.h"
    if [ -z "${ZMK_TEST_CUSTOM_SETTINGS_MODULE:-}" ]; then
        test "$(git -C "$selected_settings" rev-parse HEAD)" = \
            c6a7fef3a3be3d3ace5de9a4b0628c6418cd1f3f
        test -z "$(git -C "$selected_settings" status --porcelain)"
    fi
    {
        printf 'selected custom-settings: %s\n' "$selected_settings"
        if [ -e "$selected_settings/.git" ]; then
            git -C "$selected_settings" rev-parse HEAD
            git -C "$selected_settings" status --porcelain
        else
            echo 'NO GIT metadata; explicit working-source override'
        fi
        sha256sum "$selected_settings/src/custom_settings.c" \
            "$selected_settings/include/cormoran/zmk/custom_settings.h" \
            "$selected_settings/zephyr/module.yml"
    } > "$build_root/custom-settings-identity.txt"
fi

west build -s "$work_dir/zmk/app" -d "$build_root" -b xiao_ble/nrf52840/zmk -- \
    -DZMK_EXTRA_MODULES="$extra_modules" \
    -DSHIELD=led_patterns_test \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "${cmake_args[@]}"

bash "$repo_root/tests/integration/validate.sh" "$build_root" "$variant" "$role" \
    "$build_root/validation" "${selected_settings:-}"
