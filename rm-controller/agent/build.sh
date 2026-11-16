#!/bin/bash
# build.sh <driver checkout> <out file> [work dir]: builds nvmetrics-agent (x86_64, macOS 14+) with Xcode's clang.
# Takes the driver's resource-manager layer from its NVK patches (extract_rm_layer.py) and NVIDIA's headers
# (fetch_headers.sh); does not need the driver's patched Mesa tree, Homebrew or anything installed. The work dir
# (default build/ next to this repository's README) holds the fetched headers and is reused.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
D=$(cd "${1:?driver checkout (TheFormidable/nvidia-macos-driver)}" && pwd); OUT=${2:?output file}
W=${3:-${NVMETRICS_BUILD_CACHE:-$H/../build}}; mkdir -p "$W"; W=$(cd "$W" && pwd)
"$H/fetch_headers.sh" "$D" "$W"
rm -rf "$W/rm"; python3 -I "$H/extract_rm_layer.py" "$D" "$W/rm"
O=$W/ogkm; N=$W/rm/nvkmd/nvrm
xcrun clang -arch x86_64 -mmacosx-version-min=14.0 -O1 -Wall -Wno-unused-function -o "$OUT" \
  "$H/nvmetrics_agent.c" "$N/nvRmApi.c" "$N/nvrm_xnu.c" \
  -I"$H" -I"$W/rusd620" -I"$N" -I"$O/src/nvidia/arch/nvalloc/common/inc" -I"$O/src/nvidia/arch/nvalloc/unix/include" \
  -I"$O/src/common/sdk/nvidia/inc" -I"$O/src/nvidia/inc/kernel" -framework IOKit -framework CoreFoundation
echo "built $OUT"
