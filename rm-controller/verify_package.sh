#!/bin/bash
# Read-only inspection of our locally built package. No root or RM operations.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd -P)"
PKG="${1:-$HOME/Desktop/NVMetrics-v0151-release/NVMetrics-0.15.1-Candidate-Intel.pkg}"
[[ $(uname -s) == Darwin ]] || { echo 'macOS required.' >&2; exit 2; }
[[ -f "$PKG" ]] || { echo "Package not found: $PKG" >&2; exit 2; }
source "$HERE/find_build_python.sh"
PYTHON="$(find_build_python)" || { echo 'Compatible developer Python unavailable.' >&2; exit 2; }
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
/usr/sbin/pkgutil --expand "$PKG" "$STAGE/expanded"
SCRIPTS="$STAGE/expanded/Scripts"
# Depending on pkgutil's format/version, Scripts may already be unpacked.
if [[ ! -d "$SCRIPTS" ]]; then
  if [[ ! -f "$SCRIPTS" ]]; then
    echo 'Package is missing its Scripts member.' >&2; exit 1
  fi
  /usr/sbin/pkgutil --expand-full "$PKG" "$STAGE/full"
  SCRIPTS="$STAGE/full/Scripts"
fi
[[ -d "$SCRIPTS" ]] || { echo 'Could not expand package Scripts directory.' >&2; exit 1; }
if [[ -n "${2:-}" ]]; then
  /usr/bin/cmp -s "$2" "$SCRIPTS/payload.zip" || { echo 'Package changed the opaque ZIP bytes.' >&2; exit 1; }
  echo 'PASS: package preserved every byte of the source ZIP.'
fi
NVMETRICS_BUILD_PYTHON="$PYTHON" "$HERE/verify_scripts.sh" "$SCRIPTS" "$STAGE/unpacked"
echo 'PASS: extracted application signature, script payload and integrity manifest verified.'
echo "Package verified: $PKG"
