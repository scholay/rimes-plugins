import AppKit
import Foundation

/// Shared registration shape for host-owned modules. Mailbox can adopt this
/// later without inheriting Buffer transformations or Capsule storage.
protocol HostModuleContribution {
    var hostID: String { get }
    var moduleKey: PluginKey { get }
    var actions: Set<HostModuleAction> { get }
}

enum HostModuleAction: String { case open, search }

extension HostModuleContribution {
    var actions: Set<HostModuleAction> {
        guard let package = try? PresetBufferPluginInstallationStore.shared.package(id: moduleKey.rawID, requireEnabled: false),
              package.contribution.options?["host"] == hostID,
              let actions = package.contribution.options?["actions"] else { return [] }
        return Set(actions.components(separatedBy: ",").compactMap(HostModuleAction.init(rawValue:)))
    }
}

/// A Capsule module contributes navigation and classification. The host owns
/// every store, permission check, record operation, and rendered detail view.
protocol CapsuleModule: HostModuleContribution {
    var moduleID: CapsuleModuleID { get }
    var filters: [CapsuleModuleFilter] { get }
}

enum CapsuleModuleID: String, CaseIterable, Codable, Hashable {
    case temporary, capture, notes, resources, passwords

    var title: String {
        switch self {
        case .temporary: return "临时"
        case .capture: return "捕获"
        case .notes: return "笔记"
        case .resources: return "资源"
        case .passwords: return "密码"
        }
    }

    var symbolName: String {
        switch self {
        case .temporary: return "clock.arrow.circlepath"
        case .capture: return "camera.viewfinder"
        case .notes: return "note.text"
        case .resources: return "books.vertical"
        case .passwords: return "lock"
        }
    }

    var filters: [CapsuleModuleFilter] {
        if let package = try? PresetBufferPluginInstallationStore.shared.package(id: pluginKey.rawID, requireEnabled: false),
           let declared = package.contribution.options?["filters"] {
            return declared.components(separatedBy: ",").compactMap(CapsuleModuleFilter.init(rawValue:))
        }
        return defaultFilters
    }

    var defaultFilters: [CapsuleModuleFilter] {
        switch self {
        case .temporary: return [.all, .text, .image, .other]
        case .capture: return [.all, .image, .video]
        case .notes: return [.all, .bullet, .richText, .other]
        case .resources: return [.all, .reference, .document, .project, .skills, .other]
        case .passwords: return [.all, .login, .key, .other]
        }
    }

    var pluginKey: PluginKey {
        PluginKey(domain: .builtIn, rawID: BuiltInPluginID.capsule(self))
    }
}

enum CapsuleModuleFilter: String, Codable, Hashable {
    case all, text, image, video, bullet, richText, reference
    case document, project, skills, login, key, other

    var title: String {
        switch self {
        case .all: return "全部"
        case .text: return "文本"
        case .image: return "图片"
        case .video: return "视频"
        case .bullet: return "Bullet"
        case .richText: return "富文本"
        case .reference: return "文献"
        case .document: return "文档"
        case .project: return "项目"
        case .skills: return "Skills"
        case .login: return "登录"
        case .key: return "密钥"
        case .other: return "其他"
        }
    }
}

/// Classification is metadata-only. Password bodies are never opened here.
enum CapsuleModuleClassification {
    static func clipboard(_ item: ClipboardStoredItemMetadata) -> CapsuleModuleFilter {
        clipboard(kind: item.kind, canonicalText: item.canonicalText)
    }

    static func clipboard(_ item: ClipboardHistoryItem) -> CapsuleModuleFilter {
        clipboard(kind: item.kind, canonicalText: item.canonicalText)
    }

    static func password(_ category: CapsulePasswordCategory) -> CapsuleModuleFilter {
        switch category {
        case .login: return .login
        case .key: return .key
        case .other: return .other
        }
    }

    static func clipboard(kind: ClipboardItemKind, canonicalText: String?) -> CapsuleModuleFilter {
        switch kind {
        case .text, .link: return .text
        case .image: return .image
        case .files:
            guard let value = canonicalText,
                  !value.contains("\n") else { return .other }
            let url: URL
            if let parsed = URL(string: value), parsed.isFileURL {
                url = parsed
            } else if NSString(string: value).isAbsolutePath {
                url = URL(fileURLWithPath: value)
            } else {
                return .other
            }
            let imageExtensions: Set<String> = [
                "png", "jpg", "jpeg", "heic", "webp", "tif", "tiff", "gif", "bmp",
            ]
            return imageExtensions.contains(url.pathExtension.lowercased())
                ? .image : .other
        case .color, .unknown: return .other
        }
    }

    static func capture(_ record: CaptureRecord) -> CapsuleModuleFilter {
        switch record.kind {
        case .image, .scrolling: return .image
        case .video, .gif: return .video
        }
    }

    static func content(_ record: CapsuleContentRecord) -> CapsuleModuleFilter {
        switch record.summary.type {
        case .note:
            switch record.noteFormat {
            case .bullet: return .bullet
            case .richText: return .richText
            case .other: return .other
            }
        case .pdf: return .reference
        case .skill: return .skills
        case .resource:
            switch record.resourceType {
            case .document: return .document
            case .project: return .project
            case .other: return .other
            }
        case .image: return .image
        case .video: return .video
        case .password: return .other
        }
    }
}

/// Official modules participate in the same producer, enablement and plugin
/// settings mechanisms as Buffer plugins, but have no Buffer action protocol.
final class CapsuleBuiltInPlugin: InternalPlugin, CapsuleModule {
    let moduleID: CapsuleModuleID
    var hostID: String { "capsule" }
    var moduleKey: PluginKey { moduleID.pluginKey }
    var filters: [CapsuleModuleFilter] { moduleID.filters }
    let descriptor: PluginDescriptor

    init(_ moduleID: CapsuleModuleID) {
        self.moduleID = moduleID
        descriptor = PluginDescriptor(
            key: moduleID.pluginKey,
            wireID: nil,
            name: moduleID.title,
            symbolName: moduleID.symbolName,
            version: "1.1.0",
            summary: "Capsule \(moduleID.title)模块；停用后保留已有数据。",
            source: .builtIn,
            capabilities: [.hostModule, .capsuleModule, .localStorage],
            settings: nil,
            canUninstall: true
        )
    }

    func start() {}
    func stop() {}
    func makeSettingsViewController(subpageID: String) -> NSViewController? { nil }
}

enum CapsuleModuleAvailability {
    static var enabled: [CapsuleModuleID] {
        CapsuleModuleID.allCases.filter {
            PluginRegistry.shared.isEnabled($0.pluginKey)
        }
    }
}

enum CapsuleNavigationPolicy {
    static let modulesEnabledKey = "capsule.modules.navigation.v1"

    static var usesModules: Bool {
        if ProcessInfo.processInfo.environment["RIMES_CAPSULE_LEGACY_VIEW"] == "1" {
            return false
        }
        guard UserDefaults.standard.object(forKey: modulesEnabledKey) != nil else {
            return true
        }
        return UserDefaults.standard.bool(forKey: modulesEnabledKey)
    }
}
