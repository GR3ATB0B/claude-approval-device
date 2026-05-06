// MenuBarIcon — Claude-style asterisk/sunburst, drawn programmatically.
//
// 8 rays radiating from center, tapered, filled. Connection state is encoded
// by ray length and weight rather than colour, so the image stays a template
// (auto-tinted by macOS for dark/light menu bars).

import AppKit
import SwiftUI

enum IconState {
    case connected
    case connecting
    case disconnected
    case warning
}

extension NSImage {
    static func claudeApproverMenuIcon(state: IconState) -> NSImage {
        let size = NSSize(width: 18, height: 18)
        let img = NSImage(size: size, flipped: false) { rect in
            let center = NSPoint(x: rect.midX, y: rect.midY)
            let outerRadius: CGFloat = min(rect.width, rect.height) / 2 - 1.0

            // State controls how lively the sunburst looks.
            let (rayCount, lineWidth, innerScale, outerScale): (Int, CGFloat, CGFloat, CGFloat)
            switch state {
            case .connected:    (rayCount, lineWidth, innerScale, outerScale) = (8, 2.0, 0.05, 1.0)
            case .connecting:   (rayCount, lineWidth, innerScale, outerScale) = (8, 1.6, 0.10, 0.85)
            case .disconnected: (rayCount, lineWidth, innerScale, outerScale) = (8, 1.2, 0.30, 0.65)
            case .warning:      (rayCount, lineWidth, innerScale, outerScale) = (4, 2.2, 0.05, 1.0)
            }

            NSColor.black.setStroke()
            NSColor.black.setFill()

            // For .warning, draw a stylized X instead of a sunburst so it reads
            // unambiguously as "something's wrong" at a glance.
            if state == .warning {
                let p = NSBezierPath()
                p.lineWidth = lineWidth
                p.lineCapStyle = .round
                let inset: CGFloat = 4
                p.move(to: NSPoint(x: rect.minX + inset, y: rect.minY + inset))
                p.line(to: NSPoint(x: rect.maxX - inset, y: rect.maxY - inset))
                p.move(to: NSPoint(x: rect.maxX - inset, y: rect.minY + inset))
                p.line(to: NSPoint(x: rect.minX + inset, y: rect.maxY - inset))
                p.stroke()
                return true
            }

            // Sunburst: rayCount rays evenly spaced.
            for i in 0..<rayCount {
                let angle = Double(i) * (2 * .pi / Double(rayCount))
                let cx = cos(angle), sy = sin(angle)
                let inner = NSPoint(
                    x: center.x + cx * outerRadius * innerScale,
                    y: center.y + sy * outerRadius * innerScale
                )
                let outer = NSPoint(
                    x: center.x + cx * outerRadius * outerScale,
                    y: center.y + sy * outerRadius * outerScale
                )
                let ray = NSBezierPath()
                ray.move(to: inner)
                ray.line(to: outer)
                ray.lineWidth = lineWidth
                ray.lineCapStyle = .round
                ray.stroke()
            }

            // Filled center dot for liveness; smaller when disconnected.
            let dotR: CGFloat = (state == .connected) ? 1.6 : 1.0
            let dot = NSBezierPath(ovalIn: NSRect(
                x: center.x - dotR, y: center.y - dotR,
                width: dotR * 2, height: dotR * 2))
            dot.fill()

            return true
        }
        img.isTemplate = true
        return img
    }
}

struct MenuBarIconView: View {
    let state: IconState
    var body: some View {
        Image(nsImage: NSImage.claudeApproverMenuIcon(state: state))
    }
}
