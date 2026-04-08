import AppKit

// ── State ─────────────────────────────────────────────────────────────────
enum AppState { case idle, recording, transcribing }

extension AppState {
    var dotColor: NSColor {
        switch self {
        case .idle:         return NSColor(white: 0.47, alpha: 1)
        case .recording:    return NSColor(red: 1.0,  green: 0.27, blue: 0.27, alpha: 1)
        case .transcribing: return NSColor(red: 0.27, green: 0.55, blue: 1.0,  alpha: 1)
        }
    }
    var label: String {
        switch self {
        case .idle:         return "待机"
        case .recording:    return "录音"
        case .transcribing: return "识别"
        }
    }
}

// ── iOS Style Toggle Switch (Smaller) ───────────────────────────────────────
class ToggleSwitch: NSView {
    var isOn: Bool = true {
        didSet {
            if oldValue != isOn {
                updateAppearance()
                onToggle?(isOn)
            }
        }
    }
    var onToggle: ((Bool) -> Void)?
    
    private let trackLayer = CALayer()
    private let thumbLayer = CALayer()
    
    // Smaller size: ~2/3 of original
    private let trackWidth: CGFloat = 34
    private let trackHeight: CGFloat = 18
    private let thumbSize: CGFloat = 14
    private let padding: CGFloat = 2
    
    init(frame: NSRect, isOn: Bool = true) {
        self.isOn = isOn
        super.init(frame: frame)
        setupLayers()
        updateAppearance(animated: false)
    }
    
    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setupLayers()
        updateAppearance(animated: false)
    }
    
    private func setupLayers() {
        wantsLayer = true
        
        // Track
        trackLayer.frame = NSRect(x: (bounds.width - trackWidth) / 2,
                                  y: (bounds.height - trackHeight) / 2,
                                  width: trackWidth,
                                  height: trackHeight)
        trackLayer.cornerRadius = trackHeight / 2
        trackLayer.masksToBounds = true
        layer?.addSublayer(trackLayer)
        
        // Thumb
        thumbLayer.frame = NSRect(x: trackLayer.frame.minX + padding + (isOn ? trackWidth - thumbSize - padding * 2 : 0),
                                  y: (bounds.height - thumbSize) / 2,
                                  width: thumbSize,
                                  height: thumbSize)
        thumbLayer.cornerRadius = thumbSize / 2
        thumbLayer.backgroundColor = NSColor.white.cgColor
        thumbLayer.shadowColor = NSColor.black.cgColor
        thumbLayer.shadowOffset = CGSize(width: 0, height: 1)
        thumbLayer.shadowOpacity = 0.3
        thumbLayer.shadowRadius = 1
        layer?.addSublayer(thumbLayer)
        
        updateAppearance(animated: false)
    }
    
    private func updateAppearance(animated: Bool = true) {
        let trackColor = isOn ? NSColor(red: 0.31, green: 0.82, blue: 0.47, alpha: 1) : NSColor(white: 0.55, alpha: 1)
        let thumbX = trackLayer.frame.minX + padding + (isOn ? trackWidth - thumbSize - padding * 2 : 0)
        
        if animated {
            NSAnimationContext.runAnimationGroup { context in
                context.duration = 0.2
                context.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
                trackLayer.backgroundColor = trackColor.cgColor
                thumbLayer.position = CGPoint(x: thumbX + thumbSize / 2, y: thumbLayer.position.y)
            }
        } else {
            trackLayer.backgroundColor = trackColor.cgColor
            thumbLayer.frame.origin.x = thumbX
        }
    }
    
    override func mouseDown(with event: NSEvent) {
        isOn.toggle()
    }
}

// ── Settings Button (Smaller) ──────────────────────────────────────────────
class SettingsButton: NSView {
    var onClick: (() -> Void)?
    
    private let iconLayer = CATextLayer()
    
    override init(frame: NSRect) {
        super.init(frame: frame)
        setup()
    }
    
    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setup()
    }
    
    private func setup() {
        wantsLayer = true
        layer?.backgroundColor = NSColor(white: 0.25, alpha: 1).cgColor
        layer?.cornerRadius = 4
        
        iconLayer.string = "⚙"
        iconLayer.fontSize = 10
        iconLayer.alignmentMode = .center
        iconLayer.foregroundColor = NSColor(white: 0.8, alpha: 1).cgColor
        iconLayer.frame = bounds
        iconLayer.contentsScale = NSScreen.main?.backingScaleFactor ?? 2.0
        layer?.addSublayer(iconLayer)
    }
    
    override func mouseDown(with event: NSEvent) {
        layer?.backgroundColor = NSColor(white: 0.35, alpha: 1).cgColor
    }
    
    override func mouseUp(with event: NSEvent) {
        layer?.backgroundColor = NSColor(white: 0.25, alpha: 1).cgColor
        onClick?()
    }
    
    override func mouseExited(with event: NSEvent) {
        layer?.backgroundColor = NSColor(white: 0.25, alpha: 1).cgColor
    }
}

// ── Status view (133x133 with 3 zones - 1/3 smaller than 200x200) ───────────
class StatusView: NSView {
    var appState: AppState = .idle { didSet { needsDisplay = true } }
    var autoEnter: Bool = true {
        didSet {
            toggleSwitch?.isOn = autoEnter
            onToggleAutoEnter?(autoEnter)
        }
    }
    
    // Control visibility of auto-send UI
    var showAutoSendUI: Bool = false  // Hidden by default
    var hotkeyDescription: String = "Option+Space" { didSet { needsDisplay = true } }
    
    private var toggleSwitch: ToggleSwitch?
    private var settingsButton: SettingsButton?
    
    var onToggleAutoEnter: ((Bool) -> Void)?
    var onOpenSettings: (() -> Void)?
    
    // Window size: 133x133 (2/3 of 200)
    static let windowSize: CGFloat = 133
    
    // Zone heights (proportional)
    private let zoneAHeight: CGFloat = 75   // ~56%
    private let zoneBHeight: CGFloat = 22   // ~17%
    private let zoneCHeight: CGFloat = 36   // ~27%
    
    // Drag tracking
    private var dragStart: NSPoint?
    private var windowOrigin: NSPoint?
    private var didDrag = false
    
    override init(frame: NSRect) {
        super.init(frame: frame)
        setupSubviews()
    }
    
    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setupSubviews()
    }
    
    private func setupSubviews() {
        // Zone B - Toggle Switch (hidden by default, auto-enter is always on)
        let toggleFrame = NSRect(x: 82, y: zoneCHeight + 5, width: 40, height: 20)
        let toggle = ToggleSwitch(frame: toggleFrame, isOn: autoEnter)
        toggle.onToggle = { [weak self] isOn in
            self?.autoEnter = isOn
            self?.onToggleAutoEnter?(isOn)
        }
        toggle.isHidden = !showAutoSendUI  // Hide if showAutoSendUI is false
        addSubview(toggle)
        toggleSwitch = toggle
        
        // Zone C - Settings Button (smaller)
        let buttonSize: CGFloat = 18
        let buttonFrame = NSRect(x: bounds.width - buttonSize - 8,
                                 y: 10,
                                 width: buttonSize,
                                 height: buttonSize)
        let settingsBtn = SettingsButton(frame: buttonFrame)
        settingsBtn.onClick = { [weak self] in
            self?.onOpenSettings?()
        }
        addSubview(settingsBtn)
        settingsButton = settingsBtn
    }
    
    override func draw(_ dirtyRect: NSRect) {
        // ── Background (rounded) ──────────────────────────────────────────
        NSColor(red: 18/255, green: 18/255, blue: 24/255, alpha: 0.97).setFill()
        NSBezierPath(roundedRect: bounds, xRadius: 10, yRadius: 10).fill()
        
        let cx = bounds.midX
        
        // ═════════════════════════════════════════════════════════════════
        // Zone A - Status Indicator (Top ~56%)
        // ═════════════════════════════════════════════════════════════════
        
        // State dot (smaller radius)
        let dotR: CGFloat = 13
        let dotCY: CGFloat = bounds.height - zoneAHeight / 2 + 6
        appState.dotColor.setFill()
        NSBezierPath(ovalIn: NSRect(x: cx - dotR,
                                    y: dotCY - dotR,
                                    width: dotR * 2,
                                    height: dotR * 2)).fill()
        
        // State label (smaller font)
        let stateAttr: [NSAttributedString.Key: Any] = [
            .font: NSFont.boldSystemFont(ofSize: 14),
            .foregroundColor: NSColor(white: 0.96, alpha: 1)
        ]
        let stateStr = NSAttributedString(string: appState.label, attributes: stateAttr)
        let sw = stateStr.size().width
        stateStr.draw(at: NSPoint(x: cx - sw / 2, y: dotCY - dotR - 24))
        
        // ═════════════════════════════════════════════════════════════════
        // Zone B - Auto Send Toggle (Middle ~17%, hidden by default)
        // ═════════════════════════════════════════════════════════════════
        let zoneBTop = bounds.height - zoneAHeight
        
        // Divider line above Zone B (separating from Zone A)
        NSColor(white: 0.2, alpha: 1).setStroke()
        let topDivider = NSBezierPath()
        topDivider.move(to: NSPoint(x: 8, y: zoneBTop + zoneBHeight))
        topDivider.line(to: NSPoint(x: bounds.width - 8, y: zoneBTop + zoneBHeight))
        topDivider.lineWidth = 0.5
        topDivider.stroke()
        
        // Label "【自动发送】" - only shown when showAutoSendUI is true
        if showAutoSendUI {
            let labelAttr: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: 9),
                .foregroundColor: NSColor(white: 0.75, alpha: 1)
            ]
            let labelStr = NSAttributedString(string: "【自动发送】", attributes: labelAttr)
            labelStr.draw(at: NSPoint(x: 10, y: zoneBTop + 8))
        }
        
        // Divider line below Zone B
        let bottomDivider = NSBezierPath()
        bottomDivider.move(to: NSPoint(x: 8, y: zoneBTop))
        bottomDivider.line(to: NSPoint(x: bounds.width - 8, y: zoneBTop))
        bottomDivider.lineWidth = 0.5
        bottomDivider.stroke()
        
        // ═════════════════════════════════════════════════════════════════
        // Zone C - Hotkey hint + Settings button (Bottom ~27%)
        // ═════════════════════════════════════════════════════════════════
        
        // Hotkey hint (smaller font)
        let hintAttr: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 8),
            .foregroundColor: NSColor(white: 0.55, alpha: 1)
        ]
        let hintStr = NSAttributedString(string: hotkeyDescription, attributes: hintAttr)
        hintStr.draw(at: NSPoint(x: 10, y: 12))
    }
    
    // ── Drag / click ──────────────────────────────────────────────────────
    override func mouseDown(with e: NSEvent) {
        dragStart = e.locationInWindow
        windowOrigin = window?.frame.origin
        didDrag = false
    }
    
    override func mouseDragged(with e: NSEvent) {
        guard let s = dragStart, let o = windowOrigin, let w = window else { return }
        let dx = e.locationInWindow.x - s.x
        let dy = e.locationInWindow.y - s.y
        if abs(dx) > 4 || abs(dy) > 4 { didDrag = true }
        if didDrag { w.setFrameOrigin(NSPoint(x: o.x + dx, y: o.y + dy)) }
    }
    
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}

// ── Status window controller ───────────────────────────────────────────────
class StatusWindowController: NSWindowController {
    let statusView: StatusView
    
    var onAutoEnterChanged: ((Bool) -> Void)?
    var onOpenSettings: (() -> Void)?
    
    init() {
        let size = NSSize(width: StatusView.windowSize, height: StatusView.windowSize)
        let screen = NSScreen.main ?? NSScreen.screens[0]
        // top-right corner with padding
        let origin = NSPoint(x: screen.visibleFrame.maxX - size.width - 12,
                             y: screen.visibleFrame.maxY - size.height - 12)
        
        let win = NSWindow(
            contentRect: NSRect(origin: origin, size: size),
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )
        win.isOpaque = false
        win.backgroundColor = .clear
        win.level = .floating
        win.hasShadow = true
        win.isMovable = false
        win.collectionBehavior = [.canJoinAllSpaces, .stationary]
        win.ignoresMouseEvents = false
        
        statusView = StatusView(frame: NSRect(origin: .zero, size: size))
        win.contentView = statusView
        
        super.init(window: win)
        
        statusView.onToggleAutoEnter = { [weak self] isOn in
            self?.onAutoEnterChanged?(isOn)
        }
        statusView.onOpenSettings = { [weak self] in
            self?.onOpenSettings?()
        }
        
        win.orderFrontRegardless()
    }
    
    required init?(coder: NSCoder) { fatalError() }
    
    func update(state: AppState) {
        DispatchQueue.main.async { self.statusView.appState = state }
    }
    
    func update(hotkey: String) {
        DispatchQueue.main.async { self.statusView.hotkeyDescription = hotkey }
    }
    
    var autoEnter: Bool {
        get { statusView.autoEnter }
        set { DispatchQueue.main.async { self.statusView.autoEnter = newValue } }
    }
}
