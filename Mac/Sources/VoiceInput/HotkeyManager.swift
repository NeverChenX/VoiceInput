import AppKit

// kVK_Space = 49, kVK_Escape = 53
private let kSpace: CGKeyCode = 49
private let kEsc: CGKeyCode = 53

class HotkeyManager {
    var onStartRecording: (() -> Void)?
    var onStopRecording: (() -> Void)?
    var onCancel: (() -> Void)?
    
    private var tap: CFMachPort?
    private var src: CFRunLoopSource?
    private var recording = false
    private var consumed = false
    
    // Customizable hotkeys
    private var startHotkey: HotkeyConfig?
    private var stopHotkey: HotkeyConfig?
    
    // Current key state
    private var currentModifiers: NSEvent.ModifierFlags = []
    
    func configure(startHotkey: String, stopHotkey: String) {
        self.startHotkey = HotkeyConfig.parse(startHotkey)
        self.stopHotkey = HotkeyConfig.parse(stopHotkey)
        
        if self.startHotkey == nil {
            logError("Failed to parse start hotkey: \(startHotkey), using default")
            self.startHotkey = HotkeyConfig.parse(Config.defaultHotkey)
        }
        if self.stopHotkey == nil {
            logError("Failed to parse stop hotkey: \(stopHotkey), using default")
            self.stopHotkey = HotkeyConfig.parse(Config.defaultHotkeyStop)
        }
        
        logInfo("HotkeyManager configured: start=\(self.startHotkey?.description ?? startHotkey), stop=\(self.stopHotkey?.description ?? stopHotkey)")
    }
    
    func start() {
        let mask: CGEventMask = (1 << CGEventType.keyDown.rawValue)
                              | (1 << CGEventType.keyUp.rawValue)
                              | (1 << CGEventType.flagsChanged.rawValue)
        
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
            logError("CGEventTap 创建失败 — 请在系统设置 → 隐私与安全 → 辅助功能中授权")
            return
        }
        tap = t
        src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, t, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), src, .commonModes)
        CGEvent.tapEnable(tap: t, enable: true)
        
        let startDesc = startHotkey?.description ?? Config.defaultHotkey
        let stopDesc = stopHotkey?.description ?? Config.defaultHotkeyStop
        logInfo("HotkeyManager: 键盘钩子已安装 (\(startDesc) 开始，\(stopDesc) 停止，ESC 退出)")
    }
    
    func stop() {
        if let t = tap { CGEvent.tapEnable(tap: t, enable: false) }
        if let s = src { CFRunLoopRemoveSource(CFRunLoopGetMain(), s, .commonModes) }
        tap = nil
        src = nil
        recording = false
    }
    
    func restart() {
        stop()
        start()
    }
    
    private func handle(type: CGEventType, event: CGEvent) -> Unmanaged<CGEvent>? {
        let key = CGKeyCode(event.getIntegerValueField(.keyboardEventKeycode))
        let down = (type == .keyDown)
        let up = (type == .keyUp)
        let flagsChanged = (type == .flagsChanged)
        
        // Update current modifier state
        if flagsChanged {
            currentModifiers = NSEvent.ModifierFlags(rawValue: UInt(event.flags.rawValue))
        }
        
        // ESC → cancel recording (always works)
        if key == kEsc && down {
            if recording {
                recording = false; consumed = false
                DispatchQueue.main.async { self.onCancel?() }
                return nil
            }
            return Unmanaged.passUnretained(event)
        }

        // While recording: check stop hotkey
        if recording {
            if checkStopHotkey(key: key, down: down, flags: event.flags) {
                if down {
                    recording = false; consumed = false
                    DispatchQueue.main.async { self.onStopRecording?() }
                }
                return nil
            }
            // While recording, suppress all key events to avoid interfering with input
            if down || up {
                return nil
            }
        }
        
        // Not recording: check start hotkey
        if !recording {
            if checkStartHotkey(key: key, down: down, flags: event.flags) {
                if down {
                    logInfo("Hotkey triggered → 开始录音")
                    recording = true; consumed = true
                    DispatchQueue.main.async { self.onStartRecording?() }
                }
                return nil
            }
        }
        
        return Unmanaged.passUnretained(event)
    }
    
    private func checkStartHotkey(key: CGKeyCode, down: Bool, flags: CGEventFlags) -> Bool {
        guard let config = startHotkey else {
            // Default: Option+Space
            return key == kSpace && down && flags.contains(.maskAlternate)
        }
        
        if key != config.keyCode || !down {
            return false
        }
        
        // Check modifiers
        let eventMods = NSEvent.ModifierFlags(rawValue: UInt(flags.rawValue))
        let requiredMods = config.modifiers
        
        // For modifiers, we need to check if all required modifiers are present
        if requiredMods.contains(.option) && !eventMods.contains(.option) {
            return false
        }
        if requiredMods.contains(.command) && !eventMods.contains(.command) {
            return false
        }
        if requiredMods.contains(.control) && !eventMods.contains(.control) {
            return false
        }
        if requiredMods.contains(.shift) && !eventMods.contains(.shift) {
            return false
        }
        
        return true
    }
    
    private func checkStopHotkey(key: CGKeyCode, down: Bool, flags: CGEventFlags) -> Bool {
        guard let config = stopHotkey else {
            // Default: Space (no modifiers)
            return key == kSpace && down
        }
        
        if key != config.keyCode {
            return false
        }
        
        // For stop hotkey, we might want it to work with or without modifiers
        // depending on configuration. Here we require exact match.
        let eventMods = NSEvent.ModifierFlags(rawValue: UInt(flags.rawValue))
        let requiredMods = config.modifiers
        
        if requiredMods.contains(.option) && !eventMods.contains(.option) {
            return false
        }
        if requiredMods.contains(.command) && !eventMods.contains(.command) {
            return false
        }
        if requiredMods.contains(.control) && !eventMods.contains(.control) {
            return false
        }
        if requiredMods.contains(.shift) && !eventMods.contains(.shift) {
            return false
        }
        
        // If no modifiers required, ensure no modifiers are pressed
        if requiredMods.isEmpty {
            let hasModifier = eventMods.contains(.option) || 
                             eventMods.contains(.command) || 
                             eventMods.contains(.control) || 
                             eventMods.contains(.shift)
            if hasModifier {
                return false
            }
        }
        
        return down
    }
}
