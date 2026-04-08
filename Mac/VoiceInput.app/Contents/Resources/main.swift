// VoiceInput for macOS — single-file Swift script
// Requires: Xcode Command Line Tools  (xcode-select --install)
// Run via launcher script, no compilation needed.
import AppKit
import AVFoundation
import Foundation

// ─── Paths ────────────────────────────────────────────────────────────────────
// Launcher does `cd Resources/` before exec, so currentDirectoryPath = Resources
let kResourcesDir = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
let kConfigURL    = kResourcesDir.appendingPathComponent("config.json")
let kLogURL       = kResourcesDir.appendingPathComponent("voice_input.log")

// ─── Logger ───────────────────────────────────────────────────────────────────
func viLog(_ level: String, _ msg: String) {
    let fmt = DateFormatter(); fmt.dateFormat = "yyyy-MM-dd HH:mm:ss"
    let line = "[\(fmt.string(from: Date()))] [\(level)] \(msg)\n"
    guard let data = line.data(using: .utf8) else { return }
    DispatchQueue.global(qos: .utility).async {
        if FileManager.default.fileExists(atPath: kLogURL.path),
           let fh = try? FileHandle(forWritingTo: kLogURL) {
            fh.seekToEndOfFile(); fh.write(data); try? fh.close()
        } else { try? data.write(to: kLogURL) }
    }
}
func logInfo(_ m: String)  { viLog("INFO ", m) }
func logError(_ m: String) { viLog("ERROR", m) }

// ─── Config ───────────────────────────────────────────────────────────────────
struct AppConfig: Codable {
    var app_id:                     String  = ""
    var access_token:               String  = ""
    var secret_key:                 String  = ""
    var standard_resource_id:       String? = nil
    var standard_submit_endpoint:   String? = nil
    var standard_query_endpoint:    String? = nil
    var trigger_mode:               String? = nil   // "hotkey" | "longpress"
    var hotkey:                     String? = nil   // e.g. "Alt+Space"
    var hotkey_stop:                String? = nil
    var request_timeout_seconds:    Int?    = nil
    var poll_interval_seconds:      Double? = nil
    var poll_timeout_seconds:       Double? = nil
    var auto_enter:                 Bool?   = nil

    var resourceId:     String  { standard_resource_id     ?? "volc.seedasr.auc" }
    var submitURL:      String  { standard_submit_endpoint ?? "https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit" }
    var queryURL:       String  { standard_query_endpoint  ?? "https://openspeech.bytedance.com/api/v3/auc/bigmodel/query" }
    var requestTimeout: Double  { Double(request_timeout_seconds ?? 120) }
    var pollInterval:   Double  { poll_interval_seconds ?? 1.2 }
    var pollTimeout:    Double  { poll_timeout_seconds  ?? 45.0 }
    var triggerMode:    String  { trigger_mode ?? "hotkey" }
    var startHotkey:    String  { hotkey      ?? "Alt+Space" }
    var stopHotkey:     String  { hotkey_stop ?? "Space" }
    var autoEnterOn:    Bool    { auto_enter  ?? true }
}

func loadConfig() throws -> AppConfig {
    guard FileManager.default.fileExists(atPath: kConfigURL.path) else {
        throw NSError(domain: "VI", code: 1, userInfo: [NSLocalizedDescriptionKey:
            "找不到 config.json，路径：\(kConfigURL.path)"])
    }
    let data = try Data(contentsOf: kConfigURL)
    let c = try JSONDecoder().decode(AppConfig.self, from: data)
    guard !c.app_id.isEmpty, !c.access_token.isEmpty else {
        throw NSError(domain: "VI", code: 2, userInfo: [NSLocalizedDescriptionKey:
            "config.json 缺少 app_id 或 access_token"])
    }
    return c
}

func saveConfig(_ c: AppConfig) {
    let enc = JSONEncoder(); enc.outputFormatting = [.prettyPrinted]
    if let d = try? enc.encode(c) { try? d.write(to: kConfigURL) }
}

// ─── Hotkey ───────────────────────────────────────────────────────────────────
// Standard US QWERTY key codes (consistent on all Apple hardware including M-series)
let kLetterVK: [Character: CGKeyCode] = [
    "A":0,  "S":1,  "D":2,  "F":3,  "H":4,  "G":5,  "Z":6,  "X":7,
    "C":8,  "V":9,  "B":11, "Q":12, "W":13, "E":14, "R":15, "Y":16,
    "T":17, "O":31, "U":32, "I":34, "P":35, "L":37, "J":38, "K":40,
    "N":45, "M":46,
    "1":18, "2":19, "3":20, "4":21, "5":23, "6":22, "7":26, "8":28,
    "9":25, "0":29,
]

struct HotkeyDef {
    var vk: CGKeyCode = 49  // Space
    var alt = false, ctrl = false, shift = false, cmd = false
}

let kVK_Space:   CGKeyCode = 49
let kVK_Return:  CGKeyCode = 36
let kVK_Escape:  CGKeyCode = 53
let kVK_Tab:     CGKeyCode = 48
let kVK_Delete:  CGKeyCode = 51  // Backspace
let kVK_F1:  CGKeyCode = 122; let kVK_F2:  CGKeyCode = 120
let kVK_F3:  CGKeyCode = 99;  let kVK_F4:  CGKeyCode = 118
let kVK_F5:  CGKeyCode = 96;  let kVK_F6:  CGKeyCode = 97
let kVK_F7:  CGKeyCode = 98;  let kVK_F8:  CGKeyCode = 100
let kVK_F9:  CGKeyCode = 101; let kVK_F10: CGKeyCode = 109
let kVK_F11: CGKeyCode = 103; let kVK_F12: CGKeyCode = 111
let kFKeys: [CGKeyCode] = [kVK_F1,kVK_F2,kVK_F3,kVK_F4,kVK_F5,kVK_F6,
                            kVK_F7,kVK_F8,kVK_F9,kVK_F10,kVK_F11,kVK_F12]

func parseHotkey(_ raw: String) -> HotkeyDef {
    var h = HotkeyDef(); h.alt=false; h.ctrl=false; h.shift=false; h.cmd=false
    for p in raw.uppercased().components(separatedBy: "+") {
        let t = p.trimmingCharacters(in: .whitespaces)
        switch t {
        case "ALT","OPT","OPTION": h.alt   = true
        case "CTRL","CONTROL":     h.ctrl  = true
        case "SHIFT":              h.shift = true
        case "CMD","WIN","COMMAND":h.cmd   = true
        case "SPACE":    h.vk = kVK_Space
        case "ENTER","RETURN": h.vk = kVK_Return
        case "ESC","ESCAPE":   h.vk = kVK_Escape
        case "TAB":            h.vk = kVK_Tab
        case "DELETE","BACKSPACE","BACK": h.vk = kVK_Delete
        default:
            // F1–F12
            if t.count >= 2, t.hasPrefix("F"), let n = Int(t.dropFirst()),
               n >= 1, n <= 12 { h.vk = kFKeys[n-1] }
            // Letter or digit
            else if t.count == 1, let ch = t.first, let vk = kLetterVK[ch] { h.vk = vk }
        }
    }
    return h
}

func hotkeyString(_ h: HotkeyDef) -> String {
    var s = ""
    if h.ctrl  { s += "Ctrl+" }; if h.alt { s += "Alt+" }
    if h.shift { s += "Shift+" }; if h.cmd { s += "Cmd+" }
    switch h.vk {
    case kVK_Space:  s += "Space"
    case kVK_Return: s += "Enter"
    case kVK_Escape: s += "Escape"
    case kVK_Tab:    s += "Tab"
    case kVK_Delete: s += "Backspace"
    default:
        if let i = kFKeys.firstIndex(of: h.vk) { s += "F\(i+1)" }
        else if let pair = kLetterVK.first(where: { $0.value == h.vk }) { s += String(pair.key) }
        else { s += "Key\(h.vk)" }
    }
    return s
}

// ─── Audio recorder ───────────────────────────────────────────────────────────
class AudioRecorder {
    private var engine: AVAudioEngine?
    private var buf: [Int16] = []
    private let lock = NSLock()
    private(set) var sampleRate: Double = 16000

    func start() throws {
        let eng = AVAudioEngine(); engine = eng
        let input = eng.inputNode
        sampleRate = input.inputFormat(forBus: 0).sampleRate
        let fmt = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                sampleRate: sampleRate, channels: 1, interleaved: false)!
        buf = []
        input.installTap(onBus: 0, bufferSize: 4096, format: fmt) { [weak self] b, _ in
            guard let self, let ch = b.floatChannelData?[0] else { return }
            let n = Int(b.frameLength)
            let s = (0..<n).map { Int16(max(-1.0, min(1.0, ch[$0])) * Float(Int16.max)) }
            self.lock.lock(); self.buf.append(contentsOf: s); self.lock.unlock()
        }
        try eng.start()
        logInfo("AudioRecorder: started sr=\(sampleRate)")
    }

    func stop() -> [Int16] {
        engine?.inputNode.removeTap(onBus: 0); engine?.stop(); engine = nil
        lock.lock(); let s = buf; buf = []; lock.unlock()
        logInfo("AudioRecorder: stopped samples=\(s.count)")
        return s
    }
}

func buildWav(_ samples: [Int16], sr: Int) -> Data {
    var d = Data(); let ds = samples.count * 2
    func w4(_ v: UInt32) { var x = v.littleEndian; withUnsafeBytes(of: &x){d.append(contentsOf:$0)} }
    func w2(_ v: UInt16) { var x = v.littleEndian; withUnsafeBytes(of: &x){d.append(contentsOf:$0)} }
    d.append(contentsOf: "RIFF".utf8); w4(UInt32(36+ds))
    d.append(contentsOf: "WAVE".utf8)
    d.append(contentsOf: "fmt ".utf8); w4(16); w2(1); w2(1)
    w4(UInt32(sr)); w4(UInt32(sr*2)); w2(2); w2(16)
    d.append(contentsOf: "data".utf8); w4(UInt32(ds))
    samples.withUnsafeBytes { d.append(contentsOf: $0) }
    return d
}

// ─── ASR ─────────────────────────────────────────────────────────────────────
struct VIErr: LocalizedError {
    let msg: String; init(_ m: String) { msg = m }
    var errorDescription: String? { msg }
}

class AsrClient {
    var cfg: AppConfig
    init(_ c: AppConfig) { cfg = c }

    func recognize(_ wav: Data) async throws -> String {
        let rid = UUID().uuidString.lowercased()
        logInfo("ASR submit rid=\(rid) wav=\(wav.count)B")

        var req = URLRequest(url: URL(string: cfg.submitURL)!,
                             timeoutInterval: cfg.requestTimeout)
        req.httpMethod = "POST"
        req.httpBody = try JSONSerialization.data(withJSONObject: [
            "user": ["uid": cfg.app_id],
            "audio": ["data": wav.base64EncodedString(), "format": "wav"],
            "request": ["model_name": "bigmodel", "enable_itn": true, "enable_punc": true]
        ])
        headers(&req, rid: rid)

        let (_, sr) = try await URLSession.shared.data(for: req)
        let code = (sr as! HTTPURLResponse).value(forHTTPHeaderField: "X-Api-Status-Code") ?? ""
        let msg  = (sr as! HTTPURLResponse).value(forHTTPHeaderField: "X-Api-Message") ?? ""
        logInfo("ASR submit code=\(code)")
        guard code == "20000000" else { throw VIErr("提交失败 code=\(code) \(msg)") }

        var qreq = URLRequest(url: URL(string: cfg.queryURL)!,
                              timeoutInterval: cfg.requestTimeout)
        qreq.httpMethod = "POST"
        qreq.httpBody = try JSONSerialization.data(withJSONObject: ["id": rid])
        headers(&qreq, rid: rid)

        let deadline = Date().addingTimeInterval(cfg.pollTimeout)
        var poll = 0
        while Date() < deadline {
            poll += 1
            let (qd, qr) = try await URLSession.shared.data(for: qreq)
            let qcode = (qr as! HTTPURLResponse).value(forHTTPHeaderField: "X-Api-Status-Code") ?? ""
            let body  = (try? JSONSerialization.jsonObject(with: qd) as? [String:Any]) ?? [:]
            let result = body["result"] as? [String:Any] ?? [:]
            let text   = result["text"]   as? String ?? ""
            let status = result["status"] as? String ?? ""
            logInfo("ASR poll#\(poll) code=\(qcode) text=\(text.count)ch")
            switch qcode {
            case "20000000":
                if !text.isEmpty { return text }
                if ["fail","failed","error"].contains(status) { throw VIErr("识别失败 \(status)") }
            case "20000001": break  // processing
            case "20000003": throw VIErr("未检测到有效语音")
            case "45000292":
                try await Task.sleep(nanoseconds: UInt64(max(cfg.pollInterval,2.0)*1e9))
                continue
            default:
                throw VIErr("查询失败 code=\(qcode)")
            }
            try await Task.sleep(nanoseconds: UInt64(cfg.pollInterval*1e9))
        }
        throw VIErr("ASR 超时")
    }

    private func headers(_ r: inout URLRequest, rid: String) {
        r.setValue("application/json", forHTTPHeaderField: "Content-Type")
        r.setValue(cfg.app_id,        forHTTPHeaderField: "X-Api-App-Key")
        r.setValue(cfg.access_token,  forHTTPHeaderField: "X-Api-Access-Key")
        r.setValue(cfg.resourceId,    forHTTPHeaderField: "X-Api-Resource-Id")
        r.setValue(rid,               forHTTPHeaderField: "X-Api-Request-Id")
        r.setValue("-1",              forHTTPHeaderField: "X-Api-Sequence")
    }
}

// ─── Hotkey manager ───────────────────────────────────────────────────────────
class HotkeyMgr {
    var onStart: (() -> Void)?
    var onStop:  (() -> Void)?
    var onEsc:   (() -> Void)?

    private var tap: CFMachPort?
    private var src: CFRunLoopSource?
    private(set) var recording = false

    var mode: String = "hotkey"
    var startHK = HotkeyDef()
    var stopHK  = HotkeyDef()

    // Long-press state
    private var spaceDown: Date?
    private var lpFired = false
    private let kLongPress: TimeInterval = 1.5

    func start() {
        let mask: CGEventMask = (1 << CGEventType.keyDown.rawValue)
                              | (1 << CGEventType.keyUp.rawValue)
        let cb: CGEventTapCallBack = { _, type, event, ref in
            Unmanaged<HotkeyMgr>.fromOpaque(ref!).takeUnretainedValue()
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
            logError("CGEventTap 失败 — 请授权辅助功能权限")
            DispatchQueue.main.async {
                let a = NSAlert()
                a.messageText = "需要辅助功能权限"
                a.informativeText = "请前往：系统设置 → 隐私与安全 → 辅助功能 → 允许 VoiceInput（或 Terminal/swift）"
                a.addButton(withTitle: "好的")
                a.runModal()
            }
            return
        }
        tap = t
        src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, t, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), src, .commonModes)
        CGEvent.tapEnable(tap: t, enable: true)
        logInfo("HotkeyMgr: tap installed mode=\(mode)")
    }

    func setRecording(_ r: Bool) { recording = r }

    private func handle(type: CGEventType, event: CGEvent) -> Unmanaged<CGEvent>? {
        guard type == .keyDown || type == .keyUp else { return Unmanaged.passRetained(event) }
        let kc   = CGKeyCode(event.getIntegerValueField(.keyboardEventKeycode))
        let down = type == .keyDown
        let up   = type == .keyUp

        // ESC cancels recording/transcribing state
        if kc == kVK_Escape && down {
            if recording {
                recording = false; spaceDown = nil; lpFired = false
                DispatchQueue.main.async { self.onEsc?() }
                return nil
            }
            return Unmanaged.passRetained(event)
        }

        // ── Long-press mode ───────────────────────────────────────────────
        if mode == "longpress" {
            guard kc == kVK_Space else { return Unmanaged.passRetained(event) }
            if recording {
                if down {
                    recording = false; spaceDown = nil; lpFired = false
                    DispatchQueue.main.async { self.onStop?() }
                }
                return nil
            }
            if down {
                guard spaceDown == nil else { return nil }
                spaceDown = Date(); lpFired = false
                let t0 = spaceDown!
                DispatchQueue.global().asyncAfter(deadline: .now() + kLongPress) { [weak self] in
                    guard let self, self.spaceDown != nil,
                          abs(self.spaceDown!.timeIntervalSince(t0)) < 0.01,
                          !self.lpFired else { return }
                    self.lpFired = true
                    DispatchQueue.main.async { self.onStart?() }
                }
                return nil
            }
            if up {
                let held = spaceDown.map { Date().timeIntervalSince($0) } ?? 0
                spaceDown = nil
                if lpFired { lpFired = false; return nil }
                if held >= kLongPress - 0.2 {
                    lpFired = true
                    DispatchQueue.main.async { self.onStart?() }
                    return nil
                }
                // Short press — re-inject space
                injectKey(kVK_Space, flags: [])
                return nil
            }
            return Unmanaged.passRetained(event)
        }

        // ── Hotkey mode ───────────────────────────────────────────────────
        if recording {
            if down && (matches(event, stopHK) || matches(event, startHK)) {
                recording = false
                DispatchQueue.main.async { self.onStop?() }
                return nil
            }
            if kc == startHK.vk || kc == stopHK.vk { return nil }
            return Unmanaged.passRetained(event)
        }
        if down && matches(event, startHK) {
            DispatchQueue.main.async { self.onStart?() }
            return nil
        }
        // Suppress paired key-up for start hotkey
        if up && kc == startHK.vk && !event.flags.contains(.maskAlternate) &&
           !event.flags.contains(.maskControl) && !event.flags.contains(.maskCommand) &&
           !event.flags.contains(.maskShift) { /* let through */ }

        return Unmanaged.passRetained(event)
    }

    private func matches(_ event: CGEvent, _ h: HotkeyDef) -> Bool {
        let kc = CGKeyCode(event.getIntegerValueField(.keyboardEventKeycode))
        guard kc == h.vk else { return false }
        let f = event.flags
        return f.contains(.maskAlternate) == h.alt
            && f.contains(.maskControl)   == h.ctrl
            && f.contains(.maskShift)     == h.shift
            && f.contains(.maskCommand)   == h.cmd
    }

    private func injectKey(_ vk: CGKeyCode, flags: CGEventFlags) {
        let src = CGEventSource(stateID: .hidSystemState)
        let dn = CGEvent(keyboardEventSource: src, virtualKey: vk, keyDown: true)!
        let up = CGEvent(keyboardEventSource: src, virtualKey: vk, keyDown: false)!
        dn.flags = flags; up.flags = flags
        dn.post(tap: .cghidEventTap); up.post(tap: .cghidEventTap)
    }
}

// ─── Status view (200×200) ────────────────────────────────────────────────────
enum VIState { case idle, recording, transcribing }

class StatusView: NSView {
    var viState: VIState = .idle { didSet { needsDisplay = true } }
    var autoEnter = true  { didSet { needsDisplay = true } }
    var hint = "Alt+Space"{ didSet { needsDisplay = true } }

    var onToggle: (() -> Void)?
    var onGear:   (() -> Void)?

    private var gearRect = NSRect.zero
    private var dragOrigin: NSPoint?
    private var winOrigin:  NSPoint?
    private var didDrag = false

    override func draw(_ dirtyRect: NSRect) {
        let W = bounds.width, H = bounds.height, cx = W/2

        // Background
        NSColor(red:18/255, green:18/255, blue:24/255, alpha:0.97).setFill()
        NSBezierPath(roundedRect: bounds, xRadius: 14, yRadius: 14).fill()

        let kDiv1: CGFloat = H * 0.44
        let kDiv2: CGFloat = H * 0.27

        // ── Zone A: dot + label ──────────────────────────────────────────
        let dotR: CGFloat = 20, dotCY = H - (H * 0.56 / 2) - 8
        let (dotCol, lbl): (NSColor, String) = {
            switch viState {
            case .idle:         return (NSColor(white:0.45, alpha:1),                               "待机")
            case .recording:    return (NSColor(red:1.0, green:0.27, blue:0.27, alpha:1),           "录音")
            case .transcribing: return (NSColor(red:0.27, green:0.55, blue:1.0, alpha:1),           "识别")
            }
        }()
        dotCol.setFill()
        NSBezierPath(ovalIn: NSRect(x:cx-dotR, y:dotCY-dotR, width:dotR*2, height:dotR*2)).fill()

        let sa = NSAttributedString(string: lbl, attributes: [
            .font: NSFont.boldSystemFont(ofSize: 22),
            .foregroundColor: NSColor(white:0.9, alpha:1)
        ])
        sa.draw(at: NSPoint(x: cx - sa.size().width/2, y: dotCY - dotR - 28))

        // Dividers
        func drawDiv(_ y: CGFloat) {
            NSColor(white:0.18, alpha:1).setStroke()
            let p = NSBezierPath(); p.lineWidth = 0.5
            p.move(to: NSPoint(x:14, y:y)); p.line(to: NSPoint(x:W-14, y:y)); p.stroke()
        }
        drawDiv(kDiv1); drawDiv(kDiv2)

        // ── Zone B: auto-enter toggle ─────────────────────────────────────
        let zbCY = (kDiv1 + kDiv2) / 2
        NSAttributedString(string: "自动发送", attributes: [
            .font: NSFont.systemFont(ofSize: 13),
            .foregroundColor: NSColor(white:0.82, alpha:1)
        ]).draw(at: NSPoint(x:14, y: zbCY - 8))

        let tw: CGFloat = 50, th: CGFloat = 26
        let tx = W - tw - 14, ty = zbCY - th/2
        (autoEnter ? NSColor(red:0.19, green:0.75, blue:0.39, alpha:1)
                   : NSColor(white:0.22, alpha:1)).setFill()
        NSBezierPath(roundedRect: NSRect(x:tx, y:ty, width:tw, height:th),
                     xRadius: th/2, yRadius: th/2).fill()
        let tr = th/2 - 3
        let tcx = autoEnter ? (tx+tw-tr-4) : (tx+tr+4)
        NSColor.white.setFill()
        NSBezierPath(ovalIn: NSRect(x:tcx-tr, y:zbCY-tr, width:tr*2, height:tr*2)).fill()

        // ── Zone C: hint + gear ───────────────────────────────────────────
        let zcCY = kDiv2 / 2
        let gw: CGFloat = 30, gh: CGFloat = 24
        gearRect = NSRect(x: W-gw-10, y: zcCY-gh/2, width: gw, height: gh)
        NSColor(red:36/255, green:38/255, blue:50/255, alpha:1).setFill()
        let gp = NSBezierPath(roundedRect: gearRect, xRadius: 5, yRadius: 5); gp.fill()
        NSColor(white:0.28, alpha:1).setStroke(); gp.lineWidth = 0.5; gp.stroke()
        NSAttributedString(string: "⚙", attributes: [
            .font: NSFont.systemFont(ofSize: 14),
            .foregroundColor: NSColor(white:0.66, alpha:1)
        ]).draw(at: NSPoint(x: gearRect.midX-8, y: gearRect.midY-8))

        NSAttributedString(string: hint, attributes: [
            .font: NSFont.systemFont(ofSize: 11),
            .foregroundColor: NSColor(white:0.35, alpha:1)
        ]).draw(at: NSPoint(x:10, y: zcCY - 7))
    }

    override func mouseDown(with e: NSEvent) {
        dragOrigin = e.locationInWindow; winOrigin = window?.frame.origin; didDrag = false
    }
    override func mouseDragged(with e: NSEvent) {
        guard let s = dragOrigin, let o = winOrigin, let w = window else { return }
        let dx = e.locationInWindow.x - s.x, dy = e.locationInWindow.y - s.y
        if abs(dx) > 4 || abs(dy) > 4 { didDrag = true }
        if didDrag { w.setFrameOrigin(NSPoint(x: o.x+dx, y: o.y+dy)) }
    }
    override func mouseUp(with e: NSEvent) {
        defer { dragOrigin = nil }
        guard !didDrag else { return }
        if gearRect.contains(convert(e.locationInWindow, from: nil)) { onGear?() }
        else { onToggle?() }
    }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override var acceptsFirstResponder: Bool { true }
}

class StatusWC: NSWindowController {
    let sv: StatusView
    var onToggle: ((Bool) -> Void)?
    var onGear:   (() -> Void)?

    init() {
        let sz = NSSize(width:200, height:200)
        let sc = NSScreen.main ?? NSScreen.screens[0]
        let win = NSWindow(
            contentRect: NSRect(x: sc.visibleFrame.maxX-sz.width-12,
                                y: sc.visibleFrame.maxY-sz.height-12,
                                width: sz.width, height: sz.height),
            styleMask: [.borderless], backing: .buffered, defer: false)
        win.isOpaque = false; win.backgroundColor = .clear
        win.level = .floating; win.hasShadow = true; win.isMovable = false
        win.collectionBehavior = [.canJoinAllSpaces, .stationary]
        sv = StatusView(frame: NSRect(origin: .zero, size: sz)); win.contentView = sv
        super.init(window: win)
        sv.onToggle = { [weak self] in
            guard let self else { return }
            self.sv.autoEnter.toggle()
            self.onToggle?(self.sv.autoEnter)
        }
        sv.onGear = { [weak self] in self?.onGear?() }
        win.orderFrontRegardless()
    }
    required init?(coder: NSCoder) { fatalError() }
    func update(_ s: VIState) { DispatchQueue.main.async { self.sv.viState = s } }
}

// ─── Overlay ──────────────────────────────────────────────────────────────────
class OverlayWC: NSWindowController {
    private let main: NSTextField
    private let sub:  NSTextField
    init() {
        let sc = NSScreen.main ?? NSScreen.screens[0]
        let win = NSWindow(contentRect: sc.frame, styleMask: [.borderless],
                           backing: .buffered, defer: false)
        win.isOpaque = false
        win.backgroundColor = NSColor.black.withAlphaComponent(0.88)
        win.level = .screenSaver; win.ignoresMouseEvents = true
        win.collectionBehavior = [.canJoinAllSpaces]
        main = NSTextField(labelWithString: ""); main.font = .boldSystemFont(ofSize: 52)
        main.textColor = NSColor(white:0.96, alpha:1); main.alignment = .center
        main.translatesAutoresizingMaskIntoConstraints = false
        sub  = NSTextField(labelWithString: ""); sub.font  = .systemFont(ofSize: 18)
        sub.textColor  = NSColor(white:0.79, alpha:1); sub.alignment  = .center
        sub.translatesAutoresizingMaskIntoConstraints = false
        super.init(window: win)
        let c = win.contentView!; c.addSubview(main); c.addSubview(sub)
        NSLayoutConstraint.activate([
            main.centerXAnchor.constraint(equalTo: c.centerXAnchor),
            main.centerYAnchor.constraint(equalTo: c.centerYAnchor, constant: -20),
            sub.centerXAnchor.constraint(equalTo: c.centerXAnchor),
            sub.topAnchor.constraint(equalTo: main.bottomAnchor, constant: 12),
        ])
    }
    required init?(coder: NSCoder) { fatalError() }
    func show(_ m: String, hint: String) {
        main.stringValue = m; sub.stringValue = hint; window?.orderFrontRegardless()
    }
    func hide() { window?.orderOut(nil) }
}

// ─── Config dialog ───────────────────────────────────────────────────────────
class ConfigWC: NSWindowController, NSWindowDelegate {
    var cfg: AppConfig
    var onSave: ((AppConfig) -> Void)?

    private var fAppId  = NSTextField()
    private var fToken  = NSTextField()
    private var fSecret = NSTextField()
    private var cLongpress: NSButton!
    private var fHotkey = NSTextField()
    private var fStop   = NSTextField()
    private var cAutoEnter: NSButton!

    init(_ c: AppConfig) {
        cfg = c
        let w = NSWindow(contentRect: NSRect(x:0,y:0,width:480,height:420),
                         styleMask: [.titled,.closable], backing: .buffered, defer: false)
        w.title = "VoiceInput 配置"; w.center()
        super.init(window: w); w.delegate = self; setup()
    }
    required init?(coder: NSCoder) { fatalError() }

    private func field(_ v: String) -> NSTextField {
        let f = NSTextField(string: v); f.translatesAutoresizingMaskIntoConstraints = false
        return f
    }
    private func lbl(_ s: String) -> NSTextField {
        let f = NSTextField(labelWithString: s); f.alignment = .right
        f.translatesAutoresizingMaskIntoConstraints = false; return f
    }
    private func chk(_ t: String, on: Bool) -> NSButton {
        let b = NSButton(checkboxWithTitle: t, target: nil, action: nil)
        b.state = on ? .on : .off; b.translatesAutoresizingMaskIntoConstraints = false
        return b
    }

    private func setup() {
        guard let cv = window?.contentView else { return }
        fAppId  = field(cfg.app_id)
        fToken  = field(cfg.access_token)
        fSecret = field(cfg.secret_key)
        cLongpress = chk("长按空格 1.5 秒触发（与快捷键二选一）", on: cfg.triggerMode == "longpress")
        fHotkey = field(cfg.startHotkey)
        fStop   = field(cfg.stopHotkey)
        cAutoEnter = chk("识别后自动按 Return 发送", on: cfg.autoEnterOn)

        let lw: CGFloat = 100, fx: CGFloat = 108+16, fw: CGFloat = 330
        var y: CGFloat = 18; let rh: CGFloat = 30, ry: CGFloat = 42

        func row(_ l: String, _ f: NSTextField) {
            let lb = lbl(l); cv.addSubview(lb); cv.addSubview(f)
            NSLayoutConstraint.activate([
                lb.leadingAnchor.constraint(equalTo: cv.leadingAnchor, constant: 16),
                lb.widthAnchor.constraint(equalToConstant: lw),
                lb.centerYAnchor.constraint(equalTo: cv.bottomAnchor, constant: -(y+rh/2)),
                f.leadingAnchor.constraint(equalTo: cv.leadingAnchor, constant: fx),
                f.widthAnchor.constraint(equalToConstant: fw),
                f.centerYAnchor.constraint(equalTo: cv.bottomAnchor, constant: -(y+rh/2)),
                f.heightAnchor.constraint(equalToConstant: rh),
            ]); y += ry
        }
        func addChk(_ b: NSButton) {
            cv.addSubview(b)
            NSLayoutConstraint.activate([
                b.leadingAnchor.constraint(equalTo: cv.leadingAnchor, constant: fx),
                b.centerYAnchor.constraint(equalTo: cv.bottomAnchor, constant: -(y+rh/2)),
            ]); y += ry
        }

        row("App ID:", fAppId)
        row("Access Token:", fToken)
        row("Secret Key:", fSecret)
        y += 8; addChk(cLongpress); y += 4
        row("开始快捷键:", fHotkey)
        row("停止快捷键:", fStop)
        y += 4; addChk(cAutoEnter); y += 12

        let bSave = NSButton(title: "保存", target: self, action: #selector(doSave))
        bSave.bezelStyle = .rounded; bSave.keyEquivalent = "\r"
        bSave.translatesAutoresizingMaskIntoConstraints = false
        let bCancel = NSButton(title: "取消", target: self, action: #selector(doCancel))
        bCancel.bezelStyle = .rounded; bCancel.keyEquivalent = "\u{1b}"
        bCancel.translatesAutoresizingMaskIntoConstraints = false
        cv.addSubview(bSave); cv.addSubview(bCancel)
        NSLayoutConstraint.activate([
            bSave.trailingAnchor.constraint(equalTo: cv.trailingAnchor, constant: -16),
            bSave.bottomAnchor.constraint(equalTo: cv.bottomAnchor, constant: -(y-ry/2)),
            bSave.widthAnchor.constraint(equalToConstant: 80),
            bCancel.trailingAnchor.constraint(equalTo: bSave.leadingAnchor, constant: -10),
            bCancel.bottomAnchor.constraint(equalTo: bSave.bottomAnchor),
            bCancel.widthAnchor.constraint(equalToConstant: 80),
        ])
        cLongpress.target = self; cLongpress.action = #selector(toggleLP)
        toggleLP()
    }

    @objc private func toggleLP() {
        let lp = cLongpress.state == .on
        fHotkey.isEnabled = !lp; fStop.isEnabled = !lp
    }
    @objc private func doSave() {
        var c = cfg
        c.app_id        = fAppId.stringValue
        c.access_token  = fToken.stringValue
        c.secret_key    = fSecret.stringValue
        c.trigger_mode  = cLongpress.state == .on ? "longpress" : "hotkey"
        c.hotkey        = fHotkey.stringValue
        c.hotkey_stop   = fStop.stringValue
        c.auto_enter    = cAutoEnter.state == .on
        onSave?(c); close()
    }
    @objc private func doCancel() { close() }
}

// ─── App controller ───────────────────────────────────────────────────────────
class AppCtrl {
    var cfg: AppConfig
    var state: VIState = .idle { didSet { statusWC.update(state) } }
    var autoEnter: Bool

    let recorder  = AudioRecorder()
    var asr:        AsrClient
    let hkMgr     = HotkeyMgr()
    let statusWC  = StatusWC()
    var overlayWC: OverlayWC?
    var configWC:  ConfigWC?

    init(_ c: AppConfig) {
        cfg = c; asr = AsrClient(c); autoEnter = c.autoEnterOn
    }

    func start() {
        applyConfig()
        statusWC.sv.autoEnter = autoEnter
        statusWC.onToggle = { [weak self] v in self?.autoEnter = v }
        statusWC.onGear   = { [weak self] in self?.openConfig() }
        hkMgr.onStart = { [weak self] in self?.startRec() }
        hkMgr.onStop  = { [weak self] in self?.stopRec() }
        hkMgr.onEsc   = { [weak self] in self?.cancelState() }
        hkMgr.start()
        logInfo("AppCtrl ready mode=\(cfg.triggerMode) hotkey=\(cfg.startHotkey)")
    }

    private func applyConfig() {
        hkMgr.mode    = cfg.triggerMode
        hkMgr.startHK = parseHotkey(cfg.startHotkey)
        hkMgr.stopHK  = parseHotkey(cfg.stopHotkey)
        let hint = cfg.triggerMode == "longpress" ? "长按空格"
                                                  : hotkeyString(hkMgr.startHK)
        statusWC.sv.hint = hint; statusWC.sv.needsDisplay = true
    }

    private func startRec() {
        guard state == .idle else { return }
        do { try recorder.start() } catch { showError(error.localizedDescription); return }
        state = .recording; hkMgr.setRecording(true)
        showOv("正在聆听", hint: "按空格停止")
    }

    private func stopRec() {
        guard state == .recording else { return }
        state = .transcribing; hkMgr.setRecording(false)
        showOv("识别中", hint: "请稍候…")
        let s = recorder.stop(); let sr = Int(recorder.sampleRate)
        guard !s.isEmpty else { showError("未捕获到音频，请检查麦克风权限"); return }
        guard s.count >= Int(Double(sr)*0.25) else { showError("录音太短"); return }
        let wav = buildWav(s, sr: sr)
        Task { [weak self] in
            guard let self else { return }
            do {
                let text = try await asr.recognize(wav)
                await MainActor.run { self.paste(text) }
            } catch {
                logError(error.localizedDescription)
                await MainActor.run { self.showError(error.localizedDescription) }
            }
        }
    }

    private func cancelState() {
        hkMgr.setRecording(false); _ = recorder.stop()
        state = .idle; hideOv()
        logInfo("State cancelled by ESC")
    }

    @MainActor
    private func paste(_ text: String) {
        state = .idle; hideOv()
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        sendKey(9, flags: .maskCommand)   // Cmd+V
        if autoEnter {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { self.sendKey(36, flags: []) }
        }
    }

    private func sendKey(_ vk: CGKeyCode, flags: CGEventFlags) {
        let src = CGEventSource(stateID: .hidSystemState)
        let dn = CGEvent(keyboardEventSource: src, virtualKey: vk, keyDown: true)!
        let up = CGEvent(keyboardEventSource: src, virtualKey: vk, keyDown: false)!
        dn.flags = flags; up.flags = flags
        dn.post(tap: .cghidEventTap); up.post(tap: .cghidEventTap)
    }

    private func showOv(_ m: String, hint: String) {
        if overlayWC == nil { overlayWC = OverlayWC() }
        overlayWC?.show(m, hint: hint)
    }
    private func hideOv() { overlayWC?.hide() }
    private func showError(_ m: String) {
        state = .idle
        showOv("错误", hint: m)
        DispatchQueue.main.asyncAfter(deadline: .now()+2.6) { self.hideOv() }
    }

    func openConfig() {
        if configWC?.window?.isVisible == true {
            configWC?.window?.makeKeyAndOrderFront(nil); return
        }
        let wc = ConfigWC(cfg)
        wc.onSave = { [weak self] newCfg in
            guard let self else { return }
            self.cfg = newCfg; self.autoEnter = newCfg.autoEnterOn
            self.asr = AsrClient(newCfg)
            self.statusWC.sv.autoEnter = self.autoEnter
            self.applyConfig(); saveConfig(newCfg)
            logInfo("Config saved")
        }
        configWC = wc
        NSApp.activate(ignoringOtherApps: true); wc.showWindow(nil)
    }
}

// ─── App delegate + entry point ───────────────────────────────────────────────
class AppDelegate: NSObject, NSApplicationDelegate {
    var ctrl: AppCtrl?

    func applicationDidFinishLaunching(_ n: Notification) {
        logInfo("════════════════════════════════")
        logInfo("VoiceInput (macOS/M-series) starting")
        logInfo("WorkingDir: \(kResourcesDir.path)")

        // Request mic permission
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized: logInfo("Mic: authorized")
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .audio) { ok in logInfo("Mic: \(ok)") }
        default: logError("Mic: DENIED — 请授权麦克风")
        }

        do {
            let c = try loadConfig()
            logInfo("Config OK app_id=\(c.app_id)")
            ctrl = AppCtrl(c); ctrl?.start()
        } catch {
            logError(error.localizedDescription)
            let a = NSAlert()
            a.messageText = "VoiceInput — 配置错误"
            a.informativeText = error.localizedDescription
            a.addButton(withTitle: "退出"); a.runModal()
            NSApp.terminate(nil)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ s: NSApplication) -> Bool { false }
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let del = AppDelegate()
app.delegate = del
app.run()
