import CoreAudio
import Foundation

/// A Core Audio input device as shown in the menus.
struct AudioInputInfo: Sendable, Hashable {
    let id: AudioObjectID
    /// Persistent identifier; equals the matching `AVCaptureDevice.uniqueID`.
    let uid: String
    let name: String
    /// e.g. "StreamLine Mini+ GC311G2:07CA:3112" — carries the USB IDs.
    let modelUID: String
}

/// Thin wrappers over the Core Audio HAL property API.
enum CoreAudioDevices {
    static func inputs() -> [AudioInputInfo] {
        allDeviceIDs().compactMap { id in
            guard streamCount(id, scope: kAudioObjectPropertyScopeInput) > 0,
                  let uid = string(id, kAudioDevicePropertyDeviceUID),
                  let name = string(id, kAudioObjectPropertyName)
            else { return nil }
            return AudioInputInfo(id: id, uid: uid, name: name, modelUID: string(id, kAudioDevicePropertyModelUID) ?? "")
        }
    }

    static func deviceID(forUID uid: String) -> AudioObjectID? {
        allDeviceIDs().first { string($0, kAudioDevicePropertyDeviceUID) == uid }
    }

    static func defaultOutputDevice() -> AudioObjectID? {
        var address = globalAddress(kAudioHardwarePropertyDefaultOutputDevice)
        var id = AudioObjectID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        let status = AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &id)
        return status == noErr && id != kAudioObjectUnknown ? id : nil
    }

    static func nominalSampleRate(_ id: AudioObjectID) -> Double? {
        var address = globalAddress(kAudioDevicePropertyNominalSampleRate)
        var rate: Float64 = 0
        var size = UInt32(MemoryLayout<Float64>.size)
        let status = AudioObjectGetPropertyData(id, &address, 0, nil, &size, &rate)
        return status == noErr && rate > 0 ? rate : nil
    }

    /// Frames between the hardware and this process's IO callback in `scope`:
    /// device and stream latency, safety offset and one IO buffer.
    static func ioLatencyFrames(_ id: AudioObjectID, scope: AudioObjectPropertyScope) -> Int {
        func value(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector, _ scope: AudioObjectPropertyScope) -> Int {
            var address = AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: kAudioObjectPropertyElementMain)
            var result: UInt32 = 0
            var size = UInt32(MemoryLayout<UInt32>.size)
            return AudioObjectGetPropertyData(object, &address, 0, nil, &size, &result) == noErr ? Int(result) : 0
        }
        var total = value(id, kAudioDevicePropertyLatency, scope)
            + value(id, kAudioDevicePropertySafetyOffset, scope)
            + value(id, kAudioDevicePropertyBufferFrameSize, scope)

        var address = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreams, mScope: scope, mElement: kAudioObjectPropertyElementMain)
        var stream = AudioStreamID(0)
        var size = UInt32(MemoryLayout<AudioStreamID>.size)
        if AudioObjectGetPropertyData(id, &address, 0, nil, &size, &stream) == noErr, size > 0 {
            total += value(stream, kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal)
        }
        return total
    }

    static func allDeviceIDs() -> [AudioObjectID] {
        var address = globalAddress(kAudioHardwarePropertyDevices)
        let system = AudioObjectID(kAudioObjectSystemObject)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(system, &address, 0, nil, &size) == noErr, size > 0 else { return [] }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(system, &address, 0, nil, &size, &ids) == noErr else { return [] }
        return ids
    }

    static func string(_ id: AudioObjectID, _ selector: AudioObjectPropertySelector) -> String? {
        var address = globalAddress(selector)
        var value: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        let status = withUnsafeMutablePointer(to: &value) {
            AudioObjectGetPropertyData(id, &address, 0, nil, &size, $0)
        }
        guard status == noErr, let value else { return nil }
        return value.takeRetainedValue() as String
    }

    static func streamCount(_ id: AudioObjectID, scope: AudioObjectPropertyScope) -> Int {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreams,
            mScope: scope,
            mElement: kAudioObjectPropertyElementMain
        )
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &address, 0, nil, &size) == noErr else { return 0 }
        return Int(size) / MemoryLayout<AudioStreamID>.size
    }

    static func globalAddress(_ selector: AudioObjectPropertySelector) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
    }
}

/// A Core Audio property listener that can actually be removed.
///
/// The block-based API cannot be used from Swift for this: a Swift closure is
/// wrapped in a new block on every call, so `AudioObjectRemovePropertyListenerBlock`
/// never matches the registered one. The function-pointer API identifies a
/// listener by (proc, clientData) instead, which this object keeps stable.
final class CoreAudioListener: @unchecked Sendable {
    private let object: AudioObjectID
    private var address: AudioObjectPropertyAddress
    private let handler: @MainActor () -> Void
    private var isRegistered = false

    /// Calls `handler` on the main thread whenever `selector` on `object` changes.
    init(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector, handler: @escaping @MainActor () -> Void) {
        self.object = object
        address = CoreAudioDevices.globalAddress(selector)
        self.handler = handler
        // Retained while registered: the HAL keeps only the raw pointer.
        isRegistered = AudioObjectAddPropertyListener(
            object, &address, Self.proc, Unmanaged.passRetained(self).toOpaque()
        ) == noErr
        if !isRegistered {
            Unmanaged.passUnretained(self).release()
        }
    }

    func remove() {
        guard isRegistered else { return }
        isRegistered = false
        AudioObjectRemovePropertyListener(object, &address, Self.proc, Unmanaged.passUnretained(self).toOpaque())
        // A notification may already be in flight on a HAL thread; let it
        // finish before dropping the reference the HAL was holding.
        let pointer = Unmanaged.passUnretained(self)
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { pointer.release() }
    }

    private static let proc: AudioObjectPropertyListenerProc = { _, _, _, clientData in
        guard let clientData else { return noErr }
        let listener = Unmanaged<CoreAudioListener>.fromOpaque(clientData).takeUnretainedValue()
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                // Skip notifications that were already queued when remove() ran.
                if listener.isRegistered { listener.handler() }
            }
        }
        return noErr
    }
}
