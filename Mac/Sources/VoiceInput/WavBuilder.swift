import Foundation

func pcmToWav(_ samples: [Int16], sampleRate: Int) -> Data {
    var wav = Data()
    let dataSize = samples.count * 2
    func w4(_ v: UInt32) { var x = v.littleEndian; withUnsafeBytes(of: &x) { wav.append(contentsOf: $0) } }
    func w2(_ v: UInt16) { var x = v.littleEndian; withUnsafeBytes(of: &x) { wav.append(contentsOf: $0) } }
    wav.append(contentsOf: "RIFF".utf8); w4(UInt32(36 + dataSize))
    wav.append(contentsOf: "WAVE".utf8)
    wav.append(contentsOf: "fmt ".utf8); w4(16); w2(1); w2(1)
    w4(UInt32(sampleRate)); w4(UInt32(sampleRate * 2)); w2(2); w2(16)
    wav.append(contentsOf: "data".utf8); w4(UInt32(dataSize))
    samples.withUnsafeBytes { wav.append(contentsOf: $0) }
    return wav
}
