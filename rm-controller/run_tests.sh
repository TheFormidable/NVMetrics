#!/bin/bash
# All hardware-free suites. Never starts the installed app/helper or writes RM.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd -P)"
source "$HERE/find_build_python.sh"
PYTHON="$(find_build_python)"
export PYTHONPATH="$HERE/service:$HERE/client:$HERE"
CACHE="${NVMETRICS_BUILD_CACHE:-$HERE/.build-cache}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
export CLANG_MODULE_CACHE_PATH="$T/modules"
"$PYTHON" -B -m unittest discover -s "$HERE/tests" -v
for test in test_thermal_policy test_guard_header_compile test_sensor_reader test_policy_cli; do
  xcrun clang -std=c11 -Wall -Wno-unused-function -I "$HERE/agent" -I "$HERE/tests" "$HERE/tests/$test.c" -o "$T/$test"
  "$T/$test"
done
xcrun swiftc -module-cache-path "$T/modules" -swift-version 5 -parse-as-library "$HERE/app/FanSelectionPolicy.swift" "$HERE/tests/test_fan_selection.swift" -o "$T/test_selection"
"$T/test_selection"
xcrun swiftc -module-cache-path "$T/modules" -swift-version 5 -parse-as-library "$HERE/app/FanSelectionPolicy.swift" "$HERE/app/Controls.swift" "$HERE/app/FanGuardUIModel.swift" "$HERE/tests/test_ui_state.swift" -o "$T/test_ui"
"$T/test_ui"
# The menu-bar item's values stand still while a menu is open (the highlight must not jump when a reading arrives).
xcrun swiftc -module-cache-path "$T/modules" -swift-version 5 -parse-as-library "$HERE/app/MenuHold.swift" "$HERE/tests/test_menu_hold.swift" -o "$T/test_menu"
"$T/test_menu"
xcrun swiftc -module-cache-path "$T/modules" -swift-version 5 -parse-as-library "$HERE/app/Sample.swift" "$HERE/tests/test_gpu_profile.swift" -o "$T/test_gpu_profile"
"$T/test_gpu_profile"
# Run the original C/shell suites against the exact bundled base and patched
# NVIDIA headers. Copy into a temporary checkout to preserve bundled sources.
/usr/bin/ditto "$HERE/sources/NVMetrics" "$T/base"
"$HERE/sources/NVMetrics/agent/fetch_headers.sh" "$HERE/sources/nvidia-macos-driver" "$CACHE"
ln -s "$CACHE" "$T/base/build"
"$T/base/run_tests.sh" "$HERE/sources/nvidia-macos-driver"
# The agent's "policy" command (idle mode for the apps already open) is built only when the driver's boost-policy
# header comes out of the bundled driver's NVK patches: the overlay's extractor must produce it.
"$PYTHON" -B -I "$HERE/agent/extract_rm_layer.py" "$HERE/sources/nvidia-macos-driver" "$T/rm" >/dev/null
test -f "$T/rm/nvkmd/nvrm/nvrm_boost_policy_abi.h"
grep -F 'nvrm_xnu_boost_policy' "$T/rm/nvkmd/nvrm/nvrm_xnu.h" >/dev/null
for f in "$HERE/"*.sh "$HERE/"*.command; do
  [[ ! -f "$f" ]] || bash -n "$f"
done
bash -n "$HERE/packaging/postinstall"
echo 'PASS: all Python, C, Swift and shell suites (no hardware writes)'
