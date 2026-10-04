import AppKit

/// The Morse input bar, drawn in the Buffer's upper rail.
///
///     [ A ] [ ·− ]  ────▮▮──▮▮▮▮▮▮──|────────────────  ●
///     map   combine  sweep line (left → right, wraps)   key
///
/// The mapping slot shows the letter the pending symbols form now; the combine
/// slot shows those symbols. The sweep line advances uniformly from left to
/// right and draws each press where it happened; the light shows the key.
final class MorseInputBarView: NSView {
    static let slotHeight: CGFloat = 18
    static let mappingSlotWidth: CGFloat = 24
    static let combineSlotMinimumWidth: CGFloat = 64

    private let workspace: MorseWorkspace
    private var snapshot = MorseTapeSnapshot()
    private var frameTimer: Timer?
    private var observer: NSObjectProtocol?

    init(workspace: MorseWorkspace = .shared) {
        self.workspace = workspace
        super.init(frame: NSRect(x: 0, y: 0, width: 400, height: BufferInlineMetrics.itemHeight))
        translatesAutoresizingMaskIntoConstraints = false
        setAccessibilityElement(true)
        setAccessibilityRole(.staticText)
        observer = NotificationCenter.default.addObserver(
            forName: .morseTapeDidChange, object: workspace, queue: .main
        ) { [weak self] _ in self?.refresh() }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }

    deinit {
        frameTimer?.invalidate()
        observer.map(NotificationCenter.default.removeObserver)
    }

    override var isFlipped: Bool { true }
    override var intrinsicContentSize: NSSize {
        NSSize(width: NSView.noIntrinsicMetric, height: BufferInlineMetrics.itemHeight)
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        refresh()
    }

    /// Reads the workspace and keeps a frame timer only while the line moves.
    func refresh() {
        snapshot = workspace.tapeSnapshot()
        updateAccessibility()
        needsDisplay = true
        let animating = window != nil && workspace.tapeIsAnimating
        if animating, frameTimer == nil {
            let timer = Timer(timeInterval: 1.0 / 30, repeats: true) { [weak self] _ in
                guard let self else { return }
                self.snapshot = self.workspace.tapeSnapshot()
                self.needsDisplay = true
                if !self.workspace.tapeIsAnimating { self.stopFrames() }
            }
            frameTimer = timer
            RunLoop.main.add(timer, forMode: .common)
        } else if !animating {
            stopFrames()
        }
    }

    /// Smoke and previews: draw a fixed snapshot.
    func render(_ snapshot: MorseTapeSnapshot) {
        stopFrames()
        self.snapshot = snapshot
        updateAccessibility()
        needsDisplay = true
    }

    private func stopFrames() {
        frameTimer?.invalidate()
        frameTimer = nil
    }

    // MARK: Drawing

    private struct Layout {
        let mapping: NSRect
        let combine: NSRect
        let tape: NSRect
        let light: NSRect
    }

    private func layoutRects() -> Layout {
        let y = (bounds.height - Self.slotHeight) / 2
        let mapping = NSRect(x: 0, y: y, width: Self.mappingSlotWidth, height: Self.slotHeight)
        let glyphWidth = snapshot.pending.reduce(CGFloat(14)) { $0 + ($1 == .dot ? 9 : 17) }
        let combine = NSRect(x: mapping.maxX + 5, y: y,
                             width: max(Self.combineSlotMinimumWidth, glyphWidth),
                             height: Self.slotHeight)
        let light = NSRect(x: bounds.maxX - 12, y: bounds.midY - 4, width: 8, height: 8)
        let tape = NSRect(x: combine.maxX + 10, y: y,
                          width: max(0, light.minX - 8 - (combine.maxX + 10)),
                          height: Self.slotHeight)
        return Layout(mapping: mapping, combine: combine, tape: tape, light: light)
    }

    override func draw(_ dirtyRect: NSRect) {
        let layout = layoutRects()
        drawMappingSlot(layout.mapping)
        drawCombineSlot(layout.combine)
        drawTape(layout.tape)
        drawLight(layout.light)
    }

    private func slotPath(_ rect: NSRect) -> NSBezierPath {
        let path = NSBezierPath(roundedRect: rect, xRadius: 4, yRadius: 4)
        RimeUI.surface2.setFill()
        path.fill()
        RimeUI.border.setStroke()
        path.lineWidth = 1 / max(window?.backingScaleFactor ?? 2, 1)
        path.stroke()
        return path
    }

    private func drawMappingSlot(_ rect: NSRect) {
        _ = slotPath(rect)
        let text: String
        let color: NSColor
        if snapshot.lastRejected, snapshot.pending.isEmpty {
            text = "✕"; color = RimeUI.brandRed
        } else if let candidate = snapshot.candidate {
            text = String(candidate); color = RimeUI.textPrimary
        } else if snapshot.pendingIsDeadEnd {
            text = "?"; color = RimeUI.brandRed
        } else {
            text = "·"; color = RimeUI.textMuted
        }
        drawCentered(text, in: rect, font: .monospacedSystemFont(ofSize: 12, weight: .bold), color: color)
    }

    private func drawCombineSlot(_ rect: NSRect) {
        _ = slotPath(rect)
        guard !snapshot.pending.isEmpty else {
            drawCentered("待组合", in: rect, font: .systemFont(ofSize: 10), color: RimeUI.textMuted)
            return
        }
        // Shapes, not glyphs: a text "·" is too faint to read at this size.
        let dotSize: CGFloat = 5
        let dashWidth: CGFloat = 13
        let gap: CGFloat = 4
        let total = snapshot.pending.reduce(CGFloat(0)) { $0 + ($1 == .dot ? dotSize : dashWidth) }
            + gap * CGFloat(snapshot.pending.count - 1)
        var x = rect.midX - total / 2
        (snapshot.pendingIsDeadEnd ? RimeUI.brandRed : RimeUI.textPrimary).setFill()
        for symbol in snapshot.pending {
            if symbol == .dot {
                NSBezierPath(ovalIn: NSRect(x: x, y: rect.midY - dotSize / 2,
                                            width: dotSize, height: dotSize)).fill()
                x += dotSize + gap
            } else {
                NSBezierPath(roundedRect: NSRect(x: x, y: rect.midY - 2, width: dashWidth, height: 4),
                             xRadius: 2, yRadius: 2).fill()
                x += dashWidth + gap
            }
        }
    }

    private func drawTape(_ rect: NSRect) {
        guard rect.width > 20 else { return }
        let period = MorseWorkspace.sweepPeriod
        let now = snapshot.now
        func x(_ time: TimeInterval) -> CGFloat {
            let phase = time.truncatingRemainder(dividingBy: period) / period
            return rect.minX + CGFloat(phase) * rect.width
        }
        let baseline = rect.midY
        let cursor = x(now)
        // The gap just ahead of the cursor is where the oldest marks are wiped.
        let clearance = rect.width * 0.04

        let line = NSBezierPath()
        line.move(to: NSPoint(x: rect.minX, y: baseline))
        line.line(to: NSPoint(x: rect.maxX, y: baseline))
        line.lineWidth = 1
        RimeUI.borderStrong.setStroke()
        line.stroke()

        let visibleSince = now - period * (1 - Double(clearance / rect.width))
        for mark in snapshot.marks {
            let end = mark.end ?? now
            guard end > visibleSince else { continue }
            let start = max(mark.start, visibleSince)
            let color: NSColor
            switch mark.symbol {
            case .dash: color = RimeUI.accentSecondary
            case .dot: color = RimeUI.accentBlue
            case nil: color = snapshot.pressIsDash ? RimeUI.accentSecondary : RimeUI.accentBlue
            }
            color.setFill()
            for segment in segments(from: start, to: end, period: period) {
                let left = x(segment.0)
                let right = max(left + 2, xEnd(segment.1, rect: rect, period: period))
                let bar = NSRect(x: left, y: baseline - 3, width: max(2, right - left), height: 6)
                NSBezierPath(roundedRect: bar, xRadius: 2, yRadius: 2).fill()
            }
        }
        RimeUI.textMuted.setFill()
        for tick in snapshot.letterTicks where tick > visibleSince {
            NSBezierPath(rect: NSRect(x: x(tick) - 0.5, y: rect.minY + 1, width: 1, height: rect.height - 2)).fill()
        }
        // The sweep cursor.
        (snapshot.isPressed ? RimeUI.accentBlue : RimeUI.textSecondary).setFill()
        NSBezierPath(rect: NSRect(x: cursor - 0.75, y: rect.minY, width: 1.5, height: rect.height)).fill()
    }

    /// The x of an interval's end; an end exactly on a wrap boundary draws to
    /// the right edge rather than the left.
    private func xEnd(_ time: TimeInterval, rect: NSRect, period: TimeInterval) -> CGFloat {
        let phase = time.truncatingRemainder(dividingBy: period) / period
        return phase == 0 ? rect.maxX : rect.minX + CGFloat(phase) * rect.width
    }

    /// Splits an interval where the sweep wraps back to the left edge.
    private func segments(from start: TimeInterval, to end: TimeInterval,
                          period: TimeInterval) -> [(TimeInterval, TimeInterval)] {
        var result: [(TimeInterval, TimeInterval)] = []
        var cursor = start
        while cursor < end {
            let wrap = (floor(cursor / period) + 1) * period
            let segmentEnd = min(end, wrap)
            result.append((cursor, segmentEnd))
            cursor = segmentEnd
        }
        return result
    }

    private func drawLight(_ rect: NSRect) {
        let path = NSBezierPath(ovalIn: rect)
        if snapshot.isPressed {
            (snapshot.pressIsDash ? RimeUI.accentSecondary : RimeUI.accentBlue).setFill()
            path.fill()
        } else {
            RimeUI.borderStrong.setStroke()
            path.lineWidth = 1
            path.stroke()
        }
    }

    private func drawCentered(_ text: String, in rect: NSRect, font: NSFont, color: NSColor) {
        let string = NSAttributedString(string: text, attributes: [.font: font, .foregroundColor: color])
        let size = string.size()
        string.draw(at: NSPoint(x: rect.midX - size.width / 2, y: rect.midY - size.height / 2))
    }

    private func updateAccessibility() {
        let pending = snapshot.pending.isEmpty
            ? "无待组合电码"
            : "待组合 " + snapshot.pending.map { $0 == .dot ? "点" : "划" }.joined(separator: " ")
        let mapping = snapshot.candidate.map { "，对应 \($0)" } ?? ""
        setAccessibilityLabel("摩斯电码输入")
        setAccessibilityValue(pending + mapping + (snapshot.isPressed ? "，按键按下" : ""))
    }

    // MARK: Smoke

    var renderedMappingTextForSmoke: String {
        if snapshot.lastRejected, snapshot.pending.isEmpty { return "✕" }
        if let candidate = snapshot.candidate { return String(candidate) }
        return snapshot.pendingIsDeadEnd ? "?" : "·"
    }
    var isAnimatingForSmoke: Bool { frameTimer != nil }
}
