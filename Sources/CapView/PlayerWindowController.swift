import AppKit
import CoreImage

/// Black view that shows the video layer, plus a centered status message used
/// while there is no video (no device, no permission, no signal, ...).
final class VideoView: NSView {
    let videoLayer: CALayer
    /// Holds the video so picture filters apply to whatever it shows.
    private let videoContainer = CALayer()

    var onActivity: (() -> Void)?
    var onMouseExit: (() -> Void)?
    var onDoubleClick: (() -> Void)?
    /// Returns `true` if the key press was handled.
    var onKey: ((NSEvent) -> Bool)?
    var contextMenuProvider: (() -> NSMenu?)?

    private let messageLabel = NSTextField(wrappingLabelWithString: "")
    private let actionButton = NSButton(title: "", target: nil, action: nil)
    private let messageStack: NSStackView
    private var buttonHandler: (() -> Void)?
    private let toastLabel = NSTextField(labelWithString: "")
    private var toastTimer: Timer?

    init(videoLayer: CALayer) {
        self.videoLayer = videoLayer
        messageStack = NSStackView(views: [messageLabel, actionButton])
        super.init(frame: NSRect(x: 0, y: 0, width: 1280, height: 720))
        wantsLayer = true
        layerUsesCoreImageFilters = true
        layerContentsRedrawPolicy = .never
        videoContainer.addSublayer(videoLayer)
        layer?.addSublayer(videoContainer)

        messageLabel.alignment = .center
        messageLabel.textColor = NSColor(white: 0.85, alpha: 1)
        messageLabel.font = .systemFont(ofSize: 15, weight: .medium)
        messageLabel.maximumNumberOfLines = 0
        actionButton.bezelStyle = .rounded
        actionButton.target = self
        actionButton.action = #selector(actionButtonPressed)
        messageStack.orientation = .vertical
        messageStack.alignment = .centerX
        messageStack.spacing = 14
        messageStack.translatesAutoresizingMaskIntoConstraints = false
        messageStack.isHidden = true
        addSubview(messageStack)

        toastLabel.font = .systemFont(ofSize: 14, weight: .semibold)
        toastLabel.textColor = .white
        toastLabel.alignment = .center
        toastLabel.wantsLayer = true
        toastLabel.layer?.backgroundColor = NSColor(white: 0, alpha: 0.6).cgColor
        toastLabel.layer?.cornerRadius = 8
        toastLabel.translatesAutoresizingMaskIntoConstraints = false
        toastLabel.isHidden = true
        addSubview(toastLabel)

        NSLayoutConstraint.activate([
            messageStack.centerXAnchor.constraint(equalTo: centerXAnchor),
            messageStack.centerYAnchor.constraint(equalTo: centerYAnchor),
            messageStack.widthAnchor.constraint(lessThanOrEqualTo: widthAnchor, constant: -48),
            toastLabel.centerXAnchor.constraint(equalTo: centerXAnchor),
            toastLabel.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -32),
            toastLabel.heightAnchor.constraint(equalToConstant: 32),
            toastLabel.widthAnchor.constraint(greaterThanOrEqualToConstant: 140),
        ])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    override var wantsUpdateLayer: Bool { true }

    override func updateLayer() {
        layer?.backgroundColor = NSColor.black.cgColor
    }

    override func layout() {
        super.layout()
        // Follow window resizes immediately instead of with an implicit animation.
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        videoContainer.frame = bounds
        videoLayer.frame = videoContainer.bounds
        CATransaction.commit()
    }

    /// Applies picture corrections; `nil` removes them so the video is drawn
    /// directly again.
    func setPictureFilters(_ filters: [CIFilter]?) {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        videoContainer.filters = filters
        CATransaction.commit()
    }

    /// Shows `text` over the video area; `nil` hides the message.
    func showMessage(_ text: String?, buttonTitle: String? = nil, action: (() -> Void)? = nil) {
        messageStack.isHidden = text == nil
        messageLabel.stringValue = text ?? ""
        actionButton.isHidden = buttonTitle == nil
        actionButton.title = buttonTitle ?? ""
        buttonHandler = action
    }

    @objc private func actionButtonPressed() {
        buttonHandler?()
    }

    /// Briefly shows a short status line (volume, mute, ...) near the bottom.
    func showToast(_ text: String) {
        toastLabel.stringValue = "  \(text)  "
        toastLabel.alphaValue = 1
        toastLabel.isHidden = false
        toastTimer?.invalidate()
        toastTimer = Timer.scheduledTimer(withTimeInterval: 1.2, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated {
                // Fade out, then take the label out of the composite so the
                // video layer is the only thing on screen again.
                NSAnimationContext.runAnimationGroup { context in
                    context.duration = 0.3
                    self?.toastLabel.animator().alphaValue = 0
                } completionHandler: {
                    MainActor.assumeIsolated {
                        if self?.toastLabel.alphaValue == 0 { self?.toastLabel.isHidden = true }
                    }
                }
            }
        }
    }

    // MARK: Keyboard

    override var acceptsFirstResponder: Bool { true }

    override func keyDown(with event: NSEvent) {
        if onKey?(event) != true {
            super.keyDown(with: event)
        }
    }

    // MARK: Mouse

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(
            rect: .zero,
            options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways, .inVisibleRect],
            owner: self
        ))
    }

    override func mouseMoved(with event: NSEvent) { onActivity?() }
    override func mouseEntered(with event: NSEvent) { onActivity?() }
    override func mouseExited(with event: NSEvent) { onMouseExit?() }

    override func mouseDown(with event: NSEvent) {
        onActivity?()
        if event.clickCount == 2 {
            onDoubleClick?()
        } else {
            window?.performDrag(with: event)
        }
    }

    override func menu(for event: NSEvent) -> NSMenu? {
        contextMenuProvider?()
    }
}

/// The single player window: borderless-looking, black, aspect-locked to the
/// capture format, with the title bar buttons and cursor hidden while idle.
@MainActor
final class PlayerWindowController: NSWindowController, NSWindowDelegate {
    let videoView: VideoView
    /// Called with `true` when the window becomes visible on screen and `false`
    /// when it is fully hidden (minimized, occluded, other Space).
    var onVisibilityChange: ((Bool) -> Void)?

    private var idleTimer: Timer?
    private var videoSize = NSSize(width: 1920, height: 1080)
    private static let idleDelay: TimeInterval = 2.0

    init(videoLayer: CALayer) {
        videoView = VideoView(videoLayer: videoLayer)
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1280, height: 720),
            styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
            backing: .buffered,
            defer: false
        )
        window.title = "CapView"
        window.titleVisibility = .hidden
        window.titlebarAppearsTransparent = true
        window.backgroundColor = .black
        window.collectionBehavior = [.fullScreenPrimary, .managed]
        window.contentAspectRatio = videoSize
        window.minSize = NSSize(width: 320, height: 180)
        window.contentView = videoView
        window.initialFirstResponder = videoView
        window.center()
        window.setFrameAutosaveName("CapViewPlayer")
        super.init(window: window)
        window.delegate = self

        videoView.onActivity = { [weak self] in self?.userDidInteract() }
        videoView.onMouseExit = { [weak self] in self?.scheduleIdle(after: 0.6) }
        videoView.onDoubleClick = { [weak self] in self?.toggleFullScreen() }
        // A restored frame may not match the aspect ratio.
        fitFrameToAspect()
        scheduleIdle(after: Self.idleDelay)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    var isFullScreen: Bool { window?.styleMask.contains(.fullScreen) ?? false }

    /// Keep the window above other apps. Native full screen has its own
    /// Space, so the level drops to normal there and comes back on exit.
    var isFloating = false {
        didSet { applyLevel() }
    }

    private func applyLevel() {
        window?.level = isFloating && !isFullScreen ? .floating : .normal
    }

    func toggleFullScreen() {
        window?.toggleFullScreen(nil)
    }

    /// Locks the window to the capture format's aspect ratio.
    func setVideoSize(width: Int32, height: Int32) {
        guard width > 0, height > 0 else { return }
        videoSize = NSSize(width: CGFloat(width), height: CGFloat(height))
        window?.contentAspectRatio = videoSize
        fitFrameToAspect()
    }

    /// Gives the window the video's aspect ratio, keeping its width and top edge.
    private func fitFrameToAspect() {
        guard let window, !isFullScreen else { return }
        var content = window.contentRect(forFrameRect: window.frame)
        let height = (content.width * videoSize.height / videoSize.width).rounded()
        guard abs(height - content.height) >= 1 else { return }
        let top = content.maxY
        content.size.height = height
        content.origin.y = top - height
        window.setFrame(onScreen(window.frameRect(forContentRect: content)), display: true)
    }

    /// Moves `frame` back inside the visible area of the window's screen.
    private func onScreen(_ frame: NSRect) -> NSRect {
        guard let visible = (window?.screen ?? NSScreen.main)?.visibleFrame else { return frame }
        var frame = frame
        frame.origin.x = max(visible.minX, min(frame.minX, visible.maxX - frame.width))
        frame.origin.y = max(visible.minY, min(frame.minY, visible.maxY - frame.height))
        return frame
    }

    /// Resizes the window so one video pixel maps to one screen pixel,
    /// shrinking (keeping the aspect ratio) if that does not fit the screen.
    func resizeToActualPixels() {
        guard let window, !isFullScreen, let screen = window.screen ?? NSScreen.main else { return }
        let scale = screen.backingScaleFactor
        var size = NSSize(width: videoSize.width / scale, height: videoSize.height / scale)
        let limit = window.contentRect(forFrameRect: screen.visibleFrame).size
        let fit = min(1, limit.width / size.width, limit.height / size.height)
        size = NSSize(width: (size.width * fit).rounded(), height: (size.height * fit).rounded())
        var frame = window.contentRect(forFrameRect: window.frame)
        let top = frame.maxY
        frame.size = size
        frame.origin.y = top - size.height
        window.setFrame(onScreen(window.frameRect(forContentRect: frame)), display: true, animate: true)
    }

    // MARK: Idle chrome / cursor

    private func userDidInteract() {
        setTitlebarVisible(true)
        scheduleIdle(after: Self.idleDelay)
    }

    private func scheduleIdle(after delay: TimeInterval) {
        idleTimer?.invalidate()
        idleTimer = Timer.scheduledTimer(withTimeInterval: delay, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated { self?.becameIdle() }
        }
    }

    private func becameIdle() {
        guard let window else { return }
        let mouse = NSEvent.mouseLocation
        let pointerInside = window.frame.contains(mouse)
        // Keep the buttons reachable while the pointer sits on the title bar.
        let titlebarHeight = window.frame.height - window.contentLayoutRect.height
        if pointerInside, !isFullScreen, mouse.y > window.frame.maxY - max(titlebarHeight, 28) { return }
        setTitlebarVisible(false)
        if pointerInside, window.isKeyWindow {
            NSCursor.setHiddenUntilMouseMoves(true)
        }
    }

    private func setTitlebarVisible(_ visible: Bool) {
        guard let titlebar = window?.standardWindowButton(.closeButton)?.superview else { return }
        let alpha: CGFloat = visible ? 1 : 0
        guard titlebar.alphaValue != alpha else { return }
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.2
            titlebar.animator().alphaValue = alpha
        }
    }

    // MARK: NSWindowDelegate

    func windowDidChangeOcclusionState(_ notification: Notification) {
        onVisibilityChange?(window?.occlusionState.contains(.visible) ?? false)
    }

    func windowWillEnterFullScreen(_ notification: Notification) {
        setTitlebarVisible(true)
        window?.level = .normal
    }

    func windowDidExitFullScreen(_ notification: Notification) {
        applyLevel()
        fitFrameToAspect()
    }
}
