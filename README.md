# VoiceInput 🎙️

> Push-to-talk voice transcription for Windows & macOS — zero external dependencies.

[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20macOS-blue)](https://github.com/yourusername/VoiceInput)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

---

## English

VoiceInput is a lightweight, cross-platform voice-to-text tool that lets you dictate anywhere with a simple hotkey. Press and hold to record, release to transcribe — your words appear instantly at the cursor position.

### ✨ Features

- **Push-to-talk** — Alt+Space to start, Space to stop (configurable)
- **Zero dependencies** — Windows: pure Win32 API; macOS: native Swift + AVFoundation
- **Fast transcription** — Powered by ByteDance Volcano Engine ASR
- **Auto-paste** — Transcribed text automatically pasted at cursor + Enter
- **Visual feedback** — Recording overlay shows real-time status
- **System tray** — Runs quietly in background, ESC to exit
- **Configurable** — Hotkeys, API credentials, timeouts via JSON config

### 🚀 Quick Start

#### Windows
```bash
cd Windows
# Edit config.json with your API credentials
voice_input.exe
```

#### macOS
```bash
cd Mac
swift build
./.build/debug/VoiceInput
```

### ⚙️ Configuration

Edit `config.json`:
```json
{
  "app_id": "YOUR_APP_ID",
  "access_token": "YOUR_TOKEN",
  "secret_key": "YOUR_SECRET",
  "standard_resource_id": "volc.seedasr.auc",
  "request_timeout_seconds": 120,
  "poll_interval_seconds": 1.2,
  "poll_timeout_seconds": 45
}
```

Get API credentials from [Volcano Engine](https://www.volcengine.com/).

### 🛠️ Build from Source

**Windows (MSVC):**
```batch
build.bat
```

**Windows (MinGW):**
```bash
g++ -std=c++17 -O2 -o voice_input.exe voice_input.cpp \
    -lwinmm -lwinhttp -luser32 -lgdi32 -lole32 -mwindows
```

**macOS:**
```bash
swift build -c release
```

---

## 中文

VoiceInput 是一个轻量级跨平台语音转文字工具，通过简单的热键即可在任何地方进行语音输入。按住录音，松手转写，文字即刻出现在光标位置。

### ✨ 功能特性

- **按键即说** — Alt+Space 开始录音，Space 停止（可配置）
- **零依赖** — Windows 纯 Win32 API；macOS 原生 Swift + AVFoundation
- **快速转写** — 基于字节跳动火山引擎语音识别
- **自动粘贴** — 转写结果自动粘贴到光标位置并发送
- **视觉反馈** — 录音浮层实时显示状态
- **系统托盘** — 后台静默运行，ESC 退出
- **可配置** — 热键、API 密钥、超时时间通过 JSON 配置

### 🚀 快速开始

#### Windows
```bash
cd Windows
# 编辑 config.json 填入 API 密钥
voice_input.exe
```

#### macOS
```bash
cd Mac
swift build
./.build/debug/VoiceInput
```

### ⚙️ 配置说明

编辑 `config.json`：
```json
{
  "app_id": "你的应用ID",
  "access_token": "你的访问令牌",
  "secret_key": "你的密钥",
  "standard_resource_id": "volc.seedasr.auc",
  "request_timeout_seconds": 120,
  "poll_interval_seconds": 1.2,
  "poll_timeout_seconds": 45
}
```

API 密钥从 [火山引擎](https://www.volcengine.com/) 获取。

### 🛠️ 从源码构建

**Windows (MSVC):**
```batch
build.bat
```

**Windows (MinGW):**
```bash
g++ -std=c++17 -O2 -o voice_input.exe voice_input.cpp \
    -lwinmm -lwinhttp -luser32 -lgdi32 -lole32 -mwindows
```

**macOS:**
```bash
swift build -c release
```

---

## 📁 Project Structure

```
VoiceInput/
├── Windows/
│   ├── voice_input.cpp    # Main Windows implementation (C++)
│   ├── voice_input.exe    # Pre-built Windows binary
│   ├── build.bat          # MSVC build script
│   └── config.json        # Configuration template
│
└── Mac/
    ├── Package.swift      # Swift Package Manager manifest
    ├── Sources/
    │   └── VoiceInput/    # macOS implementation (Swift)
    ├── build.sh           # Build script
    └── config.json        # Configuration template
```

---

## 🔒 Privacy

- All audio processing happens on Volcano Engine's servers
- No local audio files are retained after transcription
- Config file stays local on your machine

---

## 📄 License

MIT License — see [LICENSE](LICENSE)

---

## 🙏 Acknowledgments

- ASR powered by [ByteDance Volcano Engine](https://www.volcengine.com/)
- Windows implementation uses pure Win32 API (no external deps)
- macOS implementation uses native AVFoundation
