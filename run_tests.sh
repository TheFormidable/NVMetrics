#!/bin/bash
# run_tests.sh <open-gpu-kernel-modules tree>: the host tests (any system with a C compiler and bash; no GPU needed).
# The tree is NVIDIA's https://github.com/NVIDIA/open-gpu-kernel-modules at tag 620.70.06 (only its public SDK headers
# are used: src/common/sdk/nvidia/inc).
set -euo pipefail
O=${1:?open-gpu-kernel-modules tree}; H=$(cd "$(dirname "$0")" && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
I="$O/src/common/sdk/nvidia/inc"; fails=0
for t in test_metrics_json test_eco_policy; do
  cc -Wall -I"$I" -I"$H/agent" "$H/agent/$t.c" -o "$T/$t" && "$T/$t" || fails=$((fails+1))
done
"$H/agent/test_nvmetrics_setting.sh" || fails=$((fails+1))
echo "$fails test file(s) failed"; [ $fails = 0 ]
