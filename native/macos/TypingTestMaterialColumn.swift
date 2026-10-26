import AppKit

/// Left-hand material column of the typing practice page: built-in articles and
/// imported books (book → chapter → passage). Only articles and passages are
/// selectable; books and chapters expand and collapse.
final class TypingTestMaterialColumn: NSView, NSOutlineViewDataSource, NSOutlineViewDelegate {
    static let preferredWidth: CGFloat = 236

    private final class Node: NSObject {
        enum Kind {
            case section(String)
            case article(TypingTestArticle)
            case book(TypingTestBook)
            case chapter(TypingTestBook, Int)
            case passage(TypingTestArticle, bookID: String, number: Int)
        }
        let kind: Kind
        let children: () -> [Node]
        private var cache: [Node]?
        var loadedChildren: [Node] { if let cache { return cache }; let built = children(); cache = built; return built }
        init(_ kind: Kind, children: @escaping () -> [Node] = { [] }) { self.kind = kind; self.children = children }
        var article: TypingTestArticle? {
            switch kind {
            case let .article(article), let .passage(article, _, _): return article
            default: return nil
            }
        }
        var bookID: String? {
            switch kind {
            case let .book(book), let .chapter(book, _): return book.id
            case let .passage(_, bookID, _): return bookID
            default: return nil
            }
        }
        var isExpandable: Bool {
            switch kind {
            case .section, .book, .chapter: return true
            default: return false
            }
        }
    }

    var onSelect: ((TypingTestArticle) -> Void)?
    var onImport: (() -> Void)?
    var onRemoveBook: ((TypingTestBook) -> Void)?
    var onDropFiles: (([URL]) -> Void)?
    var isEditingEnabled = true { didSet { importButton.isEnabled = isEditingEnabled; outline.isEnabled = isEditingEnabled; updateRemoveButton() } }

    private let library: TypingTestLibraryStore
    private let outline = NSOutlineView()
    private let importButton = RimePointingHandButton(title: "导入 EPUB", target: nil, action: nil)
    private let removeButton = RimePointingHandButton(title: "移除此书", target: nil, action: nil)
    private let caption = NSTextField(wrappingLabelWithString: "")
    private var roots: [Node] = []
    private var suppressSelection = false
    private(set) var currentArticleID: String?

    init(library: TypingTestLibraryStore) {
        self.library = library
        super.init(frame: .zero)
        translatesAutoresizingMaskIntoConstraints = false
        setAccessibilityLabel("跟打素材")
        build()
        reload()
        registerForDraggedTypes([.fileURL])
    }
    required init?(coder: NSCoder) { nil }

    // MARK: Layout

    private func build() {
        let title = NSTextField(labelWithString: "素材")
        title.font = .systemFont(ofSize: 13, weight: .semibold)
        title.textColor = RimeUI.textPrimary
        importButton.target = self
        importButton.action = #selector(importTapped)
        importButton.bezelStyle = .rounded
        importButton.controlSize = .small
        importButton.image = NSImage(systemSymbolName: "plus", accessibilityDescription: "导入 EPUB 电子书")
        importButton.imagePosition = .imageLeading
        importButton.toolTip = "导入 EPUB 电子书，切分为跟打段落；也可以把 .epub 文件拖到这里"
        importButton.refusesFirstResponder = true
        let header = NSStackView(views: [title, NSView(), importButton])
        header.orientation = .horizontal
        header.alignment = .centerY
        header.translatesAutoresizingMaskIntoConstraints = false

        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("material"))
        column.resizingMask = .autoresizingMask
        outline.addTableColumn(column)
        outline.outlineTableColumn = column
        outline.headerView = nil
        outline.backgroundColor = .clear
        outline.rowSizeStyle = .custom
        outline.rowHeight = 24
        outline.indentationPerLevel = 12
        outline.intercellSpacing = NSSize(width: 0, height: 1)
        outline.selectionHighlightStyle = .regular
        outline.floatsGroupRows = false
        outline.columnAutoresizingStyle = .uniformColumnAutoresizingStyle
        outline.dataSource = self
        outline.delegate = self
        outline.setAccessibilityLabel("素材列表")
        let scroll = NSScrollView()
        scroll.documentView = outline
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.hasHorizontalScroller = false
        scroll.autohidesScrollers = true
        scroll.borderType = .noBorder
        scroll.translatesAutoresizingMaskIntoConstraints = false

        removeButton.target = self
        removeButton.action = #selector(removeTapped)
        removeButton.bezelStyle = .rounded
        removeButton.controlSize = .small
        removeButton.refusesFirstResponder = true
        removeButton.isHidden = true
        caption.font = .systemFont(ofSize: 11)
        caption.textColor = RimeUI.textSecondary
        caption.maximumNumberOfLines = 3
        caption.preferredMaxLayoutWidth = Self.preferredWidth - 28
        let footer = NSStackView(views: [caption, removeButton])
        footer.orientation = .vertical
        footer.alignment = .leading
        footer.spacing = 6
        footer.translatesAutoresizingMaskIntoConstraints = false

        addSubview(header)
        addSubview(scroll)
        addSubview(footer)
        NSLayoutConstraint.activate([
            header.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 14),
            header.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10),
            header.topAnchor.constraint(equalTo: topAnchor, constant: 14),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 6),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -4),
            scroll.topAnchor.constraint(equalTo: header.bottomAnchor, constant: 8),
            footer.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 14),
            footer.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -14),
            footer.topAnchor.constraint(equalTo: scroll.bottomAnchor, constant: 8),
            footer.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -12),
            widthAnchor.constraint(equalToConstant: Self.preferredWidth),
        ])
    }

    // MARK: Data

    func reload() {
        let selected = currentArticleID
        let builtIn = Node(.section("内置文章")) { TypingTestArticles.all.map { Node(.article($0)) } }
        roots = [builtIn]
        if !library.books.isEmpty {
            let mine = Node(.section("我的书籍 · \(library.books.count)")) { [library] in
                library.books.map { book in
                    Node(.book(book)) {
                        book.chapters.indices.map { index in
                            Node(.chapter(book, index)) {
                                book.chapters[index].passages.indices.compactMap { passage in
                                    book.article(chapter: index, passage: passage).map {
                                        Node(.passage($0, bookID: book.id, number: passage + 1))
                                    }
                                }
                            }
                        }
                    }
                }
            }
            roots.append(mine)
        }
        suppressSelection = true
        outline.reloadData()
        roots.forEach { outline.expandItem($0) }
        suppressSelection = false
        if let selected { select(articleID: selected) }
        updateCaption()
        updateRemoveButton()
    }

    /// Expands the path to an article and highlights it without notifying `onSelect`.
    func select(articleID: String) {
        currentArticleID = articleID
        suppressSelection = true
        defer { suppressSelection = false }
        var path: [Node]?
        func search(_ node: Node, _ trail: [Node]) {
            guard path == nil else { return }
            if node.article?.id == articleID { path = trail + [node]; return }
            guard node.isExpandable else { return }
            // Only imported ids can live below a book; avoid walking every passage of every book.
            if case .book = node.kind, !articleID.hasPrefix("epub.\(node.bookID ?? "")") { return }
            if case .chapter = node.kind, !articleID.hasPrefix("epub.\(node.bookID ?? "")") { return }
            for child in node.loadedChildren { search(child, trail + [node]) }
        }
        roots.forEach { search($0, []) }
        guard let path, let target = path.last else { outline.deselectAll(nil); updateRemoveButton(); return }
        path.dropLast().forEach { outline.expandItem($0) }
        let row = outline.row(forItem: target)
        if row >= 0 {
            outline.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
            outline.scrollRowToVisible(row)
        }
        updateRemoveButton()
    }

    /// The book owning the current selection, for the remove button.
    var selectedBook: TypingTestBook? {
        guard let id = currentArticleID?.split(separator: ".").dropFirst().first.map(String.init),
              currentArticleID?.hasPrefix("epub.") == true else { return nil }
        return library.book(id: id)
    }

    /// First passage of a book, used after an import.
    func selectFirstPassage(of book: TypingTestBook) {
        guard let article = book.article(number: 1) else { return }
        select(articleID: article.id)
        onSelect?(article)
    }

    func setStatus(_ text: String?) {
        statusOverride = text
        updateCaption()
    }
    private var statusOverride: String?

    private func updateCaption() {
        if let statusOverride { caption.stringValue = statusOverride; return }
        caption.stringValue = library.books.isEmpty
            ? "导入 EPUB，把喜欢的书切成可跟打的段落。书籍只保存在本机。"
            : "已导入 \(library.books.count) 本 · 共 \(library.books.reduce(0) { $0 + $1.passageCount }) 段"
    }
    private func updateRemoveButton() { removeButton.isHidden = selectedBook == nil; removeButton.isEnabled = isEditingEnabled }

    @objc private func importTapped() { onImport?() }
    @objc private func removeTapped() { if let book = selectedBook { onRemoveBook?(book) } }

    // MARK: Outline

    func outlineView(_ outlineView: NSOutlineView, numberOfChildrenOfItem item: Any?) -> Int {
        guard let node = item as? Node else { return roots.count }
        return node.loadedChildren.count
    }
    func outlineView(_ outlineView: NSOutlineView, child index: Int, ofItem item: Any?) -> Any {
        (item as? Node).map { $0.loadedChildren[index] } ?? roots[index]
    }
    func outlineView(_ outlineView: NSOutlineView, isItemExpandable item: Any) -> Bool { (item as? Node)?.isExpandable ?? false }
    func outlineView(_ outlineView: NSOutlineView, isGroupItem item: Any) -> Bool {
        if case .section = (item as? Node)?.kind { return true }
        return false
    }
    func outlineView(_ outlineView: NSOutlineView, heightOfRowByItem item: Any) -> CGFloat {
        guard let node = item as? Node else { return 24 }
        switch node.kind {
        case .section: return 26
        case .book: return 38
        default: return 24
        }
    }
    func outlineView(_ outlineView: NSOutlineView, shouldSelectItem item: Any) -> Bool {
        guard let node = item as? Node else { return false }
        if node.isExpandable {
            // Clicking a section/book/chapter row folds it instead of selecting it.
            if outlineView.isItemExpanded(node) { outlineView.collapseItem(node) } else { outlineView.expandItem(node) }
            return false
        }
        return true
    }
    func outlineViewSelectionDidChange(_ notification: Notification) {
        guard !suppressSelection, let node = outline.item(atRow: outline.selectedRow) as? Node,
              let article = node.article else { return }
        currentArticleID = article.id
        updateRemoveButton()
        onSelect?(article)
    }

    func outlineView(_ outlineView: NSOutlineView, viewFor tableColumn: NSTableColumn?, item: Any) -> NSView? {
        guard let node = item as? Node else { return nil }
        let identifier = NSUserInterfaceItemIdentifier("material.cell")
        let cell = (outlineView.makeView(withIdentifier: identifier, owner: self) as? MaterialCell) ?? {
            let made = MaterialCell(); made.identifier = identifier; return made
        }()
        switch node.kind {
        case let .section(title): cell.configure(title: title, detail: nil, style: .section)
        case let .article(article): cell.configure(title: article.title, detail: article.displayLength, style: .row)
        case let .book(book):
            cell.configure(title: book.title, detail: "\(book.chapters.count) 章 · \(book.passageCount) 段 · \(book.author.isEmpty ? "佚名" : book.author)",
                           style: .book)
        case let .chapter(book, index):
            cell.configure(title: book.chapters[index].title, detail: "\(book.chapters[index].passages.count) 段", style: .row)
        case let .passage(article, _, number):
            cell.configure(title: "第 \(number) 段", detail: article.displayLength, style: .row)
        }
        return cell
    }

    // MARK: Drag and drop of .epub files

    private func epubURLs(_ sender: NSDraggingInfo) -> [URL] {
        let urls = sender.draggingPasteboard.readObjects(forClasses: [NSURL.self],
                                                         options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
        return urls.filter { $0.pathExtension.lowercased() == "epub" }
    }
    override func draggingEntered(_ sender: any NSDraggingInfo) -> NSDragOperation {
        isEditingEnabled && !epubURLs(sender).isEmpty ? .copy : []
    }
    override func performDragOperation(_ sender: any NSDraggingInfo) -> Bool {
        let urls = epubURLs(sender)
        guard isEditingEnabled, !urls.isEmpty else { return false }
        onDropFiles?(urls)
        return true
    }

    // MARK: Test hooks

    var smokeRowCount: Int { outline.numberOfRows }
    var smokeOutline: NSOutlineView { outline }
}

private final class MaterialCell: NSTableCellView {
    enum Style { case section, row, book }
    private let title = NSTextField(labelWithString: "")
    private let detail = NSTextField(labelWithString: "")
    private var detailBelow: [NSLayoutConstraint] = []
    private var detailBeside: [NSLayoutConstraint] = []
    private var sectionOnly: [NSLayoutConstraint] = []

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        for field in [title, detail] {
            field.lineBreakMode = .byTruncatingTail
            field.translatesAutoresizingMaskIntoConstraints = false
            addSubview(field)
        }
        title.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        detail.setContentCompressionResistancePriority(.defaultHigh, for: .horizontal)
        detail.setContentHuggingPriority(.required, for: .horizontal)
        NSLayoutConstraint.activate([
            title.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 2),
        ])
        detailBeside = [
            title.centerYAnchor.constraint(equalTo: centerYAnchor),
            detail.centerYAnchor.constraint(equalTo: centerYAnchor),
            detail.leadingAnchor.constraint(greaterThanOrEqualTo: title.trailingAnchor, constant: 6),
            detail.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
        ]
        sectionOnly = [title.centerYAnchor.constraint(equalTo: centerYAnchor),
                       title.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -8)]
        detailBelow = [
            title.topAnchor.constraint(equalTo: topAnchor, constant: 3),
            title.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
            detail.leadingAnchor.constraint(equalTo: title.leadingAnchor),
            detail.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
            detail.topAnchor.constraint(equalTo: title.bottomAnchor, constant: 1),
        ]
    }
    required init?(coder: NSCoder) { nil }

    func configure(title text: String, detail detailText: String?, style: Style) {
        title.stringValue = text
        detail.stringValue = detailText ?? ""
        detail.isHidden = detailText == nil
        toolTip = [text, detailText].compactMap { $0 }.joined(separator: " · ")
        NSLayoutConstraint.deactivate(detailBelow + detailBeside + sectionOnly)
        switch style {
        case .section:
            title.font = .systemFont(ofSize: 10, weight: .semibold)
            title.textColor = RimeUI.textMuted
            title.stringValue = text.uppercased()
            NSLayoutConstraint.activate(sectionOnly)
        case .row:
            title.font = .systemFont(ofSize: 12)
            title.textColor = RimeUI.textPrimary
            detail.font = .monospacedDigitSystemFont(ofSize: 10.5, weight: .regular)
            detail.textColor = RimeUI.textMuted
            NSLayoutConstraint.activate(detailBeside)
        case .book:
            title.font = .systemFont(ofSize: 12, weight: .semibold)
            title.textColor = RimeUI.textPrimary
            detail.font = .systemFont(ofSize: 10.5)
            detail.textColor = RimeUI.textSecondary
            NSLayoutConstraint.activate(detailBelow)
        }
    }
}
