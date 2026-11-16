#!/bin/bash
# build_app.sh <driver checkout> <out dir>: builds NVMetrics.app (x86_64, macOS 14+) into <out dir>.
# The driver checkout (TheFormidable/nvidia-macos-driver) supplies the resource-manager layer nvmetrics-agent links
# (agent/build.sh). Needs Xcode, git and network; installs nothing and writes only to <out dir> and build/.
set -euo pipefail
T=${1:?driver checkout}; OUT=${2:?output folder}
H=$(cd "$(dirname "$0")" && pwd)
APP="$OUT/NVMetrics.app"
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
"$H/../agent/build.sh" "$T" "$APP/Contents/Resources/nvmetrics-agent"
install -m 755 "$H/../agent/nvmetrics-setting.sh" "$APP/Contents/Resources/nvmetrics-setting.sh"
xcrun swiftc -O -swift-version 5 -target x86_64-apple-macos14.0 -parse-as-library \
  -o "$APP/Contents/MacOS/NVMetrics" "$H"/Sources/*.swift
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>NVMetrics</string>
  <key>CFBundleDisplayName</key><string>NVMetrics</string>
  <key>CFBundleIdentifier</key><string>com.nullmoth.nvmetrics</string>
  <key>CFBundleExecutable</key><string>NVMetrics</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>LSUIElement</key><true/>
  <key>NSHighResolutionCapable</key><true/>
  <key>CFBundleDevelopmentRegion</key><string>es</string>
</dict></plist>
PLIST
# RM v0.5: bundled attended test tools, copied BEFORE code signing.
if [[ -d "$H/GuardTools" ]]; then
  mkdir -p "$APP/Contents/Resources/GuardTools"
  install -m 644 "$H"/GuardTools/*.py "$APP/Contents/Resources/GuardTools/"
  install -m 755 "$H"/GuardTools/*.command "$APP/Contents/Resources/GuardTools/"
fi
codesign --force --deep --sign - "$APP"
echo "built $APP"
