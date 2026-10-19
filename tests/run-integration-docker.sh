#!/usr/bin/env bash
# Copyright (c) 2026 amgskobo
# SPDX-License-Identifier: MIT

set -euo pipefail
export MSYS_NO_PATHCONV=1

repo_root="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
image="${ZMK_TEST_IMAGE:-zmkfirmware/zmk-build-arm:stable}"
variant="${1:-upstream}"
role="${ZMK_TEST_LED_ROLE:-standalone}"
fail() { echo "$*" >&2; exit 2; }

[ "$#" -le 1 ] || fail "usage: $0 [upstream|dya]"
case "$variant" in upstream|dya) ;; *) fail "unsupported core: $variant" ;; esac
case "$role" in standalone|central|peripheral) ;; *) fail "unsupported role: $role" ;; esac
[ "$variant" = dya ] || [ "$role" = standalone ] || fail "stock split relay is not supported"

docker_args=(run --rm --env "ZMK_TEST_LED_ROLE=$role" --volume "$repo_root:/src:ro")
if [ -n "${ZMK_TEST_CUSTOM_SETTINGS_MODULE:-}" ]; then
    [ "$variant" = dya ] || fail "a custom-settings override requires DYA"
    [ -f "$ZMK_TEST_CUSTOM_SETTINGS_MODULE/zephyr/module.yml" ] || fail "custom-settings override has no zephyr/module.yml"
    settings_root="$(CDPATH='' cd -- "$ZMK_TEST_CUSTOM_SETTINGS_MODULE" && pwd)"
    docker_args+=(--volume "$settings_root:/custom-settings:ro"
        --env ZMK_TEST_CUSTOM_SETTINGS_MODULE=/custom-settings)
fi

if [ -n "${ZMK_TEST_OFFLINE_WORKSPACE:-}" ]; then
    [ -d "$ZMK_TEST_OFFLINE_WORKSPACE/.west" ] || fail "offline workspace has no .west"
    [ -n "${ZMK_TEST_OUTPUT:-}" ] || fail "offline integration requires a fresh ZMK_TEST_OUTPUT"
fi

if [ -n "${ZMK_TEST_OUTPUT:-}" ]; then
    case "$ZMK_TEST_OUTPUT" in /*|[A-Za-z]:/*) ;; *) fail "ZMK_TEST_OUTPUT must be absolute" ;; esac
    [ ! -e "$ZMK_TEST_OUTPUT" ] && [ ! -L "$ZMK_TEST_OUTPUT" ] || fail "evidence output already exists: $ZMK_TEST_OUTPUT"
    output_name="$(basename -- "$ZMK_TEST_OUTPUT")"
    mkdir -p -- "$(dirname -- "$ZMK_TEST_OUTPUT")"
    output_parent="$(CDPATH='' cd -- "$(dirname -- "$ZMK_TEST_OUTPUT")" && pwd)"
    docker_args+=(--volume "$output_parent:/results:rw"
        --env "ZMK_TEST_OUTPUT=/results/$output_name")
fi

if [ -n "${ZMK_TEST_OFFLINE_WORKSPACE:-}" ]; then
    workspace_root="$(CDPATH='' cd -- "$ZMK_TEST_OFFLINE_WORKSPACE" && pwd)"
    docker_args+=(--network none --volume "$workspace_root:/workspace:ro"
        --env ZMK_TEST_OFFLINE_WORKSPACE=/workspace)
fi

# The owner runner atomically admits the fresh output and verifies dependency pins.
# --rm removes only this transient container; host results and dependencies persist.
exec docker "${docker_args[@]}" "$image" /bin/bash /src/tests/integration/run.sh "$variant"
