#!/bin/bash
# Independent, read-only diagnostic build; never installs or runs as root.
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$PWD"
D="${1:-$HOME/gpu-caps-phase2B}"
CACHE="${2:-$HOME/gpu-caps-phase2-cache}"
H="$ROOT/rm-controller/agent"
B="$ROOT/rm-controller/sources/NVMetrics/agent"
[[ $(uname -s) == Darwin && $(uname -m) == x86_64 ]] || { echo 'STOP: Intel macOS required'; exit 2; }
test -f "$D/kexts/NVRM/nvrm_gpu_capabilities_abi.h"
mkdir -p "$ROOT/out"
W=$(mktemp -d "$ROOT/out/run.XXXXXX")
if [[ ! -f "$CACHE/ogkm/.nvmetrics-patched" || ! -f "$CACHE/rusd620/class/cl00de.h" ]]; then
    CACHE="$ROOT/out/header-cache"
    bash "$B/fetch_headers.sh" "$D" "$CACHE"
fi
python3 -I "$H/extract_rm_layer.py" "$D" "$W/rm"
O="$CACHE/ogkm"
N="$W/rm/nvkmd/nvrm"
test -f "$N/nvrm_gpu_capabilities_abi.h"
cmp "$N/nvrm_gpu_capabilities_abi.h" "$D/kexts/NVRM/nvrm_gpu_capabilities_abi.h"
xcrun clang -arch x86_64 -mmacosx-version-min=14.0 -O1 -Wall -Wno-unused-function \
  -o "$W/nvmetrics-agent-caps" "$H/nvmetrics_agent.c" "$N/nvRmApi.c" "$N/nvrm_xnu.c" \
  -I"$H" -I"$B" -I"$D/kexts/NVRM" -I"$CACHE/rusd620" -I"$N" \
  -I"$O/src/nvidia/arch/nvalloc/common/inc" -I"$O/src/nvidia/arch/nvalloc/unix/include" \
  -I"$O/src/common/sdk/nvidia/inc" -I"$O/src/nvidia/inc/kernel" \
  -framework IOKit -framework CoreFoundation
"$W/nvmetrics-agent-caps" caps | tee "$W/caps.jsonl"
"$W/nvmetrics-agent-caps" caps-live | tee "$W/caps-live.json"
"$W/nvmetrics-agent-caps" once | tee "$W/sample.json"
python3 - "$W" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
for line in (p / 'caps.jsonl').read_text().splitlines():
    d = json.loads(line)
    assert d['verdict'] == 'usable' and d['manual_authorized'] is False
    print('Driver verified:', sum(c['state'] == 'verified' for c in d['capabilities']))
d = json.loads((p / 'caps-live.json').read_text())
assert d['manual_authorized'] is False and d['cooler_count'] is None and d['fan_stop'] is None
print('RUSD tach mask:', hex(d['tach_slot_mask']))
print('RUSD thermal mask:', hex(d['thermal_target_mask']))
print('RUSD power mask:', hex(d['power_read_mask']))
s = json.loads((p / 'sample.json').read_text())
profile = s['gpu_profile']
assert profile['gpu_id'] == d['gpu_id']
print('Live UI profile:', profile)
PY
echo "RESULTADOS: $W"
echo 'No se instaló ni modificó ningún driver o servicio.'
