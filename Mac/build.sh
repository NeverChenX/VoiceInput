#!/bin/bash
set -e
cd "$(dirname "$0")"

echo "=== VoiceInput macOS Build ==="

# Check Swift
if ! command -v swift &>/dev/null; then
    echo "ERROR: Swift not found. Install Xcode from the App Store."
    exit 1
fi

swift build -c release 2>&1

BINARY=".build/release/VoiceInput"
if [ ! -f "$BINARY" ]; then
    echo "Build failed."
    exit 1
fi

# ── Create a minimal .app bundle ─────────────────────────────────────────
APP="VoiceInput.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
mkdir -p "$APP/Contents/Resources"

cp "$BINARY"   "$APP/Contents/MacOS/VoiceInput"
cp config.json "$APP/Contents/MacOS/config.json"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key>    <string>com.voiceinput.mac</string>
    <key>CFBundleName</key>          <string>VoiceInput</string>
    <key>CFBundleVersion</key>       <string>1.0</string>
    <key>CFBundleExecutable</key>    <string>VoiceInput</string>
    <key>LSUIElement</key>           <true/>
    <key>NSMicrophoneUsageDescription</key>
    <string>VoiceInput 需要麦克风权限来录制语音进行识别。</string>
</dict>
</plist>
PLIST

echo ""
echo "✅ Build OK: $APP"
echo ""
echo "首次运行前请在"系统设置 → 隐私与安全 → 辅助功能"中授权 VoiceInput"
echo ""
echo "用法："
echo "  double-click VoiceInput.app  — 启动"
echo "  Option+空格                   — 开始录音"
echo "  空格                          — 停止并识别"
echo "  ESC                           — 退出"
