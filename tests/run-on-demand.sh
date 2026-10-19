#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
root="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
out="${1:?fresh output required}"
if [ -e "$out" ] || [ -L "$out" ]; then echo "fresh output required" >&2; exit 2; fi
mkdir -p "$out"
ulimit -c 0
export ASAN_OPTIONS=detect_leaks=0
for mode in optimized sanitized; do
    flags=(-O2)
    if [ "$mode" = sanitized ]; then flags=(-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all); fi
    cc -std=c11 -Wall -Wextra -Werror -Wconversion -Wsign-conversion -Wshadow -pedantic \
        "${flags[@]}" -I"$root/src" "$root/tests/test_led_sync_on_demand.c" -o "$out/$mode"
    for repeat in 1 2 3; do "$out/$mode" > "$out/$mode-$repeat.log" 2>&1; done
    cmp "$out/$mode-1.log" "$out/$mode-2.log"
    cmp "$out/$mode-1.log" "$out/$mode-3.log"
done
sha256sum "$root/src/led_sync_protocol.h" "$root/src/led_sync_clock.h" \
    "$root/src/led_sync_wire.h" "$root/tests/test_led_sync_on_demand.c" "$0" > "$out/sources.sha256"
echo "LED on-demand focused optimized/sanitized: PASS3 each"
