#!/usr/bin/env bash
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$H" "$H/test_fan_runtime.c" -lm -o "$T/test_fan_runtime"
"$T/test_fan_runtime"
