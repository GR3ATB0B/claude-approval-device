// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "ClaudeApprover",
    platforms: [.macOS(.v13)],
    targets: [
        // Diagnostic CLI used during BLE bring-up.
        .executableTarget(
            name: "PairTest",
            path: "Sources/PairTest"
        ),
        // The actual menu-bar app.
        .executableTarget(
            name: "ClaudeApprover",
            path: "Sources/ClaudeApprover"
        )
    ]
)
