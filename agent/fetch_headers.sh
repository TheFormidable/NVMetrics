#!/bin/bash
# fetch_headers.sh <driver checkout> <out dir>: the NVIDIA headers nvmetrics-agent compiles against, made the same way the
# driver's build_nvk.sh makes them, plus the one thing that must match the kernel driver exactly.
#
#  <out>/ogkm      open-gpu-kernel-modules 610.57.04 (headers only), with the driver's ogkm-nvos-620.patch and
#                  ogkm-warnings.patch: what NVK and nvmtl-smi build the resource-manager layer against.
#  <out>/rusd620   the shared-data block's layout (class/cl00de.h and the ECC header it includes) from 620.70.06, the
#                  resource manager the NullMoth kext runs. 620 added a section in the middle of the block, so the 610
#                  layout puts power and fan at the wrong offsets. It goes first on the include path.
#
# Needs git and network (github.com). Writes only under <out>; a second run reuses what is there.
set -euo pipefail
D=$(cd "${1:?driver checkout}" && pwd); OUT=${2:?output folder}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
URL=https://github.com/NVIDIA/open-gpu-kernel-modules.git
HDRS="src/common/sdk/nvidia/inc src/common/inc src/nvidia/arch/nvalloc/common/inc src/nvidia/arch/nvalloc/unix/include src/nvidia/inc/kernel kernel-open/common/inc"

sparse() {   # sparse <tag> <dir>
  [ -d "$2/.git" ] && return 0
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$1" --filter=blob:none --sparse "$URL" "$2"
  git -C "$2" sparse-checkout set $HDRS
}

if [ ! -f "$OUT/ogkm/.nvmetrics-patched" ]; then
  rm -rf "$OUT/ogkm"
  sparse 610.57.04 "$OUT/ogkm"
  patch -p1 -s --no-backup-if-mismatch -d "$OUT/ogkm" < "$D/nvk/ogkm-nvos-620.patch"
  patch -p1 -s --no-backup-if-mismatch -d "$OUT/ogkm" < "$D/nvk/ogkm-warnings.patch"
  touch "$OUT/ogkm/.nvmetrics-patched"
fi

if [ ! -f "$OUT/rusd620/class/cl00de.h" ]; then
  T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
  sparse 620.70.06 "$T/ogkm620"
  mkdir -p "$OUT/rusd620/class" "$OUT/rusd620/ctrl/ctrl2080"
  cp "$T/ogkm620/src/common/sdk/nvidia/inc/class/cl00de.h" "$OUT/rusd620/class/"
  cp "$T/ogkm620/src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080ecc.h" "$OUT/rusd620/ctrl/ctrl2080/"
fi
echo "fetch_headers: $OUT/ogkm (610.57.04 + driver patches), $OUT/rusd620 (620.70.06 shared-data layout)"
