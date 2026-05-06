// ClaudeApprover — menu-bar app that bridges the BLE Claude Approval Device to
// macOS keyboard input. Receives JSON button/switch events from the device and
// posts them as CGKeyboardEvents into the focused window. Sends LED/buzzer
// commands back over the same BLE NUS link.
//
// Activation: NSApplicationActivationPolicy.accessory (no Dock icon, no
// regular window). Status item lives in the menu bar with a flame icon.

import AppKit
import SwiftUI
import Combine

@main
struct ClaudeApproverApp: App {
    @StateObject private var bridge = Bridge()

    init() {
        // Hide Dock icon — pure menu-bar app.
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    var body: some Scene {
        MenuBarExtra {
            MenuView(bridge: bridge)
        } label: {
            Image(systemName: bridge.iconName)
        }
        .menuBarExtraStyle(.window)
    }
}

/// Top-level glue that connects the BLE controller to the keyboard injector.
final class Bridge: ObservableObject {
    let ble = BLEController()
    let injector = EventInjector()

    @Published var hasAccessibility: Bool = false

    init() {
        hasAccessibility = EventInjector.ensureAccessibility(prompt: false)
        ble.onEvent = { [weak self] obj in self?.handle(obj) }
        ble.start()
    }

    var iconName: String {
        switch ble.status {
        case .connected:    return "flame.fill"
        case .connecting:   return "flame"
        case .scanning:     return "magnifyingglass.circle"
        case .disconnected: return "flame.circle"
        case .poweredOff:   return "exclamationmark.triangle"
        case .unauthorized: return "lock"
        case .unknown:      return "questionmark.circle"
        }
    }

    func requestAccessibility() {
        hasAccessibility = EventInjector.ensureAccessibility(prompt: true)
    }

    private func handle(_ obj: [String: Any]) {
        if let evt = obj["evt"] as? String {
            switch evt {
            case "hello":
                NSLog("[Bridge] hello from device: \(obj)")
            case "button":
                let id = obj["id"] as? String ?? "?"
                let action = obj["action"] as? String ?? "?"
                handleButton(id: id, action: action)
            case "switch":
                let id = obj["id"] as? String ?? "?"
                let state = obj["state"] as? String ?? "?"
                handleSwitch(id: id, state: state)
            default:
                NSLog("[Bridge] unhandled evt: \(obj)")
            }
        } else if obj["ack"] != nil {
            // Acks from the device, fine to ignore for now.
        } else if let cmd = obj["cmd"] as? String {
            // The device occasionally sends Hardware Buddy permission decisions
            // in the same direction; not relevant to us as middleman.
            NSLog("[Bridge] device cmd: \(cmd)")
        }
    }

    private func handleButton(id: String, action: String) {
        switch (id, action) {
        case ("function", "press"):   injector.pressWisprCombo()
        case ("function", "release"): injector.releaseWisprCombo()
        case ("return", "tap"):       injector.tapReturn()
        default:
            NSLog("[Bridge] button \(id) \(action) — no mapping")
        }
    }

    private func handleSwitch(id: String, state: String) {
        guard id == "auto_accept" else { return }
        if state == "on" { injector.startAutoAccept() }
        else             { injector.stopAutoAccept() }
    }
}

struct MenuView: View {
    @ObservedObject var bridge: Bridge

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Image(systemName: bridge.iconName)
                Text(statusText)
                    .bold()
            }
            if let id = bridge.ble.deviceIdentifier {
                Text("device: \(id.uuidString.prefix(8))…")
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
            }
            if !bridge.ble.lastIncomingLine.isEmpty {
                Text("last: \(bridge.ble.lastIncomingLine)")
                    .font(.caption.monospaced())
                    .lineLimit(2)
                    .foregroundStyle(.secondary)
            }

            Divider()

            if !bridge.hasAccessibility {
                Button("Grant Accessibility Permission…") {
                    bridge.requestAccessibility()
                }
                Text("Required for keystroke injection.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Divider()
            }

            Button("Reconnect") { bridge.ble.reconnect() }
            Menu("LED Status") {
                Button("Breathing") { bridge.ble.send(["cmd": "led", "mode": "breathing"]) }
                Button("Pulsing")   { bridge.ble.send(["cmd": "led", "mode": "pulsing"]) }
                Button("Solid")     { bridge.ble.send(["cmd": "led", "mode": "solid"]) }
                Button("Flash")     { bridge.ble.send(["cmd": "led", "mode": "flash"]) }
                Button("Off")       { bridge.ble.send(["cmd": "led", "mode": "off"]) }
            }
            Menu("Test Jingles") {
                Button("Approved")  { bridge.ble.send(["cmd": "jingle", "name": "approved"]) }
                Button("Denied")    { bridge.ble.send(["cmd": "jingle", "name": "denied"]) }
                Button("Thinking")  { bridge.ble.send(["cmd": "jingle", "name": "thinking"]) }
                Button("Startup")   { bridge.ble.send(["cmd": "jingle", "name": "startup"]) }
            }
            Divider()
            Button("Forget Saved Device") { bridge.ble.forgetSavedDevice() }
            Button("Quit Claude Approver") { NSApplication.shared.terminate(nil) }
        }
        .padding(12)
        .frame(width: 260)
    }

    private var statusText: String {
        switch bridge.ble.status {
        case .connected:    return "Connected"
        case .connecting:   return "Connecting…"
        case .scanning:     return "Scanning…"
        case .disconnected: return "Disconnected"
        case .poweredOff:   return "Bluetooth off"
        case .unauthorized: return "Bluetooth permission denied"
        case .unknown:      return "Initialising…"
        }
    }
}
