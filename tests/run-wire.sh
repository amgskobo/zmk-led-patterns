#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Compatibility entry; codec sensitivity has one canonical implementation.
set -euo pipefail
exec python3 "$(dirname -- "$0")/wire_negative.py" "$@"
