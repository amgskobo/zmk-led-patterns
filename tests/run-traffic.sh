#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
root="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
output="${1:?fresh output required}"
mode="${2:-candidate}"
includes="${3:-$root/src}"
if [ -e "$output" ] || [ -L "$output" ]; then echo "output must be fresh" >&2; exit 2; fi
case "$mode" in baseline64) flags=(-DLED_TRAFFIC_BASELINE64=1);; baseline56) flags=(-DLED_TRAFFIC_BASELINE56=1);; candidate) flags=();; *) exit 2;; esac
mkdir -p "$output"
cc -std=c11 -Wall -Wextra -Werror -Wconversion -Wsign-conversion -pedantic -O2 \
    "${flags[@]}" -I"$includes" "$root/tests/test_led_sync_traffic.c" -o "$output/traffic"
sha256sum "$includes/led_sync_protocol.h" "$includes/led_sync_clock.h" \
    "$root/tests/test_led_sync_traffic.c" "$0" > "$output/sources.sha256"
if [ "$mode" != baseline64 ]; then sha256sum "$includes/led_sync_wire.h" >> "$output/sources.sha256"; fi
for repeat in 1 2 3; do "$output/traffic" > "$output/repeat-$repeat.log" 2>&1; done
cmp "$output/repeat-1.log" "$output/repeat-2.log"
cmp "$output/repeat-1.log" "$output/repeat-3.log"
grep '^summary' "$output/repeat-1.log"
echo "traffic $mode: PASS"
