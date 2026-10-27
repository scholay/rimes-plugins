import AppKit

/// Double-track practice surface. The real input text view lays typed text out
/// in fixed-width cells, one row per `columns` characters; above every typed
/// row the matching row of the source text is drawn in the same cells. Both
/// tracks therefore stay column-for-column aligned, and the input method, marked
/// text and candidate window all work on ordinary NSTextView text.
final class TypingTestTrackTextView: TypingTestTextView, NSTextStorageDelegate {
    static let fontSize: CGFloat = 22
    static let cellWidth: CGFloat = 28
    private let trackFont = NSFont.systemFont(ofSize: TypingTestTrackTextView.fontSize)
    private let sourceSlot: CGFloat = 32        // source cell plus its gap, above each typed row
    private let rowGap: CGFloat = 14            // air between one source/typed pair and the next
    private let topPadding: CGFloat = 10
    private let bottomPadding: CGFloat = 24
    private var lineHeight: CGFloat = 28
    private var advanceCache: [String: CGFloat] = [:]
    private(set) var columns = 20

    private(set) var sourceCharacters: [Character] = []
    private var sourceStates: [TypingTestCharacterState] = []
    private var typedCorrectness: [Bool] = []

    /// Draws a box around every character cell in both tracks.
    var showsCells = true { didSet { if oldValue != showsCells { needsDisplay = true } } }

    override init(frame frameRect: NSRect, textContainer container: NSTextContainer?) {
        super.init(frame: frameRect, textContainer: container)
        configureTrack()
    }
    required init?(coder: NSCoder) { nil }

    private func configureTrack() {
        font = trackFont
        lineHeight = ceil(layoutManager?.defaultLineHeight(for: trackFont) ?? 28)
        textContainer?.lineFragmentPadding = 0
        textContainer?.widthTracksTextView = false
        textContainer?.heightTracksTextView = false
        isHorizontallyResizable = false
        isVerticallyResizable = true
        autoresizingMask = [.width]
        textStorage?.delegate = self
        applyContainerGeometry(width: frame.width)
        typingAttributes = trackAttributes()
        setAccessibilityLabel("文章跟打输入区，上行为原文，下行为输入")
        setAccessibilityHelp("每个格子对应原文的一个字。组字中的编码不判错；不支持粘贴或拖入文字。")
    }

    // MARK: Source

    func setSource(_ text: String, states: [TypingTestCharacterState], typedCorrectness: [Bool]) {
        let characters = Array(TypingTestSession.normalized(text))
        let changed = characters != sourceCharacters
        sourceCharacters = characters
        sourceStates = states
        self.typedCorrectness = typedCorrectness
        if changed { updateGeometry() }
        needsDisplay = true
        followCaret()
    }

    // MARK: Geometry

    private var rowPitch: CGFloat { lineHeight + sourceSlot + rowGap }
    private func typedRowTop(_ row: Int) -> CGFloat { topPadding + sourceSlot + CGFloat(row) * rowPitch }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        applyContainerGeometry(width: newSize.width)
    }

    private func applyContainerGeometry(width: CGFloat) {
        guard width > 0 else { return }
        let cell = Self.cellWidth
        let fitted = max(6, Int(floor((width - 24) / cell)))
        let wrapped = fitted != columns
        columns = fitted
        let inner = CGFloat(fitted) * cell
        textContainer?.size = NSSize(width: inner + 0.5, height: .greatestFiniteMagnitude)
        textContainerInset = NSSize(width: max(0, floor((width - inner) / 2)), height: topPadding + sourceSlot)
        if wrapped { updateGeometry() }
    }

    private func updateGeometry() {
        let rows = max(1, Int(ceil(Double(max(sourceCharacters.count, 1)) / Double(columns))))
        minSize = NSSize(width: 0, height: topPadding + sourceSlot + CGFloat(rows) * rowPitch + bottomPadding)
        sizeToFit()
        needsLayout = true
        needsDisplay = true
    }

    // MARK: Cell metrics applied to typed text

    private func trackAttributes() -> [NSAttributedString.Key: Any] {
        let style = NSMutableParagraphStyle()
        style.lineBreakMode = .byCharWrapping
        style.minimumLineHeight = lineHeight
        style.maximumLineHeight = lineHeight
        style.lineSpacing = sourceSlot + rowGap
        return [.font: trackFont, .paragraphStyle: style, .foregroundColor: RimeUI.textPrimary]
    }

    /// Advance of a character as the layout manager will really set it. Measured
    /// between two ideographs, because CJK punctuation is full-width in context
    /// but reports a narrow width when measured alone.
    private func advance(of piece: String) -> CGFloat {
        if let cached = advanceCache[piece] { return cached }
        func width(_ text: String) -> CGFloat { NSAttributedString(string: text, attributes: [.font: trackFont]).size().width }
        let width3 = width("天" + piece + "天"), width2 = width("天天")
        let value = max(1, width3 - width2)
        advanceCache[piece] = value
        return value
    }

    /// Pads every character (typed text and marked text alike) to one cell.
    func textStorage(_ textStorage: NSTextStorage, willProcessEditing editedMask: NSTextStorageEditActions,
                     range editedRange: NSRange, changeInLength delta: Int) {
        guard editedMask.contains(.editedCharacters) || editedMask.contains(.editedAttributes),
              textStorage.length > 0 else { return }
        let whole = NSRange(location: 0, length: textStorage.length)
        let base = trackAttributes()
        textStorage.addAttribute(.paragraphStyle, value: base[.paragraphStyle]!, range: whole)
        guard editedRange.length > 0, NSMaxRange(editedRange) <= textStorage.length else { return }
        textStorage.addAttribute(.font, value: trackFont, range: editedRange)
        (textStorage.string as NSString).enumerateSubstrings(
            in: editedRange, options: [.byComposedCharacterSequences, .substringNotRequired]
        ) { _, range, _, _ in
            let piece = (textStorage.string as NSString).substring(with: range)
            textStorage.addAttribute(.kern, value: max(0, Self.cellWidth - self.advance(of: piece)), range: range)
        }
    }

    // MARK: Drawing

    override func drawBackground(in rect: NSRect) {
        super.drawBackground(in: rect)
        drawTracks(in: rect)
    }

    private func drawTracks(in rect: NSRect) {
        guard !sourceCharacters.isEmpty || !string.isEmpty else { return }
        let cell = Self.cellWidth
        let left = textContainerInset.width
        let typedCount = (string as NSString).length == 0 ? 0 : TypingTestSession.normalized(string).count
        let total = max(sourceCharacters.count, typedCount)
        let rows = Int(ceil(Double(total) / Double(columns)))
        let first = max(0, Int(floor((rect.minY - typedRowTop(0)) / rowPitch)) - 1)
        guard first < rows else { return }
        let last = min(rows - 1, Int(ceil((rect.maxY - typedRowTop(0)) / rowPitch)) + 1)
        guard first <= last else { return }
        let cellBorder = RimeUI.border.withAlphaComponent(0.9)
        let accent = RimeUI.accentGreen
        let danger = RimeUI.dangerFillColor
        for row in first...last {
            let typedTop = typedRowTop(row)
            let sourceTop = typedTop - sourceSlot
            for column in 0..<columns {
                let index = row * columns + column
                if index >= total { break }
                let x = left + CGFloat(column) * cell
                let sourceRect = NSRect(x: x + 1, y: sourceTop + 1, width: cell - 2, height: sourceSlot - 3)
                let typedRect = NSRect(x: x + 1, y: typedTop + 2, width: cell - 2, height: lineHeight - 1)
                let state: TypingTestCharacterState = index < sourceStates.count ? sourceStates[index] : .pending
                let typedWrong = index < typedCorrectness.count && !typedCorrectness[index]
                if index < sourceCharacters.count {
                    switch state {
                    case .correct: accent.withAlphaComponent(0.12).setFill()
                    case .incorrect, .omitted: danger.withAlphaComponent(0.16).setFill()
                    case .pending: NSColor.clear.setFill()
                    }
                    NSBezierPath(roundedRect: sourceRect, xRadius: 5, yRadius: 5).fill()
                }
                if typedWrong {
                    danger.withAlphaComponent(0.16).setFill()
                    NSBezierPath(roundedRect: typedRect, xRadius: 5, yRadius: 5).fill()
                }
                if showsCells {
                    cellBorder.setStroke()
                    for box in [sourceRect, typedRect] where index < sourceCharacters.count || box == typedRect {
                        let path = NSBezierPath(roundedRect: box, xRadius: 5, yRadius: 5)
                        path.lineWidth = 1
                        path.stroke()
                    }
                }
                if index == typedCount, index < sourceCharacters.count {
                    accent.setStroke()
                    let path = NSBezierPath(roundedRect: typedRect.insetBy(dx: 0.5, dy: 0.5), xRadius: 5, yRadius: 5)
                    path.lineWidth = 1.5
                    path.stroke()
                }
                if index < sourceCharacters.count {
                    let color: NSColor
                    switch state {
                    case .correct: color = RimeUI.accentTextColor
                    case .incorrect, .omitted: color = RimeUI.dangerTextColor
                    case .pending: color = RimeUI.textMuted
                    }
                    let glyph = NSAttributedString(string: String(sourceCharacters[index]),
                                                   attributes: [.font: trackFont, .foregroundColor: color])
                    glyph.draw(at: NSPoint(x: x + 1, y: sourceTop + (sourceSlot - lineHeight) / 2 - 1))
                }
            }
        }
    }

    // MARK: Behaviour

    /// A typed line break would desynchronise the rows from the source.
    override func insertNewline(_ sender: Any?) { NSSound.beep() }
    override func insertLineBreak(_ sender: Any?) { NSSound.beep() }

    override func didChangeText() {
        super.didChangeText()
        needsDisplay = true
        followCaret()
    }

    /// Keeps the row being typed (and the source row above it) in view.
    private func followCaret() {
        let typed = TypingTestSession.normalized(string).count
        let row = min(max(0, typed / columns), max(0, Int(ceil(Double(max(sourceCharacters.count, 1)) / Double(columns))) - 1))
        let top = typedRowTop(row) - sourceSlot
        scrollToVisible(NSRect(x: 0, y: top - 8, width: 1, height: rowPitch + 16))
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        textColor = RimeUI.textPrimary
        needsDisplay = true
    }
}
