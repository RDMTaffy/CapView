import AppKit
import AVFoundation
import Carbon.HIToolbox

/// Wires devices, capture, audio, window and menus together.
@MainActor
final class AppController: NSObject, NSMenuDelegate {
    private let video = VideoCapture()
    private let audio = AudioPassthrough()
    private let sleepBlocker = DisplaySleepBlocker()
    private lazy var player = PlayerWindowController(videoLayer: video.displayLayer)
    private lazy var pictureAdjustments: PictureAdjustmentsController = {
        let controller = PictureAdjustmentsController(settings: Prefs.picture)
        controller.onChange = { [weak self] settings in
            Prefs.picture = settings
            self?.player.videoView.setPictureFilters(PictureFilters.make(settings))
        }
        return controller
    }()

    private var videoDevices: [VideoDeviceInfo] = []
    private var audioInputs: [AudioInputInfo] = []
    private var activeVideo: VideoDeviceInfo?
    private var activeMode: VideoMode?
    /// The last start failed; the next device change retries it.
    private var videoFailed = false
    private var capabilities = VideoCapabilities.empty
    private var audioFailure: String?

    private var hasVideoPermission = false
    private var hasAudioPermission = false
    private var shownMicrophoneHint = false
    private var isWindowVisible = true
    /// Bumped on every video (re)start so late completions can be ignored.
    private var videoGeneration = 0
    private var pendingRefresh: DispatchWorkItem?
    private var observers: [NSObjectProtocol] = []

    // Frame watchdog.
    private var lastFrameCount = 0
    private var lastFrameCheck = CACurrentMediaTime()
    private var measuredFrameRate: Double = 0
    private var secondsWithoutFrames = 0
    private var showingNoSignal = false
    private var watchdogTicks = 0
    private var lastInputGaps = 0
    private var lastInputGapTime = -Double.infinity
    /// Let the display sleep only after this long without any picture: a
    /// console blanks HDMI for a few seconds whenever it changes video mode.
    private static let noSignalSleepDelay = 300

    private let videoMenu = NSMenu(title: "비디오")
    private let audioMenu = NSMenu(title: "오디오")
    private let viewMenu = NSMenu(title: "보기")

    // MARK: Lifecycle

    func launch() {
        player.showWindow(nil)
        player.isFloating = Prefs.floatOnTop
        player.onVisibilityChange = { [weak self] visible in
            self?.isWindowVisible = visible
            self?.video.isVisible.store(visible, ordering: .relaxed)
            self?.updateSleepBlocker()
        }
        player.videoView.contextMenuProvider = { [weak self] in self?.makeContextMenu() }
        player.videoView.onKey = { [weak self] key in self?.handleKey(key) ?? false }
        player.videoView.showMessage("캡처보드를 준비하는 중…")
        player.videoView.setPictureFilters(PictureFilters.make(Prefs.picture))

        audio.volume = Prefs.volume
        audio.isMuted = Prefs.muted
        audio.onFailure = { [weak self] reason in
            // Retried by the watchdog; just say why there is no sound.
            self?.audioFailure = reason
        }
        observeDeviceChanges()
        let watchdog = Timer(timeInterval: 1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.checkFrames() }
        }
        RunLoop.main.add(watchdog, forMode: .common)

        Task {
            hasVideoPermission = await AVCaptureDevice.requestAccess(for: .video)
            hasAudioPermission = await AVCaptureDevice.requestAccess(for: .audio)
            refreshDevices()
        }
    }

    func shutdown() {
        audio.stop()
        video.stop()
        sleepBlocker.setActive(false)
    }

    // MARK: Device selection

    private func observeDeviceChanges() {
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: AVCaptureDevice.wasConnectedNotification, object: nil, queue: .main) {
            [weak self] _ in MainActor.assumeIsolated { self?.scheduleRefresh() }
        })
        observers.append(center.addObserver(forName: AVCaptureDevice.wasDisconnectedNotification, object: nil, queue: .main) {
            [weak self] notification in
            let id = (notification.object as? AVCaptureDevice)?.uniqueID
            MainActor.assumeIsolated { self?.deviceDisconnected(id) }
        })
        // Coming back from System Settings after granting access.
        observers.append(center.addObserver(forName: NSApplication.didBecomeActiveNotification, object: nil, queue: .main) {
            [weak self] _ in MainActor.assumeIsolated { self?.recheckPermissions() }
        })
        audio.observeDeviceListChanges { [weak self] in
            self?.scheduleRefresh()
        }
    }

    private func deviceDisconnected(_ id: String?) {
        // Drop the session right away: if the card comes back within the
        // refresh delay it is a new device instance that needs a new input.
        if let id, id == activeVideo?.id {
            stopVideo(message: "“\(activeVideo?.name ?? "캡처보드")” 연결이 끊겼습니다.\n다시 연결하면 자동으로 이어서 보여 줍니다.")
        }
        scheduleRefresh()
    }

    private func recheckPermissions() {
        let video = AVCaptureDevice.authorizationStatus(for: .video) == .authorized
        let audio = AVCaptureDevice.authorizationStatus(for: .audio) == .authorized
        guard video != hasVideoPermission || audio != hasAudioPermission else { return }
        hasVideoPermission = video
        hasAudioPermission = audio
        refreshDevices()
    }

    /// Plugging in a card fires several notifications (video, audio, Core Audio);
    /// handle them as one change.
    private func scheduleRefresh() {
        pendingRefresh?.cancel()
        let work = DispatchWorkItem { [weak self] in
            MainActor.assumeIsolated { self?.refreshDevices() }
        }
        pendingRefresh = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3, execute: work)
    }

    private func refreshDevices() {
        videoDevices = VideoCapture.devices()
        audioInputs = AudioPassthrough.inputDevices()

        if !hasAudioPermission, !shownMicrophoneHint {
            shownMicrophoneHint = true
            player.videoView.showToast("마이크 권한이 없어 소리가 나지 않습니다 — 오디오 메뉴 참고")
        }
        if !hasVideoPermission {
            showPermissionMessage()
        } else if let wanted = preferredVideoDevice() {
            if wanted.id != activeVideo?.id || videoFailed {
                startVideo(wanted)
            }
        } else if let name = Prefs.videoDeviceName {
            stopVideo(message: "“\(name)”을(를) 찾을 수 없습니다.\nUSB 연결을 확인하거나, 비디오 메뉴에서 다른 장치를 선택해 주세요.")
        } else {
            stopVideo(message: "캡처보드를 찾을 수 없습니다.\nUSB 연결을 확인하거나, 비디오 메뉴에서 장치를 선택해 주세요.")
        }
        updateAudio()
    }

    /// The device used last time (it moves between USB ports, so the model ID
    /// also counts). Never falls back to some other camera: a webcam's
    /// microphone would end up on the speakers. Only on first use, picks a
    /// device that looks like a capture card.
    private func preferredVideoDevice() -> VideoDeviceInfo? {
        if Prefs.videoDeviceID != nil || Prefs.videoModelID != nil {
            return videoDevices.first { $0.id == Prefs.videoDeviceID }
                ?? videoDevices.first { !$0.modelID.isEmpty && $0.modelID == Prefs.videoModelID }
        }
        let cards = videoDevices.filter { !$0.isVirtual && $0.looksLikeCaptureCard }
        return cards.first { DevicePairing.audioInput(for: $0, among: audioInputs) != nil } ?? cards.first
    }

    private func startVideo(_ device: VideoDeviceInfo) {
        activeVideo = device
        activeMode = nil
        videoFailed = false
        resetFrameWatch()
        capabilities = VideoCapture.capabilities(deviceID: device.id)
        videoGeneration += 1
        let generation = videoGeneration
        player.window?.title = device.name
        player.videoView.showMessage(nil)

        video.start(deviceID: device.id, size: Prefs.resolution, frameRate: Prefs.frameRate) { [weak self] result in
            guard let self, generation == videoGeneration else { return }
            switch result {
            case .success(let mode):
                activeMode = mode
                Prefs.videoDeviceID = device.id
                Prefs.videoModelID = device.modelID
                Prefs.videoDeviceName = device.name
                player.setVideoSize(width: mode.width, height: mode.height)
                player.videoView.showMessage(nil)
            case .failure(let error):
                activeMode = nil
                videoFailed = true
                video.stop()
                player.videoView.showMessage("“\(device.name)”을(를) 시작하지 못했습니다.\n\(error.localizedDescription)",
                                             buttonTitle: "다시 시도") { [weak self] in self?.reconnect() }
            }
            updateSleepBlocker()
        }
    }

    private func stopVideo(message: String) {
        videoGeneration += 1
        if activeVideo != nil { video.stop() }
        activeVideo = nil
        activeMode = nil
        videoFailed = false
        resetFrameWatch()
        capabilities = .empty
        player.window?.title = "CapView"
        player.videoView.showMessage(message)
        updateSleepBlocker()
    }

    /// Brings the running audio in line with the selection. The engine's own
    /// state is the truth, so this also retries after a failed restart.
    private func updateAudio() {
        let wanted = hasAudioPermission ? preferredAudioInput() : nil
        guard wanted?.uid != audio.inputUID else { return }
        let hadFailure = audioFailure != nil
        audio.stop()
        audioFailure = nil
        guard let wanted else { return }
        do {
            try audio.start(inputUID: wanted.uid)
        } catch {
            audioFailure = "\(wanted.name): \(error.localizedDescription)"
            if !hadFailure { player.videoView.showToast("오디오를 시작하지 못했습니다") }
        }
    }

    private func preferredAudioInput() -> AudioInputInfo? {
        switch Prefs.audioSelection {
        case .off:
            return nil
        case .device(let uid):
            if let input = audioInputs.first(where: { $0.uid == uid }) { return input }
        case .automatic:
            break
        }
        guard let activeVideo else { return nil }
        return DevicePairing.audioInput(for: activeVideo, among: audioInputs)
    }

    /// Runs every second: measures the frame rate, shows "no signal" when
    /// frames stop, and restarts a session that silently stopped delivering.
    private func checkFrames() {
        watchdogTicks += 1
        if watchdogTicks % 5 == 0 { updateAudio() }

        let now = CACurrentMediaTime()
        if let gaps = audio.status?.inputGaps, gaps != lastInputGaps {
            lastInputGaps = gaps
            lastInputGapTime = now
        }
        let received = video.frameCounts.received
        let newFrames = received - lastFrameCount
        measuredFrameRate = Double(newFrames) / max(now - lastFrameCheck, 0.001)
        lastFrameCount = received
        lastFrameCheck = now
        guard let activeVideo, activeMode != nil else {
            secondsWithoutFrames = 0
            return
        }

        if newFrames > 0 {
            if showingNoSignal {
                showingNoSignal = false
                player.videoView.showMessage(nil)
            }
            if secondsWithoutFrames >= Self.noSignalSleepDelay {
                secondsWithoutFrames = 0
                updateSleepBlocker()
            }
            secondsWithoutFrames = 0
            return
        }
        secondsWithoutFrames += 1
        if secondsWithoutFrames == Self.noSignalSleepDelay {
            updateSleepBlocker()
        }
        if secondsWithoutFrames >= 2, !showingNoSignal {
            showingNoSignal = true
            player.videoView.showMessage(
                "“\(activeVideo.name)”에서 영상이 들어오지 않습니다.\n게임기 전원과 HDMI 연결을 확인해 주세요.\nPlayStation은 설정에서 HDCP를 꺼야 캡처됩니다."
            )
        }
        // AVCaptureSession can stop delivering frames without any error.
        if [3, 10, 30].contains(secondsWithoutFrames) || secondsWithoutFrames % 60 == 0 {
            video.restart()
        }
    }

    private func resetFrameWatch() {
        secondsWithoutFrames = 0
        showingNoSignal = false
    }

    private func updateSleepBlocker() {
        let hasPicture = activeMode != nil && secondsWithoutFrames < Self.noSignalSleepDelay
        sleepBlocker.setActive(hasPicture && isWindowVisible)
    }

    private func showPermissionMessage() {
        stopVideo(message: "캡처보드 화면을 보려면 카메라 권한이 필요합니다.\n시스템 설정 › 개인정보 보호 및 보안 › 카메라에서 CapView를 허용해 주세요.")
        player.videoView.showMessage(
            "캡처보드 화면을 보려면 카메라 권한이 필요합니다.\n시스템 설정 › 개인정보 보호 및 보안 › 카메라에서 CapView를 허용해 주세요.",
            buttonTitle: "시스템 설정 열기"
        ) {
            Self.openPrivacySettings("Privacy_Camera")
        }
    }

    private static func openPrivacySettings(_ anchor: String) {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(anchor)") {
            NSWorkspace.shared.open(url)
        }
    }

    // MARK: Actions

    @objc private func selectVideoDevice(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? String,
              let device = videoDevices.first(where: { $0.id == id }) else { return }
        Prefs.videoDeviceID = device.id
        Prefs.videoModelID = device.modelID
        Prefs.videoDeviceName = device.name
        refreshDevices()
    }

    @objc private func selectResolution(_ sender: NSMenuItem) {
        Prefs.resolution = (sender.representedObject as? [Int32]).map { ($0[0], $0[1]) }
        if let activeVideo { startVideo(activeVideo) }
    }

    @objc private func selectFrameRate(_ sender: NSMenuItem) {
        Prefs.frameRate = sender.representedObject as? Double
        if let activeVideo { startVideo(activeVideo) }
    }

    @objc private func selectAudio(_ sender: NSMenuItem) {
        switch sender.representedObject as? String {
        case "auto": Prefs.audioSelection = .automatic
        case "off": Prefs.audioSelection = .off
        case let uid?: Prefs.audioSelection = .device(uid: uid)
        case nil: return
        }
        updateAudio()
    }

    /// Reopens everything from scratch: a fresh device input and audio engine.
    @objc private func reconnect() {
        audio.stop()
        audioFailure = nil
        stopVideo(message: "다시 연결하는 중…")
        refreshDevices()
    }

    @objc private func toggleMute() {
        guard !Self.isKeyRepeat else { return }
        audio.isMuted.toggle()
        Prefs.muted = audio.isMuted
        player.videoView.showToast(audio.isMuted ? "음소거" : "소리 켬 \(Int((audio.volume * 100).rounded()))%")
    }

    @objc private func volumeUp() { changeVolume(by: 0.1) }
    @objc private func volumeDown() { changeVolume(by: -0.1) }

    private func changeVolume(by delta: Float) {
        audio.volume = min(1, max(0, ((audio.volume + delta) * 10).rounded() / 10))
        audio.isMuted = false
        Prefs.volume = audio.volume
        Prefs.muted = false
        player.videoView.showToast("볼륨 \(Int((audio.volume * 100).rounded()))%")
    }

    @objc private func toggleFloating() {
        player.isFloating.toggle()
        Prefs.floatOnTop = player.isFloating
        player.videoView.showToast(player.isFloating ? "항상 위에 표시" : "항상 위에 표시 끔")
    }

    @objc private func actualSize() {
        player.resizeToActualPixels()
    }

    @objc private func showPictureAdjustments() {
        pictureAdjustments.show(beside: player.window)
    }

    @objc private func toggleFullScreen() {
        guard !Self.isKeyRepeat else { return }
        player.toggleFullScreen()
    }

    /// Holding a toggle key must not flip it back and forth.
    private static var isKeyRepeat: Bool {
        guard let event = NSApp.currentEvent, event.type == .keyDown else { return false }
        return event.isARepeat
    }

    @objc private func openMicrophoneSettings() {
        Self.openPrivacySettings("Privacy_Microphone")
    }

    /// Single-key shortcuts that work while the video has focus. Letters are
    /// matched by the typed character (so they follow the keyboard layout),
    /// falling back to the physical key when an input source such as Korean
    /// types something else (F → ㄹ, M → ㅡ).
    private func handleKey(_ event: NSEvent) -> Bool {
        guard event.modifierFlags.intersection([.command, .control, .option]).isEmpty else { return false }
        switch Int(event.keyCode) {
        case kVK_UpArrow: volumeUp(); return true
        case kVK_DownArrow: volumeDown(); return true
        case kVK_Escape where player.isFullScreen: toggleFullScreen(); return true
        default: break
        }
        var key = event.charactersIgnoringModifiers?.lowercased() ?? ""
        if !key.unicodeScalars.allSatisfy(\.isASCII) {
            key = [kVK_ANSI_F: "f", kVK_ANSI_M: "m"][Int(event.keyCode)] ?? key
        }
        switch key {
        case "f": toggleFullScreen()
        case "m": toggleMute()
        default: return false
        }
        return true
    }

    // MARK: Menus

    func makeMainMenu() -> NSMenu {
        let main = NSMenu()

        let appMenu = NSMenu(title: "CapView")
        appMenu.addItem(withTitle: "CapView에 관하여", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "CapView 가리기", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        let hideOthers = appMenu.addItem(withTitle: "기타 가리기", action: #selector(NSApplication.hideOtherApplications(_:)), keyEquivalent: "h")
        hideOthers.keyEquivalentModifierMask = [.command, .option]
        appMenu.addItem(withTitle: "모두 보기", action: #selector(NSApplication.unhideAllApplications(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "CapView 종료", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")

        let windowMenu = NSMenu(title: "윈도우")
        windowMenu.addItem(withTitle: "최소화", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        windowMenu.addItem(withTitle: "확대/축소", action: #selector(NSWindow.performZoom(_:)), keyEquivalent: "")
        NSApp.windowsMenu = windowMenu

        for menu in [videoMenu, audioMenu, viewMenu] {
            menu.delegate = self
        }
        for menu in [appMenu, videoMenu, audioMenu, viewMenu, windowMenu] {
            let item = main.addItem(withTitle: menu.title, action: nil, keyEquivalent: "")
            item.submenu = menu
        }
        // Populate once so key equivalents work before a menu is first opened.
        menuNeedsUpdate(videoMenu)
        menuNeedsUpdate(audioMenu)
        menuNeedsUpdate(viewMenu)
        return main
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        switch menu {
        case videoMenu: fillVideoMenu(menu)
        case audioMenu: fillAudioMenu(menu)
        case viewMenu: fillViewMenu(menu)
        default: break
        }
    }

    private func makeContextMenu() -> NSMenu {
        let menu = NSMenu()
        for (title, fill) in [("비디오", fillVideoMenu), ("오디오", fillAudioMenu)] as [(String, (NSMenu) -> Void)] {
            let submenu = NSMenu(title: title)
            fill(submenu)
            menu.addItem(withTitle: title, action: nil, keyEquivalent: "").submenu = submenu
        }
        menu.addItem(.separator())
        fillViewMenu(menu, appending: true)
        return menu
    }

    private func fillVideoMenu(_ menu: NSMenu) {
        menu.removeAllItems()
        if let activeVideo, let activeMode {
            menu.addItem(header: "\(activeVideo.name) — \(activeMode.width)×\(activeMode.height) @ \(Self.format(rate: activeMode.frameRate))")
            let dropped = video.frameCounts.dropped
            menu.addItem(disabled: String(format: "측정: %.1f fps", measuredFrameRate) + (dropped > 0 ? " · 버린 프레임 \(dropped)" : ""))
        }
        menu.addItem(header: "캡처 장치")
        if videoDevices.isEmpty {
            menu.addItem(disabled: "연결된 장치 없음")
        }
        for device in videoDevices {
            let title = device.isVirtual ? "\(device.name) (가상)" : device.name
            menu.addItem(title, self, #selector(selectVideoDevice(_:)), device.id, checked: device.id == activeVideo?.id)
        }

        menu.addItem(.separator())
        let sizes = NSMenu()
        sizes.addItem("자동 (최대)", self, #selector(selectResolution(_:)), nil, checked: Prefs.resolution == nil)
        for size in capabilities.sizes {
            let checked = Prefs.resolution.map { $0 == size } ?? false
            sizes.addItem("\(size.width) × \(size.height)", self, #selector(selectResolution(_:)), [size.width, size.height], checked: checked)
        }
        menu.addItem(withTitle: "해상도", action: nil, keyEquivalent: "").submenu = sizes

        let rates = NSMenu()
        rates.addItem("자동 (최대)", self, #selector(selectFrameRate(_:)), nil, checked: Prefs.frameRate == nil)
        let size = activeMode.map { ($0.width, $0.height) } ?? capabilities.sizes.first ?? (0, 0)
        for rate in capabilities.rates(width: size.0, height: size.1) {
            rates.addItem("\(Self.format(rate: rate)) fps", self, #selector(selectFrameRate(_:)), rate, checked: Prefs.frameRate == rate)
        }
        menu.addItem(withTitle: "프레임레이트", action: nil, keyEquivalent: "").submenu = rates

        menu.addItem(.separator())
        menu.addItem("다시 연결", self, #selector(reconnect), nil, key: "r")
    }

    private func fillAudioMenu(_ menu: NSMenu) {
        menu.removeAllItems()
        if !hasAudioPermission {
            menu.addItem(disabled: "마이크 권한이 없어 소리를 재생할 수 없습니다")
            menu.addItem("시스템 설정 열기…", self, #selector(openMicrophoneSettings), nil)
            menu.addItem(.separator())
        }
        let selection = Prefs.audioSelection
        let pairedName = activeVideo.flatMap { DevicePairing.audioInput(for: $0, among: audioInputs)?.name }
        menu.addItem("자동 — \(pairedName ?? "캡처보드 오디오 없음")", self, #selector(selectAudio(_:)), "auto",
                     checked: selection == .automatic)
        menu.addItem("끄기", self, #selector(selectAudio(_:)), "off", checked: selection == .off)
        menu.addItem(.separator())
        menu.addItem(header: "입력 장치")
        for input in audioInputs {
            menu.addItem(input.name, self, #selector(selectAudio(_:)), input.uid, checked: selection == .device(uid: input.uid))
        }
        if let audioFailure {
            menu.addItem(disabled: "⚠︎ \(audioFailure)")
        }
        if let status = audio.status {
            menu.addItem(disabled: String(format: "예상 오디오 지연 ≈ %.0f ms", status.latencyMilliseconds))
            if status.inputGaps > 0 || status.underruns > 0 {
                menu.addItem(disabled: "소리 끊김: 캡처보드 \(status.inputGaps)회 · 재생 \(status.underruns)회")
            }
            if CACurrentMediaTime() - lastInputGapTime < 300 {
                menu.addItem(disabled: "캡처보드의 USB 오디오가 끊기고 있습니다. 허브 대신 Mac에 직접 연결해 보세요.")
            }
        }

        menu.addItem(.separator())
        menu.addItem("음소거", self, #selector(toggleMute), nil, key: "m", modifiers: [], checked: audio.isMuted)
        menu.addItem("볼륨 크게", self, #selector(volumeUp), nil, key: String(Character(UnicodeScalar(NSUpArrowFunctionKey)!)), modifiers: [])
        menu.addItem("볼륨 작게", self, #selector(volumeDown), nil, key: String(Character(UnicodeScalar(NSDownArrowFunctionKey)!)), modifiers: [])
        menu.addItem(disabled: "볼륨: \(Int((audio.volume * 100).rounded()))%")
    }

    private func fillViewMenu(_ menu: NSMenu, appending: Bool = false) {
        if !appending { menu.removeAllItems() }
        menu.addItem(player.isFullScreen ? "전체 화면 종료" : "전체 화면", self, #selector(toggleFullScreen), nil,
                     key: "f", modifiers: [.command, .control])
        menu.addItem("항상 위에 표시", self, #selector(toggleFloating), nil, key: "t", checked: player.isFloating)
        menu.addItem("실제 픽셀 크기", self, #selector(actualSize), nil, key: "0")
        menu.addItem(.separator())
        menu.addItem("화면 조정…", self, #selector(showPictureAdjustments), nil, key: "e")
    }

    private static func format(rate: Double) -> String {
        rate == rate.rounded() ? String(format: "%.0f", rate) : String(format: "%.2f", rate)
    }
}

private extension NSMenu {
    @discardableResult
    func addItem(
        _ title: String,
        _ target: AnyObject,
        _ action: Selector,
        _ represented: Any?,
        key: String = "",
        modifiers: NSEvent.ModifierFlags = .command,
        checked: Bool = false
    ) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: key)
        item.target = target
        item.representedObject = represented
        item.keyEquivalentModifierMask = modifiers
        item.state = checked ? .on : .off
        addItem(item)
        return item
    }

    func addItem(header title: String) {
        addItem(NSMenuItem.sectionHeader(title: title))
    }

    func addItem(disabled title: String) {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        item.isEnabled = false
        addItem(item)
    }
}
