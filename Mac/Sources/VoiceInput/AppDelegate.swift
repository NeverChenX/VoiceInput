import AppKit
import AVFoundation

class AppDelegate: NSObject, NSApplicationDelegate {
    var controller: AppController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        logInfo("════════════════════════════════════════")
        logInfo("VoiceInput (macOS) starting up")
        logInfo("════════════════════════════════════════")

        // Request microphone permission
        AVCaptureDevice.requestAccess(for: .audio) { granted in
            if granted { logInfo("Microphone permission granted") }
            else { logError("Microphone permission DENIED") }
        }

        do {
            let cfg = try Config.load()
            logInfo("Config loaded: app_id=\(cfg.app_id) resource_id=\(cfg.resourceId)")
            let ctrl = AppController(config: cfg)
            self.controller = ctrl
            ctrl.start()
        } catch {
            logError("Config error: \(error)")
            let alert = NSAlert()
            alert.messageText     = "VoiceInput — 配置错误"
            alert.informativeText = error.localizedDescription
            alert.runModal()
            NSApp.terminate(nil)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return false
    }

    func applicationWillTerminate(_ notification: Notification) {
        logInfo("VoiceInput shutdown")
    }
}
