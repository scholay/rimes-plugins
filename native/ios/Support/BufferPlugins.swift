import Foundation
import AVFoundation
import NaturalLanguage
import Translation
import RimesCore

@MainActor final class AppleTranslationPlugin: BufferPlugin {
    let descriptor = BufferPluginDescriptor(id: "apple.translation", title: "苹果翻译 · Translate", realtime: true)
    private var session: AnyObject?
    /// Source block → translation, per language pair, for this keyboard session.
    private var cache: [String: String] = [:]
    /// Separates source blocks in `options["blocks"]`; never typed.
    static let blockSeparator = "\u{1E}"
    func availability(for request: BufferPluginRequest) async -> BufferPluginAvailability {
        guard #available(iOS 26, *) else { return .unavailable(L("苹果翻译需要 iOS 26 或更新版本", "Apple translation requires iOS 26 or later")) }
        let source = Locale.Language(identifier: request.options["source"] ?? "zh-Hans")
        let target = Locale.Language(identifier: request.options["target"] ?? "en")
        guard source != target else { return .unavailable(L("请选择不同的原文和译文语言", "Choose different source and target languages")) }
        let availability: LanguageAvailability
        if #available(iOS 26.4, *) { availability = LanguageAvailability(preferredStrategy: .lowLatency) }
        else { availability = LanguageAvailability() }
        switch await availability.status(from: source, to: target) {
        case .installed: return .ready
        case .supported: return .unavailable(L("请先在 RIMES App 的“苹果翻译语言包”中下载这两种语言", "Download these languages in RIMES → Apple translation languages"))
        case .unsupported: return .unavailable(L("苹果翻译不支持此语言对", "Apple translation does not support this language pair"))
        @unknown default: return .unavailable(L("暂时无法使用此语言对", "This language pair is currently unavailable"))
        }
    }
    func execute(_ request: BufferPluginRequest, preview: @escaping @MainActor (String) -> Void) async throws -> BufferPluginResult {
        guard #available(iOS 26, *) else { throw BufferPluginError.unavailable("Requires iOS 26") }
        let source = Locale.Language(identifier: request.options["source"] ?? "zh-Hans")
        let target = Locale.Language(identifier: request.options["target"] ?? "en")
        let translator: TranslationSession
        if #available(iOS 26.4, *) { translator = TranslationSession(installedSource: source, target: target, preferredStrategy: .lowLatency) }
        else { translator = TranslationSession(installedSource: source, target: target) }
        session = translator
        defer { session = nil }
        // One output block per source block, so blocks line up and unchanged ones are reused.
        let blocks = request.options["blocks"].map { $0.components(separatedBy: Self.blockSeparator) }.flatMap { $0.joined() == request.source ? $0 : nil } ?? [request.source]
        let spaced = !["zh", "ja", "ko"].contains(target.languageCode?.identifier ?? "")
        let pair = "\(source.minimalIdentifier)>\(target.minimalIdentifier)|"
        var output: [String] = []
        do {
            for (index, block) in blocks.enumerated() {
                try Task.checkCancellation()
                let core = block.trimmingCharacters(in: .whitespacesAndNewlines)
                guard !core.isEmpty else { continue }
                let translated: String
                if let cached = cache[pair + core] { translated = cached }
                else {
                    let response = try await translator.translate(core)
                    try Task.checkCancellation()
                    translated = response.targetText.trimmingCharacters(in: .whitespacesAndNewlines)
                    if cache.count > 256 { cache.removeAll() }
                    cache[pair + core] = translated
                }
                output += Self.outputBlocks(translated, source: block, last: index == blocks.count - 1, spaced: spaced)
                preview(output.joined())
            }
            guard !output.isEmpty else { throw CoreError.incomplete }
            return BufferPluginResult(text: output.joined(), revision: request.revision, blocks: output)
        } catch is CancellationError { throw CancellationError() }
        catch {
            if Task.isCancelled { throw CancellationError() }
            throw BufferPluginError.unavailable(L("翻译未完成。请检查语言包；若系统限制键盘访问，请在设置中开启完全访问后重试。原文已保留。", "Translation failed. Check language downloads; if keyboard access is restricted, enable Full Access and retry. Source preserved."))
        }
    }
    /// Typed input often has no punctuation, but its translation does: split the translation
    /// itself the way Default splits typing (clauses, short Latin phrases). The source's line
    /// breaks are kept; space-delimited languages get a space before the next source block.
    static func outputBlocks(_ translated: String, source: String, last: Bool, spaced: Bool) -> [String] {
        var pieces = DefaultBlockSegmenter.segments(from: translated).flatMap { hangulPhrases($0) }
        guard !pieces.isEmpty else { return [] }
        let breaks = String(source.reversed().prefix { $0.isNewline }.reversed())
        pieces[pieces.count - 1] += breaks.isEmpty ? (spaced && !last ? " " : "") : breaks
        return pieces
    }
    /// Korean separates words with spaces but has no Latin phrase rule: cut Hangul text
    /// every few words, keeping each word's trailing space with it.
    static func hangulPhrases(_ text: String, words limit: Int = 3) -> [String] {
        guard text.unicodeScalars.contains(where: { (0xAC00...0xD7A3).contains($0.value) }) else { return [text] }
        var result: [String] = [], current = "", count = 0
        for character in text {
            if !character.isWhitespace, current.last?.isWhitespace == true {
                count += 1
                if count == limit { result.append(current); current = ""; count = 0 }
            }
            current.append(character)
        }
        if !current.isEmpty { result.append(current) }
        return result
    }
    func cancel() { if #available(iOS 26, *) { (session as? TranslationSession)?.cancel() } }
}

/// One network request per explicit Run tap. The instruction is fixed when the run starts.
@MainActor final class AITextPlugin: BufferPlugin {
    let descriptor: BufferPluginDescriptor
    private let provider: ProviderConfiguration
    private let key: String
    private let consent: String
    private let instruction: String
    /// Turns the finished reply into the blocks to send (e.g. one fixed-width row per line).
    private let shape: ((String) -> [String])?
    private let thinking: AIThinking
    init(id: String, title: String, provider: ProviderConfiguration, key: String, consent: String, instruction: String,
         thinking: AIThinking = .minimal, shape: ((String) -> [String])? = nil) {
        self.provider = provider; self.key = key; self.consent = consent; self.instruction = instruction; self.shape = shape; self.thinking = thinking
        descriptor = .init(id: id, title: title, realtime: false)
    }
    func availability(for request: BufferPluginRequest) async -> BufferPluginAvailability { .ready }
    func execute(_ request: BufferPluginRequest, preview: @escaping @MainActor (String) -> Void) async throws -> BufferPluginResult {
        // Try this level's spellings in order, starting from the one this service accepted before.
        let attempts = thinking.attempts(model: provider.model)
        let memory = "ai.thinking.v2.\(consent)|\(provider.model)|\(thinking.rawValue)"
        let remembered = min(max(0, UserDefaults.standard.integer(forKey: memory)), attempts.count - 1)
        for index in remembered..<attempts.count {
            let networkRequest = try AIRequest.make(provider: provider, key: key, source: request.source, instruction: instruction, consent: consent, extra: attempts[index])
            do {
                // Thinking streams first (tagged, shown dimmed and unlabelled), then the answer.
                let text = try await AIClient().generate(networkRequest) { text, reasoning in
                    if !text.isEmpty { await preview(text) }
                    else if !reasoning.isEmpty { await preview(ThinkingText.marker + reasoning) }
                }
                if index != remembered { UserDefaults.standard.set(index, forKey: memory) }
                let reply = text.trimmingCharacters(in: .whitespacesAndNewlines)
                guard let shape else { return .init(text: reply, revision: request.revision) }
                let blocks = shape(reply)
                return .init(text: blocks.joined(), revision: request.revision, blocks: blocks)
            } catch CoreError.response(let code) where (code == 400 || code == 422) && index < attempts.count - 1 {
                continue
            }
        }
        throw CoreError.incomplete
    }
    func cancel() {} // Runner cancels the owning Task; URLSession transport observes it.
}

/// Keyboard plugins, in shortcut order. Apple Translation runs as you type; AI plugins
/// only open, then wait for the user to write and tap Run.
enum KeyboardPlugin: String, CaseIterable {
    case translate = "apple.translation", ask = "ai.ask", polish = "ai.polish", poem = "ai.poem", art = "ai.art"
    var isAI: Bool { self != .translate }
    var title: String {
        switch self {
        case .translate: return L("苹果翻译", "Translate")
        case .polish: return L("AI 润色", "AI Polish")
        case .poem: return L("AI 作诗", "AI Poem")
        case .art: return L("AI 字符画", "AI Text Art")
        case .ask: return L("快问快答", "Quick Q&A")
        }
    }
    var shortTitle: String {
        switch self {
        case .translate: return L("翻译", "Trans")
        case .polish: return L("润色", "Polish")
        case .poem: return L("作诗", "Poem")
        case .art: return L("画画", "Draw")
        case .ask: return L("快问", "Ask")
        }
    }
    var symbol: String {
        switch self { case .translate: return "translate"; case .polish: return "wand.and.stars"; case .poem: return "text.book.closed"; case .art: return "square.grid.3x3.fill"; case .ask: return "questionmark.bubble" }
    }
    var placeholder: String {
        switch self {
        case .translate: return L("输入要翻译的文字", "Type text to translate")
        case .polish: return L("写下要润色的文字，再点 ▶ 执行", "Write text to polish, then tap ▶")
        case .poem: return L("写下主题或要藏的字，再点 ▶ 作诗", "Write a theme or hidden words, then tap ▶")
        case .art: return L("写下要画的东西，再点 ▶ 画出来", "Say what to draw, then tap ▶")
        case .ask: return L("问点什么，写完点 ▶", "Ask something, then tap ▶")
        }
    }
}

extension PoemMode {
    var title: String {
        switch self {
        case .improvise: return L("即兴", "Improvise")
        case .acrosticHead: return L("藏头", "Hide at start")
        case .acrosticTail: return L("藏尾", "Hide at end")
        }
    }
    /// Fits the narrow options key.
    var shortTitle: String {
        switch self {
        case .improvise: return L("即兴", "Free")
        case .acrosticHead: return L("藏头", "Head")
        case .acrosticTail: return L("藏尾", "Tail")
        }
    }
}
extension PoemLineLength {
    var title: String {
        switch self {
        case .free: return L("不限", "Any")
        case .four: return L("四言", "4 per line")
        case .five: return L("五言", "5 per line")
        case .six: return L("六言", "6 per line")
        case .seven: return L("七言", "7 per line")
        }
    }
}
extension PoemError {
    var message: String {
        switch self {
        case .noHiddenCharacters: return L("藏头/藏尾：先写下要藏的字", "Write the characters to hide first")
        case .tooManyHiddenCharacters(let limit): return L("最多藏 \(limit) 个字", "Hide at most \(limit) characters")
        }
    }
}

/// Reads inserted translation text aloud, in order, in the target language.
@MainActor final class BlockSpeaker {
    private let synthesizer = AVSpeechSynthesizer()
    /// Replays one block now, cutting off whatever is being read; used for repeated taps.
    func replay(_ text: String, language: String?) {
        if synthesizer.isSpeaking { synthesizer.stopSpeaking(at: .immediate) }
        speak(text, language: language)
    }
    /// Queues text after anything already being read. Without a language, it is detected.
    func speak(_ text: String, language: String?) {
        let line = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !line.isEmpty else { return }
        let language = language ?? NLLanguageRecognizer.dominantLanguage(for: line)?.rawValue ?? "zh-Hans"
        let audio = AVAudioSession.sharedInstance()
        try? audio.setCategory(.playback, mode: .spokenAudio, options: [.duckOthers])
        try? audio.setActive(true)
        let utterance = AVSpeechUtterance(string: line); utterance.voice = Self.voice(for: language)
        synthesizer.speak(utterance)
    }
    func stop() {
        guard synthesizer.isSpeaking else { return }
        synthesizer.stopSpeaking(at: .immediate)
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }
    /// Translation uses minimal identifiers ("en", "zh-Hans"); speech wants a region.
    static func voice(for identifier: String) -> AVSpeechSynthesisVoice? {
        let tag = ["zh-Hans": "zh-CN", "zh-Hant": "zh-TW", "zh": "zh-CN"][identifier] ?? identifier
        if let voice = AVSpeechSynthesisVoice(language: tag) { return voice }
        let code = Locale.Language(identifier: tag).languageCode?.identifier ?? tag
        let voices = AVSpeechSynthesisVoice.speechVoices().filter { $0.language.hasPrefix(code) }
        let preferred = Locale.current.identifier.replacingOccurrences(of: "_", with: "-")
        return voices.first { $0.language == preferred } ?? voices.first
    }
}

extension TextArtStyle {
    var title: String { self == .blocks ? L("彩色方块", "Colour blocks") : L("任意字符", "Any characters") }
}

/// The pet and keyboard share one theme. `light` is retained for decoding older preferences.
enum StatusSkin: String, Codable, CaseIterable, Identifiable {
    case apple
    case light
    /// The RIMES rhino mascot (the owner's Scholay Rhino pixel animations): acts out each state.
    case rhino
    case crab, kitten, puppy, piglet
    // Animated Noto Emoji (Google, CC BY 4.0), stored as "noto-<codepoint>".
    case dog = "noto-1f415", poodle = "noto-1f429", pig = "noto-1f416", rabbit = "noto-1f407", notoCrab = "noto-1f980"
    case penguin = "noto-1f427", fox = "noto-1f98a", panda = "noto-1f43c", turtle = "noto-1f422", octopus = "noto-1f419", frog = "noto-1f438", chick = "noto-1f423"
    var id: String { rawValue }
    var isNoto: Bool { rawValue.hasPrefix("noto-") }
    var title: String {
        switch self {
        case .apple: return L("苹果原生", "Apple native")
        case .light: return L("光点", "Dot"); case .rhino: return L("RIMES 犀牛", "RIMES Rhino"); case .crab: return L("寄居蟹", "Hermit crab")
        case .kitten: return L("小猫", "Kitten"); case .puppy: return L("小狗", "Puppy"); case .piglet: return L("小猪", "Piglet")
        case .dog: return L("狗狗", "Dog"); case .poodle: return L("贵宾犬", "Poodle"); case .pig: return L("猪", "Pig")
        case .rabbit: return L("兔子", "Rabbit"); case .notoCrab: return L("螃蟹", "Crab"); case .penguin: return L("企鹅", "Penguin")
        case .fox: return L("狐狸", "Fox"); case .panda: return L("熊猫", "Panda"); case .turtle: return L("乌龟", "Turtle")
        case .octopus: return L("章鱼", "Octopus"); case .frog: return L("青蛙", "Frog"); case .chick: return L("小鸡", "Chick")
        }
    }
    var canonical: StatusSkin { self == .light ? .apple : self }
    var usesDot: Bool { canonical == .apple }
    static var themes: [StatusSkin] { allCases.filter { $0 != .light } }
    /// The skins a tap rotates through; an empty or unknown list means all of them.
    static func rotation(_ ids: [String]) -> [StatusSkin] {
        let chosen = ids.compactMap(StatusSkin.init(rawValue:)).map(\.canonical)
        return chosen.isEmpty ? themes : themes.filter(chosen.contains)
    }
}

/// Tags a preview as the model's thinking rather than its answer. A private-use character,
/// never typed and never inserted.
enum ThinkingText {
    static let marker = "\u{E000}"
}

extension AIThinking {
    var title: String {
        switch self {
        case .off: return L("关闭", "Off"); case .minimal: return L("最低", "Minimal"); case .low: return L("低", "Low")
        case .medium: return L("中", "Medium"); case .high: return L("高", "High"); case .auto: return L("自动", "Auto")
        }
    }
}
