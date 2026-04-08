#!/bin/bash
# 一次性激活脚本 — 只需运行一次
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
chmod +x "$DIR/VoiceInput.app/Contents/MacOS/VoiceInput"
# 清除 Gatekeeper 隔离标记（从网络下载时需要）
xattr -cr "$DIR/VoiceInput.app" 2>/dev/null || true
echo "✅ 激活完成。现在可以双击 VoiceInput.app 启动了。"
echo ""
echo "首次运行前请确认："
echo "  1. 系统设置 → 隐私与安全 → 辅助功能 → 允许 VoiceInput"
echo "  2. 系统设置 → 隐私与安全 → 麦克风 → 允许 VoiceInput"
