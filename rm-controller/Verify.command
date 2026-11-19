#!/bin/bash
# One-click developer verification of the existing .pkg; no rebuild or install.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd -P)"
PKG="${1:-$HOME/Desktop/NVMetrics-v0158-release/NVMetrics-0.15.8-Candidate-Intel.pkg}"
"$HERE/verify_package.sh" "$PKG"
/usr/bin/open -R "$PKG"
