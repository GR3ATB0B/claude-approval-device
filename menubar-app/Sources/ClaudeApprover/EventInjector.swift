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
        static let control: CGKeyCode   = 0x3B    // kVK_Control
        static let option: CGKeyCode    = 0x3A    // kVK_Option
    }

    private var functionHeld = false
    private var autoAcceptTimer: Timer?

    /// Releases every key we might have asserted, stops the auto-accept timer.
    /// Call this from the menu-bar "Emergency Stop" button to clear any stuck
    /// key state if something gets wedged.
    func panicReleaseAll() {
        NSLog("[Injector] PANIC release all")
        stopAutoAccept()
        functionHeld = false
        let keys: [CGKeyCode] = [
            Key.returnKey, Key.f19, Key.control, Key.option,
            0x37 /*Cmd*/, 0x38 /*LShift*/, 0x3C /*RShift*/, 0x3E /*RCtrl*/, 0x3D /*ROpt*/
        ]
        for k in keys {
            post(key: k, down: false)
        }
    }

    /// Tap a single key, no modifiers.
    func tap(key: CGKeyCode) {
        let trusted = AXIsProcessTrusted()
        NSLog("[Injector] tap key=0x\(String(key, radix: 16)) trusted=\(trusted)")
        post(key: key, down: true)
        post(key: key, down: false)
    }

    /// Press the Wispr Flow combo (Ctrl+Option+F19).
    /// Wispr's hotkey-capture UI inspects NSEvent.modifierFlags, which only
    /// reports "active" if the modifier keys are actually held — flags alone
    /// on F19 aren't enough. So we post real modifier keydowns first with the
    /// cumulative flag state, then F19, fast enough that no intermediate
    /// state matches a different hotkey.
    func pressWisprCombo() {
        guard !functionHeld else { return }
        functionHeld = true
        post(key: Key.control, down: true,  flags: [.maskControl])
        post(key: Key.option,  down: true,  flags: [.maskControl, .maskAlternate])
        post(key: Key.f19,     down: true,  flags: [.maskControl, .maskAlternate])
    }

    func releaseWisprCombo() {
        guard functionHeld else { return }
        functionHeld = false
        post(key: Key.f19,     down: false, flags: [.maskControl, .maskAlternate])
        post(key: Key.option,  down: false, flags: [.maskControl, .maskAlternate])
        post(key: Key.control, down: false, flags: [.maskControl])
    }

    func startAutoAccept(interval: TimeInterval = 1.0) {
        NSLog("[Injector] startAutoAccept interval=\(interval)")
        stopAutoAccept()
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            let timer = Timer(timeInterval: interval, repeats: true) { _ in
                NSLog("[Injector] auto-accept timer fire")
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
            NSLog("[Injector] CGEvent allocation failed")
            return
        }
        // Always assign flags. Leaving them unset lets CGEvent inherit the
        // current system modifier state, so a Return tap right after the
        // Wispr combo released could arrive as Ctrl+Opt+Return.
        event.flags = flags
        event.post(tap: .cghidEventTap)
        NSLog("[Injector] posted key=0x\(String(key, radix: 16)) down=\(down) flags=\(flags.rawValue)")
    }
}
