import AppKit

/// Right-hand material sidebar of the typing practice page. Two tabs: 文章 shows
/// the built-in articles as cards; 书籍 shows the imported books as cards and,
/// once one is opened, a flat numbered list of its pieces. The collapse control
/// lives here, in the sidebar, and folds it to a thin rail.
final class TypingTestMaterialColumn: NSView, NSTableViewDataSource, NSTableViewDelegate {
    static let preferredWidth: CGFloat = 224
    static let railWidth: CGFloat = 34

    private enum Tab: Int { case articles = 0, books = 1 }
    private enum Row {
        case article(TypingTestArticle)
        case book(TypingTestBook)
        case back(TypingTestBook)
        case piece(TypingTestArticle, label: String)
        case hint(String)
        var article: TypingTestArticle? {
            switch self {
            case let .article(article), let .piece(article, _): return article
            default: return nil
            }
        }
    }

    var onSelect: ((TypingTestArticle) -> Void)?
    var onImport: (() -> Void)?
    var onRemoveBook: ((TypingTestBook) -> Void)?
    var onDropFiles: (([URL]) -> Void)?
    var onCollapsedChange: ((Bool) -> Void)?
    var isEditingEnabled = true {
        didSet { importButton.isEnabled = isEditingEnabled; table.isEnabled = isEditingEnabled; updateRemoveButton() }
    }

    private let library: TypingTestLibraryStore
    private let table = NSTableView()
    private let tabs = NSSegmentedControl(labels: ["文章", "书籍"], trackingMode: .selectOne, target: nil, action: nil)
    private let titleLabel = NSTextField(labelWithString: "素材")
    private let importButton = RimePointingHandButton(title: "", target: nil, action: nil)
    private let collapseButton = RimePointingHandButton(title: "", target: nil, action: nil)
    private let removeButton = RimePointingHandButton(title: "移除此书", target: nil, action: nil)
    private let caption = NSTextField(wrappingLabelWithString: "")
    private var scroll: NSScrollView!
    private var footer: NSView!
    private var rows: [Row] = []
    private var tab: Tab = .articles
    private var openBook: TypingTestBook?
    private var suppressSelection = false
    private var statusOverride: String?
    private var widthConstraint: NSLayoutConstraint!
    private(set) var currentArticleID: String?
    private(set) var isCollapsed = false

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
        titleLabel.font = .systemFont(ofSize: 13, weight: .semibold)
        titleLabel.textColor = RimeUI.textPrimary
        importButton.target = self
        importButton.action = #selector(importTapped)
        importButton.bezelStyle = .rounded
        importButton.controlSize = .small
        importButton.image = NSImage(systemSymbolName: "plus", accessibilityDescription: "导入 EPUB 电子书")
        importButton.imagePosition = .imageOnly
        importButton.setAccessibilityLabel("导入 EPUB 电子书")
        importButton.toolTip = "导入 EPUB 电子书，每章成为一份素材；也可以把 .epub 文件拖到这里"
        importButton.refusesFirstResponder = true
        collapseButton.target = self
        collapseButton.action = #selector(collapseTapped)
        collapseButton.bezelStyle = .rounded
        collapseButton.controlSize = .small
        collapseButton.imagePosition = .imageOnly
        collapseButton.refusesFirstResponder = true
        tabs.target = self
        tabs.action = #selector(tabChanged)
        tabs.segmentStyle = .rounded
        tabs.controlSize = .small
        tabs.selectedSegment = 0
        tabs.setAccessibilityLabel("素材类型")

        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("material"))
        column.resizingMask = .autoresizingMask
        table.addTableColumn(column)
        table.headerView = nil
        table.backgroundColor = .clear
        table.rowSizeStyle = .custom
        table.intercellSpacing = NSSize(width: 0, height: 0)
        table.selectionHighlightStyle = .regular
        table.columnAutoresizingStyle = .uniformColumnAutoresizingStyle
        table.dataSource = self
        table.delegate = self
        table.setAccessibilityLabel("素材列表")
        scroll = NSScrollView()
        scroll.documentView = table
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
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
        caption.preferredMaxLayoutWidth = Self.preferredWidth - 26
        let footerStack = NSStackView(views: [caption, removeButton])
        footerStack.orientation = .vertical
        footerStack.alignment = .leading
        footerStack.spacing = 6
        footer = footerStack

        // Content keeps its natural width inside a clipped outer view anchored to
        // the trailing edge, so the column can fold to a rail and its collapse
        // button (at the trailing edge) stays visible.
        let inner = NSView()
        inner.translatesAutoresizingMaskIntoConstraints = false
        addSubview(inner)
        for view in [titleLabel, importButton, collapseButton, tabs, scroll!, footerStack] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            inner.addSubview(view)
        }
        wantsLayer = true
        layer?.masksToBounds = true
        widthConstraint = widthAnchor.constraint(equalToConstant: Self.preferredWidth)
        NSLayoutConstraint.activate([
            inner.trailingAnchor.constraint(equalTo: trailingAnchor),
            inner.topAnchor.constraint(equalTo: topAnchor),
            inner.bottomAnchor.constraint(equalTo: bottomAnchor),
            inner.widthAnchor.constraint(equalToConstant: Self.preferredWidth),
            collapseButton.trailingAnchor.constraint(equalTo: inner.trailingAnchor, constant: -5),
            collapseButton.topAnchor.constraint(equalTo: inner.topAnchor, constant: 12),
            importButton.trailingAnchor.constraint(equalTo: collapseButton.leadingAnchor, constant: -6),
            importButton.centerYAnchor.constraint(equalTo: collapseButton.centerYAnchor),
            titleLabel.leadingAnchor.constraint(equalTo: inner.leadingAnchor, constant: 14),
            titleLabel.centerYAnchor.constraint(equalTo: collapseButton.centerYAnchor),
            tabs.leadingAnchor.constraint(equalTo: inner.leadingAnchor, constant: 12),
            tabs.trailingAnchor.constraint(equalTo: inner.trailingAnchor, constant: -12),
            tabs.topAnchor.constraint(equalTo: collapseButton.bottomAnchor, constant: 10),
            scroll.leadingAnchor.constraint(equalTo: inner.leadingAnchor, constant: 4),
            scroll.trailingAnchor.constraint(equalTo: inner.trailingAnchor, constant: -4),
            scroll.topAnchor.constraint(equalTo: tabs.bottomAnchor, constant: 8),
            footerStack.leadingAnchor.constraint(equalTo: inner.leadingAnchor, constant: 14),
            footerStack.trailingAnchor.constraint(equalTo: inner.trailingAnchor, constant: -12),
            footerStack.topAnchor.constraint(equalTo: scroll.bottomAnchor, constant: 8),
            footerStack.bottomAnchor.constraint(equalTo: inner.bottomAnchor, constant: -10),
            widthConstraint,
        ])
        applyCollapsedAppearance()
    }

    /// Folds the sidebar to a thin rail (or opens it again).
    func setCollapsed(_ collapsed: Bool, animated: Bool = true) {
        guard collapsed != isCollapsed else { return }
        isCollapsed = collapsed
        applyCollapsedAppearance()
        let width = collapsed ? Self.railWidth : Self.preferredWidth
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

    private func applyCollapsedAppearance() {
        for view in [titleLabel, importButton, tabs, scroll!, footer!] as [NSView] { view.isHidden = isCollapsed }
        collapseButton.image = NSImage(systemSymbolName: isCollapsed ? "chevron.left" : "chevron.right",
                                       accessibilityDescription: "素材栏")
        collapseButton.toolTip = isCollapsed ? "展开素材栏" : "收起素材栏"
        collapseButton.setAccessibilityLabel(collapseButton.toolTip)
    }

    @objc private func collapseTapped() {
        setCollapsed(!isCollapsed, animated: true)
        onCollapsedChange?(isCollapsed)
    }

    // MARK: Data

    private func rebuildRows() {
        switch tab {
        case .articles:
            rows = TypingTestArticles.all.map(Row.article)
        case .books:
            if let book = openBook {
                rows = [.back(book)]
                for (index, chapter) in book.chapters.enumerated() {
                    for passage in chapter.passages.indices {
                        guard let article = book.article(chapter: index, passage: passage) else { continue }
                        let parts = chapter.passages.count
                        rows.append(.piece(article, label: parts > 1 ? "\(chapter.title) · \(passage + 1)/\(parts)" : chapter.title))
                    }
                }
            } else if library.books.isEmpty {
                rows = [.hint("还没有书。点右上角的 + 导入 EPUB，每一章会成为一份带编号的素材。")]
            } else {
                rows = library.books.map(Row.book)
            }
        }
    }

    func reload() {
        if let open = openBook, library.book(id: open.id) == nil { openBook = nil }
        tabs.setLabel(library.books.isEmpty ? "书籍" : "书籍 · \(library.books.count)", forSegment: 1)
        tabs.selectedSegment = tab.rawValue
        rebuildRows()
        suppressSelection = true
        table.reloadData()
        if let id = currentArticleID, let row = rows.firstIndex(where: { $0.article?.id == id }) {
            table.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
        }
        suppressSelection = false
        updateCaption()
        updateRemoveButton()
    }

    /// Shows the tab and book holding an article and highlights it, without notifying `onSelect`.
    func select(articleID: String) {
        currentArticleID = articleID
        let pieces = articleID.split(separator: ".")
        if pieces.count == 3, pieces[0] == "epub", let book = library.book(id: String(pieces[1])) {
            tab = .books
            openBook = book
        } else {
            tab = .articles
        }
        reload()
        if let row = rows.firstIndex(where: { $0.article?.id == articleID }) { table.scrollRowToVisible(row) }
    }

    /// The opened book, for the remove button.
    var selectedBook: TypingTestBook? { tab == .books ? openBook : nil }

    /// First piece of a book, used after an import.
    func selectFirstPassage(of book: TypingTestBook) {
        guard let article = book.article(number: 1) else { return }
        statusOverride = nil
        select(articleID: article.id)
        onSelect?(article)
    }

    func setStatus(_ text: String?) {
        statusOverride = text
        updateCaption()
    }

    private func updateCaption() {
        if let statusOverride { caption.stringValue = statusOverride; return }
        switch tab {
        case .articles: caption.stringValue = "内置文章 \(TypingTestArticles.all.count) 篇。"
        case .books:
            caption.stringValue = library.books.isEmpty ? "书籍只保存在本机，不会上传。"
                : openBook == nil ? "已导入 \(library.books.count) 本 · 点书打开章节。" : "每章一份素材，过长的章节按段编号拆开。"
        }
    }
    private func updateRemoveButton() { removeButton.isHidden = selectedBook == nil; removeButton.isEnabled = isEditingEnabled }

    @objc private func importTapped() { onImport?() }
    @objc private func removeTapped() { if let book = selectedBook { onRemoveBook?(book) } }
    @objc private func tabChanged() {
        tab = Tab(rawValue: tabs.selectedSegment) ?? .articles
        statusOverride = nil
        reload()
    }

    // MARK: Table

    func numberOfRows(in tableView: NSTableView) -> Int { rows.count }

    func tableView(_ tableView: NSTableView, heightOfRow row: Int) -> CGFloat {
        switch rows[row] {
        case .article: return 58
        case .book: return 54
        case .back: return 32
        case .piece: return 30
        case .hint: return 76
        }
    }

    func tableView(_ tableView: NSTableView, rowViewForRow row: Int) -> NSTableRowView? {
        switch rows[row] {
        case let .article(article):
            let view = MaterialRowView(); view.isCard = true; view.isSelectableCard = true
            _ = article; return view
        case .book:
            let view = MaterialRowView(); view.isCard = true; view.isSelectableCard = false
            return view
        default: return nil
        }
    }

    func tableView(_ tableView: NSTableView, shouldSelectRow row: Int) -> Bool {
        switch rows[row] {
        case .article, .piece: return true
        case let .book(book):
            openBook = book
            statusOverride = nil
            reload()
            return false
        case .back:
            openBook = nil
            statusOverride = nil
            reload()
            return false
        case .hint: return false
        }
    }

    func tableViewSelectionDidChange(_ notification: Notification) {
        guard !suppressSelection, table.selectedRow >= 0, rows.indices.contains(table.selectedRow),
              let article = rows[table.selectedRow].article else { return }
        currentArticleID = article.id
        statusOverride = nil
        updateCaption()
        onSelect?(article)
    }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        if case let .hint(text) = rows[row] {
            let label = NSTextField(wrappingLabelWithString: text)
            label.font = .systemFont(ofSize: 12)
            label.textColor = RimeUI.textSecondary
            label.preferredMaxLayoutWidth = Self.preferredWidth - 40
            let holder = NSTableCellView()
            label.translatesAutoresizingMaskIntoConstraints = false
            holder.addSubview(label)
            NSLayoutConstraint.activate([
                label.leadingAnchor.constraint(equalTo: holder.leadingAnchor, constant: 12),
                label.trailingAnchor.constraint(equalTo: holder.trailingAnchor, constant: -12),
                label.topAnchor.constraint(equalTo: holder.topAnchor, constant: 8),
            ])
            return holder
        }
        let identifier = NSUserInterfaceItemIdentifier("material.cell")
        let cell = (tableView.makeView(withIdentifier: identifier, owner: self) as? MaterialCell) ?? {
            let made = MaterialCell(); made.identifier = identifier; return made
        }()
        switch rows[row] {
        case let .article(article):
            cell.configure(title: article.title,
                           detail: "\(article.theme) · \(article.difficulty) · \(article.displayLength)",
                           leading: 14, style: .card)
        case let .book(book):
            cell.configure(title: book.title,
                           detail: "\(book.chapters.count) 章 · \(book.passageCount) 份 · \(book.author.isEmpty ? "佚名" : book.author)",
                           leading: 14, style: .card)
        case let .back(book):
            cell.configure(title: "‹ 返回书架", detail: "\(book.title)", leading: 12, style: .row)
        case let .piece(article, label):
            cell.configure(title: label, detail: article.displayLength, leading: 12, style: .row)
        case .hint: break
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

    var smokeRowCount: Int { rows.count }
    var smokeWidth: CGFloat { frame.width }
    var smokeTab: Int { tab.rawValue }
    var smokeOpenBookTitle: String? { openBook?.title }
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
