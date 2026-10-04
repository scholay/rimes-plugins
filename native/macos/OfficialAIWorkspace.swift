import AppKit
import Foundation

enum AITextWorkspacePhase: Equatable {
    case unavailable(String)
    case idle
    case running
    case ready
    case failed(String)
}

struct AITextWorkspaceOutputBlock: Equatable {
    let id: UUID
    let index: Int
    let text: String
    let title: String?
    let incomplete: Bool
}

/// A two-rail processor workspace. Source remains exclusively in BufferModel;
/// generated blocks live here and become sendable only after final validation.
final class AITextPluginWorkspace: BufferDeliveryContentSource {
    struct Job: Equatable {
        let generation: UInt64
        let requestID: UUID
        let sourceText: String
        let sourceBlockIDs: [UUID]
        let format: AITextContentFormat
    }

    var kind: AITextProviderKind { provider.kind }
    let pluginKey: PluginKey
    private let provider: any AITextProvider
    private let sourceModel: BufferModel
    private let selectionPredicate: () -> Bool
    private let generationSelectionResolver:
        (AITextProviderKind) throws -> AITextGenerationSelection
    private let workspaceIdentifier: String
    private var observers: [NSObjectProtocol] = []
    private var started = false
    private var protectedSession = false
    private var activeJob: Job?
    private var currentTask: (any AITextCancellable)?
    private var activityTimer: Timer?
    private var activityStartedAt: TimeInterval?
    private var activityMessage: String?
    private var transientThought: String?
    private var hasDetailedThought = false
    private var stableIDs: [SemanticBlockKey: UUID] = [:]
    private var streamingLogicalBlocks: [Int: AITextProviderBlock] = [:]
    private var capturedSourceText = ""
    private var capturedSourceBlockIDs: [UUID] = []
    private var outputAllowsRemoteMirror = true
    private(set) var generation: UInt64 = 0
    private(set) var phase: AITextWorkspacePhase = .idle
    private(set) var outputBlocks: [AITextWorkspaceOutputBlock] = []

    init(provider: any AITextProvider,
         sourceModel: BufferModel = .shared,
         pluginKey: PluginKey? = nil,
         generationSelectionResolver: @escaping
            (AITextProviderKind) throws -> AITextGenerationSelection = {
                try AITextGenerationPreferenceStore.shared.channelSelection(
                    connectorKind: $0
                )
            },
         isSelected: @escaping () -> Bool) {
        let resolvedPluginKey = pluginKey ?? provider.kind.pluginKey
        self.pluginKey = resolvedPluginKey
        self.provider = provider
        self.sourceModel = sourceModel
        self.generationSelectionResolver = generationSelectionResolver
        selectionPredicate = isSelected
        workspaceIdentifier = "ai-text-\(provider.kind.rawValue)"
    }

    var isSelected: Bool { selectionPredicate() }

    var isActive: Bool {
        started && isSelected && sourceModel.processingActive && !protectedSession
    }

    var sourceText: String { sourceModel.stagedText }

    var canGenerate: Bool {
        if selectedSkill == .imagegen,
           CodexImageGenerationCoordinator.shared.isRunning { return false }
        guard isActive,
              !sourceText.isEmpty,
              sourceText.utf8.count <= AITextRuntimeLimits.maximumSourceBytes,
              AITextSourcePolicy.accepts(sourceModel.blocks),
              provider.availability == .ready else { return false }
        return phase != .running
    }

    var statusText: String {
        if selectedSkill == .imagegen {
            switch CodexImageGenerationCoordinator.shared.phase {
            case .idle: break
            case .connecting: return "正在提交图片任务"
            case .generating: return "图片生成中；可以关闭 Buffer"
            case .saving: return "正在保存图片到 Mailbox"
            case .ready: return "图片已存入 Mailbox"
            case let .failed(message): return message
            }
        }
        switch phase {
        case let .unavailable(message), let .failed(message): return message
        case .idle:
            if sourceText.isEmpty { return "等待内容" }
            if !AITextSourcePolicy.accepts(sourceModel.blocks) { return "请先审阅插件内容" }
            return "可以生成"
        case .running: return activityDisplayText ?? "正在生成"
        case .ready: return "生成内容可发送"
        }
    }

    /// Reuses the current workbench's source-over-target rail view contract.
    var railSnapshot: TranslationRailSnapshot {
        if selectedSkill == .imagegen { return imageRailSnapshot }
        let railPhase: TranslationRailSnapshot.Phase
        let message: String?
        switch phase {
        case let .unavailable(value):
            railPhase = .unavailable
            message = value
        case .idle:
            railPhase = .idle
            message = nil
        case .running:
            railPhase = .translating
            message = activityDisplayText
        case .ready:
            railPhase = .ready
            message = nil
        case let .failed(value):
            railPhase = .failed
            message = value
        }
        return TranslationRailSnapshot(
            sourceText: sourceText,
            sourceSelected: sourceModel.allContentSelected,
            outputBlocks: outputBlocks.map { TranslationOutputBlock(id: $0.id, text: $0.text) },
            phase: railPhase,
            message: message,
            transientThought: phase == .running && outputBlocks.isEmpty
                ? transientThought : nil,
            targetRole: "答",
            targetEmptyText: "等待生成",
            waitingText: "等待生成",
            processingText: "正在生成",
            updatingText: "更新内容"
        )
    }

    private var selectedSkill: AITextSkillKind? {
        AITextSkillSelectionStore.shared.selected(for: kind)
    }

    private var imageRailSnapshot: TranslationRailSnapshot {
        let state = CodexImageGenerationCoordinator.shared.phase
        let railPhase: TranslationRailSnapshot.Phase
        let message: String?
        var blocks: [TranslationOutputBlock] = []
        var rows: [TranslationOutputRow] = []
        switch state {
        case .idle:
            if case let .failed(error) = phase {
                railPhase = .failed
                message = error
            } else {
                railPhase = .idle
                message = nil
            }
        case .connecting:
            railPhase = .translating
            message = "正在向 ChatGPT 提交图片任务 · \(CodexImageGenerationCoordinator.shared.elapsedSeconds) 秒"
        case .generating:
            railPhase = .translating
            message = "任务已提交 · 生成中 \(CodexImageGenerationCoordinator.shared.elapsedSeconds) 秒。可以关闭 Buffer；完成后 Mailbox 会通知你。"
        case .saving:
            railPhase = .translating
            message = "图片已生成，正在保存到 Mailbox…"
        case let .ready(threadID):
            railPhase = .ready
            message = nil
            blocks = [TranslationOutputBlock(
                id: threadID, text: "图片已生成，点击右侧按钮到 Mailbox 查看。"
            )]
            rows = [TranslationOutputRow(
                key: 0, blocks: blocks, title: "生成结果",
                action: .openMailbox(threadID)
            )]
        case let .failed(error):
            railPhase = .failed
            message = error
        }
        return TranslationRailSnapshot(
            sourceText: sourceText,
            sourceSelected: sourceModel.allContentSelected,
            outputBlocks: blocks,
            outputRows: rows,
            outputRowsAreIndependent: true,
            phase: railPhase,
            message: message,
            transientThought: railPhase == .translating ? message : nil,
            targetRole: "图",
            targetEmptyText: "等待图片",
            waitingText: "等待生成",
            processingText: "正在生成图片",
            updatingText: "正在更新"
        )
    }

    func start() {
        guard !started else { return }
        started = true
        observers.append(NotificationCenter.default.addObserver(
            forName: .codexImageGenerationDidChange,
            object: CodexImageGenerationCoordinator.shared,
            queue: .main
        ) { [weak self] _ in self?.notifyChange() })
        observers.append(NotificationCenter.default.addObserver(
            forName: .aiTextSkillSelectionDidChange,
            object: AITextSkillSelectionStore.shared,
            queue: .main
        ) { [weak self] _ in
            self?.invalidate(clearOutput: true, nextPhase: .idle)
        })
        observers.append(NotificationCenter.default.addObserver(
            forName: .bufferModelDidChange,
            object: sourceModel,
            queue: .main
        ) { [weak self] _ in
            self?.sourceDidChange()
        })
        observers.append(NotificationCenter.default.addObserver(
            forName: .activeBufferPluginDidChange,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.selectionDidChange()
        })
        // The backend is fixed per plug-in. Only the API plug-in follows the
        // account/model chosen in Connectors, which posts a connector change.
        if kind == .openAICompatible {
            observers.append(NotificationCenter.default.addObserver(
                forName: .aiTextConnectorDidChange,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                self?.configurationDidChange()
            })
        }
        if kind == .codexCLI || kind == .claudeCodeCLI {
            observers.append(NotificationCenter.default.addObserver(
                forName: .pluginConfigurationDidChange,
                object: nil,
                queue: .main
            ) { [weak self] notification in
                guard let self,
                      notification.userInfo?[
                        PluginConfigurationNotificationKey.pluginID
                      ] as? String == self.kind.pluginRawID else { return }
                self.configurationDidChange()
            })
        }
        observers.append(NotificationCenter.default.addObserver(
            forName: .aiTextConnectorAvailabilityDidChange,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.availabilityDidChange()
        })
        observers.append(NotificationCenter.default.addObserver(
            forName: .openAICompatibleConfigurationDidChange,
            object: OpenAICompatibleConfigurationStore.shared,
            queue: .main
        ) { [weak self] _ in
            guard self?.kind == .openAICompatible else { return }
            self?.configurationDidChange()
        })
        observers.append(NotificationCenter.default.addObserver(
            forName: .aiProviderProfilesDidChange,
            object: AIProviderProfileCatalogStore.shared,
            queue: .main
        ) { [weak self] _ in
            guard self?.kind == .openAICompatible else { return }
            self?.configurationDidChange()
        })
        selectionDidChange()
    }

    func stop() {
        guard started else { return }
        started = false
        observers.forEach(NotificationCenter.default.removeObserver)
        observers.removeAll()
        invalidate(clearOutput: true, nextPhase: .idle)
    }

    func selectionDidChange() {
        if !isSelected || protectedSession {
            invalidate(clearOutput: true, nextPhase: .idle)
            return
        }
        refreshAvailability()
        notifyChange()
    }

    /// Settings calls may use this after saving. OpenAI availability/generate
    /// already reload on every access; this only invalidates any old result.
    func configurationDidChange() {
        invalidate(clearOutput: true, nextPhase: .idle)
        refreshAvailability()
        notifyChange()
    }

    /// Runtime availability changes (for example a completed CLI auth probe)
    /// must update controls without deleting a generated, unsent target rail.
    private func availabilityDidChange() {
        guard isSelected, !protectedSession else { return }
        // Availability gates the next generation. It must not lock an already
        // reviewed target rail or interrupt the CLI that is currently running.
        if phase == .running || (phase == .ready && !outputBlocks.isEmpty) {
            return
        }
        refreshAvailability()
        notifyChange()
    }

    func setProtected(_ protected: Bool) {
        guard protectedSession != protected else { return }
        protectedSession = protected
        if protected {
            invalidate(clearOutput: true, nextPhase: .idle)
        } else {
            selectionDidChange()
        }
    }

    @discardableResult
    func generate() -> Bool {
        guard canGenerate else {
            refreshAvailability()
            notifyChange()
            return false
        }
        if selectedSkill == .imagegen {
            do {
                let selection = try generationSelectionResolver(kind)
                _ = try CodexImageGenerationCoordinator.shared.start(
                    prompt: sourceText,
                    modelID: selection.modelID,
                    reasoningEffort: selection.reasoningEffort
                )
                return true
            } catch {
                phase = .failed(error.localizedDescription)
                notifyChange()
                return false
            }
        }
        let plan: AITextGenerationPlan
        do {
            let selection = try generationSelectionResolver(kind)
            // The command router owns Mailbox requests so the task can outlive
            // Buffer. A direct workspace call must never accidentally launch
            // that request on this close-cancelled lifecycle.
            guard selection.destination == .inline else { return false }
            plan = try AITextGenerationPlan.capture(
                sourceModel: sourceModel,
                selection: selection
            )
        } catch {
            phase = .failed(error.localizedDescription)
            notifyChange()
            return false
        }
        cancelCurrentTask()
        generation &+= 1
        let blocks = sourceModel.blocks
        let job = Job(generation: generation,
                      requestID: plan.requestID,
                      sourceText: plan.sourceText,
                      sourceBlockIDs: blocks.map(\.id),
                      format: plan.selection.format)
        activeJob = job
        capturedSourceText = job.sourceText
        capturedSourceBlockIDs = job.sourceBlockIDs
        outputAllowsRemoteMirror = blocks.allSatisfy { $0.origin.allowsRemoteMirror }
        stableIDs.removeAll()
        streamingLogicalBlocks.removeAll()
        outputBlocks.removeAll()
        transientThought = nil
        hasDetailedThought = false
        phase = .running
        activityStartedAt = ProcessInfo.processInfo.systemUptime
        activityMessage = "正在启动 \(plan.selection.connectorKind.displayName)"
        startActivityClock(for: job)
        notifyChange()

        let relay = AITextCancellationRelay()
        currentTask = relay
        let task = provider.generate(
            AITextProviderRequest(
                requestID: plan.requestID,
                sourceText: plan.sourceText,
                preparedPrompt: plan.preparedPrompt,
                modelID: plan.selection.modelID,
                reasoningEffort: plan.selection.reasoningEffort,
                providerRoute: plan.selection.providerRoute
            ),
            onEvent: { [weak self] event in
                self?.performOnMain { workspace in
                    workspace.receive(event, for: job)
                }
            },
            completion: { [weak self] result in
                self?.performOnMain { workspace in
                    workspace.finish(result, for: job)
                }
            }
        )
        relay.install(task)
        return true
    }

    func cancel() {
        invalidate(clearOutput: true, nextPhase: .idle)
    }

    func reset() {
        invalidate(clearOutput: true, nextPhase: .idle)
    }

    @discardableResult
    func resetAndRefresh() -> Bool {
        reset()
        return generate()
    }

    private func receive(_ event: AITextProviderEvent, for job: Job) {
        guard accepts(job) else { return }
        switch event {
        case .imageGenerationStarted, .imageArtifactPath:
            // The image skill is owned by a process-long Mailbox task, never
            // by this close-cancelled text workspace.
            return
        case let .activity(activity):
            let message = Self.normalizedActivityMessage(activity.message)
            var thoughtChanged = false
            if activity.kind == .reasoning,
               !hasDetailedThought,
               outputBlocks.isEmpty,
               !message.isEmpty,
               transientThought != message {
                transientThought = message
                thoughtChanged = true
            }
            if !message.isEmpty, activityMessage != message {
                activityMessage = message
                thoughtChanged = true
            }
            guard thoughtChanged else { return }
            notifyChange()
        case let .reasoningSnapshot(summary):
            guard outputBlocks.isEmpty else { return }
            let visible = String(summary.suffix(4_096))
                .trimmingCharacters(in: .whitespacesAndNewlines)
            guard !visible.isEmpty, transientThought != visible else { return }
            transientThought = visible
            hasDetailedThought = true
            notifyChange()
        case let .blockSnapshot(block):
            guard job.format == .plain else {
                // A partial Markdown document or JSON value is not a usable
                // delivery block. Keep it transient until terminal validation
                // can publish the complete document atomically.
                let message = "\(kind.displayName) 正在流式返回"
                guard activityMessage != message else { return }
                activityMessage = message
                notifyChange()
                return
            }
            guard block.index >= 0,
                  block.index < AITextRuntimeLimits.maximumModelBlockCount,
                  let validated = try? AITextResultDecoder
                    .validateLogicalBlocks([block]).first else {
                return
            }
            streamingLogicalBlocks[validated.index] = validated
            guard let fragments = try? refinedFragments(
                Array(streamingLogicalBlocks.values)
            ) else { return }
            let snapshots = makeOutputBlocks(fragments, incomplete: true)
            guard outputBlocks != snapshots else { return }
            outputBlocks = snapshots
            transientThought = nil
            hasDetailedThought = false
            activityMessage = "\(kind.displayName) 正在流式返回"
            notifyChange()
        }
    }

    private func finish(_ result: Result<[AITextProviderBlock], AITextProviderError>,
                        for job: Job) {
        guard accepts(job) else { return }
        stopActivityClock()
        currentTask = nil
        activeJob = nil
        transientThought = nil
        hasDetailedThought = false
        switch result {
        case let .failure(error):
            outputBlocks.removeAll()
            stableIDs.removeAll()
            streamingLogicalBlocks.removeAll()
            if error == .cancelled {
                phase = .idle
            } else {
                phase = .failed(error.userFacingMessage)
            }
        case let .success(blocks):
            do {
                let fragments = try terminalFragments(
                    blocks,
                    format: job.format
                )
                outputBlocks = makeOutputBlocks(fragments, incomplete: false)
                streamingLogicalBlocks.removeAll()
                phase = .ready
            } catch let error as AITextProviderError {
                outputBlocks.removeAll()
                stableIDs.removeAll()
                streamingLogicalBlocks.removeAll()
                phase = .failed(error.userFacingMessage)
            } catch {
                outputBlocks.removeAll()
                stableIDs.removeAll()
                streamingLogicalBlocks.removeAll()
                phase = .failed(AITextProviderError.invalidResult.userFacingMessage)
            }
        }
        activityStartedAt = nil
        activityMessage = nil
        notifyChange()
    }

    private func sourceDidChange() {
        guard isActive else {
            if activeJob != nil || !outputBlocks.isEmpty || !capturedSourceText.isEmpty {
                invalidate(clearOutput: true, nextPhase: .idle)
            } else {
                notifyChange()
            }
            return
        }
        if activeJob != nil || !capturedSourceText.isEmpty || !outputBlocks.isEmpty {
            guard sourceLeaseMatches() else {
                invalidate(clearOutput: true, nextPhase: .idle)
                return
            }
        }
        notifyChange()
    }

    private func refinedFragments(_ blocks: [AITextProviderBlock]) throws
        -> [SemanticBlockFragment] {
        guard !blocks.isEmpty,
              blocks.count <= AITextRuntimeLimits.maximumModelBlockCount,
              blocks.allSatisfy({
                  $0.index >= 0
                      && $0.index < AITextRuntimeLimits.maximumModelBlockCount
              }) else {
            throw AITextProviderError.invalidResult
        }
        let logical = try AITextResultDecoder.validateLogicalBlocks(blocks)
        let fragments = SemanticBlockSegmenter.refine(
            AITextFineBlockSegmenter.normalizedLogicalBlocks(logical),
            maximumSegments: AITextRuntimeLimits.maximumBlockCount
        )
        let delivery = fragments.enumerated().map { index, fragment in
            AITextProviderBlock(index: index,
                                text: fragment.text,
                                title: fragment.title)
        }
        _ = try AITextResultDecoder.validate(delivery)
        return fragments
    }

    private func terminalFragments(
        _ blocks: [AITextProviderBlock],
        format: AITextContentFormat
    ) throws -> [SemanticBlockFragment] {
        switch format {
        case .plain:
            return try refinedFragments(blocks)
        case .markdown, .json:
            let logical = try AITextResultDecoder.validateLogicalBlocks(blocks)
            let document = logical.sorted(by: { $0.index < $1.index })
                .map(\.text)
                .joined(separator: "\n\n")
            guard !document.trimmingCharacters(
                in: .whitespacesAndNewlines
            ).isEmpty,
                  document.utf8.count
                    <= AITextRuntimeLimits.maximumWireBytes else {
                throw AITextProviderError.resultTooLarge
            }
            if format == .json {
                guard let data = document.data(using: .utf8) else {
                    throw AITextProviderError.invalidResult
                }
                do {
                    _ = try JSONSerialization.jsonObject(
                        with: data,
                        options: [.fragmentsAllowed]
                    )
                } catch {
                    throw AITextProviderError.invalidResult
                }
            }
            return [SemanticBlockFragment(
                key: SemanticBlockKey(sourceIndex: 0, childIndex: 0),
                text: document,
                title: logical.first?.title
            )]
        }
    }

    private func makeOutputBlocks(_ fragments: [SemanticBlockFragment],
                                  incomplete: Bool)
        -> [AITextWorkspaceOutputBlock] {
        fragments.enumerated().map { index, fragment in
            let id = stableIDs[fragment.key] ?? UUID()
            stableIDs[fragment.key] = id
            return AITextWorkspaceOutputBlock(id: id,
                                              index: index,
                                              text: fragment.text,
                                              title: fragment.title,
                                              incomplete: incomplete)
        }
    }

    private func accepts(_ job: Job) -> Bool {
        started
            && !protectedSession
            && isSelected
            && sourceModel.processingActive
            && activeJob == job
            && generation == job.generation
            && sourceModel.stagedText == job.sourceText
            && sourceModel.blocks.map(\.id) == job.sourceBlockIDs
            && AITextSourcePolicy.accepts(sourceModel.blocks)
    }

    private func sourceLeaseMatches() -> Bool {
        sourceModel.stagedText == capturedSourceText
            && sourceModel.blocks.map(\.id) == capturedSourceBlockIDs
    }

    private func cancelCurrentTask() {
        stopActivityClock()
        let task = currentTask
        currentTask = nil
        activeJob = nil
        task?.cancel()
    }

    private func invalidate(clearOutput: Bool,
                            nextPhase: AITextWorkspacePhase) {
        cancelCurrentTask()
        generation &+= 1
        capturedSourceText = ""
        capturedSourceBlockIDs.removeAll()
        activityStartedAt = nil
        activityMessage = nil
        transientThought = nil
        hasDetailedThought = false
        outputAllowsRemoteMirror = true
        if clearOutput {
            outputBlocks.removeAll()
            stableIDs.removeAll()
            streamingLogicalBlocks.removeAll()
        }
        phase = nextPhase
        notifyChange()
    }

    private func refreshAvailability() {
        guard isSelected, !protectedSession else {
            phase = .idle
            return
        }
        switch provider.availability {
        case .ready:
            if case .unavailable = phase {
                phase = outputBlocks.isEmpty ? .idle : .ready
            }
        case let .unavailable(message):
            phase = .unavailable(message)
        }
    }

    private func performOnMain(_ operation: @escaping (AITextPluginWorkspace) -> Void) {
        if Thread.isMainThread {
            operation(self)
        } else {
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                operation(self)
            }
        }
    }

    private var activityDisplayText: String? {
        guard phase == .running,
              let activityStartedAt else { return activityMessage }
        let elapsed = max(0, ProcessInfo.processInfo.systemUptime - activityStartedAt)
        let base = activityMessage ?? "\(kind.displayName) 正在处理"
        return "\(base) · \(Int(elapsed)) 秒"
    }

    private func startActivityClock(for job: Job) {
        stopActivityClock()
        let timer = Timer(timeInterval: 1, repeats: true) { [weak self] timer in
            guard let self else {
                timer.invalidate()
                return
            }
            guard self.accepts(job) else {
                timer.invalidate()
                if self.activityTimer === timer { self.activityTimer = nil }
                return
            }
            self.notifyChange()
        }
        activityTimer = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    private func stopActivityClock() {
        activityTimer?.invalidate()
        activityTimer = nil
    }

    private static func normalizedActivityMessage(_ value: String) -> String {
        let oneLine = value
            .replacingOccurrences(of: "\r", with: " ")
            .replacingOccurrences(of: "\n", with: " ")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return String(oneLine.prefix(120))
    }

    private func notifyChange() {
        NotificationCenter.default.post(name: .aiTextPluginWorkspaceDidChange,
                                        object: self)
        NotificationCenter.default.post(name: .derivedBufferWorkspaceDidChange,
                                        object: self)
    }

    // MARK: BufferDeliveryContentSource

    var deliveryWorkspaceID: String { workspaceIdentifier }
    var deliveryGeneration: UInt64 { generation }

    var hasIncompleteDeliveryBlocks: Bool {
        if selectedSkill == .imagegen { return false }
        return isSelected && phase == .running
    }

    var deliveryPendingBlocks: [BufferModel.Block] {
        if selectedSkill == .imagegen { return [] }
        guard isSelected,
              phase == .ready,
              sourceLeaseMatches() else { return [] }
        return outputBlocks.map { block in
            BufferModel.Block(
                id: block.id,
                text: block.text,
                origin: .processor(id: kind.processorID,
                                   allowsRemoteMirror: outputAllowsRemoteMirror)
            )
        }
    }

    func deliveryBlock(id: UUID, generation: UInt64) -> BufferModel.Block? {
        if selectedSkill == .imagegen { return nil }
        guard self.generation == generation,
              isSelected,
              phase == .ready,
              sourceLeaseMatches(),
              let block = outputBlocks.first(where: { $0.id == id }) else { return nil }
        return BufferModel.Block(
            id: block.id,
            text: block.text,
            origin: .processor(id: kind.processorID,
                               allowsRemoteMirror: outputAllowsRemoteMirror)
        )
    }

    func consumeDelivered(blockIDs: [UUID], generation: UInt64) {
        _ = consumeDeliveredAndReportTerminalDrain(
            blockIDs: blockIDs,
            generation: generation
        )
    }

    func consumeDeliveredAndReportTerminalDrain(
        blockIDs: [UUID],
        generation: UInt64
    ) -> BufferDeliveryTerminalSourceReceipt? {
        guard self.generation == generation,
              !blockIDs.isEmpty else { return nil }
        let ids = Set(blockIDs)
        let consumedIDs = Set(outputBlocks.lazy.filter {
            ids.contains($0.id)
        }.map(\.id))
        guard !consumedIDs.isEmpty else { return nil }
        outputBlocks.removeAll { ids.contains($0.id) }
        self.generation &+= 1
        let terminal = outputBlocks.isEmpty
        if terminal {
            let sourceIDs = capturedSourceBlockIDs
            capturedSourceText = ""
            capturedSourceBlockIDs.removeAll()
            stableIDs.removeAll()
            streamingLogicalBlocks.removeAll()
            phase = .idle
            refreshAvailability()
            sourceModel.consumeDelivered(blockIDs: sourceIDs)
        }
        notifyChange()
        guard terminal else { return nil }
        return BufferDeliveryTerminalSourceReceipt(
            workspaceID: deliveryWorkspaceID,
            generation: generation,
            generationAfterConsumption: self.generation,
            consumedBlockIDs: consumedIDs
        )
    }

    func markDeliveryBlockStale(id: UUID, generation: UInt64) -> Bool {
        false
    }
}

final class AITextPluginRuntimeRegistry {
    static let shared = AITextPluginRuntimeRegistry()

    /// One workspace per AI task plug-in, each bound to its own backend.
    let workspaces: [AITextPluginWorkspace]
    let connectorRegistry: AITextConnectorRegistry
    private let sourceModel: BufferModel

    init(sourceModel: BufferModel = .shared,
         selectionStore: BufferPluginSelectionStore = .shared,
         connectorSelectionStore: AITextConnectorSelectionStore = .shared,
         connectorRegistry: AITextConnectorRegistry? = nil,
         providers: [any AITextProvider]? = nil) {
        self.sourceModel = sourceModel
        let resolvedConnectorRegistry: AITextConnectorRegistry
        if let connectorRegistry {
            resolvedConnectorRegistry = connectorRegistry
        } else if let providers {
            resolvedConnectorRegistry = AITextConnectorRegistry(
                selectionStore: connectorSelectionStore,
                providers: providers
            )
        } else if connectorSelectionStore === AITextConnectorSelectionStore.shared {
            resolvedConnectorRegistry = .shared
        } else {
            resolvedConnectorRegistry = AITextConnectorRegistry(
                selectionStore: connectorSelectionStore
            )
        }
        self.connectorRegistry = resolvedConnectorRegistry
        workspaces = AITextProviderKind.allCases.compactMap { kind in
            guard let provider = resolvedConnectorRegistry.provider(for: kind) else {
                return nil
            }
            let key = kind.pluginKey
            return AITextPluginWorkspace(
                provider: provider,
                sourceModel: sourceModel,
                pluginKey: key,
                isSelected: { selectionStore.isSelected(key) }
            )
        }
    }

    var selectedWorkspace: AITextPluginWorkspace? {
        workspaces.first(where: \.isSelected)
    }

    func workspace(for kind: AITextProviderKind) -> AITextPluginWorkspace? {
        workspaces.first { $0.kind == kind }
    }

    func workspace(for key: PluginKey) -> AITextPluginWorkspace? {
        guard let kind = AITextProviderKind.channelPluginKind(for: key) else { return nil }
        return workspace(for: kind)
    }

    func startAll() { workspaces.forEach { $0.start() } }
    func stopAll() { workspaces.forEach { $0.stop() } }
    func setProtected(_ protected: Bool) {
        workspaces.forEach { $0.setProtected(protected) }
    }

    func currentDeliverySource() -> any BufferDeliveryContentSource {
        selectedWorkspace ?? sourceModel
    }
}

/// Narrow facade used by the workbench UI and delivery router. It deliberately
/// exposes only the currently selected workspace, preserving plugin mutual
/// exclusion in one place.
enum AITextWorkspaceRouter {
    static var selectedWorkspace: AITextPluginWorkspace? {
        AITextPluginRuntimeRegistry.shared.selectedWorkspace
    }

    static var railSnapshot: TranslationRailSnapshot? {
        selectedWorkspace?.railSnapshot
    }

    static var statusText: String? { selectedWorkspace?.statusText }
    static var canGenerate: Bool { selectedWorkspace?.canGenerate ?? false }
    static var isSelected: Bool { selectedWorkspace != nil }
    static var isActive: Bool { selectedWorkspace?.isActive ?? false }

    @discardableResult
    static func generate() -> Bool { selectedWorkspace?.generate() ?? false }

    @discardableResult
    static func resetAndRefresh() -> Bool {
        selectedWorkspace?.resetAndRefresh() ?? false
    }

    static func reset() { selectedWorkspace?.reset() }
    static func setProtected(_ protected: Bool) {
        AITextPluginRuntimeRegistry.shared.setProtected(protected)
    }

    static var deliverySource: (any BufferDeliveryContentSource)? {
        selectedWorkspace
    }

    static func currentDeliverySource(sourceModel: BufferModel = .shared)
        -> any BufferDeliveryContentSource {
        selectedWorkspace ?? sourceModel
    }
}
