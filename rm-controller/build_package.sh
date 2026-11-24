#!/bin/bash
# Maintainer-facing, native Intel macOS only. No privileged install or RM writes.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd -P)"
REPO="${1:-$HERE/sources/NVMetrics}"
DRIVER="${2:-$HOME/gpu-caps-phase2B}"
OUT="${3:-$HOME/Desktop/NVMetrics-v0170-release}"
[[ $(uname -s) == Darwin && $(uname -m) == x86_64 ]] || { echo 'Run on Intel macOS.' >&2; exit 2; }
source "$HERE/find_build_python.sh"
PYTHON="$(find_build_python)" || { echo 'No compatible build Python; run Build.command.' >&2; exit 2; }
REPO="$("$PYTHON" -B "$HERE/build_sources.py" repo "$REPO")"
DRIVER="$("$PYTHON" -B "$HERE/build_sources.py" driver-caps "$DRIVER")"
"$PYTHON" -c 'import PyInstaller; print("PyInstaller",PyInstaller.__version__)' || { echo 'PyInstaller is missing; run Build.command.' >&2; exit 2; }
export PYTHONPATH="$HERE/service:$HERE/client:$HERE"
export NVMETRICS_BUILD_CACHE="${NVMETRICS_BUILD_CACHE:-$HERE/.build-cache}"
NVMETRICS_BUILD_PYTHON="$PYTHON" "$HERE/run_tests.sh"
# Historical tests use their pinned driver. Never reuse their patched OGKM
# header cache for a different driver checkout; key it by the actual patches.
DRIVER_CACHE_KEY="$(shasum -a 256 "$DRIVER/nvk/ogkm-nvos-620.patch" "$DRIVER/nvk/ogkm-warnings.patch" | shasum -a 256 | cut -c1-24)"
export NVMETRICS_BUILD_CACHE="$NVMETRICS_BUILD_CACHE/driver-$DRIVER_CACHE_KEY"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
export CLANG_MODULE_CACHE_PATH="$STAGE/modules"
mkdir -p "$OUT" "$STAGE/scripts" "$STAGE/payload" "$STAGE/build"
# Leave the developer's validated checkout untouched.
/usr/bin/ditto "$REPO" "$STAGE/repo"
cp "$HERE/app/"*.swift "$STAGE/repo/app/Sources/"
cp "$HERE/agent/"*.h "$HERE/agent/"*.c "$STAGE/repo/agent/"
# The extractor of the driver's RM layer that knows the 3.0.1 boost-policy header (the base's stops at 3.0).
cp "$HERE/agent/extract_rm_layer.py" "$STAGE/repo/agent/extract_rm_layer.py"
cp "$HERE/agent/build.sh" "$STAGE/repo/agent/build.sh"
cp "$HERE/app/build_app.sh" "$STAGE/repo/app/build_app.sh"
"$STAGE/repo/app/build_app.sh" "$DRIVER" "$STAGE/build"
APP="$STAGE/payload/NVMetrics.app"
/usr/bin/ditto "$STAGE/build/NVMetrics.app" "$APP"
# Build the native multi-resolution application icon before signing the bundle.
ICONSET="$STAGE/AppIcon.iconset"
mkdir "$ICONSET"
for size in 16 32 128 256 512; do
  /usr/bin/sips -z "$size" "$size" "$HERE/app/Resources/AppIcon.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
  /usr/bin/sips -z "$((size*2))" "$((size*2))" "$HERE/app/Resources/AppIcon.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
/usr/bin/iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
test -s "$APP/Contents/Resources/AppIcon.icns"
AGENT="$APP/Contents/Resources/nvmetrics-agent"
test -x "$AGENT"
strings "$AGENT" | grep 'NVMETRICS_RM_V0170_DIAGNOSTICS' >/dev/null
# No hardware call: marker must come from the real, ABI-enabled implementation.
strings "$AGENT" | grep -F 'NVMETRICS_GPU_CAPS_COMPILED' >/dev/null
# The root guard must carry the RUSD flight recorder and its instrumented reader (diagnostic, 0.15.7).
strings "$AGENT" | grep 'NVMETRICS_RM_V0157_RUSD_FLIGHT_RECORDER' >/dev/null
# 0.15.8: the guard's RUSD retry busy-waits; it must not contain the sleeping retry of 0.15.7.
strings "$AGENT" | grep 'NVMETRICS_RM_V0158_RUSD_SPIN_RETRY' >/dev/null
# Idle mode for the apps already open needs the "policy" command built against the driver's boost-policy header.
# Without the header the agent still builds and answers {"policy":null,"why":"build"}: stop here instead.
rc=0; "$AGENT" policy bogus >"$STAGE/policy-check.log" 2>&1 || rc=$?
[[ $rc == 2 ]] && grep -F 'usage: nvmetrics-agent policy' "$STAGE/policy-check.log" >/dev/null || {
  echo 'Agent was built without the policy command.' >&2; exit 1;
}
strings "$AGENT" | grep -F '"why":"build"' >/dev/null && {
  echo 'Agent was built without the driver boost-policy header (policy would answer "build").' >&2; exit 1;
}
SIGN_ARGS=()
if [[ -n "${NVMETRICS_APP_IDENTITY:-}" ]]; then SIGN_ARGS=(--codesign-identity "$NVMETRICS_APP_IDENTITY"); fi
# Freeze the Python interpreter and standard library into relocatable onedir.
# Never use onefile: its extraction parent can invalidate the rescue PID check.
"$PYTHON" -m PyInstaller --noconfirm --clean --onedir --name nvmetrics-runtime \
  --paths "$HERE/service" --paths "$HERE/client" --paths "$HERE" \
  --distpath "$STAGE/dist" --workpath "$STAGE/work" --specpath "$STAGE" \
  --target-arch x86_64 ${SIGN_ARGS[@]+"${SIGN_ARGS[@]}"} "$HERE/runtime.py"
GUARD="$APP/Contents/Resources/GuardTools"
# Remove legacy standalone experiment launchers; the packaged UI uses only
# the self-contained runtime and the guarded IPC service.
rm -rf "$GUARD"
mkdir -p "$GUARD"
/usr/bin/ditto "$STAGE/dist/nvmetrics-runtime" "$GUARD/Runtime"
chmod +x "$GUARD/Runtime/nvmetrics-runtime"
"$PYTHON" -B - "$APP/Contents/Info.plist" <<'PY'
import plistlib,sys
from pathlib import Path
p=Path(sys.argv[1]);obj=plistlib.loads(p.read_bytes())
obj['CFBundleShortVersionString']='0.17.0';obj['CFBundleVersion']='0.17.0'
obj['CFBundleIconFile']='AppIcon.icns'
p.write_bytes(plistlib.dumps(obj))
PY
# Verify runtime CLI entrypoint works without external Python or a GPU write.
rc=0
"$GUARD/Runtime/nvmetrics-runtime" client invalid >"$STAGE/runtime-check.log" 2>&1 || rc=$?
[[ $rc == 2 ]] && grep -F 'usage: nvmetrics_ipc_v08.py' "$STAGE/runtime-check.log" >/dev/null || {
  echo 'Frozen IPC client failed its start-up check.' >&2; exit 1;
}
# Keep application signing after modifying Resources. Developer-ID distribution
# requires full nested-code signing and notarization; ad-hoc is local testing.
if [[ -n "${NVMETRICS_APP_IDENTITY:-}" ]]; then
  /usr/bin/codesign --force --options runtime --timestamp --sign "$NVMETRICS_APP_IDENTITY" "$AGENT"
  /usr/bin/codesign --force --options runtime --timestamp --sign "$NVMETRICS_APP_IDENTITY" "$APP"
else
  /usr/bin/codesign --force --sign - "$APP"
fi
/usr/bin/codesign --verify --deep --strict "$APP"
mkdir "$STAGE/payload/service"
cp "$HERE/service/"*.py "$STAGE/payload/service/"
cp "$APP/Contents/Resources/nvmetrics-setting.sh" "$STAGE/payload/service/"
"$PYTHON" -B - "$STAGE/payload" <<'PY'
from pathlib import Path
from package_install import tree_digest
import json,sys
p=Path(sys.argv[1]);(p/'integrity.json').write_text(json.dumps({'app':tree_digest(p/'NVMetrics.app'),'service':tree_digest(p/'service')}))
PY
# Preserve the signed app and its Python symlinks as an opaque native ZIP.
# pkgbuild handles only two regular script files, never the app's directory tree.
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$STAGE/payload" "$STAGE/scripts/payload.zip"
"$PYTHON" -B "$HERE/verify_package_payload.py" render "$STAGE/scripts"
echo 'Checking native ZIP round-trip before pkgbuild…'
NVMETRICS_BUILD_PYTHON="$PYTHON" "$HERE/verify_scripts.sh" "$STAGE/scripts" "$STAGE/roundtrip" "$STAGE/payload"
PKG="$OUT/NVMetrics-0.17.0-Candidate-Intel.pkg"
[[ ! -e "$PKG" ]] || { echo 'Output package already exists; choose a new directory.' >&2; exit 2; }
CANDIDATE="$STAGE/candidate.pkg"
PKG_ARGS=()
if [[ -n "${NVMETRICS_INSTALLER_IDENTITY:-}" ]]; then PKG_ARGS=(--sign "$NVMETRICS_INSTALLER_IDENTITY"); fi
/usr/bin/pkgbuild --nopayload --identifier org.nvmetrics.installer --version 0.17.0 \
  --scripts "$STAGE/scripts" ${PKG_ARGS[@]+"${PKG_ARGS[@]}"} "$CANDIDATE"
# Verify the actual package, not just the staging directory.
NVMETRICS_BUILD_PYTHON="$PYTHON" "$HERE/verify_package.sh" "$CANDIDATE" "$STAGE/scripts/payload.zip"
# Publish only after archive bytes, extracted payload and signatures pass.
/bin/mv -f "$CANDIDATE" "$PKG"
printf '\nBuilt: %s\nOpen this package in Finder, authorize Installer once, then open NVMetrics from Applications.\n' "$PKG"
