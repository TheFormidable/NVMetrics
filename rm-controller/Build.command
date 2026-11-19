#!/bin/bash
# One-click developer build. End users receive the .pkg, not this script.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd -P)"
[[ $(uname -s) == Darwin && $(uname -m) == x86_64 ]] || { echo 'Intel macOS required.' >&2; exit 2; }
source "$HERE/find_build_python.sh"
if ! PY="$(find_build_python)"; then
  echo 'No compatible CPython 3.11–3.15 with venv was found.' >&2
  echo 'Install Python 3.13 from python.org, then double-click Build.command again.' >&2
  echo 'This is only required on the developer build machine.' >&2
  if [[ -z "${NVMETRICS_BUILD_PYTHON:-}" ]]; then
    /usr/bin/open 'https://www.python.org/downloads/release/python-31317/' || true
  else
    echo 'Check NVMETRICS_BUILD_PYTHON; the explicit override was rejected.' >&2
  fi
  exit 2
fi
printf 'Developer Python: %s (%s)\n' "$PY" "$("$PY" --version)"
[[ $# -le 3 ]] || { echo 'Usage: Build.command [NVMetrics source] [driver source] [output]' >&2; exit 2; }
REPO="$("$PY" -B "$HERE/build_sources.py" repo "${1:-$HERE/sources/NVMetrics}")"
DRIVER="$("$PY" -B "$HERE/build_sources.py" driver "${2:-$HERE/sources/nvidia-macos-driver}")"
OUT="${3:-$HOME/Desktop/NVMetrics-v0159-release}"
printf 'NVMetrics source: %s\nDriver source: %s\nOutput: %s\n' "$REPO" "$DRIVER" "$OUT"
TEMP="$(mktemp -d)"
trap 'rm -rf "$TEMP"' EXIT
echo 'Preparing a temporary build environment (downloads PyInstaller from PyPI, no sudo)…'
"$PY" -m venv "$TEMP/venv"
"$TEMP/venv/bin/python" -m pip install --disable-pip-version-check -r "$HERE/requirements-build.txt"
NVMETRICS_BUILD_PYTHON="$TEMP/venv/bin/python" "$HERE/build_package.sh" "$REPO" "$DRIVER" "$OUT"
