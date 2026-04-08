import AppKit

class OverlayWindowController: NSWindowController {
    private let mainLabel: NSTextField
    private let hintLabel: NSTextField

    init() {
        let screen = NSScreen.main ?? NSScreen.screens[0]
        let win = NSWindow(
            contentRect: screen.frame,
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )
        win.isOpaque           = false
        win.backgroundColor    = NSColor.black.withAlphaComponent(0.88)
        win.level              = .screenSaver
        win.collectionBehavior = [.canJoinAllSpaces]
        win.ignoresMouseEvents = true

        mainLabel = NSTextField(labelWithString: "")
        mainLabel.font      = NSFont.boldSystemFont(ofSize: 52)
        mainLabel.textColor = NSColor(white: 0.96, alpha: 1)
        mainLabel.alignment = .center
        mainLabel.translatesAutoresizingMaskIntoConstraints = false

        hintLabel = NSTextField(labelWithString: "")
        hintLabel.font      = NSFont.systemFont(ofSize: 18)
        hintLabel.textColor = NSColor(white: 0.79, alpha: 1)
        hintLabel.alignment = .center
        hintLabel.translatesAutoresizingMaskIntoConstraints = false

        super.init(window: win)

        let content = win.contentView!
        content.addSubview(mainLabel)
        content.addSubview(hintLabel)
        NSLayoutConstraint.activate([
            mainLabel.centerXAnchor.constraint(equalTo: content.centerXAnchor),
            mainLabel.centerYAnchor.constraint(equalTo: content.centerYAnchor, constant: -20),
            hintLabel.centerXAnchor.constraint(equalTo: content.centerXAnchor),
            hintLabel.topAnchor.constraint(equalTo: mainLabel.bottomAnchor, constant: 12),
        ])
    }

    required init?(coder: NSCoder) { fatalError() }

    func show(main: String, hint: String) {
        mainLabel.stringValue = main
        hintLabel.stringValue = hint
        window?.orderFrontRegardless()
    }

    func hide() { window?.orderOut(nil) }
}
