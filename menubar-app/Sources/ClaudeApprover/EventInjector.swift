// EventInjector — translates device JSON events into macOS keyboard input via
// Quartz Event Services. Requires Accessibility permission.

import Foundation
import CoreGraphics
import AppKit

final class EventInjector {

    /// macOS virtual keycodes we use.
    private enum Key {
        static let returnKey: CGKeyCode = 0x24    // kVK_Return
        static let f19: CGKeyCode       = 0x50    // kVK_F19
    }

    private var functionHeld = false
    private var autoAcceptTimer: Timer?

    /// Tap a single key, no modifiers.
    func tap(key: CGKeyCode) {
        post(key: key, down: true)
        post(key: key, down: false)
    }

    /// Press the Wispr Flow combo (hold Ctrl+Option+F19).
    func pressWisprCombo() {
        guard !functionHeld else { return }
        functionHeld = true
        let flags: CGEventFlags = [.maskControl, .maskAlternate]
        post(key: Key.f19, down: true, flags: flags)
    }

    func releaseWisprCombo() {
        guard functionHeld else { return }
        functionHeld = false
        let flags: CGEventFlags = [.maskControl, .maskAlternate]
        post(key: Key.f19, down: false, flags: flags)
    }

    func startAutoAccept(interval: TimeInterval = 1.0) {
        stopAutoAccept()
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            let timer = Timer(timeInterval: interval, repeats: true) { _ in
                self.tap(key: Key.returnKey)
            }
            RunLoop.main.add(timer, forMode: .common)
            self.autoAcceptTimer = timer
        }
    }

    func stopAutoAccept() {
        DispatchQueue.main.async { [weak self] in
            self?.autoAcceptTimer?.invalidate()
            self?.autoAcceptTimer = nil
        }
    }

    func tapReturn() {
        tap(key: Key.returnKey)
    }

    // MARK: Permission

    /// Returns true if Accessibility access is granted (required for CGEventPost).
    /// Triggers the system prompt the first time it's called with prompt=true.
    @discardableResult
    static func ensureAccessibility(prompt: Bool = true) -> Bool {
        let opts: NSDictionary = [
            kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: prompt
        ]
        return AXIsProcessTrustedWithOptions(opts)
    }

    // MARK: Internals

    private func post(key: CGKeyCode, down: Bool, flags: CGEventFlags = []) {
        guard let event = CGEvent(keyboardEventSource: nil, virtualKey: key, keyDown: down) else {
            return
        }
        if !flags.isEmpty { event.flags = flags }
        event.post(tap: .cghidEventTap)
    }
}
