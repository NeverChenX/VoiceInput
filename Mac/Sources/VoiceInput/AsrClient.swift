import Foundation

class AsrClient {
    let config: Config

    init(config: Config) { self.config = config }

    func recognize(_ wav: Data) async throws -> String {
        let rid = UUID().uuidString.lowercased()
        logInfo("ASR recognize: request_id=\(rid) wav_bytes=\(wav.count)")

        // ── Submit ────────────────────────────────────────────────────────
        let body: [String: Any] = [
            "user": ["uid": config.app_id],
            "audio": ["data": wav.base64EncodedString(), "format": "wav"],
            "request": ["model_name": "bigmodel", "enable_itn": true, "enable_punc": true]
        ]
        let submitData = try JSONSerialization.data(withJSONObject: body)
        var req = URLRequest(url: URL(string: config.submitURL)!, timeoutInterval: config.requestTimeout)
        req.httpMethod = "POST"; req.httpBody = submitData
        setHeaders(&req, rid: rid)

        logInfo("ASR submit: \(config.submitURL) body_bytes=\(submitData.count)")
        let (_, submitResp) = try await URLSession.shared.data(for: req)
        let hr = submitResp as! HTTPURLResponse
        let code = hr.value(forHTTPHeaderField: "X-Api-Status-Code") ?? ""
        let msg  = hr.value(forHTTPHeaderField: "X-Api-Message") ?? ""
        logInfo("ASR submit response: code=\(code) msg=\(msg)")
        guard code == "20000000" else {
            throw ASRError.submit("code=\(code) msg=\(msg)")
        }
        logInfo("ASR submit OK")

        // ── Poll ──────────────────────────────────────────────────────────
        let deadline = Date().addingTimeInterval(config.pollTimeout)
        var poll = 0
        var qreq = URLRequest(url: URL(string: config.queryURL)!, timeoutInterval: config.requestTimeout)
        qreq.httpMethod = "POST"
        qreq.httpBody = try JSONSerialization.data(withJSONObject: ["id": rid])
        setHeaders(&qreq, rid: rid)

        while Date() < deadline {
            poll += 1
            let (qdata, qresp) = try await URLSession.shared.data(for: qreq)
            let qhr  = qresp as! HTTPURLResponse
            let qcode = qhr.value(forHTTPHeaderField: "X-Api-Status-Code") ?? ""
            let qmsg  = qhr.value(forHTTPHeaderField: "X-Api-Message") ?? ""
            let body  = (try? JSONSerialization.jsonObject(with: qdata) as? [String: Any]) ?? [:]
            let result = body["result"] as? [String: Any] ?? [:]
            let text   = result["text"] as? String ?? ""
            let status = result["status"] as? String ?? ""
            logInfo("ASR poll #\(poll) code=\(qcode) status=\(status) text_len=\(text.count)")

            switch qcode {
            case "20000000":
                if !text.isEmpty { logInfo("ASR success"); return text }
                if ["fail","failed","error"].contains(status) { throw ASRError.failed(status) }
            case "20000001": logInfo("ASR processing...")
            case "20000003": throw ASRError.noSpeech
            case "45000292":
                try await Task.sleep(nanoseconds: UInt64(max(config.pollInterval, 2.0) * 1e9))
                continue
            default: throw ASRError.query("code=\(qcode) msg=\(qmsg)")
            }
            try await Task.sleep(nanoseconds: UInt64(config.pollInterval * 1e9))
        }
        throw ASRError.timeout
    }

    private func setHeaders(_ req: inout URLRequest, rid: String) {
        req.setValue("application/json",   forHTTPHeaderField: "Content-Type")
        req.setValue(config.app_id,        forHTTPHeaderField: "X-Api-App-Key")
        req.setValue(config.access_token,  forHTTPHeaderField: "X-Api-Access-Key")
        req.setValue(config.resourceId,    forHTTPHeaderField: "X-Api-Resource-Id")
        req.setValue(rid,                  forHTTPHeaderField: "X-Api-Request-Id")
        req.setValue("-1",                 forHTTPHeaderField: "X-Api-Sequence")
    }

    enum ASRError: LocalizedError {
        case submit(String), failed(String), query(String), noSpeech, timeout
        var errorDescription: String? {
            switch self {
            case .submit(let m):  return "ASR 提交失败: \(m)"
            case .failed(let s):  return "ASR 识别失败: status=\(s)"
            case .query(let m):   return "ASR 查询失败: \(m)"
            case .noSpeech:       return "未检测到有效语音"
            case .timeout:        return "ASR 超时"
            }
        }
    }
}
