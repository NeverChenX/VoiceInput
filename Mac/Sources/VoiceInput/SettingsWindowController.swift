import AppKit

// ── Hotkey Capture View ────────────────────────────────────────────────────
class HotkeyCaptureView: NSTextField {
    var onHotkeyCaptured: ((String) -> Void)?
    var onCancelled: (() -> Void)?
    
    private var isCapturing = false
    private var originalValue: String = ""
    
    override init(frame: NSRect) {
        super.init(frame: frame)
        setup()
    }
    
    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setup()
    }
    
    private func setup() {
        isEditable = false
        isSelectable = false
        alignment = .center
        font = NSFont.systemFont(ofSize: 14)
        textColor = NSColor.controlTextColor
        backgroundColor = .controlBackgroundColor
        bezelStyle = .roundedBezel
        wantsLayer = true
        layer?.cornerRadius = 4
        
        // Add click gesture
        let clickGesture = NSClickGestureRecognizer(target: self, action: #selector(startCapture))
        addGestureRecognizer(clickGesture)
    }
    
    @objc private func startCapture() {
        guard !isCapturing else { return }
        isCapturing = true
        originalValue = stringValue
        stringValue = "请按下快捷键..."
        textColor = .systemBlue
        layer?.borderWidth = 2
        layer?.borderColor = NSColor.systemBlue.cgColor
        
        // Start monitoring global key events
        NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .flagsChanged]) { [weak self] event in
            self?.handleKeyEvent(event)
            return nil
        }
    }
    
    private func handleKeyEvent(_ event: NSEvent) {
        guard isCapturing else { return }
        
        if event.type == .keyDown && event.keyCode == 53 { // ESC
            cancelCapture()
            return
        }
        
        if event.type == .keyDown {
            let modifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
            let keyCode = event.keyCode
            
            // Need at least one modifier or it's a special key
            let specialKeys: [CGKeyCode] = [53, 36, 49, 48, 51, 122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111]
            let isSpecialKey = specialKeys.contains(keyCode)
            
            if modifiers.contains(.option) || modifiers.contains(.command) || 
               modifiers.contains(.control) || isSpecialKey {
                let hotkeyString = HotkeyConfig.string(from: modifiers, keyCode: keyCode)
                finishCapture(with: hotkeyString)
            }
        }
    }
    
    private func finishCapture(with hotkey: String) {
        isCapturing = false
        stringValue = hotkey
        textColor = NSColor.controlTextColor
        layer?.borderWidth = 0
        onHotkeyCaptured?(hotkey)
    }
    
    private func cancelCapture() {
        isCapturing = false
        stringValue = originalValue
        textColor = NSColor.controlTextColor
        layer?.borderWidth = 0
        onCancelled?()
    }
    
    func setHotkey(_ hotkey: String) {
        if !isCapturing {
            stringValue = hotkey
        }
    }
}

// ── Settings Window Controller ─────────────────────────────────────────────
class SettingsWindowController: NSWindowController {
    
    var onSave: ((Config) -> Void)?
    var onCancel: (() -> Void)?
    
    private var config: Config
    
    // Form fields
    private var appIdField: NSTextField!
    private var accessTokenField: NSTextField!
    private var secretKeyField: NSTextField!
    private var hotkeyStartField: HotkeyCaptureView!
    private var hotkeyStopField: HotkeyCaptureView!
    private var autoEnterCheckbox: NSButton!
    
    init(config: Config) {
        self.config = config
        
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 420, height: 380),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = "VoiceInput 设置"
        window.center()
        
        super.init(window: window)
        
        setupUI()
        loadConfig()
        
        window.delegate = self
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
    
    private func setupUI() {
        guard let contentView = window?.contentView else { return }
        
        let padding: CGFloat = 20
        let labelWidth: CGFloat = 120
        let fieldWidth: CGFloat = 240
        var y: CGFloat = 380 - padding - 20
        
        // Helper to create label
        func createLabel(_ text: String, y: CGFloat) -> NSTextField {
            let label = NSTextField(labelWithString: text)
            label.frame = NSRect(x: padding, y: y - 10, width: labelWidth, height: 20)
            label.alignment = .right
            label.font = NSFont.systemFont(ofSize: 13)
            contentView.addSubview(label)
            return label
        }
        
        // Helper to create text field
        func createTextField(y: CGFloat, placeholder: String = "", isSecure: Bool = false) -> NSTextField {
            let field: NSTextField
            if isSecure {
                field = NSSecureTextField(frame: NSRect(x: padding + labelWidth + 10, y: y - 12, width: fieldWidth, height: 24))
            } else {
                field = NSTextField(frame: NSRect(x: padding + labelWidth + 10, y: y - 12, width: fieldWidth, height: 24))
            }
            field.placeholderString = placeholder
            field.font = NSFont.systemFont(ofSize: 13)
            contentView.addSubview(field)
            return field
        }
        
        // App ID
        createLabel("App ID:", y: y)
        appIdField = createTextField(y: y, placeholder: "火山引擎应用 ID")
        y -= 42
        
        // Access Token
        createLabel("Access Token:", y: y)
        accessTokenField = createTextField(y: y, placeholder: "API 访问令牌")
        y -= 42
        
        // Secret Key
        createLabel("Secret Key:", y: y)
        secretKeyField = createTextField(y: y, placeholder: "API 密钥（可选）", isSecure: true)
        y -= 48
        
        // Divider
        let divider = NSBox(frame: NSRect(x: padding, y: y - 4, width: 380, height: 1))
        divider.boxType = .separator
        contentView.addSubview(divider)
        y -= 20
        
        // Start Hotkey
        createLabel("开始录音快捷键:", y: y)
        hotkeyStartField = HotkeyCaptureView(frame: NSRect(x: padding + labelWidth + 10, y: y - 12, width: fieldWidth, height: 28))
        contentView.addSubview(hotkeyStartField)
        y -= 42
        
        // Stop Hotkey
        createLabel("停止录音快捷键:", y: y)
        hotkeyStopField = HotkeyCaptureView(frame: NSRect(x: padding + labelWidth + 10, y: y - 12, width: fieldWidth, height: 28))
        contentView.addSubview(hotkeyStopField)
        y -= 48
        
        // Auto Enter Checkbox
        autoEnterCheckbox = NSButton(checkboxWithTitle: "识别后自动按回车发送", target: nil, action: nil)
        autoEnterCheckbox.frame = NSRect(x: padding + labelWidth + 10, y: y - 10, width: fieldWidth, height: 20)
        autoEnterCheckbox.font = NSFont.systemFont(ofSize: 13)
        contentView.addSubview(autoEnterCheckbox)
        y -= 50
        
        // Buttons
        let buttonY: CGFloat = 30
        let buttonWidth: CGFloat = 80
        let buttonSpacing: CGFloat = 12
        let buttonsCenterX: CGFloat = 210
        
        // Cancel button
        let cancelButton = NSButton(frame: NSRect(x: buttonsCenterX - buttonWidth - buttonSpacing/2, y: buttonY, width: buttonWidth, height: 28))
        cancelButton.title = "取消"
        cancelButton.bezelStyle = .rounded
        cancelButton.target = self
        cancelButton.action = #selector(cancelClicked)
        cancelButton.keyEquivalent = String(format: "%c", 27) // ESC
        contentView.addSubview(cancelButton)
        
        // Save button
        let saveButton = NSButton(frame: NSRect(x: buttonsCenterX + buttonSpacing/2, y: buttonY, width: buttonWidth, height: 28))
        saveButton.title = "保存"
        saveButton.bezelStyle = .rounded
        saveButton.keyEquivalent = "\r" // Return
        saveButton.target = self
        saveButton.action = #selector(saveClicked)
        contentView.addSubview(saveButton)
        
        // Set up hotkey capture callbacks
        hotkeyStartField.onHotkeyCaptured = { [weak self] hotkey in
            logInfo("Start hotkey captured: \(hotkey)")
        }
        hotkeyStopField.onHotkeyCaptured = { [weak self] hotkey in
            logInfo("Stop hotkey captured: \(hotkey)")
        }
    }
    
    private func loadConfig() {
        appIdField.stringValue = config.app_id
        accessTokenField.stringValue = config.access_token
        secretKeyField.stringValue = config.secret_key ?? ""
        hotkeyStartField.setHotkey(config.hotkeyStart)
        hotkeyStopField.setHotkey(config.hotkeyStopValue)
        autoEnterCheckbox.state = config.autoEnterValue ? .on : .off
    }
    
    @objc private func saveClicked() {
        let appId = appIdField.stringValue.trimmingCharacters(in: .whitespaces)
        let accessToken = accessTokenField.stringValue.trimmingCharacters(in: .whitespaces)
        
        // Validation
        if appId.isEmpty {
            showAlert(message: "App ID 不能为空", informative: "请输入火山引擎应用 ID")
            return
        }
        if accessToken.isEmpty {
            showAlert(message: "Access Token 不能为空", informative: "请输入 API 访问令牌")
            return
        }
        
        let newConfig = config.with(
            appId: appId,
            accessToken: accessToken,
            secretKey: secretKeyField.stringValue.isEmpty ? nil : secretKeyField.stringValue,
            hotkey: hotkeyStartField.stringValue,
            hotkeyStop: hotkeyStopField.stringValue,
            autoEnter: autoEnterCheckbox.state == .on
        )
        
        do {
            try newConfig.save()
            onSave?(newConfig)
            window?.close()
        } catch {
            showAlert(message: "保存失败", informative: error.localizedDescription)
        }
    }
    
    @objc private func cancelClicked() {
        onCancel?()
        window?.close()
    }
    
    private func showAlert(message: String, informative: String) {
        let alert = NSAlert()
        alert.messageText = message
        alert.informativeText = informative
        alert.alertStyle = .warning
        alert.runModal()
    }
}

// MARK: - NSWindowDelegate
extension SettingsWindowController: NSWindowDelegate {
    func windowWillClose(_ notification: Notification) {
        onCancel?()
    }
}
