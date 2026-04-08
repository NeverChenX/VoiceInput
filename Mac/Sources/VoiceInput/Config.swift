import Foundation

struct Config: Codable {
    let app_id: String
    let access_token: String
    let secret_key: String?
    let standard_resource_id: String?
    let standard_submit_endpoint: String?
    let standard_query_endpoint: String?
    let request_timeout_seconds: Int?
    let poll_interval_seconds: Double?
    let poll_timeout_seconds: Double?

    var resourceId: String      { standard_resource_id ?? "volc.seedasr.auc" }
    var submitURL: String       { standard_submit_endpoint ?? "https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit" }
    var queryURL: String        { standard_query_endpoint  ?? "https://openspeech.bytedance.com/api/v3/auc/bigmodel/query" }
    var requestTimeout: Double  { Double(request_timeout_seconds ?? 120) }
    var pollInterval: Double    { poll_interval_seconds ?? 1.2 }
    var pollTimeout: Double     { poll_timeout_seconds ?? 45.0 }

    static func load() throws -> Config {
        let exe = URL(fileURLWithPath: CommandLine.arguments[0])
        let url = exe.deletingLastPathComponent().appendingPathComponent("config.json")
        guard FileManager.default.fileExists(atPath: url.path) else {
            throw ConfigError.notFound(url.path)
        }
        let c = try JSONDecoder().decode(Config.self, from: Data(contentsOf: url))
        guard !c.app_id.isEmpty, !c.access_token.isEmpty else {
            throw ConfigError.missingCredentials
        }
        return c
    }

    enum ConfigError: LocalizedError {
        case notFound(String), missingCredentials
        var errorDescription: String? {
            switch self {
            case .notFound(let p): return "找不到 config.json：\(p)"
            case .missingCredentials: return "config.json 缺少 app_id 或 access_token"
            }
        }
    }
}
