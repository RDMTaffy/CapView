import AppKit
import CoreAudio
import PassthroughCore
import os

/// `log show --predicate 'subsystem == "com.ycs.capview"'` shows these after a
/// reported glitch (notice level, so the system keeps them).
let audioLog = Logger(subsystem: "com.ycs.capview", category: "audio")

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
    /// A backlog was dropped to keep the latency down (a short skip).
    let resyncs: Int
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
    private var outputRate: Double?
    private var lastStats: PTStats?
    private var stalledSeconds = 0
    private var healthySeconds = 0
    /// Seconds of stalled IO before the next restart; doubles while restarts do not help.
    private var stallRestartDelay = 3

    init() {
        guard let core = pt_create(16384, 4096) else {
            fatalError("Could not allocate the audio passthrough core")
        }
        self.core = core
        // Slack on top of one IO buffer per side (6.7 ms at 48 kHz): rides out an
        // input callback up to ~5 ms late, plus the 32 frames the core keeps to
        // fade out a dropout. The core raises it by itself if the output keeps
        // running dry.
        pt_set_margin_frames(core, 320)

        serviceRestartListener = CoreAudioListener(
            AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyServiceRestarted
        ) { [weak self] in
            self?.serviceRestarted()
        }
        installSystemListeners()
        NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) {
            [weak self] _ in MainActor.assumeIsolated { self?.restart(because: "wake") }
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
            CoreAudioListener(system, kAudioHardwarePropertyDefaultOutputDevice) { [weak self] in
                self?.restartOutputIfChanged(because: "default output changed")
            },
            deviceListHandler.map { CoreAudioListener(system, kAudioHardwarePropertyDevices, handler: $0) },
        ].compactMap { $0 }
    }

    /// coreaudiod restarted: device IDs and listeners from before are gone.
    private func serviceRestarted() {
        installSystemListeners()
        restart(because: "coreaudiod restarted")
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
            inputGaps: Int(stats.inputGaps),
            resyncs: Int(stats.resyncs)
        )
    }

    /// Called once a second. Logs every glitch the core counted, so a reported
    /// break-up can be matched to its cause afterwards, and restarts audio whose
    /// IO stopped altogether (a device that died without telling us).
    func checkHealth() {
        guard inputDevice != nil, outputDevice != nil else {
            lastStats = nil
            stalledSeconds = 0
            return
        }
        var stats = PTStats()
        pt_get_stats(core, &stats)
        defer { lastStats = stats }
        guard let last = lastStats else { return }

        let events = [
            ("dropouts", stats.dropouts - last.dropouts),
            ("input gaps", stats.inputGaps - last.inputGaps),
            ("jitter underruns", stats.underruns - last.underruns),
            ("input errors", stats.inputErrors - last.inputErrors),
            ("output skips", stats.outputSkips - last.outputSkips),
            ("resyncs", stats.resyncs - last.resyncs),
            ("overruns", stats.overruns - last.overruns),
        ].filter { $0.1 > 0 }
        if !events.isEmpty {
            let text = events.map { "\($0.0) +\($0.1)" }.joined(separator: ", ")
            audioLog.notice("glitch: \(text, privacy: .public); fill \(Int(stats.fillFrames)) / target \(stats.targetFrames), \(Int(stats.ratePPM)) ppm")
        }

        let stalled = stats.inputCallbacks == last.inputCallbacks || stats.outputCallbacks == last.outputCallbacks
        if !stalled {
            stalledSeconds = 0
            // Back to quick restarts only once IO has kept running for a while.
            healthySeconds += 1
            if healthySeconds >= 10 { stallRestartDelay = 3 }
            return
        }
        healthySeconds = 0
        stalledSeconds += 1
        // Core Audio's own IO restarts pause both sides for up to ~1.2 s; only
        // a longer silence is a stuck device.
        if stalledSeconds >= stallRestartDelay {
            stalledSeconds = 0
            stallRestartDelay = min(stallRestartDelay * 2, 60)
            restart(because: stats.inputCallbacks == last.inputCallbacks ? "input IO stalled" : "output IO stalled")
        }
    }

    func start(inputUID: String) throws {
        stop()
        guard let device = CoreAudioDevices.deviceID(forUID: inputUID) else {
            throw AudioPassthroughError.deviceNotFound
        }
        let inputRate = CoreAudioDevices.nominalSampleRate(device) ?? 48_000
        // Output first so it is already priming silence when input arrives.
        try startOutput(inputRate: inputRate)
        let status = pt_start_input(core, device, Self.ioFrames)
        guard status == noErr else {
            stopOutput()
            audioLog.error("input start failed: \(status)")
            throw AudioPassthroughError.coreAudio("오디오 입력 시작", status)
        }
        if pt_input_sample_rate(core) != inputRate {
            // The card changed rate in between: the converter needs the rate in use.
            stopOutput()
            do {
                try startOutput(inputRate: pt_input_sample_rate(core))
            } catch {
                pt_stop_input(core)
                throw error
            }
        }
        self.inputUID = inputUID
        inputDevice = device
        // A new card sample rate needs a new stream format on both sides.
        inputListeners = [CoreAudioListener(device, kAudioDevicePropertyNominalSampleRate) { [weak self] in
            guard let self, CoreAudioDevices.nominalSampleRate(device) != pt_input_sample_rate(self.core) else { return }
            self.restart(because: "input sample rate changed")
        }]
        applyGain()
        audioLog.notice("started: \(CoreAudioDevices.name(device) ?? "?", privacy: .public) \(Int(pt_input_sample_rate(self.core))) Hz -> \(self.outputDevice.flatMap(CoreAudioDevices.name) ?? "?", privacy: .public) \(Int(self.outputRate ?? 0)) Hz")
    }

    func stop() {
        if inputDevice != nil || outputDevice != nil {
            var stats = PTStats()
            pt_get_stats(core, &stats)
            audioLog.notice("stopped; totals: dropouts \(stats.dropouts), input gaps \(stats.inputGaps), jitter underruns \(stats.underruns), input errors \(stats.inputErrors), output skips \(stats.outputSkips), resyncs \(stats.resyncs), overruns \(stats.overruns)")
        }
        removeListeners(&inputListeners)
        pt_stop_input(core)
        stopOutput()
        inputUID = nil
        inputDevice = nil
    }

    private func restart(because reason: String) {
        guard let inputUID else { return }
        audioLog.notice("restarting: \(reason, privacy: .public)")
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
            audioLog.error("output start failed: \(status)")
            throw AudioPassthroughError.coreAudio("오디오 출력 시작", status)
        }
        outputDevice = device
        outputRate = pt_output_sample_rate(core)
        outputListeners = [CoreAudioListener(device, kAudioDevicePropertyNominalSampleRate) { [weak self] in
            self?.restartOutputIfChanged(because: "output sample rate changed")
        }]
    }

    private func stopOutput() {
        removeListeners(&outputListeners)
        pt_stop_output(core)
        outputDevice = nil
        outputRate = nil
    }

    /// The default output changed (headphones plugged in, AirPods connected,
    /// ...) or its sample rate changed: rebuild only the output side. Each
    /// rebuild is a short gap, so a notification that changed nothing is ignored.
    private func restartOutputIfChanged(because reason: String) {
        guard inputDevice != nil else { return }
        if let outputDevice, outputDevice == CoreAudioDevices.defaultOutputDevice(),
           CoreAudioDevices.nominalSampleRate(outputDevice) == outputRate {
            return
        }
        audioLog.notice("restarting output: \(reason, privacy: .public)")
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
