import AppKit

// Morse Code Buffer plug-in. Space is the key: a short press is a dot, a long
// press a dash. A pause commits the letter, a longer pause ends the word, and
// the decoded English text becomes ordinary deliverable Buffer blocks.

enum MorseSymbol: Equatable {
    case dot
    case dash

    var glyph: String { self == .dot ? "·" : "−" }
}

/// International Morse Code (ITU-R M.1677-1): letters, digits and the common
/// punctuation. English only.
enum MorseAlphabet {
    static let table: [String: Character] = [
        ".-": "A", "-...": "B", "-.-.": "C", "-..": "D", ".": "E",
        "..-.": "F", "--.": "G", "....": "H", "..": "I", ".---": "J",
        "-.-": "K", ".-..": "L", "--": "M", "-.": "N", "---": "O",
        ".--.": "P", "--.-": "Q", ".-.": "R", "...": "S", "-": "T",
        "..-": "U", "...-": "V", ".--": "W", "-..-": "X", "-.--": "Y",
        "--..": "Z",
        "-----": "0", ".----": "1", "..---": "2", "...--": "3", "....-": "4",
        ".....": "5", "-....": "6", "--...": "7", "---..": "8", "----.": "9",
        ".-.-.-": ".", "--..--": ",", "..--..": "?", ".----.": "'",
        "-.-.--": "!", "-..-.": "/", "-.--.": "(", "-.--.-": ")",
        ".-...": "&", "---...": ":", "-.-.-.": ";", "-...-": "=",
        ".-.-.": "+", "-....-": "-", "..--.-": "_", ".-..-.": "\"",
        "...-..-": "$", ".--.-.": "@",
    ]

    static func key(_ symbols: [MorseSymbol]) -> String {
        String(symbols.map { $0 == .dot ? "." : "-" })
    }

    static func character(for symbols: [MorseSymbol]) -> Character? {
        table[key(symbols)]
    }

    /// Eight or more dots is the operator's error sign: erase one character.
    static func isErrorSign(_ symbols: [MorseSymbol]) -> Bool {
        symbols.count >= 8 && symbols.allSatisfy { $0 == .dot }
    }

    /// Whether more symbols could still form a character.
    static func hasContinuation(_ symbols: [MorseSymbol]) -> Bool {
        let prefix = key(symbols)
        return table.keys.contains { $0.count > prefix.count && $0.hasPrefix(prefix) }
    }
}

/// How long a press must be to count as a dash, and how long a pause ends a
/// letter or a word. Space-bar keying is slower than a straight key.
struct MorseTiming: Equatable {
    let identifier: String
    let title: String
    let dashThreshold: TimeInterval
    let letterGap: TimeInterval
    let wordGap: TimeInterval

    static let slow = MorseTiming(identifier: "slow", title: "慢速",
                                  dashThreshold: 0.28, letterGap: 0.75, wordGap: 1.9)
    static let standard = MorseTiming(identifier: "standard", title: "标准",
                                      dashThreshold: 0.20, letterGap: 0.48, wordGap: 1.25)
    static let fast = MorseTiming(identifier: "fast", title: "快速",
                                  dashThreshold: 0.15, letterGap: 0.32, wordGap: 0.85)
    static let all = [slow, standard, fast]
}

/// Pure keying state. Times are seconds on one monotonic clock (NSEvent
/// timestamps), so a smoke can drive it without waiting.
struct MorseKeyer {
    enum Event: Equatable {
        /// A pause ended a letter. `nil` means the symbols form no character.
        case letter(Character?, [MorseSymbol])
        case erase
        case wordBreak
    }

    var timing: MorseTiming
    private(set) var pending: [MorseSymbol] = []
    private(set) var pressStartedAt: TimeInterval?
    private(set) var lastReleaseAt: TimeInterval?
    /// True between a committed letter and the word gap.
    private(set) var wordOpen = false

    init(timing: MorseTiming) { self.timing = timing }

    var isPressed: Bool { pressStartedAt != nil }

    /// Returns false for a press that is already held (key repeat).
    mutating func press(at time: TimeInterval) -> Bool {
        guard pressStartedAt == nil else { return false }
        pressStartedAt = time
        return true
    }

    mutating func release(at time: TimeInterval) -> MorseSymbol? {
        guard let start = pressStartedAt else { return nil }
        pressStartedAt = nil
        lastReleaseAt = time
        let symbol: MorseSymbol = time - start >= timing.dashThreshold ? .dash : .dot
        pending.append(symbol)
        return symbol
    }

    /// Letter and word boundaries that the silence up to `time` has reached.
    mutating func advance(to time: TimeInterval) -> [Event] {
        guard pressStartedAt == nil, let lastReleaseAt else { return [] }
        var events: [Event] = []
        let silence = time - lastReleaseAt
        if !pending.isEmpty, silence >= timing.letterGap {
            events.append(commitLetter())
        }
        if wordOpen, pending.isEmpty, silence >= timing.wordGap {
            wordOpen = false
            events.append(.wordBreak)
        }
        return events
    }

    /// Commits whatever is pending now, as for Return.
    mutating func flush() -> [Event] {
        pressStartedAt = nil
        var events: [Event] = []
        if !pending.isEmpty { events.append(commitLetter()) }
        if wordOpen {
            wordOpen = false
            events.append(.wordBreak)
        }
        return events
    }

    /// Delete: drops the last pending symbol; false when nothing is pending.
    mutating func dropLastSymbol() -> Bool {
        guard !pending.isEmpty else { return false }
        pending.removeLast()
        return true
    }

    mutating func reset() {
        pending = []
        pressStartedAt = nil
        lastReleaseAt = nil
        wordOpen = false
    }

    /// When the next boundary is due, so a single timer can wait for it.
    var nextDeadline: TimeInterval? {
        guard pressStartedAt == nil, let lastReleaseAt else { return nil }
        if !pending.isEmpty { return lastReleaseAt + timing.letterGap }
        if wordOpen { return lastReleaseAt + timing.wordGap }
        return nil
    }

    private mutating func commitLetter() -> Event {
        let symbols = pending
        pending = []
        if MorseAlphabet.isErrorSign(symbols) { return .erase }
        let character = MorseAlphabet.character(for: symbols)
        if character != nil { wordOpen = true }
        return .letter(character, symbols)
    }
}

/// What the input bar draws: the sweep line's marks and the two slots.
struct MorseTapeSnapshot: Equatable {
    struct Mark: Equatable {
        let start: TimeInterval
        let end: TimeInterval?
        let symbol: MorseSymbol?
    }

    var now: TimeInterval = 0
    var marks: [Mark] = []
    /// Times at which a letter was committed, drawn as small ticks.
    var letterTicks: [TimeInterval] = []
    var pending: [MorseSymbol] = []
    /// The character the pending symbols form right now.
    var candidate: Character?
    var pendingIsDeadEnd = false
    var isPressed = false
    var pressIsDash = false
    var lastRejected = false
}
