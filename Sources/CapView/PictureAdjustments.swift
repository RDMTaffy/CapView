import AppKit
import CoreImage
import CoreImage.CIFilterBuiltins

/// Picture corrections applied on top of the captured video.
struct PictureSettings: Codable, Equatable, Sendable {
    /// Added to every channel, -0.3 ... 0.3.
    var brightness: Double = 0
    /// 0.5 ... 1.5, 1 = unchanged.
    var contrast: Double = 1
    /// 0 (grey) ... 2, 1 = unchanged.
    var saturation: Double = 1
    /// 0.5 ... 1.5; above 1 brightens the mid-tones.
    var gamma: Double = 1
    /// Hue rotation in degrees, -180 ... 180.
    var hue: Double = 0
    /// Stretches video range (16–235) to full range (0–255). Fixes a grey,
    /// washed-out picture when the console and card disagree on RGB range.
    var expandRange = false

    static let neutral = PictureSettings()

    init() {}
}

extension PictureSettings {
    private enum CodingKeys: String, CodingKey {
        case brightness, contrast, saturation, gamma, hue, expandRange
    }

    /// Missing keys (settings saved by another version) keep their defaults,
    /// and values are clamped to what the sliders can show.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        func value(_ key: CodingKeys, _ range: ClosedRange<Double>, _ fallback: Double) -> Double {
            let v = (try? c.decodeIfPresent(Double.self, forKey: key)) ?? nil
            return min(max(v ?? fallback, range.lowerBound), range.upperBound)
        }
        brightness = value(.brightness, -0.3...0.3, 0)
        contrast = value(.contrast, 0.5...1.5, 1)
        saturation = value(.saturation, 0...2, 1)
        gamma = value(.gamma, 0.5...1.5, 1)
        hue = value(.hue, -180...180, 0)
        expandRange = ((try? c.decodeIfPresent(Bool.self, forKey: .expandRange)) ?? nil) ?? false
    }
}

/// Builds the Core Image filters for `PictureSettings`.
///
/// Core Animation runs layer filters on linear-light values, but these
/// controls (and the 16–235 range math) are meant for gamma-encoded video
/// values, as in OBS. So the chain converts to sRGB encoding first and back
/// to linear at the end; without that, range expansion crushes every shadow.
enum PictureFilters {
    /// `nil` when nothing needs changing, so the video keeps its direct path.
    static func make(_ settings: PictureSettings) -> [CIFilter]? {
        guard settings != .neutral else { return nil }
        var filters: [CIFilter] = [CIFilter.linearToSRGBToneCurve()]
        if settings.expandRange {
            let scale = CGFloat(255.0 / 219.0)
            let bias = CGFloat(-16.0 / 219.0)
            let matrix = CIFilter.colorMatrix()
            matrix.rVector = CIVector(x: scale, y: 0, z: 0, w: 0)
            matrix.gVector = CIVector(x: 0, y: scale, z: 0, w: 0)
            matrix.bVector = CIVector(x: 0, y: 0, z: scale, w: 0)
            matrix.biasVector = CIVector(x: bias, y: bias, z: bias, w: 0)
            // Codes below 16 / above 235 are not "blacker than black" afterwards.
            let clamp = CIFilter.colorClamp()
            clamp.minComponents = CIVector(x: 0, y: 0, z: 0, w: 0)
            clamp.maxComponents = CIVector(x: 1, y: 1, z: 1, w: 1)
            filters.append(matrix)
            filters.append(clamp)
        }
        if settings.gamma != 1 {
            let gamma = CIFilter.gammaAdjust()
            gamma.power = Float(1 / settings.gamma)
            filters.append(gamma)
        }
        if settings.brightness != 0 || settings.contrast != 1 || settings.saturation != 1 {
            let controls = CIFilter.colorControls()
            controls.brightness = Float(settings.brightness)
            controls.contrast = Float(settings.contrast)
            controls.saturation = Float(settings.saturation)
            filters.append(controls)
        }
        if settings.hue != 0 {
            let hue = CIFilter.hueAdjust()
            hue.angle = Float(settings.hue * .pi / 180)
            filters.append(hue)
        }
        filters.append(CIFilter.sRGBToneCurveToLinear())
        return filters
    }
}

/// The "화면 조정" panel: sliders that change the picture live.
@MainActor
final class PictureAdjustmentsController: NSWindowController {
    /// Called on every change, while a slider is being dragged too.
    var onChange: ((PictureSettings) -> Void)?
    private(set) var settings: PictureSettings

    private struct Control {
        let title: String
        let keyPath: WritableKeyPath<PictureSettings, Double>
        let range: ClosedRange<Double>
        let format: (Double) -> String
    }

    private static let controls: [Control] = [
        Control(title: "밝기", keyPath: \.brightness, range: -0.3...0.3) { value in
            let percent = Int((value * 100).rounded())
            return percent > 0 ? "+\(percent)" : "\(percent)"
        },
        Control(title: "대비", keyPath: \.contrast, range: 0.5...1.5) { "\(Int(($0 * 100).rounded()))%" },
        Control(title: "채도", keyPath: \.saturation, range: 0...2) { "\(Int(($0 * 100).rounded()))%" },
        Control(title: "감마", keyPath: \.gamma, range: 0.5...1.5) { String(format: "%.2f", $0) },
        Control(title: "색조", keyPath: \.hue, range: -180...180) { "\(Int($0.rounded()))°" },
    ]

    private var needsInitialPosition: Bool
    private var sliders: [NSSlider] = []
    private var valueLabels: [NSTextField] = []
    private let rangeCheckbox = NSButton(checkboxWithTitle: "색 범위 확장 (화면이 회색빛으로 뿌옇게 보일 때)", target: nil, action: nil)

    init(settings: PictureSettings) {
        self.settings = settings
        let panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 360, height: 260),
            styleMask: [.titled, .closable, .utilityWindow, .hudWindow],
            backing: .buffered,
            defer: true
        )
        panel.title = "화면 조정"
        panel.isFloatingPanel = true
        // One level above the player, which floats too when 항상 위에 표시 is on.
        panel.level = NSWindow.Level(rawValue: NSWindow.Level.floating.rawValue + 1)
        // Clicking a slider must not take keyboard focus from the video (F, M, Esc).
        panel.becomesKeyOnlyIfNeeded = true
        panel.collectionBehavior = [.fullScreenAuxiliary]
        panel.isReleasedWhenClosed = false
        needsInitialPosition = !panel.setFrameUsingName("CapViewPictureAdjustments")
        super.init(window: panel)
        panel.contentView = makeContent()
        panel.setFrameAutosaveName("CapViewPictureAdjustments")
        refreshControls()
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    /// Shows the panel; the first time, next to the top-right of `window`.
    func show(beside window: NSWindow?) {
        guard let panel = self.window else { return }
        if needsInitialPosition, let window {
            needsInitialPosition = false
            panel.setFrameTopLeftPoint(NSPoint(x: window.frame.maxX - panel.frame.width - 16, y: window.frame.maxY - 40))
        }
        panel.orderFront(nil)
    }

    // MARK: Layout

    private func makeContent() -> NSView {
        var rows: [[NSView]] = []
        for (index, control) in Self.controls.enumerated() {
            let title = NSTextField(labelWithString: control.title)
            title.alignment = .right
            let slider = NSSlider(
                value: settings[keyPath: control.keyPath],
                minValue: control.range.lowerBound,
                maxValue: control.range.upperBound,
                target: self,
                action: #selector(sliderChanged(_:))
            )
            slider.isContinuous = true
            slider.tag = index
            slider.setAccessibilityLabel(control.title)
            slider.widthAnchor.constraint(equalToConstant: 190).isActive = true
            let value = NSTextField(labelWithString: "")
            value.font = .monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
            value.alignment = .right
            value.widthAnchor.constraint(equalToConstant: 44).isActive = true
            let reset = NSButton(
                image: NSImage(systemSymbolName: "arrow.counterclockwise", accessibilityDescription: "\(control.title) 초기화")!,
                target: self,
                action: #selector(resetOne(_:))
            )
            reset.isBordered = false
            reset.tag = index
            reset.toolTip = "\(control.title) 초기화"
            sliders.append(slider)
            valueLabels.append(value)
            rows.append([title, slider, value, reset])
        }
        let grid = NSGridView(views: rows)
        grid.rowSpacing = 6
        grid.columnSpacing = 8
        for index in 0..<grid.numberOfRows {
            grid.row(at: index).height = 24
            grid.row(at: index).yPlacement = .center
        }

        rangeCheckbox.target = self
        rangeCheckbox.action = #selector(rangeChanged(_:))
        let resetAll = NSButton(title: "모두 기본값", target: self, action: #selector(resetAll))

        let stack = NSStackView(views: [grid, rangeCheckbox, resetAll])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 14
        stack.edgeInsets = NSEdgeInsets(top: 16, left: 16, bottom: 16, right: 16)
        stack.setCustomSpacing(18, after: rangeCheckbox)
        return stack
    }

    private func refreshControls() {
        for (index, control) in Self.controls.enumerated() {
            let value = settings[keyPath: control.keyPath]
            sliders[index].doubleValue = value
            sliders[index].setAccessibilityValueDescription(control.format(value))
            valueLabels[index].stringValue = control.format(value)
        }
        rangeCheckbox.state = settings.expandRange ? .on : .off
    }

    // MARK: Actions

    @objc private func sliderChanged(_ sender: NSSlider) {
        let control = Self.controls[sender.tag]
        var value = sender.doubleValue
        // Snap to the default near the middle so "unchanged" is easy to hit.
        let neutral = PictureSettings.neutral[keyPath: control.keyPath]
        if abs(value - neutral) < (control.range.upperBound - control.range.lowerBound) * 0.015 {
            value = neutral
        }
        settings[keyPath: control.keyPath] = value
        valueLabels[sender.tag].stringValue = control.format(value)
        sender.setAccessibilityValueDescription(control.format(value))
        onChange?(settings)
    }

    @objc private func resetOne(_ sender: NSButton) {
        let keyPath = Self.controls[sender.tag].keyPath
        settings[keyPath: keyPath] = PictureSettings.neutral[keyPath: keyPath]
        refreshControls()
        onChange?(settings)
    }

    @objc private func rangeChanged(_ sender: NSButton) {
        settings.expandRange = sender.state == .on
        onChange?(settings)
    }

    @objc private func resetAll() {
        settings = .neutral
        refreshControls()
        onChange?(settings)
    }
}
