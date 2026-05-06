// MenuBarIcon — custom logo for the menu-bar status item, drawn programmatically.
//
// The icon is a stylized check-mark inside a soft rounded square, vector-drawn
// each render. Returned as a template image so macOS auto-tints it for dark
// and light menu bars. Connection state changes the fill: connected = solid,
// connecting = half-filled, disconnected = outline only.

import AppKit
import SwiftUI

enum IconState {
    case connected
    case connecting
    case disconnected
    case warning   // bluetooth off / unauthorized / unknown
}

extension NSImage {
    static func claudeApproverMenuIcon(state: IconState) -> NSImage {
        let size = NSSize(width: 18, height: 18)
        let img = NSImage(size: size, flipped: false) { rect in
            let bg = rect.insetBy(dx: 0.5, dy: 0.5)
            let path = NSBezierPath(roundedRect: bg, xRadius: 5, yRadius: 5)
            NSColor.black.setStroke()
            path.lineWidth = 1.5

            switch state {
            case .connected:
                NSColor.black.setFill()
                path.fill()
                drawCheck(in: rect, color: .white)
            case .connecting:
                // Half-filled: top half stroked, bottom half filled
                path.stroke()
                let bottomHalf = NSBezierPath()
                bottomHalf.move(to: NSPoint(x: bg.minX, y: bg.midY))
                bottomHalf.line(to: NSPoint(x: bg.minX, y: bg.minY + 5))
                bottomHalf.appendArc(withCenter: NSPoint(x: bg.minX + 5, y: bg.minY + 5), radius: 5, startAngle: 180, endAngle: 270)
                bottomHalf.line(to: NSPoint(x: bg.maxX - 5, y: bg.minY))
                bottomHalf.appendArc(withCenter: NSPoint(x: bg.maxX - 5, y: bg.minY + 5), radius: 5, startAngle: 270, endAngle: 360)
                bottomHalf.line(to: NSPoint(x: bg.maxX, y: bg.midY))
                bottomHalf.close()
                NSColor.black.setFill()
                bottomHalf.fill()
            case .disconnected:
                path.stroke()
            case .warning:
                NSColor.black.setFill()
                path.fill()
                drawExclamation(in: rect, color: .white)
            }
            return true
        }
        img.isTemplate = true
        return img
    }

    private static func drawCheck(in rect: NSRect, color: NSColor) {
        let p = NSBezierPath()
        p.move(to: NSPoint(x: rect.minX + 4.5, y: rect.midY + 0.5))
        p.line(to: NSPoint(x: rect.minX + 7,   y: rect.midY - 2.5))
        p.line(to: NSPoint(x: rect.maxX - 4,   y: rect.midY + 3))
        color.setStroke()
        p.lineWidth = 2.0
        p.lineCapStyle = .round
        p.lineJoinStyle = .round
        p.stroke()
    }

    private static func drawExclamation(in rect: NSRect, color: NSColor) {
        color.setFill()
        let bar = NSBezierPath(roundedRect:
            NSRect(x: rect.midX - 1, y: rect.midY - 1, width: 2, height: 6),
            xRadius: 1, yRadius: 1)
        bar.fill()
        let dot = NSBezierPath(ovalIn:
            NSRect(x: rect.midX - 1, y: rect.midY - 4.5, width: 2, height: 2))
        dot.fill()
    }
}

/// SwiftUI wrapper so MenuBarExtra label can re-render when state flips.
struct MenuBarIconView: View {
    let state: IconState
    var body: some View {
        Image(nsImage: NSImage.claudeApproverMenuIcon(state: state))
    }
}
