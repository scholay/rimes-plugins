import Compression
import CryptoKit
import Foundation

extension Notification.Name {
    static let typingTestLibraryDidChange = Notification.Name("RimeBuffer.TypingTestLibrary.didChange")
}

// MARK: - Imported books

/// A user-imported book, already cut into practice passages. Passages are the
/// only text persisted here and they never leave the local library directory.
struct TypingTestBook: Codable, Equatable, Identifiable {
    struct Chapter: Codable, Equatable {
        var title: String
        var passages: [String]
    }

    var version = 1
    /// Which cutting rules produced the passages. Bumped when the rules change, so
    /// re-importing the same file replaces an older cut, and results recorded
    /// against the old text stay under their own article version.
    var cut = TypingTestBook.currentCut
    static let currentCut = 2
    /// First 16 hex characters of the SHA-256 of the imported file.
    var id: String
    var title: String
    var author: String
    var importedAt: TimeInterval
    var chapters: [Chapter]

    init(id: String, title: String, author: String, importedAt: TimeInterval, chapters: [Chapter]) {
        self.id = id; self.title = title; self.author = author; self.importedAt = importedAt; self.chapters = chapters
    }
    private enum CodingKeys: String, CodingKey { case version, cut, id, title, author, importedAt, chapters }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        version = try c.decode(Int.self, forKey: .version)
        cut = try c.decodeIfPresent(Int.self, forKey: .cut) ?? 1
        id = try c.decode(String.self, forKey: .id)
        title = try c.decode(String.self, forKey: .title)
        author = try c.decode(String.self, forKey: .author)
        importedAt = try c.decode(TimeInterval.self, forKey: .importedAt)
        chapters = try c.decode([Chapter].self, forKey: .chapters)
    }

    var passageCount: Int { chapters.reduce(0) { $0 + $1.passages.count } }
    var characterCount: Int { chapters.reduce(0) { $0 + $1.passages.reduce(0) { $0 + $1.count } } }

    /// Article identities are positional (`epub.<book>.<n>`, n starts at 1).
    /// The cut points are persisted, so the numbering never shifts when the
    /// splitting heuristics change in a later release.
    func article(chapter: Int, passage: Int) -> TypingTestArticle? {
        guard chapters.indices.contains(chapter), chapters[chapter].passages.indices.contains(passage) else { return nil }
        let before = chapters[..<chapter].reduce(0) { $0 + $1.passages.count }
        let text = chapters[chapter].passages[passage]
        let count = chapters[chapter].passages.count
        return TypingTestArticle(
            id: "epub.\(id).\(before + passage + 1)", version: cut,
            language: TypingTestPassageSplitter.language(of: text),
            title: count > 1 ? "\(title) · \(chapters[chapter].title) · \(passage + 1)/\(count)"
                             : "\(title) · \(chapters[chapter].title)",
            theme: author.isEmpty ? title : author, difficulty: "导入", text: text)
    }

    func article(number: Int) -> TypingTestArticle? {
        guard number >= 1 else { return nil }
        var remaining = number - 1
        for (index, chapter) in chapters.enumerated() {
            if remaining < chapter.passages.count { return article(chapter: index, passage: remaining) }
            remaining -= chapter.passages.count
        }
        return nil
    }
}

// MARK: - Passage cutting

enum TypingTestPassageSplitter {
    /// Letters and ideographs, with a cheap path for ASCII (Unicode properties are slow in bulk).
    static func isLetter(_ scalar: Unicode.Scalar) -> Bool {
        if scalar.value < 128 { return (65...90).contains(scalar.value) || (97...122).contains(scalar.value) }
        return isIdeograph(scalar) || scalar.properties.isAlphabetic
    }

    static func isIdeograph(_ scalar: Unicode.Scalar) -> Bool {
        (0x3400...0x9FFF).contains(scalar.value) || (0xF900...0xFAFF).contains(scalar.value)
            || (0x20000...0x2FA1F).contains(scalar.value)
    }
    /// Characters between which a typeset space is dropped.
    static func isWide(_ scalar: Unicode.Scalar) -> Bool {
        isIdeograph(scalar) || (0x3000...0x303F).contains(scalar.value)
            || (0xFF00...0xFFEF).contains(scalar.value)
    }

    static func language(of text: String) -> TypingTestLanguage {
        var wide = 0, latin = 0
        for scalar in text.unicodeScalars {
            if isIdeograph(scalar) { wide += 1 }
            else if isLetter(scalar) { latin += 1 }
        }
        return wide >= latin && wide > 0 ? .chinese : .english
    }

    /// Whitespace runs become one space, invisible and control characters are
    /// removed, and a space between two ideographs (an artifact of pretty-printed
    /// markup) is dropped. A space beside Latin text is typography and stays.
    static func tidy(_ raw: String) -> String {
        var scalars: [Unicode.Scalar] = []
        var pendingSpace = false
        for scalar in raw.unicodeScalars {
            switch scalar.value {
            case 0x200B, 0x200C, 0xFEFF, 0x00AD, 0x2060: continue
            default: break
            }
            if scalar.properties.isWhitespace || scalar.value == 0x00A0 {
                pendingSpace = !scalars.isEmpty
                continue
            }
            if scalar.value < 0x20 || scalar.value == 0x7F { continue }
            if pendingSpace {
                if let last = scalars.last, !(isWide(last) && isWide(scalar)) { scalars.append(" ") }
                pendingSpace = false
            }
            scalars.append(scalar)
        }
        var text = ""
        text.unicodeScalars.append(contentsOf: scalars)
        return text
    }

    private static let closers: Set<Character> = ["”", "’", "」", "』", "）", ")", "》", "】", "\"", "'", "〕"]
    private static let wideStops: Set<Character> = ["。", "！", "？", "；", "…", "!", "?", ";"]

    static func sentences(of paragraph: String) -> [String] {
        let characters = Array(paragraph)
        var result: [String] = []
        var start = 0, index = 0
        while index < characters.count {
            let character = characters[index]
            var isStop = wideStops.contains(character)
            if character == "." {
                // Latin full stop: a sentence end only before whitespace/end.
                let next = index + 1 < characters.count ? characters[index + 1] : " "
                isStop = next == " " || closers.contains(next)
            }
            guard isStop else { index += 1; continue }
            var end = index + 1
            while end < characters.count, wideStops.contains(characters[end]) || closers.contains(characters[end]) { end += 1 }
            while end < characters.count, characters[end] == " " { end += 1 }
            result.append(String(characters[start..<end]))
            start = end
            index = end
        }
        if start < characters.count { result.append(String(characters[start...])) }
        return result
    }

    private static func hardCut(_ sentence: String, limit: Int) -> [String] {
        var characters = Array(sentence)
        var pieces: [String] = []
        let soft: Set<Character> = ["，", "、", "：", ",", ":", " "]
        while characters.count > limit {
            var cut = limit
            if let found = characters[(limit / 2)..<limit].lastIndex(where: { soft.contains($0) }) { cut = found + 1 }
            pieces.append(String(characters[..<cut]))
            characters.removeFirst(cut)
        }
        if !characters.isEmpty { pieces.append(String(characters)) }
        return pieces
    }

    /// Pieces stay short enough to type in one sitting and end on a paragraph
    /// boundary whenever they can. A paragraph longer than the limit is cut at
    /// sentence ends into evenly sized parts. All limits are far below the
    /// session's 2,048-character cap.
    static func pieceLimit(wide: Bool) -> Int { wide ? 800 : 1_000 }
    static let pieceLimit = 1_000          // upper bound for validation, either script
    static let minimumPiece = 150

    static func passages(from paragraphs: [String]) -> [String] {
        let wide = language(of: paragraphs.joined()) == .chinese
        let limit = pieceLimit(wide: wide)
        let separator = wide ? "" : " "
        var result: [String] = []
        var current = ""
        func flush() {
            let trimmed = current.trimmingCharacters(in: .whitespaces)
            if !trimmed.isEmpty { result.append(trimmed) }
            current = ""
        }
        for paragraph in paragraphs {
            if paragraph.count > limit {
                // Long paragraph: its own run of balanced parts, cut at sentence ends.
                flush()
                let parts = Int((Double(paragraph.count) / Double(limit)).rounded(.up))
                let target = Int((Double(paragraph.count) / Double(parts)).rounded(.up))
                for sentence in sentences(of: paragraph) {
                    for piece in sentence.count > limit ? hardCut(sentence, limit: limit) : [sentence] {
                        if current.count >= target || current.count + piece.count > limit { flush() }
                        current += piece
                    }
                }
                flush()
                continue
            }
            if !current.isEmpty, current.count + separator.count + paragraph.count > limit { flush() }
            if !current.isEmpty { current += separator }
            current += paragraph
        }
        flush()
        // A tiny trailing piece reads better attached to the one before it.
        if result.count > 1, let last = result.last, last.count < minimumPiece,
           let previous = result.dropLast().last, previous.count + last.count + separator.count <= limit + limit / 6 {
            result.removeLast(2)
            result.append(previous + separator + last)
        }
        return result
    }

    // MARK: Practice-friendly text

    private static let decorative: Set<UInt32> = Set("※★☆◆◇■□●○◎◈▲△▼▽♦♠♥♣▶◀►◄〓§¶†‡".unicodeScalars.map(\.value))
    private static let repeatable: Set<Character> = ["，", "。", "！", "？", "；", "：", "、", ",", ".", "!", "?", ";", ":", "-", "_", "=", "*", "~", "～", "·"]

    private static let linkPattern = try! NSRegularExpression(pattern: #"https?://\S+|www\.\S+|\S+@\S+\.\S+"#)
    private static let footnotePattern = try! NSRegularExpression(pattern: #"[\[［【(（]\s*(注\s*)?\d{1,3}\s*[\]］】)）]"#)
    private static let rulePattern = try! NSRegularExpression(pattern: #"[\-_=*~～]{2,}"#)
    private static func strip(_ expression: NSRegularExpression, from text: String) -> String {
        expression.stringByReplacingMatches(in: text, range: NSRange(text.startIndex..., in: text), withTemplate: "")
    }

    /// Removes what is awkward to type and carries no reading value: decorative
    /// symbols, footnote markers, links and addresses, rules made of symbols, and
    /// runs of repeated punctuation.
    static func practiceText(_ paragraph: String) -> String {
        // The address pattern backtracks badly on long unspaced text; only run it when a hint is present.
        let mayHaveLink = paragraph.contains("@") || paragraph.contains("http") || paragraph.contains("www.")
        var text = strip(footnotePattern, from: mayHaveLink ? strip(linkPattern, from: paragraph) : paragraph)
        text = String(String.UnicodeScalarView(text.unicodeScalars.filter { !decorative.contains($0.value) }))
        var out: [Character] = []
        for character in text {
            if let last = out.last, last == character, repeatable.contains(character) { continue }
            out.append(character)
        }
        return tidy(strip(rulePattern, from: String(out)))
    }

    /// Share of characters that are neither letters, ideographs nor spaces.
    static func symbolRatio(_ text: String) -> Double {
        guard !text.isEmpty else { return 1 }
        let symbols = text.unicodeScalars.filter { !(isLetter($0) || $0 == " ") }.count
        return Double(symbols) / Double(text.unicodeScalars.count)
    }

    /// True for a paragraph worth practising: enough letters or ideographs.
    static func isReadable(_ paragraph: String) -> Bool {
        paragraph.unicodeScalars.filter { isLetter($0) }.count >= 4
            && symbolRatio(paragraph) <= 0.35
    }
}

// MARK: - ZIP container

/// Minimal read-only ZIP reader for EPUB: stored and deflated entries, no
/// encryption or Zip64. Entry names are dictionary keys only; nothing is ever
/// extracted to disk, so a hostile path cannot escape anywhere.
struct TypingTestZipArchive {
    struct Entry { let method: UInt16; let compressedSize: Int; let size: Int; let offset: Int; let encrypted: Bool }
    enum ZipError: LocalizedError {
        case notAnArchive, unsupported(String), tooLarge, corrupt(String)
        var errorDescription: String? {
            switch self {
            case .notAnArchive: return "文件不是有效的 EPUB（ZIP）压缩包"
            case let .unsupported(detail): return "暂不支持此 EPUB：\(detail)"
            case .tooLarge: return "EPUB 内容超过安全大小限制"
            case let .corrupt(detail): return "EPUB 已损坏：\(detail)"
            }
        }
    }
    static let maximumArchiveBytes = 96 * 1_048_576
    static let maximumEntryBytes = 24 * 1_048_576
    static let maximumEntries = 20_000

    private let bytes: [UInt8]
    private(set) var entries: [String: Entry] = [:]
    private var inflatedBudget = 160 * 1_048_576

    private func u16(_ at: Int) -> Int { Int(bytes[at]) | Int(bytes[at + 1]) << 8 }
    private func u32(_ at: Int) -> Int { u16(at) | u16(at + 2) << 16 }

    init(data: Data) throws {
        guard data.count >= 22, data.count <= Self.maximumArchiveBytes else {
            throw data.count > Self.maximumArchiveBytes ? ZipError.tooLarge : ZipError.notAnArchive
        }
        bytes = [UInt8](data)
        var eocd: Int?
        var probe = bytes.count - 22
        let floor = max(0, bytes.count - 22 - 65_535)
        while probe >= floor {
            if bytes[probe] == 0x50, bytes[probe + 1] == 0x4B, bytes[probe + 2] == 0x05, bytes[probe + 3] == 0x06 { eocd = probe; break }
            probe -= 1
        }
        guard let end = eocd else { throw ZipError.notAnArchive }
        let count = u16(end + 10), directorySize = u32(end + 12), directoryOffset = u32(end + 16)
        if count == 0xFFFF || directorySize == 0xFFFF_FFFF || directoryOffset == 0xFFFF_FFFF {
            throw ZipError.unsupported("Zip64 压缩包")
        }
        guard count <= Self.maximumEntries, directoryOffset + directorySize <= bytes.count else { throw ZipError.corrupt("目录越界") }
        var cursor = directoryOffset
        for _ in 0..<count {
            guard cursor + 46 <= bytes.count, u32(cursor) == 0x0201_4B50 else { throw ZipError.corrupt("目录项") }
            let flags = u16(cursor + 8), method = UInt16(u16(cursor + 10))
            let compressed = u32(cursor + 20), size = u32(cursor + 24)
            let nameLength = u16(cursor + 28), extra = u16(cursor + 30), comment = u16(cursor + 32)
            let offset = u32(cursor + 42)
            guard cursor + 46 + nameLength + extra + comment <= bytes.count else { throw ZipError.corrupt("目录项长度") }
            let nameBytes = Array(bytes[(cursor + 46)..<(cursor + 46 + nameLength)])
            let name = String(bytes: nameBytes, encoding: .utf8) ?? String(bytes: nameBytes, encoding: .isoLatin1) ?? ""
            if entries[name] == nil {
                entries[name] = Entry(method: method, compressedSize: compressed, size: size, offset: offset, encrypted: flags & 1 != 0)
            }
            cursor += 46 + nameLength + extra + comment
        }
    }

    func contains(_ name: String) -> Bool { entries[name] != nil }

    mutating func read(_ name: String) throws -> Data {
        guard let entry = entries[name] else { throw ZipError.corrupt("缺少 \(name)") }
        guard !entry.encrypted else { throw ZipError.unsupported("加密内容（DRM）") }
        guard entry.size <= Self.maximumEntryBytes, entry.size <= inflatedBudget else { throw ZipError.tooLarge }
        let header = entry.offset
        guard header + 30 <= bytes.count, u32(header) == 0x0403_4B50 else { throw ZipError.corrupt("本地头 \(name)") }
        let start = header + 30 + u16(header + 26) + u16(header + 28)
        guard start >= 0, start + entry.compressedSize <= bytes.count else { throw ZipError.corrupt("数据越界 \(name)") }
        inflatedBudget -= entry.size
        switch entry.method {
        case 0:
            guard entry.compressedSize == entry.size else { throw ZipError.corrupt("大小不符 \(name)") }
            return Data(bytes[start..<(start + entry.size)])
        case 8:
            if entry.size == 0 { return Data() }
            var output = [UInt8](repeating: 0, count: entry.size)
            let written = bytes.withUnsafeBufferPointer { source in
                output.withUnsafeMutableBufferPointer { destination in
                    compression_decode_buffer(destination.baseAddress!, entry.size,
                                              source.baseAddress! + start, entry.compressedSize,
                                              nil, COMPRESSION_ZLIB)
                }
            }
            guard written == entry.size else { throw ZipError.corrupt("解压失败 \(name)") }
            return Data(output)
        default:
            throw ZipError.unsupported("压缩方式 \(entry.method)")
        }
    }
}

// MARK: - EPUB

enum TypingTestEPUBError: LocalizedError {
    case missingPackage, noSpine, noText, tooLarge
    var errorDescription: String? {
        switch self {
        case .missingPackage: return "EPUB 缺少 container.xml 或包文件（OPF）"
        case .noSpine: return "EPUB 没有可阅读的章节"
        case .noText: return "没有从这本 EPUB 中读出可供跟打的正文"
        case .tooLarge: return "这本书的正文太大，请换一本较短的或拆分后再导入"
        }
    }
}

enum TypingTestEPUBImporter {
    static let maximumPassages = 6_000
    static let maximumCharacters = 6 * 1_048_576
    static let minimumChapterCharacters = 30

    static func parse(data: Data, importedAt: TimeInterval = Date().timeIntervalSince1970) throws -> TypingTestBook {
        var archive = try TypingTestZipArchive(data: data)
        guard archive.contains("META-INF/container.xml") else { throw TypingTestEPUBError.missingPackage }
        let container = ContainerParser.parse(try archive.read("META-INF/container.xml"))
        guard let packagePath = container, archive.contains(packagePath) else { throw TypingTestEPUBError.missingPackage }
        let package = PackageParser.parse(try archive.read(packagePath))
        let base = packagePath.contains("/") ? String(packagePath[..<packagePath.lastIndex(of: "/")!]) + "/" : ""
        let manifest = Dictionary(package.manifest.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        let spine = package.spine.filter(\.linear).compactMap { manifest[$0.idref] }
            .filter { $0.mediaType.contains("html") && !$0.properties.split(separator: " ").contains("nav") }
        guard !spine.isEmpty else { throw TypingTestEPUBError.noSpine }

        var chapters: [TypingTestBook.Chapter] = []
        var totalCharacters = 0, totalPassages = 0
        for (index, item) in spine.enumerated() {
            let path = resolve(base: base, href: item.href)
            guard archive.contains(path), let document = decode(try archive.read(path)) else { continue }
            let extracted = HTMLText.extract(document)
            let raw = extracted.paragraphs
            let paragraphs = raw.map(TypingTestPassageSplitter.practiceText).filter(TypingTestPassageSplitter.isReadable)
            let text = paragraphs.joined()
            guard text.count >= minimumChapterCharacters,
                  !isFrontMatter(heading: extracted.heading, first: raw.first, count: text.count),
                  !looksLikeCatalog(raw) else { continue }
            let passages = TypingTestPassageSplitter.passages(from: paragraphs)
            guard !passages.isEmpty else { continue }
            totalCharacters += text.count
            totalPassages += passages.count
            guard totalCharacters <= maximumCharacters, totalPassages <= maximumPassages else { throw TypingTestEPUBError.tooLarge }
            let title = extracted.heading.flatMap { $0.isEmpty ? nil : String($0.prefix(40)) } ?? "第 \(index + 1) 部分"
            chapters.append(.init(title: title, passages: passages))
        }
        guard !chapters.isEmpty else { throw TypingTestEPUBError.noText }
        // Number the chapters so every piece has a stable, sortable code: 001, 002, …
        let width = max(2, String(chapters.count).count)
        for index in chapters.indices {
            chapters[index].title = String(format: "%0\(width)d", index + 1) + " · " + chapters[index].title
        }
        let digest = SHA256.hash(data: data).prefix(8).map { String(format: "%02x", $0) }.joined()
        let title = TypingTestPassageSplitter.tidy(package.title)
        return TypingTestBook(id: digest, title: title.isEmpty ? "未命名图书" : String(title.prefix(80)),
                              author: String(TypingTestPassageSplitter.tidy(package.author).prefix(60)),
                              importedAt: importedAt, chapters: chapters)
    }

    /// A contents page is many short lines, mostly titles and numbers.
    private static func looksLikeCatalog(_ paragraphs: [String]) -> Bool {
        guard paragraphs.count >= 8 else { return false }
        let average = Double(paragraphs.reduce(0) { $0 + $1.count }) / Double(paragraphs.count)
        let shortLines = paragraphs.filter { $0.count <= 24 }.count
        let numbered = paragraphs.filter { $0.last?.isNumber == true || $0.first?.isNumber == true }.count
        return (average < 18 && shortLines * 10 >= paragraphs.count * 8)
            || (shortLines * 10 >= paragraphs.count * 9 && numbered * 10 >= paragraphs.count * 5)
    }

    /// Contents, copyright and similar short pages are not reading material.
    private static func isFrontMatter(heading: String?, first: String?, count: Int) -> Bool {
        guard count < 4_000 else { return false }
        let names = ["目录", "目 录", "contents", "table of contents", "版权", "版权信息", "版权页", "copyright", "封面", "cover", "title page", "书名页"]
        func matches(_ text: String?) -> Bool {
            guard let text = text?.lowercased().trimmingCharacters(in: .whitespacesAndNewlines), !text.isEmpty else { return false }
            return names.contains { text == $0 || text.hasPrefix($0) && text.count <= $0.count + 12 }
                || text.hasPrefix("目录") || text.hasPrefix("table of contents")
        }
        return matches(heading) || matches(first)
    }

    private static func resolve(base: String, href: String) -> String {
        let withoutFragment = href.split(separator: "#", maxSplits: 1, omittingEmptySubsequences: false).first.map(String.init) ?? href
        let decoded = withoutFragment.removingPercentEncoding ?? withoutFragment
        var parts: [Substring] = []
        for part in (base + decoded).split(separator: "/", omittingEmptySubsequences: true) {
            if part == "." { continue }
            if part == ".." { if !parts.isEmpty { parts.removeLast() }; continue }
            parts.append(part)
        }
        return parts.joined(separator: "/")
    }

    private static func decode(_ data: Data) -> String? {
        if data.starts(with: [0xFF, 0xFE]) || data.starts(with: [0xFE, 0xFF]) { return String(data: data, encoding: .utf16) }
        return String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1)
    }

    // MARK: XML pieces

    private final class ContainerParser: NSObject, XMLParserDelegate {
        private var path: String?
        static func parse(_ data: Data) -> String? {
            let delegate = ContainerParser()
            let parser = XMLParser(data: data)
            parser.shouldResolveExternalEntities = false
            parser.delegate = delegate
            parser.parse()
            return delegate.path
        }
        func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?,
                    qualifiedName qName: String?, attributes attributeDict: [String: String] = [:]) {
            if path == nil, elementName.hasSuffix("rootfile"), let full = attributeDict["full-path"] { path = full }
        }
    }

    private struct ManifestItem { let id: String; let href: String; let mediaType: String; let properties: String }
    private struct SpineItem { let idref: String; let linear: Bool }

    private final class PackageParser: NSObject, XMLParserDelegate {
        var title = "", author = ""
        var manifest: [ManifestItem] = []
        var spine: [SpineItem] = []
        private var capturing: String?
        private var buffer = ""
        static func parse(_ data: Data) -> PackageParser {
            let delegate = PackageParser()
            let parser = XMLParser(data: data)
            parser.shouldResolveExternalEntities = false
            parser.delegate = delegate
            parser.parse()
            return delegate
        }
        private func local(_ name: String) -> String { name.split(separator: ":").last.map(String.init) ?? name }
        func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?,
                    qualifiedName qName: String?, attributes attributeDict: [String: String] = [:]) {
            switch local(elementName) {
            case "item":
                if let id = attributeDict["id"], let href = attributeDict["href"] {
                    manifest.append(.init(id: id, href: href, mediaType: attributeDict["media-type"] ?? "",
                                          properties: attributeDict["properties"] ?? ""))
                }
            case "itemref":
                if let idref = attributeDict["idref"] { spine.append(.init(idref: idref, linear: attributeDict["linear"] != "no")) }
            case "title" where title.isEmpty && elementName.contains(":"): capturing = "title"; buffer = ""
            case "creator" where author.isEmpty: capturing = "creator"; buffer = ""
            default: break
            }
        }
        func parser(_ parser: XMLParser, foundCharacters string: String) { if capturing != nil { buffer += string } }
        func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?, qualifiedName qName: String?) {
            guard let current = capturing, local(elementName) == current else { return }
            if current == "title" { title = buffer } else { author = buffer }
            capturing = nil
        }
    }

    // MARK: XHTML → paragraphs

    /// Tolerant tag scanner. Real-world EPUB markup is often not well-formed
    /// XML (HTML entities, unclosed tags), so this never fails; it just reads.
    enum HTMLText {
        private static let skipped: Set<String> = ["script", "style", "head", "rt", "rp", "svg", "noscript", "nav", "math"]
        private static let blocks: Set<String> = ["p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "ul", "ol", "tr", "table",
                                                  "blockquote", "section", "article", "pre", "br", "hr", "dd", "dt", "dl",
                                                  "figcaption", "figure", "header", "footer", "aside", "body", "caption"]
        private static let entities: [String: String] = [
            "amp": "&", "lt": "<", "gt": ">", "quot": "\"", "apos": "'", "nbsp": " ", "ndash": "–", "mdash": "—",
            "hellip": "…", "lsquo": "‘", "rsquo": "’", "ldquo": "“", "rdquo": "”", "copy": "©", "middot": "·",
            "laquo": "«", "raquo": "»", "bull": "•", "shy": "",
        ]

        static func extract(_ html: String) -> (heading: String?, paragraphs: [String]) {
            let s = Array(html.unicodeScalars)
            var paragraphs: [String] = []
            var paragraph = ""
            var heading: String?
            var headingTag: String?
            var headingBuffer = ""
            var skipTag: String?
            var skipDepth = 0
            var i = 0

            func flush() {
                let tidy = TypingTestPassageSplitter.tidy(paragraph)
                if !tidy.isEmpty { paragraphs.append(tidy) }
                paragraph = ""
            }
            func matches(_ text: String, at index: Int) -> Bool {
                let needle = Array(text.unicodeScalars)
                guard index + needle.count <= s.count else { return false }
                for (offset, scalar) in needle.enumerated() where s[index + offset] != scalar { return false }
                return true
            }
            func skip(to marker: String, from index: Int) -> Int {
                var j = index
                while j < s.count, !matches(marker, at: j) { j += 1 }
                return min(s.count, j + marker.unicodeScalars.count)
            }
            func append(_ text: String) {
                guard skipTag == nil else { return }
                paragraph += text
                if headingTag != nil { headingBuffer += text }
            }

            while i < s.count {
                let scalar = s[i]
                if scalar == "<" {
                    if matches("<!--", at: i) { i = skip(to: "-->", from: i + 4); continue }
                    if matches("<![CDATA[", at: i) {
                        let end = skip(to: "]]>", from: i + 9)
                        var text = ""
                        text.unicodeScalars.append(contentsOf: s[(i + 9)..<max(i + 9, end - 3)])
                        append(text)
                        i = end; continue
                    }
                    if i + 1 < s.count, s[i + 1] == "!" || s[i + 1] == "?" { i = skip(to: ">", from: i + 1); continue }
                    // Tag: scan to the closing ">" outside quotes.
                    var j = i + 1
                    var quote: Unicode.Scalar?
                    while j < s.count {
                        if let q = quote { if s[j] == q { quote = nil } }
                        else if s[j] == "\"" || s[j] == "'" { quote = s[j] }
                        else if s[j] == ">" { break }
                        j += 1
                    }
                    var body = ""
                    body.unicodeScalars.append(contentsOf: s[(i + 1)..<min(j, s.count)])
                    i = min(s.count, j + 1)
                    var isClosing = false
                    if body.hasPrefix("/") { isClosing = true; body.removeFirst() }
                    let selfClosing = body.hasSuffix("/")
                    let name = String(body.prefix { !$0.isWhitespace && $0 != "/" }).lowercased()
                        .split(separator: ":").last.map(String.init) ?? ""
                    if let current = skipTag {
                        if name == current {
                            if isClosing { skipDepth -= 1; if skipDepth == 0 { skipTag = nil } }
                            else if !selfClosing { skipDepth += 1 }
                        }
                        continue
                    }
                    if !isClosing, !selfClosing, skipped.contains(name) { skipTag = name; skipDepth = 1; continue }
                    if blocks.contains(name) { flush() }
                    if ["h1", "h2", "h3"].contains(name) {
                        if !isClosing, heading == nil, headingTag == nil { headingTag = name; headingBuffer = "" }
                        else if isClosing, headingTag == name {
                            heading = TypingTestPassageSplitter.tidy(headingBuffer)
                            headingTag = nil
                        }
                    }
                    continue
                }
                if scalar == "&" {
                    var j = i + 1
                    while j < s.count, j - i <= 10, s[j] != ";", s[j] != "&", s[j] != "<", !s[j].properties.isWhitespace { j += 1 }
                    if j < s.count, s[j] == ";" {
                        var name = ""
                        name.unicodeScalars.append(contentsOf: s[(i + 1)..<j])
                        var decoded: String?
                        if name.hasPrefix("#x") || name.hasPrefix("#X") {
                            decoded = UInt32(name.dropFirst(2), radix: 16).flatMap(Unicode.Scalar.init).map { String($0) }
                        } else if name.hasPrefix("#") {
                            decoded = UInt32(name.dropFirst()).flatMap(Unicode.Scalar.init).map { String($0) }
                        } else { decoded = entities[name] }
                        if let decoded { append(decoded); i = j + 1; continue }
                    }
                }
                var text = ""
                var j = i
                while j < s.count, s[j] != "<", s[j] != "&" || j == i { text.unicodeScalars.append(s[j]); j += 1 }
                append(text)
                i = j
            }
            flush()
            // The chapter heading is the chapter's name in the list, not practice text.
            if let heading, !heading.isEmpty, paragraphs.first == heading { paragraphs.removeFirst() }
            return (heading, paragraphs)
        }
    }
}

// MARK: - Local library

/// Books the user imported on this Mac. Each lives in its own JSON file below
/// `typing-library/`; the directory is never uploaded or synchronised.
final class TypingTestLibraryStore {
    static let shared = TypingTestLibraryStore()
    static let maximumBooks = 40
    static let maximumBookBytes = 12 * 1_048_576

    enum ImportOutcome { case added(TypingTestBook), replaced(TypingTestBook), alreadyPresent(TypingTestBook) }
    enum LibraryError: LocalizedError {
        case libraryFull, unreadable
        var errorDescription: String? {
            switch self {
            case .libraryFull: return "文库已达 \(TypingTestLibraryStore.maximumBooks) 本上限，请先移除不需要的书"
            case .unreadable: return "无法读取所选文件"
            }
        }
    }

    private let directory: URL
    private(set) var books: [TypingTestBook] = []
    private(set) var storageIssue: String?

    init(storageRoot: URL? = nil) {
        let environment = ProcessInfo.processInfo.environment
        let environmentRoot = environment["RIMEBUFFER_LOCAL_DATA_ROOT"] ?? environment["RIMEBUFFER_USER_DIR"]
        let root = storageRoot
            ?? environmentRoot.map { URL(fileURLWithPath: $0, isDirectory: true) }
            ?? FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/\(RimesPaths.directoryName)")
        directory = root.appendingPathComponent("typing-library", isDirectory: true)
        reload()
    }

    func book(id: String) -> TypingTestBook? { books.first { $0.id == id } }

    /// Resolves `epub.<book>.<n>` for scoring history, or nil when the book was removed.
    func article(id: String) -> TypingTestArticle? {
        let pieces = id.split(separator: ".")
        guard pieces.count == 3, pieces[0] == "epub", let number = Int(pieces[2]),
              let book = book(id: String(pieces[1])) else { return nil }
        return book.article(number: number)
    }

    private func reload() {
        var loaded: [TypingTestBook] = []
        do {
            try LocalMetricsFileSecurity.validateDirectoryIfPresent(directory)
            let names = (try? FileManager.default.contentsOfDirectory(atPath: directory.path)) ?? []
            for name in names.sorted() where name.hasSuffix(".json") && name.count == 21 {
                guard let data = try? LocalMetricsFileSecurity.readIfPresent(from: directory.appendingPathComponent(name),
                                                                             maximumBytes: Self.maximumBookBytes),
                      let book = try? JSONDecoder().decode(TypingTestBook.self, from: data),
                      Self.valid(book), name == book.id + ".json" else { continue }
                loaded.append(book)
            }
        } catch {
            storageIssue = error.localizedDescription
        }
        books = loaded.sorted { $0.importedAt > $1.importedAt }
    }

    /// File read and parsing touch no store state, so callers may run this off the main thread.
    static func parseFile(at url: URL) throws -> TypingTestBook {
        let values = try url.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
        guard values.isRegularFile == true, let size = values.fileSize else { throw LibraryError.unreadable }
        guard size <= TypingTestZipArchive.maximumArchiveBytes else { throw TypingTestZipArchive.ZipError.tooLarge }
        guard let data = try? Data(contentsOf: url, options: .mappedIfSafe) else { throw LibraryError.unreadable }
        return try TypingTestEPUBImporter.parse(data: data)
    }

    @discardableResult
    func importEPUB(at url: URL) throws -> ImportOutcome { try add(Self.parseFile(at: url)) }

    @discardableResult
    func add(_ book: TypingTestBook) throws -> ImportOutcome {
        let existing = self.book(id: book.id)
        // The same file imported again only replaces the stored text when the
        // cutting rules have moved on (an older cut of the same book).
        if let existing, existing.cut >= book.cut { return .alreadyPresent(existing) }
        guard existing != nil || books.count < Self.maximumBooks else { throw LibraryError.libraryFull }
        guard Self.valid(book) else { throw TypingTestEPUBError.tooLarge }
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        try LocalMetricsFileSecurity.write(try encoder.encode(book), to: directory.appendingPathComponent(book.id + ".json"),
                                           maximumBytes: Self.maximumBookBytes)
        books.removeAll { $0.id == book.id }
        books.insert(book, at: 0)
        NotificationCenter.default.post(name: .typingTestLibraryDidChange, object: self)
        return existing == nil ? .added(book) : .replaced(book)
    }

    @discardableResult
    func remove(id: String) -> Bool {
        guard book(id: id) != nil else { return false }
        let file = directory.appendingPathComponent(id + ".json")
        guard !LocalMetricsFileSecurity.pathEntryIsSymbolicLink(file),
              (try? FileManager.default.removeItem(at: file)) != nil else { return false }
        books.removeAll { $0.id == id }
        NotificationCenter.default.post(name: .typingTestLibraryDidChange, object: self)
        return true
    }

    static func valid(_ book: TypingTestBook) -> Bool {
        book.version == 1 && book.cut >= 1 && book.id.count == 16 && book.id.allSatisfy(\.isHexDigit)
            && !book.chapters.isEmpty && book.passageCount <= TypingTestEPUBImporter.maximumPassages
            && book.chapters.allSatisfy { chapter in
                !chapter.passages.isEmpty && chapter.passages.allSatisfy {
                    !$0.isEmpty && $0.count <= TypingTestPassageSplitter.pieceLimit * 2 && $0.utf8.count <= TypingTestSession.maximumTextBytes
                }
            }
    }
}
