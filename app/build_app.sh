#!/bin/bash
# build_app.sh <patched Mesa tree> <out dir>: builds NVMetrics.app (x86_64, macOS 14+) into <out dir>.
# The Mesa tree is the one ./build.sh patches for NVK (it carries the resource-manager layer nvmetrics-agent links).
# Needs Xcode's command-line tools; installs nothing and touches nothing outside <out dir>.
set -euo pipefail
T=${1:?patched Mesa tree}; OUT=${2:?output folder}
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
codesign --force --deep --sign - "$APP"
echo "built $APP"
