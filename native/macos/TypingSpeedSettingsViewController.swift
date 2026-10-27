import AppKit
import Carbon
import CryptoKit
import Foundation
import UniformTypeIdentifiers

/// Owns one local article editor. Persisted results contain metrics, not text.
final class TypingSpeedSettingsViewController: NSViewController, TypingPracticeTelemetrySink {
    private let isHistory: Bool
    private let historyStore: TypingTestHistoryStore
    private let library: TypingTestLibraryStore
    private let contextProvider: (() -> TypingTestContext)?
    private let clockProvider: () -> TimeInterval
    private let telemetryEnabled: Bool
    private var observers: [NSObjectProtocol] = []
    private var workspaceObservers: [NSObjectProtocol] = []
    private var sourceObserver: NSObjectProtocol?
    private var timer: Timer?
    private var session: TypingTestSession?
    private var latestSnapshot: TypingTestSnapshot?
    private var isArmed = false
    private var isStopping = false
    private var isImporting = false
    private var selectedArticle = TypingTestArticles.defaultArticle
    private var frozenSourceID: String?
    private static let lastArticleKey = "TypingTest.lastArticleID"
    private lazy var materials = TypingTestMaterialColumn(library: library)
    private weak var pageScroll: TypingTestPageScrollView?
    private var materialsDivider = TypingTestDividerView()
    private let materialsButton = RimePointingHandButton(title: "", target: nil, action: nil)
    private let contextPicker = NSPopUpButton(frame: .zero, pullsDown: false)
    private let startButton = RimePointingHandButton(title: "开始跟打", target: nil, action: nil)
    private let finishButton = RimePointingHandButton(title: "结束", target: nil, action: nil)
    private let articleTitle = NSTextField(labelWithString: "")
    private let statusLabel = NSTextField(labelWithString: "")
    private let articleDetail = NSTextField(labelWithString: "")
    private let progressLabel = NSTextField(labelWithString: "0%")
    private let timeLabel = NSTextField(labelWithString: "00:00")
    private let progress = NSProgressIndicator()
    private let editor = TypingTestTrackTextView(frame: .zero, textContainer: nil)
    private let gridButton = RimePointingHandButton(title: "", target: nil, action: nil)
    private let speedCard = TypingTestStatTile(title: "有效速度", symbolName: "speedometer")
    private let keysCard = TypingTestStatTile(title: "击键", symbolName: "keyboard")
    private let backspaceCard = TypingTestStatTile(title: "回退", symbolName: "delete.left")
    private let accuracyCard = TypingTestStatTile(title: "正确率", symbolName: "scope")
    private let speedChart = MetricsLineChartView()
    private let resultStack = NSStackView()
    private let historyRows = NSStackView()
    private let historySummary = NSTextField(labelWithString: "")
    private let resultDetail = NSTextField(wrappingLabelWithString: "")
    private let storageWarning = NSTextField(wrappingLabelWithString: "")
    private let clearButton = RimePointingHandButton(title: "清空成绩…", target: nil, action: nil)
    private let repairButton = RimePointingHandButton(title: "备份并重建…", target: nil, action: nil)
    private var renderedTargetStates: [TypingTestCharacterState]?
    private var renderedTypedCorrectness: [Bool]?
    private static let gridHiddenKey = "TypingTest.gridHidden"
    private let cellsButton = RimePointingHandButton(title: "", target: nil, action: nil)
    private var cellViews: [NSView] = []
    private var resultVisible = false
    private var cellsHidden = false
    private static let materialsCollapsedKey = "TypingTest.materialsCollapsed"
    private static let cellsHiddenKey = "TypingTest.cellsHidden"

    init(subpageID: String, historyStore: TypingTestHistoryStore = .shared,
         library: TypingTestLibraryStore = .shared,
         contextProvider: (() -> TypingTestContext)? = nil,
         clockProvider: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime },
         telemetryEnabled: Bool = true) {
        isHistory = subpageID == "history"
        self.historyStore = historyStore
        self.library = library
        self.contextProvider = contextProvider
        self.clockProvider = clockProvider
        self.telemetryEnabled = telemetryEnabled
        super.init(nibName: nil, bundle: nil)
    }
    required init?(coder: NSCoder) { nil }
    deinit {
        timer?.invalidate()
        if telemetryEnabled { TypingPracticeTelemetry.shared.deactivate(textView: editor) }
        observers.forEach(NotificationCenter.default.removeObserver)
        workspaceObservers.forEach(NSWorkspace.shared.notificationCenter.removeObserver)
        if let sourceObserver { DistributedNotificationCenter.default().removeObserver(sourceObserver) }
    }

    override func loadView() {
        configureControls()
        let right = NSStackView()
        right.orientation = .vertical
        right.alignment = .leading
        right.distribution = .fill
        right.spacing = 10
        right.edgeInsets = NSEdgeInsets(top: 14, left: 18, bottom: 14, right: 18)
        right.translatesAutoresizingMaskIntoConstraints = false
        if isHistory { buildHistory(in: right) } else { buildPractice(in: right) }
        storageWarning.font = .systemFont(ofSize: 11)
        storageWarning.textColor = RimeUI.warningTextColor
        storageWarning.isHidden = true
        add(storageWarning, to: right)

        // The main area is the only thing that scrolls. The material sidebar sits
        // beside it at window height, so it stays put however long the text is.
        let root = TypingTestDocumentView()
        root.onDetach = { [weak self] in self?.interrupt(reason: .interrupted, message: "已离开跟打页 · 本次记为练习") }
        let document = TypingTestFlippedView()
        document.translatesAutoresizingMaskIntoConstraints = false
        document.addSubview(right)
        NSLayoutConstraint.activate([
            right.leadingAnchor.constraint(equalTo: document.leadingAnchor),
            right.trailingAnchor.constraint(equalTo: document.trailingAnchor),
            right.topAnchor.constraint(equalTo: document.topAnchor),
            right.bottomAnchor.constraint(equalTo: document.bottomAnchor),
        ])
        // The page scroll's minimum height stays low: the content (the full text of
        // the piece) decides how tall the document really is.
        let page = TypingTestPageScrollView(document: document, minimumHeight: 0, minimumWidth: 400)
        materialsDivider = TypingTestDividerView()
        for subview in [page, materialsDivider, materials] {
            subview.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(subview)
        }
        NSLayoutConstraint.activate([
            page.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            page.topAnchor.constraint(equalTo: root.topAnchor),
            page.bottomAnchor.constraint(equalTo: root.bottomAnchor),
            materialsDivider.leadingAnchor.constraint(equalTo: page.trailingAnchor),
            materialsDivider.topAnchor.constraint(equalTo: root.topAnchor),
            materialsDivider.bottomAnchor.constraint(equalTo: root.bottomAnchor),
            materialsDivider.widthAnchor.constraint(equalToConstant: 1),
            materials.leadingAnchor.constraint(equalTo: materialsDivider.trailingAnchor),
            materials.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            materials.topAnchor.constraint(equalTo: root.topAnchor),
            materials.bottomAnchor.constraint(equalTo: root.bottomAnchor),
        ])
        materials.onSelect = { [weak self] in self?.choose($0) }
        materials.onCollapsedChange = { [weak self] collapsed in
            guard let self else { return }
            if self.telemetryEnabled { UserDefaults.standard.set(collapsed, forKey: Self.materialsCollapsedKey) }
            self.applyMaterialsVisibility()
        }
        materials.onImport = { [weak self] in self?.importTapped() }
        materials.onDropFiles = { [weak self] in self?.importFiles($0) }
        materials.onRemoveBook = { [weak self] in self?.removeBookTapped($0) }

        pageScroll = page
        view = root
        view.identifier = NSUserInterfaceItemIdentifier(isHistory ? "typing-test-history" : "typing-test-practice")
    }

    /// Adds a full-width row to the right-hand column, honouring its insets.
    private func add(_ row: NSView, to stack: NSStackView, flexible: Bool = false) {
        stack.addArrangedSubview(row)
        row.leadingAnchor.constraint(equalTo: stack.leadingAnchor, constant: stack.edgeInsets.left).isActive = true
        row.trailingAnchor.constraint(equalTo: stack.trailingAnchor, constant: -stack.edgeInsets.right).isActive = true
        row.setContentHuggingPriority(flexible ? .init(200) : .init(760), for: .vertical)
        row.setContentCompressionResistancePriority(flexible ? .init(200) : .init(760), for: .vertical)
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        observe(.typingTestLibraryDidChange) { [weak self] _ in self?.materials.reload() }
        observe(.typingTestHistoryDidChange) { [weak self] _ in
            guard let self, self.isHistory else { return }
            self.refreshHistory(rebuildContexts: true)
        }
        for name in [Notification.Name.inputConfigurationDidChange, .chordExtensionDidChange,
                     .chordKeymapDidChange, .chordDurationDidChange] {
            observe(name) { [weak self] _ in self?.interrupt(reason: .configurationChanged, message: "方案已改变 · 本次记为练习") }
        }
        for name in [NSWindow.didResignKeyNotification, NSWindow.willCloseNotification] {
            observe(name) { [weak self] notification in
                guard let self, let window = notification.object as? NSWindow,
                      window === self.view.window else { return }
                self.interrupt(reason: .focusLost, message: "窗口已失焦 · 本次记为练习")
            }
        }
        for name in [NSWorkspace.willSleepNotification, NSWorkspace.sessionDidResignActiveNotification] {
            workspaceObservers.append(NSWorkspace.shared.notificationCenter.addObserver(
                forName: name, object: nil, queue: .main
            ) { [weak self] _ in self?.interrupt(reason: .interrupted, message: "会话已暂停 · 本次记为练习") })
        }
        sourceObserver = DistributedNotificationCenter.default().addObserver(
            forName: Notification.Name(kTISNotifySelectedKeyboardInputSourceChanged as String),
            object: nil, queue: .main
        ) { [weak self] _ in
            guard let self, self.isArmed,
                  RimeInputSourceAuthority.currentInputSourceID() != self.frozenSourceID else { return }
            self.interrupt(reason: .inputSourceChanged, message: "输入源已改变 · 本次记为练习")
        }
        if telemetryEnabled, let id = UserDefaults.standard.string(forKey: Self.lastArticleKey),
           let remembered = article(id: id) { selectedArticle = remembered }
        if telemetryEnabled {
            cellsHidden = UserDefaults.standard.bool(forKey: Self.cellsHiddenKey)
            editor.showsCells = !UserDefaults.standard.bool(forKey: Self.gridHiddenKey)
            setMaterialsCollapsed(UserDefaults.standard.bool(forKey: Self.materialsCollapsedKey), animated: false)
        } else {
            setMaterialsCollapsed(false, animated: false)
        }
        applyCellsVisibility()
        applyGridButton()
        materials.select(articleID: selectedArticle.id)
        if isHistory { refreshHistory(rebuildContexts: true) } else { resetArticle() }
    }

    override func viewWillDisappear() {
        interrupt(reason: .interrupted, message: "已离开跟打页 · 本次记为练习")
        super.viewWillDisappear()
    }
    private func observe(_ name: Notification.Name, action: @escaping (Notification) -> Void) {
        observers.append(NotificationCenter.default.addObserver(forName: name, object: nil,
                                                                 queue: .main, using: action))
    }

    private func configureControls() {
        contextPicker.target = self
        contextPicker.action = #selector(contextChanged)
        contextPicker.setAccessibilityLabel("筛选输入方案与文章版本")
        startButton.target = self
        startButton.action = #selector(startTapped)
        startButton.bezelStyle = .rounded
        finishButton.target = self
        finishButton.action = #selector(finishTapped)
        finishButton.bezelStyle = .rounded
        finishButton.refusesFirstResponder = true
        startButton.refusesFirstResponder = true
        finishButton.isEnabled = false
        clearButton.target = self
        clearButton.action = #selector(clearHistoryTapped)
        clearButton.bezelStyle = .rounded
        repairButton.target = self
        repairButton.action = #selector(repairHistoryTapped)
        repairButton.bezelStyle = .rounded
        repairButton.isHidden = true
        gridButton.target = self
        gridButton.action = #selector(toggleGrid)
        gridButton.bezelStyle = .rounded
        gridButton.controlSize = .small
        gridButton.imagePosition = .imageOnly
        gridButton.refusesFirstResponder = true
        materialsButton.target = self
        materialsButton.action = #selector(showMaterials)
        materialsButton.bezelStyle = .rounded
        materialsButton.controlSize = .small
        materialsButton.imagePosition = .imageOnly
        materialsButton.refusesFirstResponder = true
        materialsButton.image = NSImage(systemSymbolName: "sidebar.right", accessibilityDescription: "素材栏")
        materialsButton.toolTip = "展开素材栏"
        materialsButton.setAccessibilityLabel("展开素材栏")
        cellsButton.target = self
        cellsButton.action = #selector(toggleCells)
        cellsButton.bezelStyle = .rounded
        cellsButton.controlSize = .small
        cellsButton.imagePosition = .imageOnly
        cellsButton.refusesFirstResponder = true
        articleTitle.font = .systemFont(ofSize: 15, weight: .semibold)
        articleTitle.textColor = RimeUI.textPrimary
        articleTitle.lineBreakMode = .byTruncatingTail
        articleTitle.setContentCompressionResistancePriority(.init(100), for: .horizontal)
        articleTitle.setContentHuggingPriority(.init(600), for: .horizontal)
        articleDetail.font = .systemFont(ofSize: 11)
        articleDetail.textColor = RimeUI.textSecondary
        articleDetail.lineBreakMode = .byTruncatingTail
        articleDetail.setContentCompressionResistancePriority(.init(100), for: .horizontal)
        articleDetail.setContentHuggingPriority(.init(600), for: .horizontal)
        statusLabel.font = .systemFont(ofSize: 11, weight: .medium)
        statusLabel.textColor = RimeUI.textSecondary
        statusLabel.lineBreakMode = .byTruncatingTail
        statusLabel.setContentCompressionResistancePriority(.init(100), for: .horizontal)
        statusLabel.setContentHuggingPriority(.init(600), for: .horizontal)
        timeLabel.font = .monospacedDigitSystemFont(ofSize: 15, weight: .medium)
        progressLabel.font = .monospacedDigitSystemFont(ofSize: 11, weight: .medium)
        progress.isIndeterminate = false
        progress.minValue = 0
        progress.maxValue = 1
        progress.style = .bar
        progress.controlSize = .small
        editor.isEditable = false
        editor.onPhysicalKey = { [weak self] event in
            guard let self else { return }
            TypingPracticeTelemetry.shared.noteLocalKey(event, textView: self.editor)
        }
        editor.onCompositionStarted = { [weak self] in
            guard let self, self.isArmed else { return }
            self.session?.start(at: self.now)
        }
        editor.onCommittedText = { [weak self] text, insertion in self?.committedTextChanged(text, insertion: insertion) }
        editor.onWillResignFocus = { [weak self] in self?.session?.markPractice(reason: .focusLost) }
        editor.onFocusLost = { [weak self] in
            self?.interrupt(reason: .focusLost, message: "输入区已失焦 · 本次记为练习")
        }
        editor.onAssistedInput = { [weak self] in
            self?.session?.markPractice(reason: .assistedInput)
            self?.statusLabel.stringValue = "已阻止粘贴或辅助输入 · 本次记为练习"
        }
        [resultStack, historyRows].forEach {
            $0.orientation = .vertical
            $0.alignment = .leading
            $0.spacing = 8
        }
        resultDetail.font = .systemFont(ofSize: 11)
        resultDetail.textColor = RimeUI.textSecondary
        resultDetail.maximumNumberOfLines = 3
        speedChart.emptyMessage = "开始输入后，速度曲线会在这里生长"
        speedChart.xAxisLabel = isHistory ? "测次" : "用时"
        speedChart.translatesAutoresizingMaskIntoConstraints = false
    }

    private func buildPractice(in content: NSStackView) {
        let random = iconButton("shuffle", help: "在当前书籍或内置文章里随机换一段", action: #selector(randomTapped))
        let titles = TypingTestTitleBlock(title: articleTitle, detail: articleDetail)
        add(headerRow(titles, trailing: [random, startButton, finishButton, cellsButton, materialsButton]), to: content)
        // The four readings and the speed curve are cards of one band.
        speedChart.toolTip =
            "速度轨迹 · 计时含思考与纠错 · 标点逐字对照 · 仅保存成绩，不保存输入正文。"
            + "中文有效速度 = 正确字符 ÷ 总分钟；英文 WPM = 正确字符 ÷ 5 ÷ 总分钟。"
            + "过程正确率保留改正前的错误，最终字准单独展示。"
        speedChart.setAccessibilityLabel("速度轨迹")
        let readings = NSStackView(views: [metricRow([speedCard, keysCard]), metricRow([backspaceCard, accuracyCard])])
        readings.orientation = .vertical
        readings.alignment = .leading
        readings.distribution = .fillEqually
        readings.spacing = 8
        for pair in readings.arrangedSubviews { pair.widthAnchor.constraint(equalTo: readings.widthAnchor).isActive = true }
        let strip = row([readings, speedChart])
        strip.distribution = .fillEqually
        strip.spacing = 8
        strip.heightAnchor.constraint(equalToConstant: TypingTestStatTile.height * 2 + 8).isActive = true
        speedChart.heightAnchor.constraint(equalTo: strip.heightAnchor).isActive = true
        add(strip, to: content)
        resultStack.isHidden = true
        add(resultStack, to: content)
        progress.translatesAutoresizingMaskIntoConstraints = false
        progress.widthAnchor.constraint(equalToConstant: 110).isActive = true
        add(row([statusLabel, flexible(), progress, progressLabel, timeLabel]), to: content)
        // One card, double track: each source row over the row being typed.
        add(trackPanel(), to: content, flexible: true)
        cellViews = [strip]
    }
    private func buildHistory(in content: NSStackView) {
        let titles = TypingTestTitleBlock(title: articleTitle, detail: articleDetail)
        add(headerRow(titles, trailing: [repairButton, clearButton, cellsButton, materialsButton]), to: content)
        add(contextPicker, to: content)
        let strip = metricRow([speedCard, keysCard, backspaceCard, accuracyCard])
        add(strip, to: content)
        cellViews = [strip]
        historySummary.font = .systemFont(ofSize: 12, weight: .medium)
        add(historySummary, to: content)
        speedChart.emptyMessage = "这篇文章还没有成绩，去完成第一次跟打吧"
        speedChart.onSelectSample = { [weak self] id in self?.selectHistoryResult(id: id) }
        speedChart.toolTip = "只比较同篇、同版、同方案；非正式练习不连入成绩曲线。正文不进入历史。"
        speedChart.heightAnchor.constraint(equalToConstant: 140).isActive = true
        add(speedChart, to: content)
        resultStack.isHidden = true
        add(resultStack, to: content)
        // The list scrolls on its own so a long history never stretches the page.
        let list = NSScrollView()
        list.drawsBackground = false
        list.hasVerticalScroller = true
        list.autohidesScrollers = true
        list.borderType = .noBorder
        let holder = TypingTestFlippedView()
        holder.translatesAutoresizingMaskIntoConstraints = false
        historyRows.translatesAutoresizingMaskIntoConstraints = false
        holder.addSubview(historyRows)
        list.documentView = holder
        NSLayoutConstraint.activate([
            holder.leadingAnchor.constraint(equalTo: list.contentView.leadingAnchor),
            holder.trailingAnchor.constraint(equalTo: list.contentView.trailingAnchor),
            holder.topAnchor.constraint(equalTo: list.contentView.topAnchor),
            historyRows.leadingAnchor.constraint(equalTo: holder.leadingAnchor),
            historyRows.trailingAnchor.constraint(equalTo: holder.trailingAnchor),
            historyRows.topAnchor.constraint(equalTo: holder.topAnchor),
            historyRows.bottomAnchor.constraint(equalTo: holder.bottomAnchor),
            list.heightAnchor.constraint(greaterThanOrEqualToConstant: 90),
        ])
        add(list, to: content, flexible: true)
    }
    private var now: TimeInterval { clockProvider() }

    private func article(id: String) -> TypingTestArticle? {
        TypingTestArticles.article(id: id) ?? library.article(id: id)
    }

    /// A different material was chosen in the left column (or by shuffle/import).
    private func choose(_ article: TypingTestArticle) {
        guard article.id != selectedArticle.id || session != nil else { return }
        interrupt(reason: .interrupted, message: "已切换素材")
        selectedArticle = article
        if telemetryEnabled { UserDefaults.standard.set(article.id, forKey: Self.lastArticleKey) }
        if isHistory { refreshHistory(rebuildContexts: true) } else { resetArticle() }
    }
    @objc private func randomTapped() {
        let pick: TypingTestArticle?
        let pieces = selectedArticle.id.split(separator: ".")
        if pieces.count == 3, pieces[0] == "epub", let book = library.book(id: String(pieces[1])), book.passageCount > 1 {
            pick = (1...book.passageCount).filter { "epub.\(book.id).\($0)" != selectedArticle.id }.randomElement()
                .flatMap { book.article(number: $0) }
        } else {
            pick = TypingTestArticles.all.filter { $0.id != selectedArticle.id }.randomElement()
        }
        guard let pick else { return }
        materials.select(articleID: pick.id)
        choose(pick)
    }
    @objc private func contextChanged() { refreshHistory(rebuildContexts: false) }

    // MARK: EPUB import

    private func importTapped() {
        guard !isArmed, !isImporting else { return }
        let panel = NSOpenPanel()
        panel.title = "导入 EPUB 电子书"
        panel.message = "选择一本或多本 .epub 电子书；它们会被切分成可跟打的段落，只保存在本机。"
        panel.prompt = "导入"
        panel.allowedContentTypes = UTType(filenameExtension: "epub").map { [$0] } ?? []
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = false
        let finish: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            if response == .OK { self?.importFiles(panel.urls) }
        }
        if let window = view.window { panel.beginSheetModal(for: window, completionHandler: finish) }
        else { finish(panel.runModal()) }
    }

    func importFiles(_ urls: [URL]) {
        let urls = urls.filter { $0.pathExtension.lowercased() == "epub" }
        guard !isArmed, !isImporting, !urls.isEmpty else { return }
        isImporting = true
        materials.isEditingEnabled = false
        materials.setStatus("正在导入 \(urls.count) 本…")
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let parsed = urls.map { url in Result { try TypingTestLibraryStore.parseFile(at: url) } }
            DispatchQueue.main.async { self?.finishImport(urls: urls, parsed: parsed) }
        }
    }

    private func finishImport(urls: [URL], parsed: [Result<TypingTestBook, Error>]) {
        var added: [TypingTestBook] = []
        var existing: TypingTestBook?
        var problems: [String] = []
        for (url, result) in zip(urls, parsed) {
            do {
                switch try library.add(result.get()) {
                case let .added(book), let .replaced(book): added.append(book)
                case let .alreadyPresent(book): existing = book
                }
            } catch { problems.append("\(url.lastPathComponent)：\(error.localizedDescription)") }
        }
        isImporting = false
        materials.isEditingEnabled = !isArmed
        materials.reload()
        if let book = added.first ?? existing { materials.selectFirstPassage(of: book) }
        var message = added.map { "已导入《\($0.title)》· \($0.passageCount) 段" }
        if existing != nil, added.isEmpty { message.append("这本书已在素材里") }
        message += problems
        materials.setStatus(message.joined(separator: "\n"))
        if !isHistory, !isArmed { statusLabel.stringValue = message.first ?? statusLabel.stringValue }
    }

    private func removeBookTapped(_ book: TypingTestBook) {
        confirm(title: "移除《\(book.title)》？",
                detail: "只从本机素材里删除这本书的段落。已保存的跟打成绩会保留，但这些段落的原文不再可选。",
                button: "移除") { [weak self] in
            guard let self, self.library.remove(id: book.id) else { return }
            if self.selectedArticle.id.hasPrefix("epub.\(book.id).") {
                self.materials.reload()
                self.materials.select(articleID: TypingTestArticles.defaultArticle.id)
                self.choose(TypingTestArticles.defaultArticle)
            }
            self.materials.setStatus(nil)
        }
    }

    private func resetArticle() {
        timer?.invalidate(); timer = nil
        session = nil; latestSnapshot = nil; isArmed = false
        editor.acceptsTestInput = false
        editor.isEditable = false
        editor.resetForTest()
        articleTitle.stringValue = selectedArticle.title
        articleDetail.stringValue = "\(selectedArticle.theme)  ·  \(selectedArticle.difficulty)  ·  \(selectedArticle.displayLength)"
        statusLabel.stringValue = "准备好后开始，第一枚按键触发计时"
        setResultHeight(visible: false)
        startButton.title = "开始跟打"
        finishButton.isEnabled = false
        resultStack.isHidden = true
        speedChart.samples = []
        speedChart.unit = selectedArticle.language == .english ? "WPM" : "字/分"
        timeLabel.stringValue = "00:00"
        progress.doubleValue = 0
        progressLabel.stringValue = "0%"
        updateMetricCards(nil)
        renderedTargetStates = nil
        renderedTypedCorrectness = nil
        renderTarget(states: [])
        refreshStorageWarning()
    }

    @objc private func startTapped() {
        if isArmed { interrupt(reason: .interrupted, message: "已重新开始") }
        resetArticle()
        let context = contextProvider?() ?? currentContext()
        frozenSourceID = context.inputSourceID
        session = TypingTestSession(article: selectedArticle, context: context)
        isArmed = true
        editor.isEditable = true
        editor.acceptsTestInput = true
        view.window?.makeFirstResponder(editor)
        if telemetryEnabled { TypingPracticeTelemetry.shared.activate(textView: editor, sink: self) }
        startButton.title = "重新开始"
        finishButton.isEnabled = true
        statusLabel.stringValue = context.mode == .practice ? "重练 · 从第一个字开始" : "首次挑战 · 从第一个字开始"
        let timer = Timer(timeInterval: 0.25, repeats: true) { [weak self] _ in self?.refreshLive() }
        self.timer = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    private func currentContext() -> TypingTestContext {
        let sourceID = RimeInputSourceAuthority.currentInputSourceID()
        let own = sourceID.map { RimeInputSourceAuthority.isOwnInputSourceID($0) } ?? false
        let schema = own ? InputConfigurationStore.shared.selectedSchemaID : (sourceID ?? "unknown")
        let isChord = own && ChordExtensionStore.isChordSchema(schema)
        let keymap = isChord ? ChordKeymapStore.shared.activeProfile : nil
        let revision = keymap.flatMap { profile -> String? in
            guard let data = try? ChordKeymapStore.shared.exportData(profile) else { return nil }
            return SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
                + String(format: ":%.3f", ChordSettings.duration)
        }
        let prior = historyStore.results.contains {
            $0.articleID == selectedArticle.id && $0.articleVersion == selectedArticle.version
                && $0.context.schemaID == schema && $0.context.keymapRevision == revision
                && $0.context.inputSourceID == sourceID
        }
        return TypingTestContext(schemaID: schema, keymapID: keymap?.id,
                                       mode: prior ? .practice : .firstAttempt,
                                       chordCountingAvailable: isChord, inputSourceID: sourceID,
                                       keymapRevision: revision)
    }

    func practiceKey(_ event: NSEvent, isComposing: Bool) {
        guard isArmed, let session else { return }
        session.recordKey(at: now, isRepeat: event.isARepeat,
                          isBackspace: event.keyCode == 51,
                          isComposing: isComposing)
        refreshLive()
    }
    func practiceChord(schemaID: String) {
        guard isArmed, schemaID == session?.context.schemaID else { return }
        session?.recordChord(at: now)
        refreshLive()
    }
    func practiceBackspaceBecameComposition() {
        guard isArmed else { return }
        session?.reclassifyBackspaceAsComposition()
        refreshLive()
    }
    private func committedTextChanged(_ text: String, insertion: Range<Int>?) {
        guard isArmed, let session else { return }
        guard session.reconcileCommittedText(text, at: now, explicitInsertionRange: insertion) else {
            interrupt(reason: .assistedInput, message: "输入超出测速范围 · 本次记为练习")
            return
        }
        refreshLive()
        if latestSnapshot?.canComplete == true { completeSession() }
    }
    private func refreshLive() {
        guard isArmed, let session else { return }
        let snapshot = session.snapshot(at: now)
        latestSnapshot = snapshot
        render(snapshot)
    }
    private func render(_ snapshot: TypingTestSnapshot) {
        let metrics = snapshot.metrics
        updateMetricCards(metrics)
        timeLabel.stringValue = Self.duration(metrics.elapsedSeconds)
        progress.doubleValue = metrics.progress
        progressLabel.stringValue = String(format: "%.0f%%", metrics.progress * 100)
        renderTarget(states: snapshot.targetStates, typed: snapshot.typedCorrectness)
        speedChart.samples = metrics.speedSamples.enumerated().map { index, sample in
            MetricsChartSample(id: String(index), label: Self.duration(sample.elapsedSeconds),
                               value: selectedArticle.language == .english ? sample.effectiveCPM / 5 : sample.effectiveCPM,
                               position: sample.elapsedSeconds)
        }
    }
    @objc private func finishTapped() {
        if editor.hasMarkedText() {
            statusLabel.stringValue = "请先确认或取消当前组字，再结束"
            view.window?.makeFirstResponder(editor)
            return
        }
        completeSession()
    }
    private func completeSession() {
        guard isArmed, let session else { return }
        finish(session: session, message: nil)
    }
    private func interrupt(reason: TypingTestPracticeReason, message: String) {
        guard isArmed, !isStopping, let session else { return }
        session.markPractice(reason: reason)
        finish(session: session, message: message)
    }
    private func finish(session: TypingTestSession, message: String?) {
        guard !isStopping else { return }
        if contextProvider == nil, RimeInputSourceAuthority.currentInputSourceID() != frozenSourceID {
            session.markPractice(reason: .inputSourceChanged)
        }
        isStopping = true; isArmed = false
        if telemetryEnabled { TypingPracticeTelemetry.shared.deactivate(textView: editor) }
        timer?.invalidate(); timer = nil
        editor.acceptsTestInput = false
        editor.isEditable = false
        finishButton.isEnabled = false
        startButton.title = "同篇重练"
        if let result = session.finish(at: now) {
            latestSnapshot = session.snapshot(at: now)
            if let snapshot = latestSnapshot { render(snapshot) }
            let saved = historyStore.add(result)
            renderResult(result)
            statusLabel.stringValue = message ?? (saved ? (result.isComplete ? "本次完成 · 成绩已保存" : "提前结束 · 已保存为练习") : "本次结束 · 成绩未能保存")
        } else {
            session.cancel()
            statusLabel.stringValue = message ?? "尚未输入，不产生空成绩"
        }
        refreshStorageWarning()
        isStopping = false
    }

    private func updateMetricCards(_ metrics: TypingTestMetrics?) {
        let english = selectedArticle.language == .english
        speedCard.update(value: metrics.map { Self.number(english ? $0.wordsPerMinute : $0.effectiveCPM) } ?? "—",
                         unit: english ? "WPM" : "字/分", detail: "正确成文字符 ÷ 总分钟；英文每 5 字符折算一词。")
        keysCard.update(value: metrics.map { String($0.physicalKeyCount) } ?? "—",
                        unit: "键",
                        detail: metrics.map { "\(Self.number($0.keysPerSecond)) 键/秒；自动重复 \($0.repeatKeyCount) 次" } ?? "物理按键与并击拍数分开计算")
        backspaceCard.update(value: metrics.map { String($0.backspaceCount) } ?? "—", unit: "次",
                             detail: metrics.map { "组字退格 \($0.compositionBackspaceCount) · 上屏退格 \($0.committedBackspaceCount) · 删除 \($0.deletedCharacterCount) 字符" } ?? "组字退格与上屏回改分开记录")
        accuracyCard.update(value: metrics?.processAccuracy.map { Self.number($0 * 100) } ?? "—", unit: "%",
                            detail: "正确提交尝试 ÷ 全部提交尝试；改正不会抹去先前错误，未上屏组字不判错。")
    }
    private func renderResult(_ result: TypingTestResult) {
        clear(resultStack)
        setResultHeight(visible: true)
        let metrics = result.metrics
        let final = TypingTestStatTile(title: "最终字准", symbolName: "checkmark.circle")
        final.update(value: metrics.finalAccuracy.map { Self.number($0 * 100) } ?? "—", unit: "%",
                     detail: "正确字符 ÷ (正确 + 替换错误 + 漏字 + 多字)")
        let correction = TypingTestStatTile(title: "回改", symbolName: "arrow.uturn.backward")
        correction.update(value: String(metrics.correctionCount), unit: "次", detail: "实际删除 \(metrics.deletedCharacterCount) 字符")
        let chord = TypingTestStatTile(title: "并击拍数", symbolName: "waveform.path")
        chord.update(value: metrics.chordCount.map(String.init) ?? "—", unit: "拍", detail: "仅记录可信 RIMES 并击事件；缺少数据不当作零。")
        let cards = metricRow([final, correction, chord])
        resultStack.addArrangedSubview(cards)
        cards.widthAnchor.constraint(equalTo: resultStack.widthAnchor).isActive = true
        resultDetail.stringValue = "错 \(metrics.substitutionCount) · 漏 \(metrics.omissionCount) · 多 \(metrics.extraCharacterCount)　｜　\(result.context.schemaID)　｜　\(result.context.mode == .firstAttempt ? "首次" : "重练")\(result.practiceReasons.isEmpty ? "" : " · 非正式练习")"
        resultDetail.toolTip = "文章版本 \(result.articleVersion)；组字退格 \(metrics.compositionBackspaceCount)，上屏退格 \(metrics.committedBackspaceCount)；码长 \(metrics.codeLength.map(Self.number) ?? "—") 键/字。"
        resultStack.addArrangedSubview(resultDetail)
        resultDetail.widthAnchor.constraint(equalTo: resultStack.widthAnchor).isActive = true
    }

    /// The result row adds height; raise the page minimum instead of squeezing the editor.
    private func setResultHeight(visible: Bool) {
        resultVisible = visible
        applyCellsVisibility()
    }

    /// Stat cells (and the chart) can be folded away to give the text more room.
    private func applyCellsVisibility() {
        cellViews.forEach { $0.isHidden = cellsHidden }
        if !resultVisible || cellsHidden { resultStack.isHidden = true } else { resultStack.isHidden = false }
        // The history page keeps its own scrolling list and wants some height;
        // the practice page is exactly as tall as its content.
        pageScroll?.setMinimumHeight(isHistory ? (cellsHidden ? 494 : 560) + (resultVisible && !cellsHidden ? 84 : 0) : 0)
        let symbol = cellsHidden ? "rectangle.grid.2x2" : "rectangle.grid.2x2.fill"
        cellsButton.image = NSImage(systemSymbolName: symbol, accessibilityDescription: "数据格")
        cellsButton.toolTip = cellsHidden ? "显示数据格" : "隐藏数据格，给文字留出更多空间"
        cellsButton.setAccessibilityLabel(cellsButton.toolTip)
    }

    @objc private func toggleCells() {
        cellsHidden.toggle()
        if telemetryEnabled { UserDefaults.standard.set(cellsHidden, forKey: Self.cellsHiddenKey) }
        applyCellsVisibility()
    }

    private func setMaterialsCollapsed(_ collapsed: Bool, animated: Bool) {
        materials.setCollapsed(collapsed, animated: animated)
        applyMaterialsVisibility()
    }

    /// With the sidebar hidden nothing of it remains; the header offers the way back.
    private func applyMaterialsVisibility() {
        materialsDivider.isHidden = materials.isCollapsed
        materialsButton.isHidden = !materials.isCollapsed
    }

    @objc private func showMaterials() {
        setMaterialsCollapsed(false, animated: true)
        if telemetryEnabled { UserDefaults.standard.set(false, forKey: Self.materialsCollapsedKey) }
    }

    private func renderTarget(states: [TypingTestCharacterState], typed: [Bool] = []) {
        guard renderedTargetStates != states || renderedTypedCorrectness != typed else { return }
        renderedTargetStates = states
        renderedTypedCorrectness = typed
        editor.setSource(selectedArticle.text, states: states, typedCorrectness: typed)
    }

    private func refreshHistory(rebuildContexts: Bool) {
        let articleResults = historyStore.results.filter { $0.articleID == selectedArticle.id }
            .sorted { $0.completedAt > $1.completedAt }
        if rebuildContexts {
            let selected = contextPicker.selectedItem?.representedObject as? String
            contextPicker.removeAllItems()
            var seen = Set<String>()
            for result in articleResults where seen.insert(Self.comparisonKey(result)).inserted {
                let revision = result.context.keymapRevision.map { " · \($0.prefix(6))" } ?? ""
                contextPicker.addItem(withTitle: "\(result.context.schemaID) · 文章 v\(result.articleVersion)\(revision)")
                contextPicker.lastItem?.representedObject = Self.comparisonKey(result)
                contextPicker.lastItem?.toolTip = "\(result.context.inputSourceID ?? "未知输入源") · \(result.context.keymapID ?? "普通键位") · \(result.context.keymapRevision ?? "")"
            }
            if contextPicker.numberOfItems == 0 { contextPicker.addItem(withTitle: "暂无输入方案记录") }
            if let selected, let item = contextPicker.itemArray.first(where: { $0.representedObject as? String == selected }) {
                contextPicker.select(item)
            }
        }
        let key = contextPicker.selectedItem?.representedObject as? String
        let results = articleResults.filter { Self.comparisonKey($0) == key }
        articleTitle.stringValue = selectedArticle.title
        articleDetail.stringValue = "\(selectedArticle.theme) · \(selectedArticle.displayLength) · \(results.count) 次记录"
        updateMetricCards(results.first?.metrics)
        resultStack.isHidden = true
        setResultHeight(visible: false)
        historySummary.stringValue = results.isEmpty ? "等待你的第一次成绩" : "最近成绩 · \(results.filter(\.isComplete).count) 次完成"
        let visible = Array(results.prefix(30))
        speedChart.unit = selectedArticle.language == .english ? "WPM" : "字/分"
        speedChart.samples = visible.reversed().map { result in
            MetricsChartSample(id: result.id.uuidString, label: Self.shortDate(result.completedAt),
                               value: result.isComparable ? result.displaySpeed : nil,
                               detail: "\(result.context.mode == .firstAttempt ? "首次" : "重练")\(result.practiceReasons.isEmpty ? "" : " · 练习")")
        }
        clear(historyRows)
        for result in visible {
            let icon = NSImageView()
            icon.image = NSImage(systemSymbolName: result.isComparable ? "checkmark.circle.fill" : "pause.circle", accessibilityDescription: nil)
            icon.contentTintColor = result.isComparable ? RimeUI.accentGreen : RimeUI.warningTextColor
            let speed = label(Self.number(result.displaySpeed), size: 22, weight: .semibold)
            speed.font = .monospacedDigitSystemFont(ofSize: 22, weight: .semibold)
            let caption = label("\(Self.shortDate(result.completedAt))  ·  \(result.context.mode == .firstAttempt ? "首次" : "重练")\(result.practiceReasons.isEmpty ? "" : " · 练习")", size: 11)
            caption.textColor = RimeUI.textSecondary
            let accuracy = label("\(result.metrics.processAccuracy.map { Self.number($0 * 100) } ?? "—")%", size: 13, weight: .medium)
            accuracy.toolTip = "过程正确率；最终字准 \(result.metrics.finalAccuracy.map { Self.number($0 * 100) } ?? "—")%"
            let detail = row([icon, speed, label(speedChart.unit, size: 10), flexible(), caption, accuracy])
            detail.edgeInsets = NSEdgeInsets(top: 9, left: 12, bottom: 9, right: 12)
            let box = roundedBox(detail)
            box.identifier = NSUserInterfaceItemIdentifier(result.id.uuidString)
            box.addGestureRecognizer(NSClickGestureRecognizer(target: self, action: #selector(historyRowClicked(_:))))
            box.toolTip = "查看这次成绩的最终字准、回改与并击数据"
            historyRows.addArrangedSubview(box)
            box.widthAnchor.constraint(equalTo: historyRows.widthAnchor).isActive = true
        }
        if results.isEmpty {
            let empty = label("完成一次跟打后，这里会显示速度趋势与成绩。", size: 12)
            empty.textColor = RimeUI.textSecondary
            historyRows.addArrangedSubview(empty)
        }
        refreshStorageWarning()
    }
    @objc private func historyRowClicked(_ gesture: NSClickGestureRecognizer) {
        guard let id = gesture.view?.identifier?.rawValue else { return }
        selectHistoryResult(id: id)
    }
    private func selectHistoryResult(id: String) {
        guard let result = historyStore.results.first(where: { $0.id.uuidString == id }),
              result.articleID == selectedArticle.id else { return }
        speedChart.selectedSampleID = id
        updateMetricCards(result.metrics)
        historySummary.stringValue = "\(Self.shortDate(result.completedAt)) · \(result.context.mode == .firstAttempt ? "首次挑战" : "同篇重练")"
        renderResult(result)
    }
    private static func comparisonKey(_ result: TypingTestResult) -> String {
        "\(result.articleVersion)|\(result.language.rawValue)|\(result.context.schemaID)|\(result.context.keymapID ?? "")|\(result.context.keymapVersion.map(String.init) ?? "")|\(result.context.keymapRevision ?? "")|\(result.context.inputSourceID ?? "")"
    }
    private func refreshStorageWarning() {
        if let issue = historyStore.storageIssue {
            storageWarning.stringValue = "成绩存储暂不可用，原文件不会被覆盖。\(issue)"
            storageWarning.isHidden = false
            repairButton.isHidden = false
            clearButton.isEnabled = false
        } else {
            storageWarning.isHidden = true
            repairButton.isHidden = true
            clearButton.isEnabled = !historyStore.results.isEmpty
        }
    }

    @objc private func clearHistoryTapped() {
        confirm(title: "清空文章跟打成绩？", detail: "只删除本机文章跟打的成绩记录，无法撤销。日常统计与原来的输入速度历史不会改变。", button: "清空成绩") { [weak self] in
            guard let self else { return }
            _ = self.historyStore.clearAll()
            self.refreshHistory(rebuildContexts: true)
        }
    }
    @objc private func repairHistoryTapped() {
        confirm(title: "备份并重建跟打成绩库？", detail: "无法读取的原路径条目会原样移到同目录的备份，再创建空成绩库；不会读取符号链接目标，也不影响日常统计。", button: "备份并重建") { [weak self] in
            guard let self else { return }
            _ = self.historyStore.repairReadOnlyStore()
            self.refreshHistory(rebuildContexts: true)
        }
    }
    private func confirm(title: String, detail: String, button: String, action: @escaping () -> Void) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = title
        alert.informativeText = detail
        alert.addButton(withTitle: button)
        alert.addButton(withTitle: "取消")
        if let window = view.window {
            alert.beginSheetModal(for: window) { if $0 == .alertFirstButtonReturn { action() } }
        } else {
            alert.window.appearance = RimeUI.appKitAppearance
            if alert.runModal() == .alertFirstButtonReturn { action() }
        }
    }

    /// One card holding the double track: source row above, typing row below.
    private func trackPanel() -> NSView {
        let title = label("跟打", size: 11, weight: .semibold)
        let hint = label("上行原文 · 下行输入 · 首键计时 · 组字不判错", size: 10.5)
        hint.textColor = RimeUI.textMuted
        hint.setContentCompressionResistancePriority(.init(100), for: .horizontal)
        // No scroll view of its own: the card is as tall as the whole text and the
        // page scrolls, so every row can be reached with the ordinary scroll gesture.
        editor.translatesAutoresizingMaskIntoConstraints = false
        editor.setContentHuggingPriority(.init(250), for: .vertical)
        editor.setContentCompressionResistancePriority(.required, for: .vertical)
        gridButton.translatesAutoresizingMaskIntoConstraints = false
        let card = TypingTestSurfaceView()
        for subview in [title, hint, gridButton, editor] {
            subview.translatesAutoresizingMaskIntoConstraints = false
            card.addSubview(subview)
        }
        NSLayoutConstraint.activate([
            title.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: 14),
            title.centerYAnchor.constraint(equalTo: gridButton.centerYAnchor),
            gridButton.topAnchor.constraint(equalTo: card.topAnchor, constant: 7),
            gridButton.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -10),
            hint.leadingAnchor.constraint(greaterThanOrEqualTo: title.trailingAnchor, constant: 10),
            hint.trailingAnchor.constraint(equalTo: gridButton.leadingAnchor, constant: -10),
            hint.centerYAnchor.constraint(equalTo: gridButton.centerYAnchor),
            editor.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: 2),
            editor.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -2),
            editor.topAnchor.constraint(equalTo: gridButton.bottomAnchor, constant: 5),
            editor.bottomAnchor.constraint(equalTo: card.bottomAnchor, constant: -6),
        ])
        return card
    }

    @objc private func toggleGrid() {
        editor.showsCells.toggle()
        if telemetryEnabled { UserDefaults.standard.set(!editor.showsCells, forKey: Self.gridHiddenKey) }
        applyGridButton()
    }

    private func applyGridButton() {
        gridButton.image = NSImage(systemSymbolName: editor.showsCells ? "squareshape.split.3x3" : "square.dashed",
                                   accessibilityDescription: "格子")
        gridButton.toolTip = editor.showsCells ? "隐藏格子" : "显示格子"
        gridButton.setAccessibilityLabel(gridButton.toolTip)
    }

    private func roundedBox(_ content: NSView) -> NSView {
        let box = TypingTestSurfaceView()
        box.translatesAutoresizingMaskIntoConstraints = false
        content.translatesAutoresizingMaskIntoConstraints = false
        box.addSubview(content)
        NSLayoutConstraint.activate([
            content.leadingAnchor.constraint(equalTo: box.leadingAnchor),
            content.trailingAnchor.constraint(equalTo: box.trailingAnchor),
            content.topAnchor.constraint(equalTo: box.topAnchor),
            content.bottomAnchor.constraint(equalTo: box.bottomAnchor),
        ])
        return box
    }
    private func row(_ views: [NSView]) -> NSStackView {
        let stack = NSStackView(views: views)
        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.distribution = .fill
        stack.spacing = 10
        return stack
    }
    /// Title on the left, controls on the right; the title's edge is pinned to the
    /// controls, so it truncates only when it truly has no room left.
    private func headerRow(_ titles: NSView, leading: [NSView] = [], trailing: [NSView]) -> NSView {
        let container = NSView()
        let controls = row(trailing)
        controls.distribution = .gravityAreas
        controls.setContentHuggingPriority(.required, for: .horizontal)
        controls.setContentCompressionResistancePriority(.required, for: .horizontal)
        controls.translatesAutoresizingMaskIntoConstraints = false
        titles.translatesAutoresizingMaskIntoConstraints = false
        container.addSubview(titles)
        container.addSubview(controls)
        var titleLeading = container.leadingAnchor
        var titleInset: CGFloat = 0
        if let first = leading.first {
            first.translatesAutoresizingMaskIntoConstraints = false
            container.addSubview(first)
            NSLayoutConstraint.activate([
                first.leadingAnchor.constraint(equalTo: container.leadingAnchor),
                first.centerYAnchor.constraint(equalTo: container.centerYAnchor),
            ])
            titleLeading = first.trailingAnchor
            titleInset = 10
        }
        NSLayoutConstraint.activate([
            titles.leadingAnchor.constraint(equalTo: titleLeading, constant: titleInset),
            titles.topAnchor.constraint(equalTo: container.topAnchor),
            titles.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            controls.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            controls.centerYAnchor.constraint(equalTo: container.centerYAnchor),
            titles.trailingAnchor.constraint(equalTo: controls.leadingAnchor, constant: -12),
        ])
        return container
    }
    private func metricRow(_ views: [NSView]) -> NSStackView {
        let stack = row(views)
        stack.distribution = .fillEqually
        return stack
    }
    private func label(_ text: String, size: CGFloat, weight: NSFont.Weight = .regular) -> NSTextField {
        let field = NSTextField(labelWithString: text)
        field.font = .systemFont(ofSize: size, weight: weight)
        field.textColor = RimeUI.textPrimary
        field.lineBreakMode = .byTruncatingTail
        return field
    }
    private func flexible() -> NSView {
        let spacer = NSView()
        // Below every label's compression resistance, so the gap absorbs slack
        // before any text is truncated.
        spacer.setContentHuggingPriority(.init(50), for: .horizontal)
        return spacer
    }
    private func iconButton(_ symbol: String, help: String, action: Selector) -> NSButton {
        let button = RimePointingHandButton(title: "", target: self, action: action)
        button.image = NSImage(systemSymbolName: symbol, accessibilityDescription: help)
        button.bezelStyle = .rounded
        button.toolTip = help
        return button
    }
    private func clear(_ stack: NSStackView) {
        stack.arrangedSubviews.forEach { stack.removeArrangedSubview($0); $0.removeFromSuperview() }
    }
    private static func number(_ value: Double) -> String {
        guard value.isFinite else { return "—" }
        return String(format: value >= 100 ? "%.0f" : "%.1f", value)
    }
    private static func duration(_ seconds: TimeInterval) -> String {
        let value = max(0, Int(seconds.isFinite ? seconds : 0))
        return String(format: "%02d:%02d", value / 60, value % 60)
    }
    private static func shortDate(_ time: TimeInterval) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "MM.dd HH:mm"
        return formatter.string(from: Date(timeIntervalSince1970: time))
    }
    /// Isolated rendering hooks: no user window, keyboard event, or live store mutation.
    func prepareForSmoke(articleID: String, committedText: String = "") {
        _ = view
        guard let article = article(id: articleID) else { return }
        selectedArticle = article
        materials.select(articleID: articleID)
        if isHistory { refreshHistory(rebuildContexts: true); return }
        resetArticle()
        if !committedText.isEmpty {
            let smoke = TypingTestSession(article: article, context: TypingTestContext(schemaID: "smoke"))
            smoke.start(at: 10)
            for index in 0..<committedText.count { smoke.recordKey(at: 10 + Double(index) * 0.08) }
            _ = smoke.reconcileCommittedText(committedText, at: 20)
            editor.string = committedText
            render(smoke.snapshot(at: 20))
        }
    }
    var smokeMaterialRowCount: Int { materials.smokeRowCount }
    var smokeMaterials: TypingTestMaterialColumn { materials }
    var smokePageScroll: NSScrollView? { pageScroll }
    func smokeToggleCells() { toggleCells() }
    func smokeToggleMaterials() { setMaterialsCollapsed(!materials.isCollapsed, animated: false) }
    var smokeCellsHidden: Bool { cellsHidden }
    var smokeMaterialsButtonShown: Bool { !materialsButton.isHidden }
    var smokeMaterialsDividerShown: Bool { !materialsDivider.isHidden }
    private func smokeFrame(_ target: NSView) -> NSRect { target.convert(target.bounds, to: view) }
    var smokeChartFrame: NSRect? { speedChart.window == nil && speedChart.superview == nil ? nil : smokeFrame(speedChart) }
    var smokeReadingFrame: NSRect? {
        var union: NSRect?
        for card in [speedCard, keysCard, backspaceCard, accuracyCard] {
            let frame = smokeFrame(card)
            union = union.map { $0.union(frame) } ?? frame
        }
        return union
    }
    var smokeTrack: TypingTestTrackTextView { editor }
    var smokeTargetString: String { String(editor.sourceCharacters) }
    var smokeEditor: TypingTestTextView { editor }
    var smokeChart: MetricsLineChartView { speedChart }
    var smokeIsArmed: Bool { isArmed }
    var smokeHasTimer: Bool { timer?.isValid == true }
    var smokeSessionSnapshot: TypingTestSnapshot? { session?.snapshot(at: now) }
    func smokeStartByButton() { startButton.performClick(nil) }
    func smokeFinishByButton() { finishButton.performClick(nil) }
}

private final class TypingTestDocumentView: NSView {
    var onDetach: (() -> Void)?
    private var wasAttached = false
    override var isFlipped: Bool { true }
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil { wasAttached = true }
        else if wasAttached { onDetach?() }
    }
}

private final class TypingTestFlippedView: NSView {
    override var isFlipped: Bool { true }
}

/// The page's only scroll view. The document always fills the viewport and
/// never drops below its minimum, so inner panels flex instead of clipping.
private final class TypingTestPageScrollView: NSScrollView {
    private let minimumHeightConstraint: NSLayoutConstraint

    init(document: NSView, minimumHeight: CGFloat, minimumWidth: CGFloat) {
        minimumHeightConstraint = document.heightAnchor.constraint(greaterThanOrEqualToConstant: minimumHeight)
        super.init(frame: .zero)
        drawsBackground = false
        hasVerticalScroller = true
        hasHorizontalScroller = true
        autohidesScrollers = true
        borderType = .noBorder
        document.translatesAutoresizingMaskIntoConstraints = false
        documentView = document
        // At least as tall as the viewport, never pulling the viewport taller: the
        // content (the whole text) may exceed it, and then the page scrolls.
        let fillHeight = document.heightAnchor.constraint(greaterThanOrEqualTo: contentView.heightAnchor)
        let fillWidth = document.widthAnchor.constraint(equalTo: contentView.widthAnchor)
        fillWidth.priority = .defaultHigh
        NSLayoutConstraint.activate([
            document.leadingAnchor.constraint(equalTo: contentView.leadingAnchor),
            document.topAnchor.constraint(equalTo: contentView.topAnchor),
            fillHeight, fillWidth, minimumHeightConstraint,
            document.widthAnchor.constraint(greaterThanOrEqualToConstant: minimumWidth),
        ])
    }
    required init?(coder: NSCoder) { nil }

    func setMinimumHeight(_ value: CGFloat) { minimumHeightConstraint.constant = value }
}

private class TypingTestSurfaceView: NSView {
    var cornerRadius: CGFloat { 12 }
    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        let path = NSBezierPath(roundedRect: bounds.insetBy(dx: 0.5, dy: 0.5), xRadius: cornerRadius, yRadius: cornerRadius)
        RimeUI.surface.setFill(); path.fill()
        RimeUI.border.setStroke(); path.lineWidth = 1; path.stroke()
    }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        needsDisplay = true
    }
}

private final class TypingTestDividerView: NSView {
    override func draw(_ dirtyRect: NSRect) {
        RimeUI.border.setFill()
        bounds.fill()
    }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        needsDisplay = true
    }
}

/// Compact measurement used by both pages: a caption over a value and unit.
final class TypingTestStatTile: NSView {
    static let height: CGFloat = 56
    private let surface = TypingTestStatSurface()
    private let caption = NSTextField(labelWithString: "")
    private let valueLabel = NSTextField(labelWithString: "—")
    private let unitLabel = NSTextField(labelWithString: "")
    private let title: String

    init(title: String, symbolName: String) {
        self.title = title
        super.init(frame: .zero)
        translatesAutoresizingMaskIntoConstraints = false
        let icon = NSImageView()
        icon.image = RimeUI.symbol(symbolName, pointSize: 11, weight: .medium)
        icon.contentTintColor = RimeUI.accentTextColor
        caption.stringValue = title
        caption.font = .systemFont(ofSize: 10.5, weight: .medium)
        caption.textColor = RimeUI.textSecondary
        caption.lineBreakMode = .byTruncatingTail
        caption.setContentCompressionResistancePriority(.init(100), for: .horizontal)
        valueLabel.font = .monospacedDigitSystemFont(ofSize: 20, weight: .semibold)
        valueLabel.textColor = RimeUI.textPrimary
        valueLabel.lineBreakMode = .byTruncatingTail
        valueLabel.setContentCompressionResistancePriority(.init(200), for: .horizontal)
        unitLabel.font = .systemFont(ofSize: 10)
        unitLabel.textColor = RimeUI.textMuted
        unitLabel.lineBreakMode = .byTruncatingTail
        unitLabel.setContentCompressionResistancePriority(.init(100), for: .horizontal)
        let heading = NSStackView(views: [icon, caption])
        heading.orientation = .horizontal; heading.spacing = 4; heading.alignment = .centerY
        let reading = NSStackView(views: [valueLabel, unitLabel])
        reading.orientation = .horizontal; reading.spacing = 4; reading.alignment = .firstBaseline
        let stack = NSStackView(views: [heading, reading])
        stack.orientation = .vertical; stack.spacing = 3; stack.alignment = .leading
        for subview in [surface, stack] { subview.translatesAutoresizingMaskIntoConstraints = false; addSubview(subview) }
        NSLayoutConstraint.activate([
            surface.leadingAnchor.constraint(equalTo: leadingAnchor), surface.trailingAnchor.constraint(equalTo: trailingAnchor),
            surface.topAnchor.constraint(equalTo: topAnchor), surface.bottomAnchor.constraint(equalTo: bottomAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 9),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -5),
            stack.centerYAnchor.constraint(equalTo: centerYAnchor),
            heightAnchor.constraint(equalToConstant: Self.height),
        ])
        setAccessibilityElement(true)
        setAccessibilityRole(.staticText)
        update(value: "—")
    }
    required init?(coder: NSCoder) { nil }

    func update(value: String, unit: String = "", detail: String = "") {
        valueLabel.stringValue = value
        unitLabel.stringValue = unit
        toolTip = detail.isEmpty ? nil : detail
        setAccessibilityLabel("\(title == "正确率" ? "过程正确率" : title) \(value) \(unit)")
    }
}

private final class TypingTestStatSurface: TypingTestSurfaceView {
    override var cornerRadius: CGFloat { 8 }
}

/// Title over detail. It has no intrinsic width: the row hands it all the room
/// left of the buttons and both lines truncate only when that runs out.
private final class TypingTestTitleBlock: NSView {
    init(title: NSTextField, detail: NSTextField) {
        super.init(frame: .zero)
        translatesAutoresizingMaskIntoConstraints = false
        for field in [title, detail] {
            field.translatesAutoresizingMaskIntoConstraints = false
            field.lineBreakMode = .byTruncatingTail
            field.setContentCompressionResistancePriority(.init(100), for: .horizontal)
            field.setContentHuggingPriority(.init(50), for: .horizontal)
            addSubview(field)
            field.leadingAnchor.constraint(equalTo: leadingAnchor).isActive = true
            field.trailingAnchor.constraint(equalTo: trailingAnchor).isActive = true
        }
        NSLayoutConstraint.activate([
            title.topAnchor.constraint(equalTo: topAnchor),
            detail.topAnchor.constraint(equalTo: title.bottomAnchor, constant: 2),
            detail.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
        setContentCompressionResistancePriority(.init(100), for: .horizontal)
        setContentHuggingPriority(.init(50), for: .horizontal)
    }
    required init?(coder: NSCoder) { nil }
}
