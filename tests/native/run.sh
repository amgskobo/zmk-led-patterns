#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Compatibility entry; current scheduled assertions have one canonical runner.
set -euo pipefail
exec bash "$(dirname -- "$0")/run_scheduled.sh" "$@"
