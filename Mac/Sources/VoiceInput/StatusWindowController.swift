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

// ── Status view ───────────────────────────────────────────────────────────
class StatusView: NSView {
    var appState: AppState = .idle  { didSet { needsDisplay = true } }
    var autoEnter: Bool    = true   { didSet { needsDisplay = true } }

    // Drag tracking
    private var dragStart:    NSPoint?
    private var windowOrigin: NSPoint?
    private var didDrag = false

    var onToggleAutoEnter: (() -> Void)?

    override func draw(_ dirtyRect: NSRect) {
        // ── Background (rounded) ──────────────────────────────────────────
        NSColor(red: 22/255, green: 22/255, blue: 28/255, alpha: 0.95).setFill()
        NSBezierPath(roundedRect: bounds, xRadius: 12, yRadius: 12).fill()

        let cx = bounds.midX

        // ── State dot ─────────────────────────────────────────────────────
        let dotR: CGFloat = 7
        let dotCY: CGFloat = bounds.height - 18
        appState.dotColor.setFill()
        NSBezierPath(ovalIn: NSRect(x: cx-dotR, y: dotCY-dotR,
                                   width: dotR*2, height: dotR*2)).fill()

        // ── State label ───────────────────────────────────────────────────
        let stateAttr: [NSAttributedString.Key: Any] = [
            .font: NSFont.boldSystemFont(ofSize: 12),
            .foregroundColor: NSColor(white: 0.9, alpha: 1)
        ]
        let stateStr = NSAttributedString(string: appState.label, attributes: stateAttr)
        let sw = stateStr.size().width
        stateStr.draw(at: NSPoint(x: cx - sw/2, y: dotCY - dotR - 19))

        // ── Divider ───────────────────────────────────────────────────────
        let divY: CGFloat = bounds.height * 0.38
        NSColor(white: 0.25, alpha: 1).setStroke()
        let path = NSBezierPath()
        path.move(to: NSPoint(x: 8, y: divY))
        path.line(to: NSPoint(x: bounds.width - 8, y: divY))
        path.lineWidth = 0.5
        path.stroke()

        // ── Mode label ────────────────────────────────────────────────────
        let modeColor = autoEnter
            ? NSColor(red: 0.31, green: 0.82, blue: 0.47, alpha: 1)
            : NSColor(white: 0.55, alpha: 1)
        let modeAttr: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 10),
            .foregroundColor: modeColor
        ]
        let modeStr = NSAttributedString(string: autoEnter ? "↵ 自动" : "手动",
                                         attributes: modeAttr)
        let mw = modeStr.size().width
        modeStr.draw(at: NSPoint(x: cx - mw/2, y: 7))
    }

    // ── Drag / click ──────────────────────────────────────────────────────
    override func mouseDown(with e: NSEvent) {
        dragStart    = e.locationInWindow
        windowOrigin = window?.frame.origin
        didDrag      = false
    }

    override func mouseDragged(with e: NSEvent) {
        guard let s = dragStart, let o = windowOrigin, let w = window else { return }
        let dx = e.locationInWindow.x - s.x
        let dy = e.locationInWindow.y - s.y
        if abs(dx) > 4 || abs(dy) > 4 { didDrag = true }
        if didDrag { w.setFrameOrigin(NSPoint(x: o.x + dx, y: o.y + dy)) }
    }

    override func mouseUp(with e: NSEvent) {
        if !didDrag { onToggleAutoEnter?() }
        dragStart = nil
    }

    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}

// ── Status window controller ───────────────────────────────────────────────
class StatusWindowController: NSWindowController {
    let statusView: StatusView

    var onAutoEnterChanged: ((Bool) -> Void)?

    init() {
        let size  = NSSize(width: 64, height: 64)
        let screen = NSScreen.main ?? NSScreen.screens[0]
        // top-right corner
        let origin = NSPoint(x: screen.visibleFrame.maxX - size.width - 10,
                             y: screen.visibleFrame.maxY - size.height - 10)

        let win = NSWindow(
            contentRect: NSRect(origin: origin, size: size),
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )
        win.isOpaque            = false
        win.backgroundColor     = .clear
        win.level               = .floating
        win.hasShadow           = true
        win.isMovable           = false   // manual drag in StatusView
        win.collectionBehavior  = [.canJoinAllSpaces, .stationary]
        win.ignoresMouseEvents  = false

        statusView = StatusView(frame: NSRect(origin: .zero, size: size))
        win.contentView = statusView

        super.init(window: win)

        statusView.onToggleAutoEnter = { [weak self] in
            guard let self else { return }
            self.statusView.autoEnter.toggle()
            logInfo("Auto-enter: \(self.statusView.autoEnter ? "ON" : "OFF")")
            self.onAutoEnterChanged?(self.statusView.autoEnter)
        }

        win.orderFrontRegardless()
    }

    required init?(coder: NSCoder) { fatalError() }

    func update(state: AppState) {
        DispatchQueue.main.async { self.statusView.appState = state }
    }

    var autoEnter: Bool { statusView.autoEnter }
}
