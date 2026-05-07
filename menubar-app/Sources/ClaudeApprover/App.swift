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
            MenuBarIconView(state: bridge.iconState)
        }
        .menuBarExtraStyle(.window)
    }
}

/// Top-level glue that connects the BLE controller to the keyboard injector.
final class Bridge: ObservableObject {
    let ble = BLEController()
    let injector = EventInjector()
    private var httpServer: LocalHTTPServer?

    @Published var hasAccessibility: Bool = false
    @Published var httpRunning: Bool = false
    @Published var autoAcceptOn: Bool = false  // software toggle, bypasses broken D2 switch

    init() {
        hasAccessibility = EventInjector.ensureAccessibility(prompt: false)
        ble.onEvent = { [weak self] obj in self?.handle(obj) }
        ble.start()
        startHTTPServer()
    }

    private func startHTTPServer() {
        let server = LocalHTTPServer { [weak self] method, path, body in
            self?.handleHTTP(method: method, path: path, body: body) ?? (404, Data())
        }
        do {
            try server.start()
            httpServer = server
            DispatchQueue.main.async { self.httpRunning = true }
        } catch {
            NSLog("[HTTP] failed to start: \(error)")
        }
    }

    private func handleHTTP(method: String, path: String, body: Data) -> (Int, Data) {
        let json = (try? JSONSerialization.jsonObject(with: body)) as? [String: Any] ?? [:]
        switch (method, path) {
        case ("GET", "/status"):
            let payload: [String: Any] = [
                "connected": ble.status == .connected,
                "ble_status": String(describing: ble.status),
                "device_id": ble.deviceIdentifier?.uuidString ?? "",
                "last_event": ble.lastIncomingLine
            ]
            return (200, jsonData(payload))
        case ("POST", "/led"):
            ble.send(["cmd": "led", "mode": json["mode"] as? String ?? "breathing"])
            return (200, jsonData(["ok": true]))
        case ("POST", "/jingle"):
            ble.send(["cmd": "jingle", "name": json["name"] as? String ?? "startup"])
            return (200, jsonData(["ok": true]))
        case ("POST", "/thinking/start"):
            // Claude Code hook: user just submitted a prompt → spinner is on.
            ble.send(["cmd": "led", "mode": "pulsing"])
            return (200, jsonData(["ok": true]))
        case ("POST", "/thinking/stop"):
            // Claude Code hook: assistant finished → spinner gone, beep + solid.
            ble.send(["cmd": "led", "mode": "solid"])
            ble.send(["cmd": "jingle", "name": "done"])
            return (200, jsonData(["ok": true]))
        case ("POST", "/tone"):
            let freq = json["freq"] as? Int ?? 1000
            let ms = json["ms"] as? Int ?? 100
            ble.send(["cmd": "tone", "freq": freq, "ms": ms])
            return (200, jsonData(["ok": true]))
        default:
            return (404, jsonData(["error": "unknown route"]))
        }
    }

    private func jsonData(_ obj: [String: Any]) -> Data {
        (try? JSONSerialization.data(withJSONObject: obj)) ?? Data()
    }

    var iconState: IconState {
        switch ble.status {
        case .connected:                       return .connected
        case .connecting, .scanning:           return .connecting
        case .disconnected, .unknown:          return .disconnected
        case .poweredOff, .unauthorized:       return .warning
        }
    }

    func requestAccessibility() {
        hasAccessibility = EventInjector.ensureAccessibility(prompt: true)
    }

    /// Software auto-accept toggle. The physical D2 switch is unreliable
    /// (stuck LOW), so this bypasses BLE switch events entirely and drives
    /// the Return-spam timer directly from the menu UI.
    func toggleAutoAccept() {
        autoAcceptOn.toggle()
        if autoAcceptOn { injector.startAutoAccept() }
        else            { injector.stopAutoAccept() }
        NSLog("[Bridge] software auto-accept = \(autoAcceptOn)")
    }

    /// Releases every key we might have held down, stops the auto-accept
    /// timer, and asks the device to flip its LED off so the device's switch
    /// state mirrors what the host now expects.
    func emergencyStop() {
        injector.panicReleaseAll()
        ble.send(["cmd": "led", "mode": "off"])
        NSLog("[Bridge] EMERGENCY STOP")
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
        // D2 hardware is unreliable — switch events from the device are
        // logged but ignored. Auto-accept is now driven from the menu UI.
        NSLog("[Bridge] BLE switch event ignored: \(id)=\(state) (D2 stuck — use menu toggle)")
    }
}

struct MenuView: View {
    @ObservedObject var bridge: Bridge

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Circle()
                    .fill(statusColor)
                    .frame(width: 9, height: 9)
                MenuBarIconView(state: bridge.iconState)
                Text(statusText).bold()
            }

            // Big AUTO-ACCEPT toggle — software replacement for the broken D2 switch.
            Button(action: { bridge.toggleAutoAccept() }) {
                HStack {
                    Image(systemName: bridge.autoAcceptOn
                          ? "checkmark.circle.fill"
                          : "circle")
                    Text(bridge.autoAcceptOn ? "AUTO-ACCEPT: ON" : "AUTO-ACCEPT: OFF").bold()
                }
                .frame(maxWidth: .infinity, alignment: .center)
                .padding(.vertical, 6)
                .background((bridge.autoAcceptOn ? Color.green : Color.gray).opacity(0.22))
                .cornerRadius(6)
            }
            .buttonStyle(.plain)
            .help("Spams the Return key every second while ON. Click to toggle.")

            // Big red EMERGENCY STOP — released stuck keys + stops auto-accept.
            Button(action: {
                bridge.emergencyStop()
                bridge.autoAcceptOn = false
            }) {
                HStack {
                    Image(systemName: "stop.circle.fill")
                    Text("Emergency Stop").bold()
                }
                .frame(maxWidth: .infinity, alignment: .center)
                .padding(.vertical, 4)
                .background(Color.red.opacity(0.18))
                .cornerRadius(6)
            }
            .buttonStyle(.plain)
            .keyboardShortcut(.escape)
            .help("Release all held keys, stop auto-accept timer, clear stuck modifiers.")

            if let id = bridge.ble.deviceIdentifier {
                Text("device: \(id.uuidString.prefix(8))…")
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
            }
            if let pct = bridge.ble.batteryPercent {
                HStack(spacing: 4) {
                    Image(systemName: batteryIcon(for: pct))
                        .foregroundStyle(batteryColor(for: pct))
                    Text("\(pct)%")
                        .font(.caption.monospaced().bold())
                    if let mv = bridge.ble.batteryMillivolts {
                        Text("(\(String(format: "%.2f", Double(mv) / 1000.0)) V)")
                            .font(.caption.monospaced())
                            .foregroundStyle(.secondary)
                    }
                }
            }
            if !bridge.ble.lastIncomingLine.isEmpty {
                Text("last: \(bridge.ble.lastIncomingLine)")
                    .font(.caption.monospaced())
                    .lineLimit(2)
                    .foregroundStyle(.secondary)
            }

            HStack(spacing: 12) {
                Label(bridge.hasAccessibility ? "Keys" : "No Keys",
                      systemImage: bridge.hasAccessibility ? "keyboard" : "keyboard.badge.eye")
                    .font(.caption)
                    .foregroundStyle(bridge.hasAccessibility ? .green : .red)
                Label(bridge.httpRunning ? "MCP" : "MCP off",
                      systemImage: bridge.httpRunning ? "network" : "network.slash")
                    .font(.caption)
                    .foregroundStyle(bridge.httpRunning ? .green : .red)
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

    private func batteryIcon(for pct: Int) -> String {
        switch pct {
        case 80...:    return "battery.100"
        case 60..<80:  return "battery.75"
        case 40..<60:  return "battery.50"
        case 20..<40:  return "battery.25"
        default:       return "battery.0"
        }
    }

    private func batteryColor(for pct: Int) -> Color {
        switch pct {
        case 30...: return .green
        case 15..<30: return .orange
        default: return .red
        }
    }

    private var statusColor: Color {
        switch bridge.ble.status {
        case .connected:                       return .green
        case .connecting, .scanning:           return .orange
        case .disconnected, .unknown:          return .gray
        case .poweredOff, .unauthorized:       return .red
        }
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
