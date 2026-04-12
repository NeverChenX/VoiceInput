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
        // macOS virtual key codes follow physical keyboard layout, NOT alphabetical order
        case "a": keyCode = 0;  case "s": keyCode = 1;  case "d": keyCode = 2
        case "f": keyCode = 3;  case "h": keyCode = 4;  case "g": keyCode = 5
        case "z": keyCode = 6;  case "x": keyCode = 7;  case "c": keyCode = 8
        case "v": keyCode = 9;  case "b": keyCode = 11; case "q": keyCode = 12
        case "w": keyCode = 13; case "e": keyCode = 14; case "r": keyCode = 15
        case "y": keyCode = 16; case "t": keyCode = 17; case "o": keyCode = 31
        case "u": keyCode = 32; case "i": keyCode = 34; case "p": keyCode = 35
        case "l": keyCode = 37; case "j": keyCode = 38; case "k": keyCode = 40
        case "n": keyCode = 45; case "m": keyCode = 46
        // Number key codes are also non-sequential
        case "0": keyCode = 29; case "1": keyCode = 18; case "2": keyCode = 19
        case "3": keyCode = 20; case "4": keyCode = 21; case "5": keyCode = 23
        case "6": keyCode = 22; case "7": keyCode = 26; case "8": keyCode = 28
        case "9": keyCode = 25
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
        // Letter key codes (physical layout order)
        case 0: keyName="A"; case 1: keyName="S"; case 2: keyName="D"
        case 3: keyName="F"; case 4: keyName="H"; case 5: keyName="G"
        case 6: keyName="Z"; case 7: keyName="X"; case 8: keyName="C"
        case 9: keyName="V"; case 11: keyName="B"; case 12: keyName="Q"
        case 13: keyName="W"; case 14: keyName="E"; case 15: keyName="R"
        case 16: keyName="Y"; case 17: keyName="T"; case 31: keyName="O"
        case 32: keyName="U"; case 34: keyName="I"; case 35: keyName="P"
        case 37: keyName="L"; case 38: keyName="J"; case 40: keyName="K"
        case 45: keyName="N"; case 46: keyName="M"
        // Number key codes
        case 29: keyName="0"; case 18: keyName="1"; case 19: keyName="2"
        case 20: keyName="3"; case 21: keyName="4"; case 23: keyName="5"
        case 22: keyName="6"; case 26: keyName="7"; case 28: keyName="8"
        case 25: keyName="9"
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
