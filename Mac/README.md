# VoiceInput — 语音输入工具

按下快捷键录音，自动识别语音并粘贴文字到当前光标位置。支持 Windows 和 macOS 双平台。

---

## 目录

- [核心功能](#核心功能)
- [快捷键](#快捷键)
- [界面组件](#界面组件)
- [配置项](#配置项)
- [状态机](#状态机)
- [ASR 语音识别引擎](#asr-语音识别引擎)
- [音频录制](#音频录制)
- [错误处理](#错误处理)
- [平台实现差异](#平台实现差异)
- [文件结构](#文件结构)
- [编译与运行](#编译与运行)

---

## 核心功能

| 功能 | 说明 |
|------|------|
| 语音录音 | 按快捷键开始录音，麦克风实时采集 PCM 音频 |
| 语音识别 | 调用火山引擎 Seed ASR 大模型，支持中英文混合识别，自动标点、逆文本正则化 |
| 自动粘贴 | 识别完成后自动写入剪贴板，模拟 Ctrl+V (Windows) / Cmd+V (macOS) 粘贴到当前光标位置 |
| 自动发送 | 可选功能，粘贴后自动按 Enter/Return 发送（可在状态窗口一键切换） |
| ESC 取消 | 录音中按 ESC 取消录音，丢弃音频，不提交识别，显示"已取消"提示 |
| 前台窗口恢复 | (Windows) 录音前记住前台窗口，粘贴时自动恢复焦点 |

---

## 快捷键

### 默认快捷键

| 操作 | Windows | macOS |
|------|---------|-------|
| 开始录音 | Alt+Space | Option+Space |
| 停止录音并识别 | Space | Space |
| 取消录音 | ESC | ESC |

### 自定义快捷键

可在设置对话框中自定义开始/停止录音的快捷键，支持以下组合：

**修饰键**: Alt/Opt/Option, Ctrl/Control, Shift, Win/Cmd/Command

**功能键**: F1–F12, Space, Enter/Return, Tab, Esc, Backspace/Delete

**字母键**: A–Z

**数字键**: 0–9

**Windows 额外支持**: Insert, Home, End, PageUp, PageDown, 方向键, 十六进制 VK 码 (如 "VK41")

快捷键输入方式：点击设置对话框中的输入框，直接按下想要的组合键即可捕获。按 ESC 取消捕获。

---

## 界面组件

### 状态悬浮窗（200x200，屏幕右上角）

常驻显示的半透明深色圆角窗口，分为三个区域：

**Zone A — 状态指示区（上方 56%）**

| 状态 | 圆点颜色 | 标签 |
|------|----------|------|
| 待机 | 灰色 RGB(120,120,120) | 待机 |
| 录音中 | 红色 RGB(255,70,70) | 录音 |
| 识别中 | 蓝色 RGB(70,140,255) | 识别 |

- 圆点半径 20px，居中显示
- 状态文字 22pt 粗体，圆点下方

**Zone B — 自动发送开关（中间 17%）**

- 左侧：标签"自动发送"
- 右侧：iOS 风格拨动开关（58x28px / 50x26px）
- 开启：绿色滑块，关闭：灰色滑块
- 点击区域即可切换

**Zone C — 快捷键提示 + 设置按钮（底部）**

- 左侧：当前快捷键提示文字（如 "Alt+Space"）
- 右侧：齿轮图标按钮 "⚙"，点击打开设置对话框

**交互操作**：
- 拖拽窗口任意区域可移动位置（4px 阈值防误触）
- 点击中间区域切换自动发送
- 点击齿轮图标打开设置

**窗口属性**：
- 始终置顶 (TOPMOST)
- 不抢焦点 (NOACTIVATE)
- 深色背景 RGB(18,18,24)，97% 不透明度
- 圆角 14px

---

### 全屏遮罩（Overlay）

录音/识别时覆盖全屏的半透明黑色遮罩：

| 状态 | 主文字 | 副文字 |
|------|--------|--------|
| 录音中 | "Listening" / "正在聆听" | "Press Space to stop" / "按空格停止" |
| 识别中 | "Transcribing" / "识别中" | "Please wait..." / "请稍候..." |
| 错误 | "Error" / "错误" | 具体错误信息 |
| 已取消 | "已取消" | （空） |

**样式**：
- 背景：黑色 88% 不透明度
- 主文字：52–64pt 粗体，白色
- 副文字：18–24pt 常规，浅灰色
- 鼠标穿透（不影响操作）
- 覆盖所有工作区/虚拟屏幕

**自动消失**：
- 错误提示：2.6 秒后隐藏
- 取消提示：0.8 秒后隐藏

---

### 设置对话框

| 字段 | 说明 | 必填 |
|------|------|------|
| App ID | 火山引擎应用 ID | 是 |
| Access Token | API 访问令牌 | 是 |
| Secret Key | API 密钥 | 否 |
| 开始录音快捷键 | 触发录音的组合键（点击输入框后按键捕获） | 否（默认 Alt+Space） |
| 停止录音快捷键 | 停止录音的按键（点击输入框后按键捕获） | 否（默认 Space） |
| 自动发送 | (macOS) 识别后自动按 Return 发送 | 否 |

**快捷键输入框特殊行为**：
- 获得焦点时清空内容，显示"请按下快捷键..."
- 按下任意组合键后自动填入（如 "Ctrl+Shift+A"）
- 按 ESC 取消捕获，恢复原值
- (Windows) 蓝色边框提示等待输入状态

**按钮**：
- 保存（快捷键 Enter）：验证必填项，保存 config.json，立即生效
- 取消（快捷键 ESC）：关闭对话框不保存

---

### 系统托盘（仅 Windows）

- 图标：系统信息图标
- 提示文字："VoiceInput — 点击状态窗口⚙配置快捷键"
- 启动时气泡通知："VoiceInput 已就绪"
- 右键菜单：退出 VoiceInput

---

## 配置项

配置保存在 `config.json` 文件中，修改后立即生效，无需重启。

### config.json 完整字段

```json
{
  "app_id": "1380983374",
  "access_token": "WoG6IZCkz0QCyoiBWKwOtuIMSnfmGk40",
  "secret_key": "jhECtn1EMVy4-myE9rhWiqDORTKtrNxe",
  "standard_resource_id": "volc.seedasr.auc",
  "standard_submit_endpoint": "https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit",
  "standard_query_endpoint": "https://openspeech.bytedance.com/api/v3/auc/bigmodel/query",
  "hotkey": "Alt+Space",
  "hotkey_stop": "Space",
  "auto_enter": true,
  "request_timeout_seconds": 120,
  "poll_interval_seconds": 1.2,
  "poll_timeout_seconds": 45
}
```

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| app_id | string | （必填） | 火山引擎应用 ID |
| access_token | string | （必填） | API 访问令牌 |
| secret_key | string | "" | API 密钥 |
| standard_resource_id | string | "volc.seedasr.auc" | ASR 资源 ID |
| standard_submit_endpoint | string | (见上) | 提交接口 URL |
| standard_query_endpoint | string | (见上) | 查询接口 URL |
| hotkey | string | "Alt+Space" | 开始录音快捷键 |
| hotkey_stop | string | "Space" | 停止录音快捷键 |
| auto_enter | bool | true | 识别后自动发送 |
| request_timeout_seconds | int | 120 | HTTP 请求超时（秒） |
| poll_interval_seconds | float | 1.2 | ASR 轮询间隔（秒） |
| poll_timeout_seconds | float | 45 | ASR 轮询总超时（秒） |

---

## 状态机

```
┌──────┐   开始快捷键   ┌──────────┐   停止快捷键   ┌──────────────┐
│ IDLE │ ─────────────→ │ RECORDING│ ─────────────→ │ TRANSCRIBING │
└──────┘                └──────────┘                └──────────────┘
   ↑                        │                            │
   │         ESC 取消       │                            │
   │ ◄──────────────────────┘                            │
   │                                                     │
   │              识别成功 / 识别失败 / 超时               │
   │ ◄──────────────────────────────────────────────────┘
```

| 触发 | 从状态 | 到状态 | 动作 |
|------|--------|--------|------|
| 开始快捷键 | IDLE | RECORDING | 启动麦克风录音，显示全屏遮罩 |
| 停止快捷键 | RECORDING | TRANSCRIBING | 停止录音，提交 ASR，显示等待遮罩 |
| ESC | RECORDING | IDLE | 丢弃音频，显示"已取消"，0.8 秒后隐藏遮罩 |
| ESC | TRANSCRIBING | IDLE | 取消等待，显示"已取消" |
| 识别成功 | TRANSCRIBING | IDLE | 写入剪贴板，模拟粘贴，隐藏遮罩 |
| 识别失败/超时 | TRANSCRIBING | IDLE | 显示错误信息，2.6 秒后隐藏遮罩 |

---

## ASR 语音识别引擎

使用火山引擎 Seed ASR 大模型（豆包语音识别），异步提交+轮询模式。

### 提交阶段

```
POST /api/v3/auc/bigmodel/submit

Headers:
  Content-Type: application/json
  X-Api-App-Key: {app_id}
  X-Api-Access-Key: {access_token}
  X-Api-Resource-Id: {resource_id}
  X-Api-Request-Id: {uuid}
  X-Api-Sequence: -1

Body:
{
  "user": { "uid": "{app_id}" },
  "audio": { "data": "{base64_wav}", "format": "wav" },
  "request": { "model_name": "bigmodel", "enable_itn": true, "enable_punc": true }
}
```

- 成功响应：状态码 `20000000`
- 失败：抛出错误，显示在遮罩中

### 轮询阶段

```
POST /api/v3/auc/bigmodel/query

Body: { "id": "{request_id}" }
```

| 响应码 | 含义 | 处理 |
|--------|------|------|
| 20000000 | 成功 | 提取 result.text 返回 |
| 20000001 | 处理中 | 等待 poll_interval 后重试 |
| 20000003 | 无有效语音 | 抛出"未检测到有效语音" |
| 45000292 | QPS 限流 | 等待 max(poll_interval, 2秒) 后重试 |
| 其他 | 查询失败 | 抛出错误 |

### 音频格式要求

- 格式：WAV
- 采样率：16,000 Hz
- 位深：16-bit
- 声道：单声道 (mono)
- 编码：PCM

---

## 音频录制

### Windows

- **API**: WinMM (waveIn)
- **采样率**: 固定 16,000 Hz
- **格式**: PCM, 16-bit, 单声道
- **缓冲区**: 4 个 x 4,096 采样点，循环填充
- **回调**: WIM_DATA 消息驱动，数据追加到缓冲区
- **线程安全**: std::mutex 保护音频数据

### macOS

- **API**: AVAudioEngine + inputNode tap
- **采样率**: 运行时检测（适配硬件实际采样率）
- **格式**: Float32 采集，转换为 Int16 存储
- **缓冲区**: 每次 tap 回调 4,096 帧
- **转换**: float [-1.0, 1.0] → Int16 [-32767, 32767]
- **线程安全**: NSLock 保护音频缓冲区

### WAV 构建

两端均自行构建 WAV 文件（不依赖第三方库）：
- RIFF 头 + 文件大小
- fmt 块（PCM, 单声道, 16-bit, 采样率）
- data 块（原始 PCM 数据）
- 小端字节序

### 最短录音限制

- 最短录音时长：0.25 秒
- 低于此时长会提示"录音太短"

---

## 错误处理

| 错误场景 | 处理方式 |
|----------|----------|
| config.json 不存在 | 弹出对话框提示，程序退出 |
| app_id 或 access_token 为空 | 弹出对话框提示，程序退出 |
| 麦克风权限被拒绝 | (macOS) 弹出辅助功能权限提示；(Windows) 显示错误遮罩 |
| 辅助功能权限未授予 | (macOS) 弹出系统对话框指引授权 |
| 录音设备打开失败 | 显示错误遮罩，状态回到 IDLE |
| 未捕获到音频 | 显示"未捕获到音频，请检查麦克风权限" |
| 录音太短 (< 0.25秒) | 显示"录音太短，请说话后再停止" |
| ASR 提交失败 | 显示错误码和信息 |
| 无有效语音 (20000003) | 显示"未检测到有效语音" |
| QPS 限流 (45000292) | 自动等待 2 秒后重试（用户无感知） |
| ASR 超时 | 显示"ASR 超时" |
| HTTP 连接失败 | 显示网络错误信息 |

所有错误遮罩在 2.6 秒后自动隐藏，状态恢复到 IDLE。

---

## 平台实现差异

| 特性 | Windows | macOS |
|------|---------|-------|
| 编程语言 | C++17 | Swift 5 |
| 外部依赖 | 无（纯 Win32 API） | 无（纯系统框架） |
| 键盘钩子 | SetWindowsHookEx (WH_KEYBOARD_LL) | CGEventTap |
| 音频 API | WinMM (waveIn) | AVAudioEngine |
| HTTP 客户端 | WinHTTP | URLSession |
| 剪贴板 | Win32 Clipboard API | NSPasteboard |
| 按键模拟 | keybd_event | CGEvent.post |
| 遮罩窗口 | 分层窗口 (SetLayeredWindowAttributes) | NSWindow (.screenSaver level) |
| 系统托盘 | Shell_NotifyIcon | 无（accessory 模式） |
| 采样率 | 固定 16kHz | 运行时检测 |
| 异步模型 | std::thread + Win32 消息循环 | GCD + async/await |
| 日志线程安全 | std::mutex | DispatchQueue |
| 配置对话框 | Win32 原生控件 | Cocoa NSWindowController |
| DPI 感知 | SetProcessDPIAware() | 系统自动处理 |
| 进程模式 | 标准窗口进程 | NSApplication.accessory |

---

## 文件结构

```
VoiceInput/
├── README.md                            # 本文档
├── voice_input.log                      # 运行日志
│
├── Windows/
│   ├── voice_input.cpp                  # 主程序源码（C++17, ~1700行）
│   ├── voice_input.exe                  # 编译产物（独立可执行，无需 DLL）
│   ├── config.json                      # 配置文件
│   ├── voice_input.log                  # 运行日志
│   ├── error.log                        # 错误日志
│   └── build.bat                        # 编译脚本
│
└── Mac/
    ├── VoiceInput.app/                  # macOS 应用包
    │   └── Contents/
    │       ├── MacOS/VoiceInput         # 启动脚本（自动编译+运行）
    │       ├── Resources/main.swift     # 主程序源码（Swift, ~800行）
    │       ├── Resources/config.json    # 配置文件
    │       └── Info.plist               # 应用信息（Bundle ID, 权限声明）
    ├── Sources/VoiceInput/              # Swift Package 源码（多文件版本）
    │   ├── AppController.swift          # 应用控制器
    │   ├── AppDelegate.swift            # 应用委托
    │   ├── AsrClient.swift              # ASR 客户端
    │   ├── AudioRecorder.swift          # 音频录制
    │   ├── Config.swift                 # 配置加载
    │   ├── HotkeyManager.swift          # 快捷键管理
    │   ├── Logger.swift                 # 日志
    │   ├── OverlayWindowController.swift # 遮罩窗口
    │   ├── StatusWindowController.swift # 状态窗口
    │   ├── WavBuilder.swift             # WAV 构建
    │   └── main.swift                   # 入口点
    ├── Package.swift                    # Swift Package 配置
    ├── config.json                      # 配置文件（开发用）
    ├── activate.sh                      # 首次激活脚本
    └── build.sh                         # 手动编译脚本
```

---

## 编译与运行

### Windows

```batch
# 编译（需要 g++ / MinGW）
cd Windows
build.bat

# 运行
voice_input.exe
```

要求：Windows 10+，g++ (MinGW) 编译器

### macOS

```bash
# 首次激活（清除 Gatekeeper 隔离标记）
cd Mac
bash activate.sh

# 运行（双击或命令行）
open VoiceInput.app
```

首次启动自动编译（需要 Xcode 命令行工具：`xcode-select --install`）。

**必需权限**：
1. 系统设置 → 隐私与安全 → 辅助功能 → 允许 VoiceInput
2. 系统设置 → 隐私与安全 → 麦克风 → 允许 VoiceInput
