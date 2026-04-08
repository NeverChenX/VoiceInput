import Foundation
import AppKit

struct Config: Codable {
    var app_id: String
    var access_token: String
    var secret_key: String?
    var standard_resource_id: String?
    var standard_submit_endpoint: String?
    var standard_query_endpoint: String?
    var request_timeout_seconds: Int?
    var poll_interval_seconds: Double?
    var poll_timeout_seconds: Double?
    var hotkey: String?
    var hotkey_stop: String?
    var auto_enter: Bool?
    
    // Default values
    static let defaultHotkey = "Option+Space"
    static let defaultHotkeyStop = "Space"
    static let defaultAutoEnter = true
    
    var resourceId: String      { standard_resource_id ?? "volc.seedasr.auc" }
    var submitURL: String       { standard_submit_endpoint ?? "https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit" }
    var queryURL: String        { standard_query_endpoint  ?? "https://openspeech.bytedance.com/api/v3/auc/bigmodel/query" }
    var requestTimeout: Double  { Double(request_timeout_seconds ?? 120) }
    var pollInterval: Double    { poll_interval_seconds ?? 1.2 }
    var pollTimeout: Double     { poll_timeout_seconds ?? 45.0 }
    var hotkeyStart: String     { hotkey ?? Self.defaultHotkey }
    var hotkeyStopValue: String { hotkey_stop ?? Self.defaultHotkeyStop }
    var autoEnterValue: Bool    { auto_enter ?? Self.defaultAutoEnter }
    
    static var configURL: URL {
        let exe = URL(fileURLWithPath: CommandLine.arguments[0])
        return exe.deletingLastPathComponent().appendingPathComponent("config.json")
    }
    
    static func load() throws -> Config {
        guard FileManager.default.fileExists(atPath: configURL.path) else {
            throw ConfigError.notFound(configURL.path)
        }
        let c = try JSONDecoder().decode(Config.self, from: Data(contentsOf: configURL))
        guard !c.app_id.isEmpty, !c.access_token.isEmpty else {
            throw ConfigError.missingCredentials
        }
        return c
    }
    
    func save() throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(self)
        try data.write(to: Self.configURL)
        logInfo("Config saved to \(Self.configURL.path)")
    }
    
    // Create a copy with new values
    func with(
        appId: String? = nil,
        accessToken: String? = nil,
        secretKey: String? = nil,
        hotkey: String? = nil,
        hotkeyStop: String? = nil,
        autoEnter: Bool? = nil
    ) -> Config {
        var copy = self
        if let v = appId { copy.app_id = v }
        if let v = accessToken { copy.access_token = v }
        if let v = secretKey { copy.secret_key = v }
        if let v = hotkey { copy.hotkey = v }
        if hotkeyStop != nil { copy.hotkey_stop = hotkeyStop }
        if let v = autoEnter { copy.auto_enter = v }
        return copy
    }
    
    enum ConfigError: LocalizedError {
        case notFound(String), missingCredentials, saveFailed(String)
        var errorDescription: String? {
            switch self {
            case .notFound(let p): return "找不到 config.json：\(p)"
            case .missingCredentials: return "config.json 缺少 app_id 或 access_token"
            case .saveFailed(let m): return "保存配置失败：\(m)"
            }
        }
    }
}

// MARK: - Hotkey Parsing

struct HotkeyConfig {
    let modifiers: NSEvent.ModifierFlags
    let keyCode: CGKeyCode
    let description: String
    
    static func parse(_ string: String) -> HotkeyConfig? {
        let parts = string.split(separator: "+").map { $0.trimmingCharacters(in: .whitespaces) }
        guard parts.count >= 1 else { return nil }
        
        var modifiers: NSEvent.ModifierFlags = []
        let keyPart = parts.last!
        
        for part in parts.dropLast() {
            switch part.lowercased() {
            case "option", "opt", "alt":
                modifiers.insert(.option)
            case "command", "cmd":
                modifiers.insert(.command)
            case "control", "ctrl":
                modifiers.insert(.control)
            case "shift":
                modifiers.insert(.shift)
            default:
                break
            }
        }
        
        let keyCode: CGKeyCode
        switch keyPart.lowercased() {
        case "space": keyCode = 49
        case "return", "enter": keyCode = 36
        case "esc", "escape": keyCode = 53
        case "tab": keyCode = 48
        case "delete", "backspace": keyCode = 51
        case "a"..."z":
            keyCode = CGKeyCode(0 + keyPart.unicodeScalars.first!.value - Character("a").unicodeScalars.first!.value)
        case "0"..."9":
            keyCode = CGKeyCode(29 + keyPart.unicodeScalars.first!.value - Character("0").unicodeScalars.first!.value)
        case "f1": keyCode = 122
        case "f2": keyCode = 120
        case "f3": keyCode = 99
        case "f4": keyCode = 118
        case "f5": keyCode = 96
        case "f6": keyCode = 97
        case "f7": keyCode = 98
        case "f8": keyCode = 100
        case "f9": keyCode = 101
        case "f10": keyCode = 109
        case "f11": keyCode = 103
        case "f12": keyCode = 111
        default:
            return nil
        }
        
        return HotkeyConfig(modifiers: modifiers, keyCode: keyCode, description: string)
    }
    
    static func string(from flags: NSEvent.ModifierFlags, keyCode: CGKeyCode) -> String {
        var parts: [String] = []
        
        if flags.contains(.control) { parts.append("Ctrl") }
        if flags.contains(.option) { parts.append("Option") }
        if flags.contains(.shift) { parts.append("Shift") }
        if flags.contains(.command) { parts.append("Cmd") }
        
        let keyName: String
        switch keyCode {
        case 49: keyName = "Space"
        case 36: keyName = "Return"
        case 53: keyName = "Esc"
        case 48: keyName = "Tab"
        case 51: keyName = "Delete"
        case 0...25: keyName = String(Character(UnicodeScalar(65 + keyCode)!))
        case 29...38: keyName = String(Character(UnicodeScalar(48 + keyCode - 29)!))
        case 122: keyName = "F1"
        case 120: keyName = "F2"
        case 99: keyName = "F3"
        case 118: keyName = "F4"
        case 96: keyName = "F5"
        case 97: keyName = "F6"
        case 98: keyName = "F7"
        case 100: keyName = "F8"
        case 101: keyName = "F9"
        case 109: keyName = "F10"
        case 103: keyName = "F11"
        case 111: keyName = "F12"
        default: keyName = "Key\(keyCode)"
        }
        
        parts.append(keyName)
        return parts.joined(separator: "+")
    }
}
