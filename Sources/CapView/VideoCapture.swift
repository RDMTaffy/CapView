import AVFoundation
import CoreAudio
import Synchronization

/// A capture device as shown in the menus.
struct VideoDeviceInfo: Sendable, Hashable {
    let id: String
    let name: String
    let modelID: String
    /// Software cameras (e.g. "OBS Virtual Camera") are never auto-selected.
    let isVirtual: Bool
    /// Offers at least 720p at 50+ fps, as capture cards do (most webcams don't).
    let looksLikeCaptureCard: Bool
}

/// The format the device is actually running at.
struct VideoMode: Sendable, Hashable {
    let width: Int32
    let height: Int32
    let frameRate: Double
}

/// What the device can do, for building the resolution / frame-rate menus.
struct VideoCapabilities: Sendable {
    /// Distinct sizes, largest first.
    let sizes: [(width: Int32, height: Int32)]
    /// Distinct frame rates per size ("1920x1080" → [60, 59.94, ...]), fastest first.
    let frameRates: [String: [Double]]

    static let empty = VideoCapabilities(sizes: [], frameRates: [:])

    func rates(width: Int32, height: Int32) -> [Double] {
        frameRates["\(width)x\(height)"] ?? []
    }
}

enum VideoCaptureError: LocalizedError {
    case deviceMissing
    case cannotAddInput(String?)
    case noUsableFormat
    case notRunning

    var errorDescription: String? {
        switch self {
        case .deviceMissing: "장치가 연결 해제되었습니다."
        case .cannotAddInput(let reason?): "장치를 열 수 없습니다. (\(reason))"
        case .cannotAddInput(nil): "장치를 열 수 없습니다. 다른 앱이 독점 사용 중인지 확인해 주세요."
        case .noUsableFormat: "지원하는 비디오 포맷이 없습니다."
        case .notRunning: "캡처를 시작하지 못했습니다. 다른 앱이 장치를 쓰고 있는지 확인해 주세요."
        }
    }
}

/// Captures video frames and shows them on `displayLayer` as soon as they
/// arrive: no queue, no display link, no copies (IOSurface-backed buffers are
/// scaled and color-converted by the compositor).
///
/// Session work runs on a private serial queue because `startRunning()`
/// blocks; frames arrive on their own high-priority queue.
final class VideoCapture: NSObject, @unchecked Sendable, AVCaptureVideoDataOutputSampleBufferDelegate {
    let displayLayer: AVSampleBufferDisplayLayer

    /// Skip drawing while the window is hidden; frames are still counted.
    let isVisible = Atomic<Bool>(true)
    private let framesReceived = Atomic<Int>(0)
    private let framesDropped = Atomic<Int>(0)

    private let session = AVCaptureSession()
    private let output = AVCaptureVideoDataOutput()
    private let sessionQueue = DispatchQueue(label: "capview.video-session")
    private let frameQueue = DispatchQueue(label: "capview.video-frames", qos: .userInteractive)
    /// Thread-safe for enqueueing from a background queue (AVSampleBufferVideoRenderer.h).
    private nonisolated(unsafe) let renderer: AVSampleBufferVideoRenderer

    // Session queue only.
    private var input: AVCaptureDeviceInput?
    private var lastChoice: VideoFormatPicker.Choice?

    @MainActor
    override init() {
        displayLayer = AVSampleBufferDisplayLayer()
        displayLayer.videoGravity = .resizeAspect
        // Transparent outside the picture: the view behind is black, and picture
        // filters then cannot lift the letterbox bars to grey.
        displayLayer.backgroundColor = nil
        renderer = displayLayer.sampleBufferRenderer
        super.init()
        output.alwaysDiscardsLateVideoFrames = true
        output.setSampleBufferDelegate(self, queue: frameQueue)
        NotificationCenter.default.addObserver(
            forName: AVCaptureSession.runtimeErrorNotification, object: session, queue: nil
        ) { [weak self] _ in
            // e.g. the device was briefly unavailable; try again shortly.
            guard let self else { return }
            sessionQueue.asyncAfter(deadline: .now() + 1) { [weak self] in self?.restartOnQueue() }
        }
    }

    /// Frames delivered / dropped since launch.
    var frameCounts: (received: Int, dropped: Int) {
        (framesReceived.load(ordering: .relaxed), framesDropped.load(ordering: .relaxed))
    }

    // MARK: Device discovery

    static func devices() -> [VideoDeviceInfo] {
        AVCaptureDevice.DiscoverySession(
            deviceTypes: [.external],
            mediaType: .video,
            position: .unspecified
        ).devices.map { device in
            VideoDeviceInfo(
                id: device.uniqueID,
                name: device.localizedName,
                modelID: device.modelID,
                isVirtual: device.transportType == Int32(bitPattern: kAudioDeviceTransportTypeVirtual),
                looksLikeCaptureCard: device.formats.contains { format in
                    CMVideoFormatDescriptionGetDimensions(format.formatDescription).width >= 1280
                        && format.videoSupportedFrameRateRanges.contains { $0.maxFrameRate >= 50 }
                }
            )
        }
    }

    static func capabilities(deviceID: String) -> VideoCapabilities {
        guard let device = AVCaptureDevice(uniqueID: deviceID) else { return .empty }
        var sizes: [(width: Int32, height: Int32)] = []
        var rates: [String: Set<Double>] = [:]
        for format in device.formats where VideoFormatPicker.isUsable(format) {
            let d = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
            let key = "\(d.width)x\(d.height)"
            if rates[key] == nil { sizes.append((d.width, d.height)) }
            for range in format.videoSupportedFrameRateRanges {
                rates[key, default: []].insert(VideoFormatPicker.rounded(range.maxFrameRate))
            }
        }
        sizes.sort { $0.width * $0.height > $1.width * $1.height }
        return VideoCapabilities(sizes: sizes, frameRates: rates.mapValues { $0.sorted(by: >) })
    }

    // MARK: Session control

    /// Switches the session to `deviceID` at the requested size / frame rate
    /// (`nil` = largest / fastest) and starts it.
    func start(
        deviceID: String,
        size: (width: Int32, height: Int32)?,
        frameRate: Double?,
        completion: @escaping @MainActor @Sendable (Result<VideoMode, VideoCaptureError>) -> Void
    ) {
        sessionQueue.async { [self] in
            let result = configureAndStart(deviceID: deviceID, size: size, frameRate: frameRate)
            DispatchQueue.main.async {
                MainActor.assumeIsolated { completion(result) }
            }
        }
    }

    func stop() {
        sessionQueue.async { [self] in
            session.stopRunning()
            if let input {
                session.removeInput(input)
                self.input = nil
            }
            lastChoice = nil
            renderer.flush(removingDisplayedImage: true, completionHandler: nil)
        }
    }

    /// Restarts a running session that stopped delivering frames.
    func restart() {
        sessionQueue.async { [self] in restartOnQueue() }
    }

    private func restartOnQueue() {
        guard let input, let lastChoice else { return }
        session.stopRunning()
        // A device that was unplugged and replugged is a new instance; the old
        // input never delivers again.
        var device = input.device
        if !device.isConnected, let fresh = AVCaptureDevice(uniqueID: device.uniqueID),
           let freshInput = try? AVCaptureDeviceInput(device: fresh) {
            session.beginConfiguration()
            session.removeInput(input)
            if session.canAddInput(freshInput) {
                session.addInput(freshInput)
                self.input = freshInput
                device = fresh
            }
            session.commitConfiguration()
        }
        session.startRunning()
        reapplyIfReset(lastChoice, on: device)
    }

    /// Some UVC drivers reset the format when the session starts.
    private func reapplyIfReset(_ choice: VideoFormatPicker.Choice, on device: AVCaptureDevice) {
        if device.activeFormat != choice.format || device.activeVideoMinFrameDuration != choice.frameDuration {
            apply(choice, to: device)
        }
    }

    private func configureAndStart(
        deviceID: String,
        size: (width: Int32, height: Int32)?,
        frameRate: Double?
    ) -> Result<VideoMode, VideoCaptureError> {
        guard let device = AVCaptureDevice(uniqueID: deviceID) else { return .failure(.deviceMissing) }
        guard let choice = VideoFormatPicker.choose(from: device.formats, size: size, frameRate: frameRate) else {
            return .failure(.noUsableFormat)
        }

        session.beginConfiguration()
        if input?.device.uniqueID != deviceID || input?.device.isConnected == false {
            if let input { session.removeInput(input) }
            input = nil
            let newInput: AVCaptureDeviceInput
            do {
                newInput = try AVCaptureDeviceInput(device: device)
            } catch {
                session.commitConfiguration()
                return .failure(.cannotAddInput(error.localizedDescription))
            }
            guard session.canAddInput(newInput) else {
                session.commitConfiguration()
                return .failure(.cannotAddInput(nil))
            }
            session.addInput(newInput)
            input = newInput
        }
        if !session.outputs.contains(output), session.canAddOutput(output) {
            session.addOutput(output)
        }
        apply(choice, to: device)
        // Without explicit settings the output follows the session preset and
        // may scale or convert; ask for the device's own pixels instead. (If
        // another app holds the device lock, that is not `choice`.)
        let running = device.activeFormat
        let dims = CMVideoFormatDescriptionGetDimensions(running.formatDescription)
        output.videoSettings = [
            kCVPixelBufferPixelFormatTypeKey as String: VideoFormatPicker.outputPixelFormat(for: running),
            kCVPixelBufferWidthKey as String: Int(dims.width),
            kCVPixelBufferHeightKey as String: Int(dims.height),
            kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](),
        ]
        if let connection = output.connection(with: .video) {
            if connection.isVideoMinFrameDurationSupported { connection.videoMinFrameDuration = choice.frameDuration }
            if connection.isVideoMaxFrameDurationSupported { connection.videoMaxFrameDuration = choice.frameDuration }
        }
        session.commitConfiguration()

        if !session.isRunning {
            session.startRunning()
        }
        // Start failures are only reported through runtimeErrorNotification.
        guard session.isRunning else { return .failure(.notRunning) }
        lastChoice = choice
        reapplyIfReset(choice, on: device)

        let active = CMVideoFormatDescriptionGetDimensions(device.activeFormat.formatDescription)
        let duration = device.activeVideoMinFrameDuration
        let fps = duration.seconds > 0 ? VideoFormatPicker.rounded(1 / duration.seconds) : choice.frameRate
        return .success(VideoMode(width: active.width, height: active.height, frameRate: fps))
    }

    /// Sets format and frame rate. The lock is released right away: holding it
    /// would degrade other apps sharing the device. If another app holds it
    /// (e.g. OBS), the device keeps that app's settings.
    private func apply(_ choice: VideoFormatPicker.Choice, to device: AVCaptureDevice) {
        guard (try? device.lockForConfiguration()) != nil else { return }
        device.activeFormat = choice.format
        device.activeVideoMinFrameDuration = choice.frameDuration
        device.activeVideoMaxFrameDuration = choice.frameDuration
        device.unlockForConfiguration()
    }

    // MARK: AVCaptureVideoDataOutputSampleBufferDelegate (frame queue)

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        framesReceived.wrappingAdd(1, ordering: .relaxed)
        guard isVisible.load(ordering: .relaxed) else { return }
        // Show this frame now, replacing anything older, regardless of timestamps.
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: true),
           CFArrayGetCount(attachments) > 0 {
            let dictionary = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(
                dictionary,
                Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                Unmanaged.passUnretained(kCFBooleanTrue).toOpaque()
            )
        }
        if renderer.status == .failed {
            renderer.flush()
        }
        renderer.enqueue(sampleBuffer)
    }

    func captureOutput(_ output: AVCaptureOutput, didDrop sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        framesDropped.wrappingAdd(1, ordering: .relaxed)
    }
}

/// Chooses the device format for a requested size / frame rate.
enum VideoFormatPicker {
    struct Choice {
        let format: AVCaptureDevice.Format
        let frameDuration: CMTime
        let frameRate: Double
    }

    private static let uncompressed: [OSType] = [
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,   // '420v'
        kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,    // '420f'
        kCVPixelFormatType_422YpCbCr8_yuvs,                // 'yuvs'
        kCVPixelFormatType_422YpCbCr8,                     // '2vuy'
    ]

    /// Lower is better. 4:2:0 is the least bandwidth and what the GPU wants;
    /// compressed formats (MJPEG, H.264) still work but cost a decode step.
    private static func pixelFormatRank(_ format: AVCaptureDevice.Format) -> Int {
        uncompressed.firstIndex(of: CMFormatDescriptionGetMediaSubType(format.formatDescription)) ?? uncompressed.count
    }

    /// The device's own pixel format when it is uncompressed, so no conversion
    /// happens; compressed formats are decoded to 4:2:0.
    static func outputPixelFormat(for format: AVCaptureDevice.Format) -> OSType {
        let subtype = CMFormatDescriptionGetMediaSubType(format.formatDescription)
        return uncompressed.contains(subtype) ? subtype : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
    }

    static func isUsable(_ format: AVCaptureDevice.Format) -> Bool {
        format.mediaType == .video && !format.videoSupportedFrameRateRanges.isEmpty
    }

    /// Rounds 59.9400599 to 59.94 so equal rates compare equal.
    static func rounded(_ rate: Double) -> Double {
        (rate * 100).rounded() / 100
    }

    static func choose(
        from formats: [AVCaptureDevice.Format],
        size: (width: Int32, height: Int32)?,
        frameRate: Double?
    ) -> Choice? {
        let usable = formats.filter(isUsable)
        guard !usable.isEmpty else { return nil }

        func area(_ f: AVCaptureDevice.Format) -> Int64 {
            let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            return Int64(d.width) * Int64(d.height)
        }

        func maxRate(_ f: AVCaptureDevice.Format) -> Double {
            f.videoSupportedFrameRateRanges.map(\.maxFrameRate).max() ?? 0
        }

        let exact = size.map { size in
            usable.filter {
                let d = CMVideoFormatDescriptionGetDimensions($0.formatDescription)
                return d.width == size.width && d.height == size.height
            }
        } ?? []
        // For "largest", only consider sizes that reach the wanted rate (or
        // 50+ fps): a card offering 4K30 and 1080p60 should play 1080p60.
        let fastest = usable.map(maxRate).max() ?? 0
        let minimumRate = min(frameRate ?? 50, fastest) - 0.5
        let fast = usable.filter { maxRate($0) >= minimumRate }
        let largest = fast.map(area).max() ?? 0
        let candidates = exact.isEmpty ? fast.filter { area($0) == largest } : exact

        // Always use a range's own minFrameDuration: UVC devices report exact
        // 100 ns intervals (this card's "60" is 1000000/60000240), so a built
        // CMTime(1, 60) is not in any range and would be rejected.
        var best: (choice: Choice, score: (Double, Int))?
        for format in candidates {
            for range in format.videoSupportedFrameRateRanges {
                let rate = range.maxFrameRate
                // Closest to the requested rate (or fastest), then best pixel format.
                let rateScore = frameRate.map { -abs(rate - $0) } ?? rate
                let score = (rateScore, -pixelFormatRank(format))
                if best == nil || score > best!.score {
                    best = (Choice(format: format, frameDuration: range.minFrameDuration, frameRate: rounded(rate)), score)
                }
            }
        }
        return best?.choice
    }
}
