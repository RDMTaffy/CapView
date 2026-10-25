import Foundation

/// How the audio input is chosen.
enum AudioSelection: Equatable, Sendable {
    /// Use the audio device that belongs to the selected capture card.
    case automatic
    /// Play no audio.
    case off
    /// A specific Core Audio input device, by UID.
    case device(uid: String)
}

/// User choices persisted between launches. `nil` means "pick automatically".
@MainActor
enum Prefs {
    private static let defaults = UserDefaults.standard

    static var videoDeviceID: String? {
        get { defaults.string(forKey: "videoDeviceID") }
        set { defaults.set(newValue, forKey: "videoDeviceID") }
    }

    /// The saved device's model ID. A UVC device's unique ID contains the USB
    /// port location, so this finds the same card after it moves to another port.
    static var videoModelID: String? {
        get { defaults.string(forKey: "videoModelID") }
        set { defaults.set(newValue, forKey: "videoModelID") }
    }

    /// Shown when the saved device is missing.
    static var videoDeviceName: String? {
        get { defaults.string(forKey: "videoDeviceName") }
        set { defaults.set(newValue, forKey: "videoDeviceName") }
    }

    /// Requested capture size; `nil` picks the largest the device offers.
    static var resolution: (width: Int32, height: Int32)? {
        get {
            let parts = (defaults.string(forKey: "resolution") ?? "").split(separator: "x")
            guard parts.count == 2, let w = Int32(parts[0]), let h = Int32(parts[1]) else { return nil }
            return (w, h)
        }
        set { defaults.set(newValue.map { "\($0.width)x\($0.height)" }, forKey: "resolution") }
    }

    /// Requested frame rate; `nil` picks the fastest the device offers.
    static var frameRate: Double? {
        get { defaults.object(forKey: "frameRate") as? Double }
        set { defaults.set(newValue, forKey: "frameRate") }
    }

    static var audioSelection: AudioSelection {
        get {
            switch defaults.string(forKey: "audioSelection") {
            case nil, "auto": .automatic
            case "off": .off
            case let uid?: .device(uid: uid)
            }
        }
        set {
            let raw = switch newValue {
            case .automatic: "auto"
            case .off: "off"
            case .device(let uid): uid
            }
            defaults.set(raw, forKey: "audioSelection")
        }
    }

    static var volume: Float {
        get { defaults.object(forKey: "volume") as? Float ?? 1.0 }
        set { defaults.set(newValue, forKey: "volume") }
    }

    static var muted: Bool {
        get { defaults.bool(forKey: "muted") }
        set { defaults.set(newValue, forKey: "muted") }
    }

    static var picture: PictureSettings {
        get {
            guard let data = defaults.data(forKey: "picture"),
                  let settings = try? JSONDecoder().decode(PictureSettings.self, from: data) else { return .neutral }
            return settings
        }
        set { defaults.set(try? JSONEncoder().encode(newValue), forKey: "picture") }
    }

    static var floatOnTop: Bool {
        get { defaults.bool(forKey: "floatOnTop") }
        set { defaults.set(newValue, forKey: "floatOnTop") }
    }
}
