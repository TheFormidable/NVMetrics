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
# RUSD flight recorder (0.15.7): built against NVIDIA's real cl00de.h (620.70.06 layout) and RusdReadData().
RUSD_INC=(-I "$HERE/agent" -I "$HERE/sources/NVMetrics/agent" -I "$HERE/tests" -I "$CACHE/rusd620" -I "$CACHE/ogkm/src/common/sdk/nvidia/inc")
xcrun clang -std=c11 -O1 -Wall -Wno-unused-function "${RUSD_INC[@]}" "$HERE/tests/test_rusd_flight_recorder.c" -o "$T/test_rusd_flight_recorder"
"$T/test_rusd_flight_recorder" "$T/rusd-flight-test.jsonl"
xcrun clang -std=c11 -O1 -Wall -Wno-unused-function "${RUSD_INC[@]}" "$HERE/tests/test_guard_rusd_scenarios.c" -o "$T/test_guard_rusd_scenarios"
mkdir "$T/rusd-scenarios"
"$T/test_guard_rusd_scenarios" "$T/rusd-scenarios"
# 0.15.8: the retry path with the real monotonic clock and CPU pause; it must not link any sleep.
xcrun clang -std=c11 -O1 -Wall -Wno-unused-function "${RUSD_INC[@]}" "$HERE/tests/test_rusd_retry_realclock.c" -o "$T/test_rusd_retry_realclock" -lpthread
"$T/test_rusd_retry_realclock"
if nm -u "$T/test_rusd_retry_realclock" | grep -Eq '(^|[ _])(nanosleep|usleep|sleep)($|@)'; then
  echo 'FAIL: the RUSD retry path links a sleep function' >&2; exit 1
fi
# Every line the recorder and the guard wrote must be valid JSON (JSONL for the root logs).
"$PYTHON" -B - "$T/rusd-flight-test.jsonl" "$T/rusd-scenarios" <<'PY'
import json,sys
from pathlib import Path
files=[Path(sys.argv[1])]+sorted(Path(sys.argv[2]).glob('*.jsonl'))+sorted(Path(sys.argv[2]).glob('*.stdout'))
n=sum(1 for f in files for line in f.read_text().splitlines() if json.loads(line) is not None)
print(f'PASS: {n} flight-recorder/guard JSON lines in {len(files)} files parse')
PY
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
