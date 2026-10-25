// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "CapView",
    platforms: [.macOS(.v15)],
    targets: [
        .executableTarget(
            name: "CapView",
            dependencies: ["PassthroughCore"],
            path: "Sources/CapView",
            linkerSettings: [
                .linkedFramework("AVFoundation"),
                .linkedFramework("CoreAudio"),
                .linkedFramework("AudioToolbox"),
                .linkedFramework("IOKit"),
            ]
        ),
        // Audio IO-thread code. -Werror=function-effects makes the compiler
        // reject any allocation or locking inside CA_REALTIME_API functions.
        .target(
            name: "PassthroughCore",
            path: "Sources/PassthroughCore",
            cSettings: [.unsafeFlags(["-Wfunction-effects", "-Werror=function-effects"])],
            linkerSettings: [
                .linkedFramework("AudioToolbox"),
                .linkedFramework("CoreAudio"),
            ]
        ),
    ]
)
