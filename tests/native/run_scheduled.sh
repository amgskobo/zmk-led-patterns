#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
script="$(realpath -- "$0")"
if [ "$#" -eq 2 ] && [ "$1" = --test-admission ]; then
    output="$2"
    case "$output" in /*) ;; *) echo 'output must be absolute' >&2; exit 2;; esac
    if [ -e "$output" ] || [ -L "$output" ]; then
        echo "evidence output already exists: $output" >&2; exit 2
    fi
    mkdir -p -- "$(dirname -- "$output")"
    mkdir -- "$output"
    mkdir -- "$output/existing" "$output/target"
    printf 'retain native evidence\n' > "$output/target/sentinel"
    ln -s -- "$output/target" "$output/directory-link"
    ln -s -- "$output/missing-target" "$output/dangling-link"
    sha256sum "$output/target/sentinel" > "$output/sentinel-before.sha256"
    failed=0
    for name in existing directory-link dangling-link; do
        set +e
        bash "$script" /workspace-must-not-be-used "$output/$name" > "$output/$name.log" 2>&1
        result=$?
        set -e
        printf '%s\n' "$result" > "$output/$name.exit"
        if [ "$result" -ne 2 ]; then failed=1; fi
        grep -q 'evidence output already exists' "$output/$name.log" || failed=1
    done
    sha256sum "$output/target/sentinel" > "$output/sentinel-after.sha256"
    cmp "$output/sentinel-before.sha256" "$output/sentinel-after.sha256"
    test ! -e "$output/missing-target"
    test "$failed" -eq 0
    echo 'LED scheduled native admission: PASS (three existing/symlink controls)'
    exit 0
fi
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 RESOLVED_WORKSPACE FRESH_OUTPUT [--share-render-tx-queue|--old-draw|--old-request|--early-sync-stage|--old-rx-disposition|--accept-noncanonical-wire|--coast-wakeup|--retry-exhausted|--no-coast-rebase|--idle-off-activity|--no-render-rollback|--skip-replay|--cancel-running-tx]" >&2
    echo "       $0 --test-admission FRESH_OUTPUT" >&2
    exit 2
fi
output="$2"
case "$output" in /*) ;; *) echo 'output must be absolute' >&2; exit 2;; esac
if [ -e "$output" ] || [ -L "$output" ]; then
    echo "evidence output already exists: $output" >&2; exit 2
fi
mode="${3:-regular}"
mutation=''
case "$mode" in
    regular) roles=(central peripheral);;
    --share-render-tx-queue) roles=(peripheral); mutation='-DLED_NATIVE_SHARE_TX_QUEUE=1';;
    --old-draw) roles=(central); mutation='-DLED_NATIVE_OLD_DRAW=1';;
    --old-request) roles=(central); mutation='-DLED_NATIVE_OLD_REQUEST=1';;
    --early-sync-stage) roles=(central peripheral); mutation='-DLED_NATIVE_EARLY_SYNC_STAGE=1';;
    --old-rx-disposition) roles=(peripheral); mutation='-DLED_NATIVE_OLD_RX_DISPOSITION=1';;
    --accept-noncanonical-wire) roles=(peripheral); mutation='-DLED_NATIVE_ACCEPT_NONCANONICAL_WIRE=1';;
    --coast-wakeup) roles=(central peripheral); mutation='-DLED_NATIVE_COAST_WAKEUP=1';;
    --retry-exhausted) roles=(central); mutation='-DLED_NATIVE_RETRY_EXHAUSTED=1';;
    --no-coast-rebase) roles=(peripheral); mutation='-DLED_NATIVE_NO_COAST_REBASE=1';;
    --idle-off-activity) roles=(central); mutation='-DLED_NATIVE_IDLE_OFF_ACTIVITY=1';;
    --no-render-rollback) roles=(central peripheral); mutation='-DLED_NATIVE_NO_RENDER_ROLLBACK=1';;
    --skip-replay) roles=(central peripheral); mutation='-DLED_NATIVE_SKIP_REPLAY=1';;
    --cancel-running-tx) roles=(central peripheral); mutation='-DLED_NATIVE_CANCEL_RUNNING_TX=1';;
    *) echo "unknown native control: $mode" >&2; exit 2;;
esac
workspace="$(cd -- "$1" && pwd -P)"
repo_root="$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd -P)"
pin=10ba6d0cb38bc3d258775d27982f707599320085
test "$(git -C "$workspace/zephyr" rev-parse HEAD)" = "$pin"
test -z "$(git -C "$workspace/zephyr" status --porcelain)"
mkdir -p -- "$(dirname -- "$output")"
mkdir -- "$output"
trap 'result=$?; printf "%s\n" "$result" > "$output/exit-code"' EXIT
export PYTHONDONTWRITEBYTECODE=1
export ZEPHYR_BASE="$workspace/zephyr"
{
    printf 'bash tests/native/run_scheduled.sh %q %q' "$workspace" "$output"
    if [ "$mode" != regular ]; then printf ' %q' "$mode"; fi
    printf '\n'
} > "$output/command.txt"
printf 'Zephyr %s %s\nbackend native_sim/native/64\nmode %s\n' "$ZEPHYR_BASE" "$pin" "$mode" > "$output/dependencies.txt"
{ gcc --version; cmake --version; } > "$output/tools.txt"
sources=(src/led_sync_clock.h src/led_sync_protocol.h src/led_sync_wire.h src/led_scheduled_sync.c
         src/led_scheduled_sync.h src/behavior_led_pattern.c tests/runtime/led_stubs.h
         tests/runtime/run.py tests/native/scheduled_harness.c tests/native/run_scheduled.sh
         tests/native/CMakeLists.txt tests/native/prj.conf include/zmk-led-patterns/led_pattern.h
         include/dt-bindings/zmk/led_pattern.h)
for source in "${sources[@]}"; do sha256sum "$repo_root/$source"; done > "$output/source-before.sha256"
python3 "$repo_root/tests/runtime/run.py" --native-scheduled "$output/scheduled.c"
mkdir -p -- "$output/native-module/zephyr"
printf 'name: led-scheduled-kernel-fixture\n' > "$output/native-module/zephyr/module.yml"
failed=0
for role in "${roles[@]}"; do
    role_output="$output/$role"
    mkdir -- "$role_output"
    flags="$mutation"
    if [ "$role" = peripheral ]; then flags="$flags -DLED_NATIVE_PERIPHERAL=1"; fi
    printf 'role=%s\nflags=%s\n' "$role" "$flags" > "$role_output/variant.txt"
    cmake -S "$repo_root/tests/native" -B "$role_output/build" -GNinja \
        -DBOARD=native_sim/native/64 -DZEPHYR_MODULES="$output/native-module" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DLED_NATIVE_SOURCE="$output/scheduled.c" -DEXTRA_CFLAGS="$flags" \
        > "$role_output/build.log" 2>&1
    cmake --build "$role_output/build" --parallel 2 >> "$role_output/build.log" 2>&1
    test "$(wc -l < "$role_output/build/zephyr_modules.txt")" -eq 1
    grep -Fq '"led-scheduled-kernel-fixture"' "$role_output/build/zephyr_modules.txt"
    grep -Fq "$output/native-module" "$role_output/build/zephyr_modules.txt"
    binary="$role_output/build/zephyr/zephyr.exe"
    sha256sum "$binary" "$output/scheduled.c" > "$role_output/sha256.txt"
    names=(blocked_tx_does_not_delay_reserved_draw codec_rejects_malformed_without_mutation
           stage_waits_for_local_render_completion
           render_failure_never_reports_synced due_commit_precedes_queued_animation
           frozen_mapping_keeps_accepted_deadline pm_darkens_before_blocked_tx_and_fences_resume
           unilateral_resume_completes_new_session
           unchanged_coast_has_no_transport_or_worker_spin
           animation_continues_after_clock_expiry_without_tx
           long_coast_refit_rebases_only_at_future_edge
           failed_transaction_goes_silent_until_real_trigger
           invalid_request_fields_preserve_state_and_work
           disconnect_retires_retry_and_preserves_redraw
           retired_tx_accepts_new_work_while_old_send_is_blocked)
    if [ "$role" = central ]; then
        names+=(concurrent_requests_preserve_independent_fields
                 disabled_idle_policy_activity_and_endpoint_are_radio_noops)
    else
        names+=(ignored_relay_never_replays_pending_desired)
    fi
    printf '%s\n' "${names[@]}" > "$role_output/required-tests.txt"
    for repeat in 1 2 3; do
        set +e
        timeout 45s "$binary" > "$role_output/repeat-$repeat.log" 2>&1
        result=$?
        set -e
        printf '%s\n' "$result" > "$role_output/repeat-$repeat.exit"
        if [ "$result" -ne 0 ]; then failed=1; fi
        grep -q 'PROJECT EXECUTION SUCCESSFUL' "$role_output/repeat-$repeat.log" || failed=1
        for name in "${names[@]}"; do
            grep -Eq "PASS - test_${name} in" "$role_output/repeat-$repeat.log" || failed=1
        done
    done
done
for source in "${sources[@]}"; do sha256sum "$repo_root/$source"; done > "$output/source-after.sha256"
cmp "$output/source-before.sha256" "$output/source-after.sha256"
test -z "$(git -C "$workspace/zephyr" status --porcelain)"
test "$failed" -eq 0
echo 'LED scheduled actual Zephyr: PASS (unchanged binaries, three executions per role)'
