#!/usr/bin/env bash
# Host-only test; no GPU access.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror -I"$ROOT" "$ROOT/test_fan_preview.c" -lm -o "$T/test_fan_preview"
"$T/test_fan_preview"
