#!/bin/bash
# Compile the new model and agent without installing or starting the app.
set -euo pipefail
cd "$(dirname "$0")"
W=$(mktemp -d "$PWD/phase3-test.XXXXXX")
xcrun swiftc -swift-version 5 -o "$W/test-profile" rm-controller/app/Sample.swift rm-controller/tests/test_gpu_profile.swift
"$W/test-profile"
bash PROBAR-FASE2C.command "$@"
echo 'Phase 3 model verification complete. To build the complete app package, run:'
echo 'bash rm-controller/Build.command "$PWD/rm-controller/sources/NVMetrics" "$HOME/gpu-caps-phase2B" "$PWD/release-v0159"'
