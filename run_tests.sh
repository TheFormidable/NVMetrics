#!/bin/bash
# run_tests.sh <driver checkout>: the host tests (any system with a C compiler, bash, git and python3; no GPU needed).
# Fetches the same headers the build uses (agent/fetch_headers.sh, into build/) and also checks that the driver's
# resource-manager layer can still be rebuilt from its NVK patches.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); D=$(cd "${1:?driver checkout}" && pwd); W=$H/build
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
"$H/agent/fetch_headers.sh" "$D" "$W" >/dev/null
O=$W/ogkm; fails=0
INC="-I$H/agent -I$W/rusd620 -I$O/src/common/sdk/nvidia/inc"
for t in test_metrics_json test_eco_policy test_boost_auto test_health; do
  cc -Wall -Wno-unused-function $INC "$H/agent/$t.c" -o "$T/$t" && "$T/$t" || fails=$((fails+1))
done
"$H/agent/test_nvmetrics_setting.sh" || fails=$((fails+1))
python3 -I "$H/agent/extract_rm_layer.py" "$D" "$T/rm" && echo "ok    the driver's RM layer rebuilds from its patches" || fails=$((fails+1))
echo "$fails test file(s) failed"; [ $fails = 0 ]
