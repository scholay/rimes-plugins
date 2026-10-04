import AppKit

extension Notification.Name {
    /// High-frequency: a press, release or letter changed the input bar. The
    /// bar redraws itself; the rest of the workbench waits for the ordinary
    /// derived-workspace change.
    static let morseTapeDidChange = Notification.Name("RimeBuffer.Morse.tapeDidChange")
}

/// The Morse workspace: Space presses in, decoded English words out. Each
/// finished word is a Buffer block the user sends like any other; the word
/// still being keyed stays in the rail until a pause or Return closes it.
final class MorseWorkspace: DerivedBufferWorkspace, DerivedOptionPickerControls {
    static let shared = MorseWorkspace()
    static let pluginKey = PluginKey(domain: .builtIn, rawID: BuiltInPluginID.morse)
    static let speedKey = "\(RimesIdentity.preferenceKeyPrefix)Morse.speed"
    /// Seconds of history the sweep line shows before it wraps.
    static let sweepPeriod: TimeInterval = 4

    private struct Word {
        let id: UUID
        var text: String
        var closed: Bool
        let leadingSpace: Bool

        var blockText: String { (leadingSpace ? " " : "") + text }
    }

    let workspacePluginKey = MorseWorkspace.pluginKey
    let workbenchDisplayName = "摩斯电码"

    private let defaults: UserDefaults
    private let notificationCenter: NotificationCenter
    private let sound: MorseSoundOutput
    private let clock: () -> TimeInterval
    private let selectionPredicate: () -> Bool
    private var keyer: MorseKeyer
    private var words: [Word] = []
    private var sessionHasWords = false
    private var marks: [MorseTapeSnapshot.Mark] = []
    private var letterTicks: [TimeInterval] = []
    private var lastRejectedAt: TimeInterval?
    private var boundaryTimer: Timer?
    private var observers: [NSObjectProtocol] = []
    private var started = false
    private var protectedSession = false
    /// Printable keys swallowed on key-down, so their key-ups are too.
    private var ownedKeys = Set<UInt16>()
    private(set) var generation: UInt64 = 0

    init(defaults: UserDefaults = .standard,
         notificationCenter: NotificationCenter = .default,
         sound: MorseSoundOutput = MorseSidetone.shared,
         clock: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime },
         isSelected: @escaping () -> Bool = {
            BufferPluginSelectionStore.shared.isSelected(MorseWorkspace.pluginKey)
         }) {
        self.defaults = defaults
        self.notificationCenter = notificationCenter
        self.sound = sound
        self.clock = clock
        selectionPredicate = isSelected
        let stored = defaults.string(forKey: Self.speedKey)
        keyer = MorseKeyer(timing: MorseTiming.all.first { $0.identifier == stored } ?? .standard)
    }

    var isSelected: Bool { selectionPredicate() }
    var isActive: Bool { started && isSelected && !protectedSession }
    var timing: MorseTiming { keyer.timing }

    // MARK: Lifecycle

    func start() {
        guard !started else { return }
        started = true
        observers.append(notificationCenter.addObserver(
            forName: .activeBufferPluginDidChange, object: nil, queue: .main
        ) { [weak self] _ in self?.selectionDidChange() })
        selectionDidChange()
    }

    func stop() {
        guard started else { return }
        started = false
        observers.forEach(notificationCenter.removeObserver)
        observers.removeAll()
        reset()
        sound.shutdown()
    }

    func setProtected(_ protected: Bool) {
        guard protectedSession != protected else { return }
        protectedSession = protected
        if protected {
            // Protection hides the text; nothing keyed under it survives.
            reset()
            sound.shutdown()
        }
        notifyChange()
    }

    /// Closing Buffer keeps finished words, like any Buffer text, but never
    /// leaves a tone sounding or a press half-counted.
    func workbenchWillPause() {
        releaseHeldKey()
        notifyTape()
    }

    @discardableResult
    func requestRefresh() -> Bool {
        reset()
        return true
    }

    private func selectionDidChange() {
        if !isSelected {
            releaseHeldKey()
            sound.shutdown()
        }
        notifyChange()
    }

    // MARK: Keys

    /// Called by the input controller for every key event while Morse is the
    /// selected plug-in and Buffer owns the field. Returns true to consume it.
    func handleKey(code: UInt16, isDown: Bool, isRepeat: Bool,
                   modifiers: NSEvent.ModifierFlags, characters: String?,
                   timestamp: TimeInterval) -> Bool {
        dispatchPrecondition(condition: .onQueue(.main))
        guard isActive else { return false }
        let shortcut = !modifiers.intersection([.command, .control, .option]).isEmpty
        if !isDown, ownedKeys.remove(code) != nil {
            if code == Self.spaceKeyCode { release(at: timestamp) }
            return true
        }
        guard isDown, !shortcut else { return false }
        switch code {
        case Self.spaceKeyCode:
            ownedKeys.insert(code)
            if !isRepeat { press(at: timestamp) }
            return true
        case Self.deleteKeyCode:
            return eraseOne()
        default:
            // Letters typed while keying Morse would land in the hidden source
            // rail; swallow them. Return, Escape, Tab and arrows stay Buffer's.
            guard let characters, let scalar = characters.unicodeScalars.first,
                  !CharacterSet.controlCharacters.contains(scalar),
                  scalar.value < 0xF700 else { return false }
            ownedKeys.insert(code)
            return true
        }
    }

    /// A release of a key Morse took on key-down is always Morse's, even if
    /// focus has moved since: otherwise the tone could keep sounding.
    func consumeOwnedKeyUp(code: UInt16, timestamp: TimeInterval) -> Bool {
        dispatchPrecondition(condition: .onQueue(.main))
        guard ownedKeys.remove(code) != nil else { return false }
        if code == Self.spaceKeyCode { release(at: timestamp) }
        return true
    }

    static let spaceKeyCode: UInt16 = 49
    static let deleteKeyCode: UInt16 = 51

    func press(at time: TimeInterval) {
        guard isActive, keyer.press(at: time) else { return }
        boundaryTimer?.invalidate()
        marks.append(.init(start: time, end: nil, symbol: nil))
        trimHistory(now: time)
        sound.keyDown()
        notifyTape()
    }

    func release(at time: TimeInterval) {
        guard let symbol = keyer.release(at: time) else { return }
        sound.keyUp()
        if let last = marks.indices.last, marks[last].end == nil {
            marks[last] = .init(start: marks[last].start, end: time, symbol: symbol)
        }
        scheduleBoundary()
        notifyTape()
    }

    /// Applies every boundary the silence up to `time` has reached. The timer
    /// calls this; a smoke calls it directly with a synthetic time.
    func advance(to time: TimeInterval) {
        apply(keyer.advance(to: time), at: time)
        scheduleBoundary()
    }

    /// Delete: the last pending symbol first, then the last decoded character.
    @discardableResult
    func eraseOne() -> Bool {
        guard isActive else { return false }
        if keyer.dropLastSymbol() {
            if keyer.pending.isEmpty { boundaryTimer?.invalidate() }
            notifyTape()
            return true
        }
        guard eraseLastCharacter() else { return false }
        notifyChange()
        return true
    }

    private func releaseHeldKey() {
        if keyer.isPressed { release(at: clock()) }
        ownedKeys.removeAll()
        sound.keyUp()
    }

    private func apply(_ events: [MorseKeyer.Event], at time: TimeInterval) {
        guard !events.isEmpty else { return }
        var changed = false
        for event in events {
            switch event {
            case let .letter(character?, _):
                append(character)
                letterTicks.append(time)
                sound.letterBlip()
                changed = true
            case .letter(nil, _):
                lastRejectedAt = time
                sound.rejectBlip()
            case .erase:
                _ = eraseLastCharacter()
                sound.rejectBlip()
                changed = true
            case .wordBreak:
                if let last = words.indices.last, !words[last].closed {
                    words[last].closed = true
                    changed = true
                }
            }
        }
        trimHistory(now: time)
        notifyTape()
        if changed { notifyChange() }
    }

    private func append(_ character: Character) {
        if let last = words.indices.last, !words[last].closed {
            words[last].text.append(character)
        } else {
            words.append(Word(id: UUID(), text: String(character), closed: false,
                              leadingSpace: sessionHasWords))
            sessionHasWords = true
        }
    }

    private func eraseLastCharacter() -> Bool {
        guard let last = words.indices.last else { return false }
        words[last].text.removeLast()
        words[last].closed = false
        if words[last].text.isEmpty { words.removeLast() }
        generation &+= 1
        return true
    }

    private func scheduleBoundary() {
        boundaryTimer?.invalidate()
        boundaryTimer = nil
        guard let deadline = keyer.nextDeadline else { return }
        let delay = max(0.005, deadline - clock())
        let timer = Timer(timeInterval: delay, repeats: false) { [weak self] _ in
            guard let self else { return }
            self.advance(to: max(self.clock(), deadline))
        }
        boundaryTimer = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    private func trimHistory(now: TimeInterval) {
        let horizon = now - Self.sweepPeriod
        marks.removeAll { ($0.end ?? now) < horizon }
        letterTicks.removeAll { $0 < horizon }
    }

    private func reset() {
        boundaryTimer?.invalidate()
        boundaryTimer = nil
        keyer.reset()
        words = []
        sessionHasWords = false
        marks = []
        letterTicks = []
        lastRejectedAt = nil
        ownedKeys.removeAll()
        sound.keyUp()
        generation &+= 1
        notifyTape()
        notifyChange()
    }

    // MARK: Presentation

    func tapeSnapshot(now: TimeInterval? = nil) -> MorseTapeSnapshot {
        let now = now ?? clock()
        let pending = keyer.pending
        var snapshot = MorseTapeSnapshot()
        snapshot.now = now
        snapshot.marks = marks.filter { ($0.end ?? now) >= now - Self.sweepPeriod }
        snapshot.letterTicks = letterTicks.filter { $0 >= now - Self.sweepPeriod }
        snapshot.pending = pending
        snapshot.candidate = pending.isEmpty ? nil : MorseAlphabet.character(for: pending)
        snapshot.pendingIsDeadEnd = !pending.isEmpty && snapshot.candidate == nil
            && !MorseAlphabet.hasContinuation(pending) && !MorseAlphabet.isErrorSign(pending)
        snapshot.isPressed = keyer.isPressed
        if let start = keyer.pressStartedAt {
            snapshot.pressIsDash = now - start >= keyer.timing.dashThreshold
        }
        snapshot.lastRejected = lastRejectedAt.map { now - $0 < 1.2 } ?? false
        return snapshot
    }

    /// The bar animates only while something on it is still moving.
    var tapeIsAnimating: Bool {
        guard isActive else { return false }
        if keyer.isPressed { return true }
        let now = clock()
        return marks.contains { ($0.end ?? now) > now - Self.sweepPeriod }
            || (lastRejectedAt.map { now - $0 < 1.2 } ?? false)
    }

    var outputText: String { words.map(\.blockText).joined() }

    var statusText: String {
        if protectedSession { return "受保护状态下暂停" }
        if keyer.isPressed { return "按住中" }
        if !keyer.pending.isEmpty { return "停顿即成字母" }
        if words.contains(where: { !$0.closed }) { return "停顿更久即结束单词" }
        return "短按空格为点，长按为划"
    }

    var railSnapshot: TranslationRailSnapshot {
        let blocks = protectedSession ? [] : words.map {
            TranslationOutputBlock(id: $0.id, text: $0.blockText, deliveryReady: $0.closed)
        }
        return TranslationRailSnapshot(
            sourceText: "",
            sourceRailPinned: true,
            outputBlocks: blocks,
            phase: blocks.isEmpty ? .idle : .ready,
            sourceRole: "码",
            targetRole: "文",
            sourceEmptyText: "",
            targetEmptyText: "短按空格为点，长按为划；停顿成字母",
            waitingText: "等待电码",
            processingText: "正在解码",
            updatingText: "正在解码"
        )
    }

    // MARK: Speed picker

    var optionPickerOptions: [DerivedOptionPickerOption] {
        MorseTiming.all.map { DerivedOptionPickerOption(identifier: $0.identifier, title: $0.title) }
    }

    var selectedOptionPickerID: String { keyer.timing.identifier }

    var optionPickerToolTip: String {
        let timing = keyer.timing
        return "速度：长于 \(Int(timing.dashThreshold * 1000)) ms 为划，"
            + "停顿 \(Int(timing.letterGap * 1000)) ms 成字母，"
            + "\(Int(timing.wordGap * 1000)) ms 结束单词"
    }

    @discardableResult
    func setOptionPickerSelection(_ identifier: String) -> Bool {
        guard let timing = MorseTiming.all.first(where: { $0.identifier == identifier }) else {
            return false
        }
        keyer.timing = timing
        defaults.set(identifier, forKey: Self.speedKey)
        scheduleBoundary()
        notifyChange()
        return true
    }

    private func notifyChange() {
        notificationCenter.post(name: .derivedBufferWorkspaceDidChange, object: self)
    }

    private func notifyTape() {
        notificationCenter.post(name: .morseTapeDidChange, object: self)
    }

    // MARK: BufferDeliveryContentSource

    var deliveryWorkspaceID: String { "morse" }
    var deliveryGeneration: UInt64 { generation }
    var supportsIncrementalDelivery: Bool { true }
    var hasIncompleteDeliveryBlocks: Bool {
        isActive && (keyer.isPressed || !keyer.pending.isEmpty || words.contains { !$0.closed })
    }

    var deliveryPendingBlocks: [BufferModel.Block] {
        guard isActive else { return [] }
        return words.prefix { $0.closed }.map {
            BufferModel.Block(id: $0.id, text: $0.blockText,
                              origin: .processor(id: "morse", allowsRemoteMirror: true))
        }
    }

    /// Return sends what has been keyed so far: the pending letter and the
    /// open word are committed first.
    @discardableResult
    func prepareForDelivery() -> Bool {
        guard isActive else { return false }
        if !keyer.isPressed {
            boundaryTimer?.invalidate()
            apply(keyer.flush(), at: clock())
        }
        return !deliveryPendingBlocks.isEmpty
    }

    func deliveryBlock(id: UUID, generation: UInt64) -> BufferModel.Block? {
        guard generation == self.generation else { return nil }
        return deliveryPendingBlocks.first { $0.id == id }
    }

    func consumeDelivered(blockIDs: [UUID], generation: UInt64) {
        _ = consumeDeliveredAndReportTerminalDrain(blockIDs: blockIDs, generation: generation)
    }

    func consumeDeliveredAndReportTerminalDrain(
        blockIDs: [UUID], generation: UInt64
    ) -> BufferDeliveryTerminalSourceReceipt? {
        guard generation == self.generation, !blockIDs.isEmpty else { return nil }
        let ids = Set(blockIDs)
        let before = words.count
        words.removeAll { $0.closed && ids.contains($0.id) }
        guard words.count != before else { return nil }
        self.generation &+= 1
        notifyChange()
        let terminal = words.isEmpty && keyer.pending.isEmpty && !keyer.isPressed
        guard terminal else { return nil }
        return BufferDeliveryTerminalSourceReceipt(
            workspaceID: deliveryWorkspaceID,
            generation: generation,
            generationAfterConsumption: self.generation,
            consumedBlockIDs: ids
        )
    }

    func markDeliveryBlockStale(id: UUID, generation: UInt64) -> Bool { false }
}

/// The sound the workspace plays; a smoke substitutes a recorder.
protocol MorseSoundOutput: AnyObject {
    func keyDown()
    func keyUp()
    func letterBlip()
    func rejectBlip()
    func shutdown()
}

extension MorseSidetone: MorseSoundOutput {}

final class MorseInternalPlugin: InternalPlugin {
    private static let catalog = PresetBufferPluginCatalog.entry(id: BuiltInPluginID.morse)!
    let descriptor = PluginDescriptor(
        key: MorseWorkspace.pluginKey, wireID: nil, name: catalog.nameZH,
        symbolName: "dot.radiowaves.left.and.right", version: catalog.version,
        summary: catalog.summaryZH, source: .builtIn,
        capabilities: [.bufferAction], settings: nil, canUninstall: true
    )

    func start() { MorseWorkspace.shared.start() }
    func stop() { MorseWorkspace.shared.stop() }
    func makeSettingsViewController(subpageID: String) -> NSViewController? { nil }
}
