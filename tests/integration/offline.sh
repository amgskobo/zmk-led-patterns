#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Resolve explicitly supplied, already populated dependencies without fetching.
prepare_offline_workspace() {
    test -n "${ZMK_TEST_OFFLINE_WORKSPACE:-}" || return 1
    test -n "${ZMK_TEST_OUTPUT:-}" || {
        echo "offline integration requires a fresh ZMK_TEST_OUTPUT" >&2
        return 2
    }
    case "$ZMK_TEST_OUTPUT" in
        /*) ;;
        *) echo "ZMK_TEST_OUTPUT must be absolute" >&2; return 2 ;;
    esac
    if [ -e "$ZMK_TEST_OUTPUT" ]; then
        echo "evidence output already exists: $ZMK_TEST_OUTPUT" >&2
        return 2
    fi
    work_dir="$(cd -- "$ZMK_TEST_OFFLINE_WORKSPACE" && pwd)"
    test -d "$work_dir/.west" || {
        echo "offline workspace has no .west: $work_dir" >&2
        return 2
    }
    case "$variant" in
        upstream) core_pin=5b51501fead672c41b5cfb396f3dafe0894bf4e9 ;;
        dya) core_pin=e5c9b6915b56801193e359dd9bad4a167ce0d1b8 ;;
        *) echo "unsupported offline core: $variant" >&2; return 2 ;;
    esac
    local name path expected actual
    for name in zmk zephyr; do
        path="$work_dir/$name"
        expected="$core_pin"
        if [ "$name" = zephyr ]; then
            expected=10ba6d0cb38bc3d258775d27982f707599320085
        fi
        actual="$(git -C "$path" rev-parse HEAD)"
        if [ "$actual" != "$expected" ] || [ -n "$(git -C "$path" status --porcelain)" ]; then
            echo "$name: expected unchanged $expected, got $actual" >&2
            return 2
        fi
    done
    mkdir -p -- "$(dirname -- "$ZMK_TEST_OUTPUT")"
    mkdir -- "$ZMK_TEST_OUTPUT"
    build_root="$ZMK_TEST_OUTPUT"
    for name in zmk zephyr; do
        printf '%s %s %s\n' "$name" "$work_dir/$name" "$(git -C "$work_dir/$name" rev-parse HEAD)"
    done | tee "$build_root/dependencies.txt"
    cd -- "$work_dir"
}
