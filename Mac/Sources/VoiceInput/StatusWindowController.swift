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

    // Audio level for wave animation (0.0-1.0), redraws driven by waveTimer
    var audioLevel: Float = 0

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

    // Wave animation state
    private var waveTick: Double = 0
    private var smoothLevel: Double = 0
    private let barCount = 24
    private var barPhases: [Double] = []
    private var waveTimer: Timer?

    // Drag tracking
    private var dragStart: NSPoint?
    private var windowOrigin: NSPoint?
    private var didDrag = false

    override init(frame: NSRect) {
        super.init(frame: frame)
        barPhases = (0..<barCount).map { _ in Double.random(in: 0...(2 * .pi)) }
        setupSubviews()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        barPhases = (0..<barCount).map { _ in Double.random(in: 0...(2 * .pi)) }
        setupSubviews()
    }

    func startWaveAnimation() {
        guard waveTimer == nil else { return }
        waveTimer = Timer.scheduledTimer(withTimeInterval: 1.0/30.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.waveTick += 1
            self.needsDisplay = true
        }
    }

    func stopWaveAnimation() {
        waveTimer?.invalidate()
        waveTimer = nil
        waveTick = 0
        smoothLevel = 0
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
        // Zone A - Status Indicator with Wave Bars (Top ~56%)
        // ═════════════════════════════════════════════════════════════════

        let isRec = (appState == .recording)
        let isTranscribing = (appState == .transcribing)
        let t = waveTick * 0.033

        if isRec || isTranscribing {
            // Smooth audio level for responsive but non-jittery animation
            if isRec {
                var target = Double(audioLevel) * 6.0
                if target > 1.0 { target = 1.0 }
                target = pow(target, 0.6)
                let alpha = (target > smoothLevel) ? 0.45 : 0.15
                smoothLevel += (target - smoothLevel) * alpha
            } else {
                smoothLevel *= 0.8
            }

            let pulse = isRec
                ? 0.75 + 0.25 * sin(t * 4.0)
                : 0.5  + 0.5  * sin(t * 1.5)

            // Wave bars layout
            let barsX0: CGFloat = 10
            let barsX1: CGFloat = bounds.width - 10
            let barStep = (barsX1 - barsX0) / CGFloat(barCount)
            let barW = barStep - 2
            let waveCY = bounds.height - zoneAHeight / 2 + 6
            let maxH: CGFloat = 28

            // Center line
            NSColor(red: 22/255, green: 22/255, blue: 40/255, alpha: 1).setStroke()
            let centerLine = NSBezierPath()
            centerLine.move(to: NSPoint(x: barsX0, y: waveCY))
            centerLine.line(to: NSPoint(x: barsX1, y: waveCY))
            centerLine.lineWidth = 0.5
            centerLine.stroke()

            // Neon gradient colors: teal→blue→violet (recording) or blue→indigo (transcribing)
            let cLeft  = isRec ? (r: 0.0, g: 0.86, b: 0.63) : (r: 0.12, g: 0.47, b: 1.0)
            let cMid   = isRec ? (r: 0.0, g: 0.55, b: 1.0)  : (r: 0.31, g: 0.24, b: 1.0)
            let cRight = isRec ? (r: 0.71, g: 0.0, b: 1.0)  : (r: 0.63, g: 0.12, b: 1.0)

            for i in 0..<barCount {
                let frac = Double(i) / Double(barCount - 1)
                // Interpolate color
                let (cr, cg, cb): (Double, Double, Double)
                if frac < 0.5 {
                    let f = frac * 2.0
                    cr = cLeft.r + (cMid.r - cLeft.r) * f
                    cg = cLeft.g + (cMid.g - cLeft.g) * f
                    cb = cLeft.b + (cMid.b - cLeft.b) * f
                } else {
                    let f = (frac - 0.5) * 2.0
                    cr = cMid.r + (cRight.r - cMid.r) * f
                    cg = cMid.g + (cRight.g - cMid.g) * f
                    cb = cMid.b + (cRight.b - cMid.b) * f
                }
                let barColor = NSColor(red: cr, green: cg, blue: cb, alpha: 1)

                var hNorm: Double
                if isRec {
                    let s1 = sin(t * 6.0  + barPhases[i])
                    let s2 = sin(t * 10.0 + Double(i) * 0.5)
                    let s3 = sin(t * 15.0 + Double(i) * 0.8 + barPhases[i] * 0.6)
                    var pattern = 0.45*(0.5+0.5*s1) + 0.30*(0.5+0.5*s2) + 0.25*(0.5+0.5*s3)
                    let barVar = 0.7 + 0.3 * (0.5 + 0.5 * sin(barPhases[i] * 3.0 + t * 2.0))
                    pattern *= barVar
                    hNorm = 0.02 + smoothLevel * 1.1 * pattern
                    if hNorm > 1.0 { hNorm = 1.0 }
                } else {
                    let s1 = sin(t * 1.8 + barPhases[i])
                    let s2 = sin(t * 3.0 + Double(i) * 0.3)
                    hNorm = 0.05 + 0.50*(0.5+0.5*s1) + 0.25*(0.5+0.5*s2)
                }
                hNorm *= (0.85 + 0.15 * pulse)
                var h = CGFloat(hNorm) * maxH
                if h < 2 { h = 2 }

                let bx = barsX0 + CGFloat(i) * barStep

                // Subtle glow
                let glowColor = NSColor(red: cr * 0.2 + 8.0/255.0 * 0.8,
                                        green: cg * 0.2 + 8.0/255.0 * 0.8,
                                        blue: cb * 0.2 + 18.0/255.0 * 0.8, alpha: 1)
                glowColor.setFill()
                NSBezierPath(roundedRect: NSRect(x: bx - 1, y: waveCY - h - 2,
                                                 width: barW + 2, height: (h + 2) * 2),
                             xRadius: 2, yRadius: 2).fill()

                // Bar
                barColor.setFill()
                NSBezierPath(roundedRect: NSRect(x: bx, y: waveCY - h,
                                                 width: barW, height: h * 2),
                             xRadius: barW / 2, yRadius: barW / 2).fill()
            }

            // State label below wave bars
            let stateAttr: [NSAttributedString.Key: Any] = [
                .font: NSFont.boldSystemFont(ofSize: 12),
                .foregroundColor: isRec
                    ? NSColor(red: 1.0, green: 0.25, blue: 0.25, alpha: 1)
                    : NSColor(red: 0.2, green: 0.6, blue: 1.0, alpha: 1)
            ]
            let stateStr = NSAttributedString(string: isRec ? "聆听中" : "识别中", attributes: stateAttr)
            let sw = stateStr.size().width
            stateStr.draw(at: NSPoint(x: cx - sw / 2, y: waveCY - maxH - 22))
        } else {
            // Idle: show dot indicator
            let dotR: CGFloat = 13
            let dotCY: CGFloat = bounds.height - zoneAHeight / 2 + 6
            appState.dotColor.setFill()
            NSBezierPath(ovalIn: NSRect(x: cx - dotR,
                                        y: dotCY - dotR,
                                        width: dotR * 2,
                                        height: dotR * 2)).fill()

            let stateAttr: [NSAttributedString.Key: Any] = [
                .font: NSFont.boldSystemFont(ofSize: 14),
                .foregroundColor: NSColor(white: 0.96, alpha: 1)
            ]
            let stateStr = NSAttributedString(string: appState.label, attributes: stateAttr)
            let sw = stateStr.size().width
            stateStr.draw(at: NSPoint(x: cx - sw / 2, y: dotCY - dotR - 24))
        }

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
