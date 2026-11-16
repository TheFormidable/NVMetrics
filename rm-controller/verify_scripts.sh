#!/bin/bash
# Read-only, run the same native extraction as the installer (without root).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd -P)"
source "$HERE/find_build_python.sh"
PYTHON="$(find_build_python)"
export PYTHONPATH="$HERE/service:$HERE/client:$HERE"
SCRIPTS="$1"
DEST="$2"
[[ ! -e "$DEST" && ! -L "$DEST" ]] || { echo 'Verifier destination already exists.' >&2; exit 1; }
"$PYTHON" -B "$HERE/verify_package_payload.py" envelope "$SCRIPTS"
mkdir -m 700 "$DEST"
/usr/bin/ditto -x -k "$SCRIPTS/payload.zip" "$DEST"
if [[ -n "${3:-}" ]]; then
  "$PYTHON" -B "$HERE/verify_package_payload.py" compare "$3" "$DEST/payload"
fi
"$PYTHON" -B "$HERE/verify_package_payload.py" payload "$DEST/payload"
/usr/bin/codesign --verify --deep --strict "$DEST/payload/NVMetrics.app"
echo 'PASS: native ZIP extraction and application signature.'
