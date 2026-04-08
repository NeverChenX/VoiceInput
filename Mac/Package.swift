// swift-tools-version:5.7
import PackageDescription

let package = Package(
    name: "VoiceInput",
    platforms: [.macOS(.v12)],
    targets: [
        .executableTarget(
            name: "VoiceInput",
            path: "Sources/VoiceInput",
            linkerSettings: [
                .linkedFramework("AVFoundation"),
                .linkedFramework("AppKit"),
                .linkedFramework("Carbon"),
            ]
        )
    ]
)
