import AppKit

@MainActor
class AppController {
    private(set) var config: Config
    var state: AppState = .idle { didSet { statusWC.update(state: state) } }

    private let recorder   = AudioRecorder()
    private var asr: AsrClient
    private let hotkey     = HotkeyManager()
    private let statusWC   = StatusWindowController()
    private var overlayWC: OverlayWindowController?
    private var settingsWC: SettingsWindowController?

    init(config: Config) {
        self.config = config
        self.asr    = AsrClient(config: config)
    }

    func start() {
        // Configure hotkeys from config
        hotkey.configure(startHotkey: config.hotkeyStart, stopHotkey: config.hotkeyStopValue)
        
        // Set up status window callbacks
        statusWC.onAutoEnterChanged = { [weak self] isOn in
            guard let self = self else { return }
            // Update config in memory
            self.config = self.config.with(autoEnter: isOn)
            // Try to save to disk
            try? self.config.save()
        }
        
        statusWC.onOpenSettings = { [weak self] in
            self?.openSettings()
        }
        
        // Initialize status window with config values
        statusWC.autoEnter = config.autoEnterValue
        statusWC.update(hotkey: config.hotkeyStart)

        // Set up hotkey callbacks
        hotkey.onStartRecording = { [weak self] in self?.startRecording() }
        hotkey.onStopRecording  = { [weak self] in self?.stopRecording() }
        hotkey.onCancel         = { [weak self] in self?.cancelRecording() }
        hotkey.start()

        logInfo("AppController ready — \(config.hotkeyStart): 录音, \(config.hotkeyStopValue): 停止, ESC: 取消")
    }
    
    private func openSettings() {
        guard settingsWC == nil else {
            // Settings window already open, bring to front
            settingsWC?.window?.makeKeyAndOrderFront(nil)
            return
        }
        
        settingsWC = SettingsWindowController(config: config)
        
        settingsWC?.onSave = { [weak self] newConfig in
            guard let self = self else { return }
            self.config = newConfig
            
            // Update hotkeys
            self.asr = AsrClient(config: newConfig)
            self.hotkey.configure(startHotkey: newConfig.hotkeyStart, stopHotkey: newConfig.hotkeyStopValue)
            self.hotkey.restart()

            // Update status window
            self.statusWC.update(hotkey: newConfig.hotkeyStart)
            self.statusWC.autoEnter = newConfig.autoEnterValue
            
            logInfo("Settings saved and applied")
            self.settingsWC = nil
        }
        
        settingsWC?.onCancel = { [weak self] in
            self?.settingsWC = nil
        }
        
        settingsWC?.window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    // ── Recording lifecycle ────────────────────────────────────────────────
    private func startRecording() {
        guard state == .idle else { return }
        let stopKey = config.hotkeyStopValue
        do {
            try recorder.start()
        } catch {
            showError(error.localizedDescription); return
        }
        state = .recording
        showOverlay("正在聆听", hint: "按 \(stopKey) 停止")
        logInfo("State → RECORDING")
    }

    private func stopRecording() {
        guard state == .recording else { return }
        state = .transcribing
        showOverlay("识别中", hint: "请稍候…")
        logInfo("State → TRANSCRIBING")

        let samples    = recorder.stop()
        let sr         = Int(recorder.sampleRate)
        let minSamples = Int(Double(sr) * 0.25)

        if samples.isEmpty {
            showError("未捕获到音频，请检查麦克风权限"); return
        }
        if samples.count < minSamples {
            showError("录音太短，请说话后再停止"); return
        }

        let wav = pcmToWav(samples, sampleRate: sr)
        logInfo("WAV built: \(wav.count) bytes")

        Task { [weak self] in
            guard let self else { return }
            do {
                let text = try await asr.recognize(wav)
                await MainActor.run { self.pasteText(text) }
            } catch {
                logError("识别出错: \(error)")
                await MainActor.run { self.showError(error.localizedDescription) }
            }
        }
    }

    private func cancelRecording() {
        _ = recorder.stop()
        state = .idle
        showOverlay("已取消", hint: "")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) { self.hideOverlay() }
        logInfo("State cancelled by ESC")
    }

    // ── Paste ──────────────────────────────────────────────────────────────
    private func pasteText(_ text: String) {
        state = .idle
        hideOverlay()

        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        logInfo("Clipboard set: \(text.count) chars")

        // Cmd+V
        postKey(virtualKey: 9, flags: .maskCommand)
        logInfo("Cmd+V sent")

        if statusWC.autoEnter {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
                self.postKey(virtualKey: 36, flags: [])   // Return
                logInfo("Return sent")
            }
        }
    }

    private func postKey(virtualKey: CGKeyCode, flags: CGEventFlags) {
        let src = CGEventSource(stateID: .hidSystemState)
        let dn = CGEvent(keyboardEventSource: src, virtualKey: virtualKey, keyDown: true)!
        let up = CGEvent(keyboardEventSource: src, virtualKey: virtualKey, keyDown: false)!
        dn.flags = flags; up.flags = flags
        dn.post(tap: .cghidEventTap)
        up.post(tap: .cghidEventTap)
    }

    // ── Overlay helpers ────────────────────────────────────────────────────
    private func showOverlay(_ main: String, hint: String) {
        if overlayWC == nil { overlayWC = OverlayWindowController() }
        overlayWC?.show(main: main, hint: hint)
    }

    private func hideOverlay() { overlayWC?.hide() }

    private func showError(_ msg: String) {
        state = .idle
        showOverlay("错误", hint: msg)
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.6) { self.hideOverlay() }
    }
}
