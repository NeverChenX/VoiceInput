import Foundation

class Logger {
    static let shared = Logger()
    private let logURL: URL
    private let queue = DispatchQueue(label: "com.voiceinput.logger")

    private init() {
        let exe = URL(fileURLWithPath: CommandLine.arguments[0])
        logURL = exe.deletingLastPathComponent().appendingPathComponent("voice_input.log")
    }

    func write(_ level: String, _ message: String) {
        let fmt = DateFormatter()
        fmt.dateFormat = "yyyy-MM-dd HH:mm:ss"
        let line = "[\(fmt.string(from: Date()))] [\(level)] \(message)\n"
        queue.async {
            guard let data = line.data(using: .utf8) else { return }
            if FileManager.default.fileExists(atPath: self.logURL.path),
               let fh = try? FileHandle(forWritingTo: self.logURL) {
                fh.seekToEndOfFile(); fh.write(data); fh.closeFile()
            } else {
                try? data.write(to: self.logURL)
            }
        }
    }

    func info(_ m: String)  { write("INFO ", m) }
    func warn(_ m: String)  { write("WARN ", m) }
    func error(_ m: String) { write("ERROR", m) }
}

func logInfo(_ m: String)  { Logger.shared.info(m) }
func logWarn(_ m: String)  { Logger.shared.warn(m) }
func logError(_ m: String) { Logger.shared.error(m) }
