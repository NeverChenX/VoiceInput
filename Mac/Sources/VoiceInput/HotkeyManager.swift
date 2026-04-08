import AppKit

// kVK_Space = 49, kVK_Escape = 53
private let kSpace: CGKeyCode = 49
private let kEsc:   CGKeyCode = 53

class HotkeyManager {
    var onStartRecording: (() -> Void)?
    var onStopRecording:  (() -> Void)?
    var onExit:           (() -> Void)?

    private var tap: CFMachPort?
    private var src: CFRunLoopSource?
    private var recording = false
    private var consumed  = false  // was Option+Space consumed?

    func start() {
        let mask: CGEventMask = (1 << CGEventType.keyDown.rawValue)
                              | (1 << CGEventType.keyUp.rawValue)

        let cb: CGEventTapCallBack = { _, type, event, ref in
            Unmanaged<HotkeyManager>.fromOpaque(ref!).takeUnretainedValue()
                .handle(type: type, event: event)
        }

        guard let t = CGEvent.tapCreate(
            tap: .cgSessionEventTap,
            place: .headInsertEventTap,
            options: .defaultTap,
            eventsOfInterest: mask,
            callback: cb,
            userInfo: Unmanaged.passUnretained(self).toOpaque()
        ) else {
            logError("CGEventTap 创建失败 — 请在"系统设置 → 隐私与安全 → 辅助功能"中授权")
            return
        }
        tap = t
        src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, t, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), src, .commonModes)
        CGEvent.tapEnable(tap: t, enable: true)
        logInfo("HotkeyManager: 键盘钩子已安装 (Option+Space 开始，Space 停止，ESC 退出)")
    }

    func stop() {
        if let t = tap { CGEvent.tapEnable(tap: t, enable: false) }
        if let s = src { CFRunLoopRemoveSource(CFRunLoopGetMain(), s, .commonModes) }
    }

    private func handle(type: CGEventType, event: CGEvent) -> Unmanaged<CGEvent>? {
        let key  = CGKeyCode(event.getIntegerValueField(.keyboardEventKeycode))
        let down = (type == .keyDown)
        let up   = (type == .keyUp)

        // ESC → exit
        if key == kEsc && down {
            DispatchQueue.main.async { self.onExit?() }
            return nil
        }

        guard key == kSpace else { return Unmanaged.passRetained(event) }

        // While recording: Space → stop
        if recording {
            if down {
                recording = false; consumed = false
                DispatchQueue.main.async { self.onStopRecording?() }
            }
            return nil
        }

        // Option+Space → start recording
        if down && event.flags.contains(.maskAlternate) {
            logInfo("Option+Space → 开始录音")
            recording = true; consumed = true
            DispatchQueue.main.async { self.onStartRecording?() }
            return nil
        }

        // Suppress the key-up that paired with Option+Space
        if up && consumed { consumed = false; return nil }

        return Unmanaged.passRetained(event)
    }
}
