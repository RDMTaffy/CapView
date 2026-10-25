import AppKit
import CoreAudio
import PassthroughCore

enum AudioPassthroughError: LocalizedError {
    case deviceNotFound
    case noOutputDevice
    case coreAudio(String, OSStatus)

    var errorDescription: String? {
        switch self {
        case .deviceNotFound: "오디오 장치를 찾을 수 없습니다."
        case .noOutputDevice: "소리를 낼 출력 장치가 없습니다."
        case .coreAudio(let step, let status): "\(step) 실패 (OSStatus \(status))"
        }
    }
}

/// Live numbers for the menu.
struct AudioStatus {
    /// Estimated time from the card's audio input to the speakers.
    let latencyMilliseconds: Double
    /// The output ran dry although the input was flowing.
    let underruns: Int
    /// The capture card itself stopped delivering audio for a moment.
    let inputGaps: Int
}

/// Plays a capture card's audio input on the system default output device
/// with low latency. The real-time work happens in the C `PassthroughCore`;
/// this class only configures it and follows device changes.
@MainActor
final class AudioPassthrough {
    /// IO buffer size requested on both devices (2.7 ms at 48 kHz).
    static let ioFrames: UInt32 = 128

    var volume: Float = 1 { didSet { applyGain() } }
    var isMuted = false { didSet { applyGain() } }
    /// Called when audio stopped on its own (a restart after wake, an output
    /// switch or a coreaudiod restart failed), with the reason.
    var onFailure: ((String) -> Void)?

    /// The input currently playing, or `nil` when stopped.
    private(set) var inputUID: String?

    private let core: OpaquePointer
    private var inputDevice: AudioObjectID?
    private var outputDevice: AudioObjectID?
    private var inputListeners: [CoreAudioListener] = []
    private var outputListeners: [CoreAudioListener] = []
    private var systemListeners: [CoreAudioListener] = []
    private var serviceRestartListener: CoreAudioListener?
    private var deviceListHandler: (@MainActor () -> Void)?

    init() {
        guard let core = pt_create(16384, 4096) else {
            fatalError("Could not allocate the audio passthrough core")
        }
        self.core = core
        // Slack on top of one IO buffer per side; the core raises it by itself
        // if the output keeps running dry.
        pt_set_margin_frames(core, 128)

        serviceRestartListener = CoreAudioListener(
            AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyServiceRestarted
        ) { [weak self] in
            self?.serviceRestarted()
        }
        installSystemListeners()
        NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) {
            [weak self] _ in MainActor.assumeIsolated { self?.restart() }
        }
    }

    static func inputDevices() -> [AudioInputInfo] {
        CoreAudioDevices.inputs()
    }

    /// Calls `handler` whenever audio devices appear or disappear.
    func observeDeviceListChanges(_ handler: @escaping @MainActor () -> Void) {
        deviceListHandler = handler
        installSystemListeners()
    }

    private func installSystemListeners() {
        removeListeners(&systemListeners)
        let system = AudioObjectID(kAudioObjectSystemObject)
        systemListeners = [
            CoreAudioListener(system, kAudioHardwarePropertyDefaultOutputDevice) { [weak self] in self?.restartOutput() },
            deviceListHandler.map { CoreAudioListener(system, kAudioHardwarePropertyDevices, handler: $0) },
        ].compactMap { $0 }
    }

    /// coreaudiod restarted: device IDs and listeners from before are gone.
    private func serviceRestarted() {
        installSystemListeners()
        restart()
        deviceListHandler?()
    }

    var status: AudioStatus? {
        guard let inputDevice, let outputDevice else { return nil }
        var stats = PTStats()
        pt_get_stats(core, &stats)
        let inputRate = pt_input_sample_rate(core)
        let outputRate = CoreAudioDevices.nominalSampleRate(outputDevice) ?? inputRate
        let input = Double(CoreAudioDevices.ioLatencyFrames(inputDevice, scope: kAudioObjectPropertyScopeInput)) / inputRate
        let output = Double(CoreAudioDevices.ioLatencyFrames(outputDevice, scope: kAudioObjectPropertyScopeOutput)) / outputRate
        return AudioStatus(
            latencyMilliseconds: (input + stats.fillFrames / inputRate + output) * 1000,
            underruns: Int(stats.underruns),
            inputGaps: Int(stats.inputGaps)
        )
    }

    func start(inputUID: String) throws {
        stop()
        guard let device = CoreAudioDevices.deviceID(forUID: inputUID) else {
            throw AudioPassthroughError.deviceNotFound
        }
        // Output first so it is already priming silence when input arrives.
        try startOutput(inputRate: CoreAudioDevices.nominalSampleRate(device) ?? 48_000)
        let status = pt_start_input(core, device, Self.ioFrames)
        guard status == noErr else {
            stopOutput()
            throw AudioPassthroughError.coreAudio("오디오 입력 시작", status)
        }
        self.inputUID = inputUID
        inputDevice = device
        // A new card sample rate needs a new stream format on both sides.
        inputListeners = [CoreAudioListener(device, kAudioDevicePropertyNominalSampleRate) { [weak self] in self?.restart() }]
        applyGain()
    }

    func stop() {
        removeListeners(&inputListeners)
        pt_stop_input(core)
        stopOutput()
        inputUID = nil
        inputDevice = nil
    }

    private func restart() {
        guard let inputUID else { return }
        do {
            try start(inputUID: inputUID)
        } catch {
            onFailure?(error.localizedDescription)
        }
    }

    private func startOutput(inputRate: Double) throws {
        guard let device = CoreAudioDevices.defaultOutputDevice() else {
            throw AudioPassthroughError.noOutputDevice
        }
        let status = pt_start_output(core, device, Self.ioFrames, inputRate)
        guard status == noErr else {
            throw AudioPassthroughError.coreAudio("오디오 출력 시작", status)
        }
        outputDevice = device
        outputListeners = [CoreAudioListener(device, kAudioDevicePropertyNominalSampleRate) { [weak self] in self?.restartOutput() }]
    }

    private func stopOutput() {
        removeListeners(&outputListeners)
        pt_stop_output(core)
        outputDevice = nil
    }

    /// The default output changed (headphones plugged in, AirPods connected,
    /// ...) or its sample rate changed: rebuild only the output side.
    private func restartOutput() {
        guard inputDevice != nil else { return }
        stopOutput()
        do {
            try startOutput(inputRate: pt_input_sample_rate(core))
        } catch {
            // Never leave the input running into nothing; the owner retries.
            stop()
            onFailure?(error.localizedDescription)
        }
    }

    private func removeListeners(_ listeners: inout [CoreAudioListener]) {
        listeners.forEach { $0.remove() }
        listeners.removeAll()
    }

    private func applyGain() {
        // Squared so the volume steps feel even to the ear.
        pt_set_volume(core, isMuted ? 0 : volume * volume)
    }
}
