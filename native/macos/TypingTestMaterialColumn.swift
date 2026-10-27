import AppKit

/// Left-hand material column of the typing practice page: built-in articles and
/// imported books (book → chapter → passage). Only articles and passages are
/// selectable; books and chapters expand and collapse.
final class TypingTestMaterialColumn: NSView, NSOutlineViewDataSource, NSOutlineViewDelegate {
    static let preferredWidth: CGFloat = 204

    private final class Node: NSObject {
        enum Kind {
            case section(String)
            case article(TypingTestArticle)
            case book(TypingTestBook)
            case chapter(TypingTestBook, Int)
            case passage(TypingTestArticle, bookID: String, label: String, indent: CGFloat)
        }
        let kind: Kind
        let children: () -> [Node]
        private var cache: [Node]?
        var loadedChildren: [Node] { if let cache { return cache }; let built = children(); cache = built; return built }
        init(_ kind: Kind, children: @escaping () -> [Node] = { [] }) { self.kind = kind; self.children = children }
        var article: TypingTestArticle? {
            switch kind {
            case let .article(article), let .passage(article, _, _, _): return article
            default: return nil
            }
        }
        var bookID: String? {
            switch kind {
            case let .book(book), let .chapter(book, _): return book.id
            case let .passage(_, bookID, _, _): return bookID
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
    private let importButton = RimePointingHandButton(title: "", target: nil, action: nil)
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
        importButton.imagePosition = .imageOnly
        importButton.setAccessibilityLabel("导入 EPUB 电子书")
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
        outline.indentationPerLevel = 0   // cards fill the row; nested rows indent themselves
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
        caption.preferredMaxLayoutWidth = Self.preferredWidth - 22
        let footer = NSStackView(views: [caption, removeButton])
        footer.orientation = .vertical
        footer.alignment = .leading
        footer.spacing = 6
        footer.translatesAutoresizingMaskIntoConstraints = false

        // Content keeps its natural width inside a clipped outer view, so the
        // column can fold to nothing without breaking its own constraints.
        let inner = NSView()
        inner.translatesAutoresizingMaskIntoConstraints = false
        addSubview(inner)
        inner.addSubview(header)
        inner.addSubview(scroll)
        inner.addSubview(footer)
        wantsLayer = true
        layer?.masksToBounds = true
        widthConstraint = widthAnchor.constraint(equalToConstant: Self.preferredWidth)
        NSLayoutConstraint.activate([
            inner.leadingAnchor.constraint(equalTo: leadingAnchor),
            inner.topAnchor.constraint(equalTo: topAnchor),
            inner.bottomAnchor.constraint(equalTo: bottomAnchor),
            inner.widthAnchor.constraint(equalToConstant: Self.preferredWidth),
            header.leadingAnchor.constraint(equalTo: inner.leadingAnchor, constant: 12),
            header.trailingAnchor.constraint(equalTo: inner.trailingAnchor, constant: -8),
            header.topAnchor.constraint(equalTo: inner.topAnchor, constant: 12),
            scroll.leadingAnchor.constraint(equalTo: inner.leadingAnchor, constant: 4),
            scroll.trailingAnchor.constraint(equalTo: inner.trailingAnchor, constant: -2),
            scroll.topAnchor.constraint(equalTo: header.bottomAnchor, constant: 8),
            footer.leadingAnchor.constraint(equalTo: inner.leadingAnchor, constant: 12),
            footer.trailingAnchor.constraint(equalTo: inner.trailingAnchor, constant: -10),
            footer.topAnchor.constraint(equalTo: scroll.bottomAnchor, constant: 8),
            footer.bottomAnchor.constraint(equalTo: inner.bottomAnchor, constant: -10),
            widthConstraint,
        ])
    }

    private var widthConstraint: NSLayoutConstraint!
    private(set) var isCollapsed = false

    /// Folds the column away (or back) so the practice area can use the space.
    func setCollapsed(_ collapsed: Bool, animated: Bool = true) {
        guard collapsed != isCollapsed else { return }
        isCollapsed = collapsed
        let width = collapsed ? 0 : Self.preferredWidth
        if animated {
            NSAnimationContext.runAnimationGroup { context in
                context.duration = 0.18
                context.allowsImplicitAnimation = true
                widthConstraint.animator().constant = width
            }
        } else {
            widthConstraint.constant = width
        }
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
                            let parts = book.chapters[index].passages.count
                            // A chapter that is one piece is selectable itself; several parts expand.
                            if parts == 1, let article = book.article(chapter: index, passage: 0) {
                                return Node(.passage(article, bookID: book.id, label: book.chapters[index].title, indent: 24))
                            }
                            return Node(.chapter(book, index)) {
                                book.chapters[index].passages.indices.compactMap { passage in
                                    book.article(chapter: index, passage: passage).map {
                                        Node(.passage($0, bookID: book.id, label: "第 \(passage + 1) / \(parts) 部分", indent: 36))
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
        case .article: return 58
        case .book: return 54
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

    /// Built-in articles and book headers are cards; chapters and passages stay compact rows.
    func outlineView(_ outlineView: NSOutlineView, rowViewForItem item: Any) -> NSTableRowView? {
        guard let node = item as? Node else { return nil }
        switch node.kind {
        case .article, .book:
            let row = MaterialRowView()
            row.isCard = true
            row.isSelectableCard = node.article != nil
            return row
        default: return nil
        }
    }

    func outlineView(_ outlineView: NSOutlineView, viewFor tableColumn: NSTableColumn?, item: Any) -> NSView? {
        guard let node = item as? Node else { return nil }
        let identifier = NSUserInterfaceItemIdentifier("material.cell")
        let cell = (outlineView.makeView(withIdentifier: identifier, owner: self) as? MaterialCell) ?? {
            let made = MaterialCell(); made.identifier = identifier; return made
        }()
        switch node.kind {
        case let .section(title): cell.configure(title: title, detail: nil, leading: 8, style: .section)
        case let .article(article):
            cell.configure(title: article.title,
                           detail: "\(article.theme) · \(article.difficulty) · \(article.displayLength)",
                           leading: 14, style: .card)
        case let .book(book):
            cell.configure(title: book.title,
                           detail: "\(book.chapters.count) 章 · \(book.passageCount) 段 · \(book.author.isEmpty ? "佚名" : book.author)",
                           leading: 30, style: .card)
        case let .chapter(book, index):
            cell.configure(title: book.chapters[index].title, detail: "\(book.chapters[index].passages.count) 部分",
                           leading: 24, style: .row)
        case let .passage(article, _, label, indent):
            cell.configure(title: label, detail: article.displayLength, leading: indent, style: .row)
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
    var smokeWidth: CGFloat { frame.width }
    var smokeOutline: NSOutlineView { outline }
}

private final class MaterialCell: NSTableCellView {
    enum Style { case section, row, card }
    private let title = NSTextField(labelWithString: "")
    private let detail = NSTextField(labelWithString: "")
    private let meta = NSTextField(labelWithString: "")
    private var cardOnly: [NSLayoutConstraint] = []
    private var detailBelow: [NSLayoutConstraint] = []
    private var detailBeside: [NSLayoutConstraint] = []
    private var sectionOnly: [NSLayoutConstraint] = []
    private var rowLeading: NSLayoutConstraint!
    private var cardLeading: NSLayoutConstraint!

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        for field in [title, detail, meta] {
            field.lineBreakMode = .byTruncatingTail
            field.translatesAutoresizingMaskIntoConstraints = false
            addSubview(field)
        }
        meta.setContentCompressionResistancePriority(.required, for: .horizontal)
        meta.setContentHuggingPriority(.required, for: .horizontal)
        cardLeading = title.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 14)
        cardOnly = [
            cardLeading,
            title.topAnchor.constraint(equalTo: topAnchor, constant: 11),
            title.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -12),
            detail.leadingAnchor.constraint(equalTo: title.leadingAnchor),
            detail.topAnchor.constraint(equalTo: title.bottomAnchor, constant: 3),
            detail.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -12),
            meta.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -14),
            meta.centerYAnchor.constraint(equalTo: title.centerYAnchor),
        ]
        title.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        detail.setContentCompressionResistancePriority(.defaultHigh, for: .horizontal)
        detail.setContentHuggingPriority(.required, for: .horizontal)
        rowLeading = title.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 2)
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

    func configure(title text: String, detail detailText: String?, meta metaText: String? = nil, leading: CGFloat = 2, style: Style) {
        rowLeading.constant = leading
        cardLeading.constant = leading
        title.stringValue = text
        detail.stringValue = detailText ?? ""
        detail.isHidden = detailText == nil
        meta.stringValue = metaText ?? ""
        meta.isHidden = metaText == nil
        toolTip = [text, detailText].compactMap { $0 }.joined(separator: " · ")
        var inactive: [NSLayoutConstraint] = detailBelow
        inactive.append(contentsOf: detailBeside)
        inactive.append(contentsOf: sectionOnly)
        inactive.append(contentsOf: cardOnly)
        inactive.append(rowLeading)
        NSLayoutConstraint.deactivate(inactive)
        switch style {
        case .section:
            title.font = .systemFont(ofSize: 10, weight: .semibold)
            title.textColor = RimeUI.textMuted
            title.stringValue = text.uppercased()
            NSLayoutConstraint.activate(sectionOnly)
            rowLeading.isActive = true
        case .row:
            title.font = .systemFont(ofSize: 12)
            title.textColor = RimeUI.textPrimary
            detail.font = .monospacedDigitSystemFont(ofSize: 10.5, weight: .regular)
            detail.textColor = RimeUI.textMuted
            NSLayoutConstraint.activate(detailBeside)
            rowLeading.isActive = true
        case .card:
            title.font = .systemFont(ofSize: 12.5, weight: .semibold)
            title.textColor = RimeUI.textPrimary
            detail.font = .systemFont(ofSize: 10.5)
            detail.textColor = RimeUI.textSecondary
            meta.font = .monospacedDigitSystemFont(ofSize: 10.5, weight: .medium)
            meta.textColor = RimeUI.accentTextColor
            NSLayoutConstraint.activate(cardOnly)
        }
    }
}

/// A rounded card behind built-in articles and book headers; the selected card
/// gets an accent border instead of the stock list highlight.
private final class MaterialRowView: NSTableRowView {
    var isCard = false
    var isSelectableCard = false
    override var isSelected: Bool { didSet { needsDisplay = true } }

    override func drawBackground(in dirtyRect: NSRect) {
        guard isCard else { super.drawBackground(in: dirtyRect); return }
        let selected = isSelected && isSelectableCard
        let path = NSBezierPath(roundedRect: bounds.insetBy(dx: 4, dy: 2.5), xRadius: 10, yRadius: 10)
        (selected ? RimeUI.accentGreen.withAlphaComponent(0.12) : RimeUI.surface).setFill()
        path.fill()
        (selected ? RimeUI.accentGreen : RimeUI.border).setStroke()
        path.lineWidth = selected ? 1.5 : 1
        path.stroke()
    }
    override func drawSelection(in dirtyRect: NSRect) {
        if !isCard { super.drawSelection(in: dirtyRect) }
    }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        needsDisplay = true
    }
}
