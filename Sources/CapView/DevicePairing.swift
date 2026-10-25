import Foundation

/// Finds the audio input that belongs to the same capture card as a video device.
///
/// Capture cards show up as two devices: a UVC camera and a USB audio input.
/// They usually share a name ("StreamLine Mini+ GC311G2" / "Cam Link 4K"), but
/// cheap cards use generic names ("USB Video" / "USB Digital Audio"), so the
/// USB vendor/product IDs are compared as well:
///   video modelID: "UVC Camera VendorID_1994 ProductID_12562"   (decimal)
///   audio modelUID: "StreamLine Mini+ GC311G2:07CA:3112"          (hex)
enum DevicePairing {
    static func audioInput(for video: VideoDeviceInfo, among inputs: [AudioInputInfo]) -> AudioInputInfo? {
        if let byName = inputs.first(where: { $0.name == video.name }) {
            return byName
        }
        if let ids = usbIDs(fromVideoModelID: video.modelID) {
            let needle = String(format: ":%04X:%04X", ids.vendor, ids.product)
            if let byUSB = inputs.first(where: { $0.modelUID.uppercased().contains(needle) }) {
                return byUSB
            }
        }
        return nil
    }

    /// Parses "... VendorID_1994 ProductID_12562" into (0x07CA, 0x3112).
    static func usbIDs(fromVideoModelID modelID: String) -> (vendor: Int, product: Int)? {
        func number(after key: String) -> Int? {
            guard let range = modelID.range(of: key) else { return nil }
            return Int(modelID[range.upperBound...].prefix { $0.isNumber })
        }
        guard let vendor = number(after: "VendorID_"), let product = number(after: "ProductID_") else {
            return nil
        }
        return (vendor, product)
    }
}
