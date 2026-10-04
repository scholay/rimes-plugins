import AppKit

/// Product-level modules share registration and navigation, not a transcript
/// schema. In particular, a PTY is never persisted as a chatbot conversation.
enum MailboxModuleID: String, CaseIterable, Codable {
    case terminal, chat, inbox
    var title: String {
        switch self { case .terminal: return "终端"; case .chat: return "对话"; case .inbox: return "收件" }
    }
    var symbol: String {
        switch self { case .terminal: return "terminal"; case .chat: return "bubble.left"; case .inbox: return "tray" }
    }
    var pluginKey: PluginKey { PluginKey(domain: .builtIn, rawID: "builtin.mailbox.\(rawValue)") }
    var filters: [MailboxInboxFilter] {
        guard let package = try? PresetBufferPluginInstallationStore.shared.package(id: pluginKey.rawID, requireEnabled: false),
              let filters = package.contribution.options?["filters"] else { return [] }
        let names: [String: MailboxInboxFilter] = ["all": .all, "unread": .unread, "attention": .attention, "archived": .archived]
        return filters.components(separatedBy: ",").compactMap { names[$0] }
    }
}

protocol MailboxModule: HostModuleContribution {
    var moduleID: MailboxModuleID { get }
}

final class MailboxBuiltInPlugin: InternalPlugin, MailboxModule {
    let moduleID: MailboxModuleID
    var hostID: String { "mailbox" }
    var moduleKey: PluginKey { moduleID.pluginKey }
    let descriptor: PluginDescriptor
    init(_ module: MailboxModuleID) {
        moduleID = module
        descriptor = PluginDescriptor(
            key: module.pluginKey, wireID: nil, name: module.title,
            symbolName: module.symbol, version: "1.1.0",
            summary: "Mailbox \(module.title)工作区；停用保留已有内容。",
            source: .builtIn, capabilities: [.hostModule, .mailboxModule, .localStorage],
            settings: nil, canUninstall: true
        )
    }
    func start() {}
    func stop() {
        // Module disablement hides access; hiding or switching UI is never an
        // implicit kill signal for an accepted interactive process.
    }
    func makeSettingsViewController(subpageID: String) -> NSViewController? { nil }
}

enum MailboxContentWorkspace: String, Codable { case chat, inbox }
enum MailboxInboxFilter: Int, CaseIterable { case all, unread, attention, archived }

extension MailboxThread {
    var contentWorkspace: MailboxContentWorkspace {
        if let workspace { return workspace }
        if source.replyCapability == .localNotesOnly
            || messages.contains(where: { $0.imageFileName != nil })
            || title?.hasPrefix("图片 · ") == true { return .inbox }
        return .chat
    }
    var displayTitle: String {
        if let title, !title.isEmpty { return title }
        let body = messages.first(where: { $0.kind == .content })?.body ?? source.displayName
        let firstLine = body.split(whereSeparator: \.isNewline).first.map(String.init) ?? body
        return String(firstLine.prefix(44))
    }
    var needsAttention: Bool { review?.state == .pending || generation?.phase == .failed }
    var workspaceStatus: String {
        if archivedAt != nil { return "已归档" }
        if generation?.phase == .generating { return "生成中" }
        if generation?.phase == .failed { return "请求中断" }
        if review?.state == .pending { return "待处理" }
        return "已保存"
    }
}

enum MailboxWorkspaceRules {
    static func threads(_ threads: [MailboxThread], module: MailboxModuleID,
                        query: String, filter: MailboxInboxFilter) -> [MailboxThread] {
        threads.filter { thread in
            guard module != .terminal,
                  thread.contentWorkspace == (module == .chat ? .chat : .inbox) else { return false }
            if filter == .archived {
                guard thread.archivedAt != nil else { return false }
            } else {
                guard thread.archivedAt == nil else { return false }
                if filter == .unread && !thread.unread { return false }
                if filter == .attention && !thread.needsAttention { return false }
            }
            let query = query.trimmingCharacters(in: .whitespacesAndNewlines)
            return query.isEmpty || thread.displayTitle.localizedCaseInsensitiveContains(query)
                || thread.source.displayName.localizedCaseInsensitiveContains(query)
                || thread.messages.contains { $0.body.localizedCaseInsensitiveContains(query) }
        }
    }
}
