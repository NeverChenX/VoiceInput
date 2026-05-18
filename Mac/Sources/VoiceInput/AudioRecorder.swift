import AVFoundation

class AudioRecorder {
    private var engine: AVAudioEngine?
    private var samples: [Int16] = []
    private let lock = NSLock()
    private(set) var sampleRate: Double = 16000
    private var _level: Float = 0  // current RMS level 0.0-1.0
    private let levelLock = NSLock()

    /// Current audio RMS level (0.0-1.0), thread-safe
    var level: Float {
        levelLock.lock(); defer { levelLock.unlock() }
        return _level
    }

    func start() throws {
        let eng = AVAudioEngine()
        engine = eng
        let input = eng.inputNode
        let fmt = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                sampleRate: input.inputFormat(forBus: 0).sampleRate,
                                channels: 1, interleaved: false)!
        sampleRate = fmt.sampleRate
        samples = []

        input.installTap(onBus: 0, bufferSize: 4096, format: fmt) { [weak self] buf, _ in
            guard let self, let ch = buf.floatChannelData?[0] else { return }
            let n = Int(buf.frameLength)
            // Compute RMS level for wave indicator
            if n > 0 {
                var sum: Double = 0
                for i in 0..<n { sum += Double(ch[i]) * Double(ch[i]) }
                var rms = Float(sqrt(sum / Double(n)))
                if rms > 1.0 { rms = 1.0 }
                self.levelLock.lock(); self._level = rms; self.levelLock.unlock()
            }
            let s16 = (0..<n).map { i -> Int16 in
                Int16(max(-1, min(1, ch[i])) * Float(Int16.max))
            }
            self.lock.lock(); self.samples.append(contentsOf: s16); self.lock.unlock()
        }
        try eng.start()
        logInfo("AudioRecorder: started sampleRate=\(sampleRate)")
    }

    func stop() -> [Int16] {
        engine?.inputNode.removeTap(onBus: 0)
        engine?.stop()
        engine = nil
        levelLock.lock(); _level = 0; levelLock.unlock()
        lock.lock(); let s = samples; samples = []; lock.unlock()
        logInfo("AudioRecorder: stopped samples=\(s.count) duration_ms=\(s.count * 1000 / Int(sampleRate))")
        return s
    }
}
