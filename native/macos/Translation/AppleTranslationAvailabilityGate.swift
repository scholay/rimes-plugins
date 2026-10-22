import Foundation
import Translation

/// Decides, before a translation runs, whether macOS would have to ask the
/// person for something.
///
/// `TranslationSession.prepareTranslation()` and `translate(_:)` both present
/// the system's language-pack download sheet when a pack is missing. That is
/// right when the person asked to download, and wrong in the middle of
/// typing: the sheet steals attention from the field they are writing in. So
/// ordinary translation consults the installed state first and, when a pack is
/// missing, fails quietly with a message instead of raising the sheet. Only an
/// explicit download request (`allowsDownloadPrompt`) may reach the system UI.
@available(macOS 15.0, *)
enum AppleTranslationAvailabilityGate {
    enum Verdict: Equatable {
        /// Every needed pack is on this Mac; translating raises no UI.
        case ready
        /// The pair is supported but a pack must be downloaded first.
        case needsDownload
        /// The pair (or the detected source language) cannot be translated.
        case unsupported
    }

    static let needsDownloadMessage =
        "本地翻译语言包尚未下载，请在“设置 › 实时翻译”中下载"
    static let unsupportedMessage = "本地翻译不支持当前语言组合"

    static func verdict(for session: TranslationSession,
                        sampleText: String) async -> Verdict {
        guard let target = session.targetLanguage else { return .unsupported }
        let availability = LanguageAvailability()
        let status: LanguageAvailability.Status
        if let source = session.sourceLanguage {
            status = await availability.status(from: source, to: target)
        } else {
            // An unpinned source is detected from the text itself.
            do {
                status = try await availability.status(for: sampleText, to: target)
            } catch {
                return .unsupported
            }
        }
        return verdict(for: status)
    }

    static func verdict(for status: LanguageAvailability.Status) -> Verdict {
        switch status {
        case .installed: return .ready
        case .supported: return .needsDownload
        case .unsupported: return .unsupported
        @unknown default: return .unsupported
        }
    }

    static func message(for verdict: Verdict) -> String? {
        switch verdict {
        case .ready: return nil
        case .needsDownload: return needsDownloadMessage
        case .unsupported: return unsupportedMessage
        }
    }
}
