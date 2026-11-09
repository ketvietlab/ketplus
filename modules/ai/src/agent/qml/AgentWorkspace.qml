import Ket
import QtQuick
import Ket as Kit
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Effects
import QtQuick.Layouts
import "TranscriptPosition.js" as TranscriptPosition

Item {
    id: root
    objectName: "agentWorkspaceRoot"

    property alias prompt: composerArea.prompt
    property alias permissionMenu: composerArea.permissionMenu
    property alias skillsPopup: composerArea.skillsPopup
    property alias modelButton: composerArea.modelButton
    property alias usageRingButton: composerArea.usageRingButton
    property alias modelPicker: modelPopup
    property alias imageViewer: imagePreview
    property alias limitsPopover: usageLimitsPopover
    function scheduleDraftSave() { composerDraftSaveTimer.restart() }

    // The reading column the transcript text and composer share, as in the mock: the host
    // sets its width (760 by default); 0 lets the conversation fill the pane.
    readonly property real conversationColumnWidth: agentHost.conversationWidth > 0
        ? KetTheme.px(agentHost.conversationWidth, KetTheme.zoom) : Number.POSITIVE_INFINITY
    readonly property int messageCount: messages.count
    property string selectedModel: ""
    property string modelLabel: "Codex default"
    property string selectedEffort: ""
    property var efforts: []
    property string defaultEffort: ""
    function effortLabel(value) {
        const names = {none: "None", minimal: "Minimal", low: "Light", medium: "Medium", high: "High", xhigh: "Extra high", "extra-high": "Extra high", max: "Maximum", ultra: "Ultra"}
        return names[value] || value
    }
    function enabledSkillCount() {
        let count = 0
        for (let skill of agentHost.skills || []) if (skill.enabled) ++count
        return count
    }
    property string selectedProvider: "codex"
    readonly property string usageProvider: (agentHost.statusProvider || selectedProvider || "codex").toLowerCase()
    readonly property var usageSnapshot: agentHost.usageSnapshot || ({})
    readonly property bool usageMatchesProvider: (usageSnapshot.provider || "") === usageProvider
    readonly property var usageContext: usageMatchesProvider ? (usageSnapshot.context || {}) : ({})
    readonly property var weeklyQuota: usageProvider === "codex" ? (agentHost.weeklyLimit || {}) : ({})
    readonly property var usageLimitRows: {
        if (usageMatchesProvider && usageSnapshot.session && usageSnapshot.session.mode === "api_key") return []
        if (usageMatchesProvider && usageSnapshot.limits && usageSnapshot.limits.length > 0)
            return usageSnapshot.limits
        if (weeklyQuota.available || weeklyQuota.loading)
            return [{id: "weekly", label: "Weekly · all models",
                     usedPercent: weeklyQuota.usedPercent,
                     resetsAt: weeklyQuota.resetsAt,
                     available: weeklyQuota.available}]
        return []
    }
    readonly property real usageRingProgress: {
        if (usageContext.available && Number(usageContext.limitTokens) > 0)
            return Math.max(0, Math.min(1,
                Number(usageContext.usedTokens) / Number(usageContext.limitTokens)))
        // Providers name their windows differently, so the ring shows the tightest one.
        let tightest = -1
        for (let row of usageLimitRows)
            if (row.available) tightest = Math.max(tightest, Number(row.usedPercent))
        if (tightest >= 0)
            return Math.max(0, Math.min(1, tightest / 100))
        if (weeklyQuota.available)
            return Math.max(0, Math.min(1, Number(weeklyQuota.usedPercent) / 100))
        return 0
    }
    readonly property bool usageCompactEnabled: usageProvider === "codex"
                                                && providerSupports("codex", "threadCompact")
        && agentHost.agentAvailable
        && !agentHost.sessionRunning
        && usageContext.available === true
        && Number(usageContext.limitTokens) > 0
    property var permissionProfiles: agentHost.permissions
    property string permission: "workspace-write"
    readonly property var currentPermission: {
        for (let p of permissionProfiles) if (p.id === permission) return p
        return {label: "Permissions", elevated: false, id: ""}
    }
    readonly property bool permissionElevated: currentPermission.elevated === true
    readonly property color permissionAccent: permissionElevated
                                              ? (KetTheme.t.warning)
                                              : KetTheme.t.textSecondary
    function permissionIconSource(profileId) {
        if (profileId === "read-only")
            return "qrc:/agent/icons/lock.svg"
        if (profileId === "workspace-write")
            return "qrc:/agent/icons/folder.svg"
        return "qrc:/agent/icons/shield.svg"
    }
    function permissionLabelColor(profile) {
        if (profile && profile.elevated === true)
            return KetTheme.t.warning
        return KetTheme.t.textMain
    }
    property bool modelsLoading: false
    function refreshModels() {
        if (!agentHost.agentAvailable || modelsLoading) return
        modelsLoading = true
        agentHost.requestModels()
    }
    function openModelPicker() {
        skillsPopup.close()
        permissionMenu.close()
        usageLimitsPopover.close()
        modelPopup.open()
        if (agentHost.models.length === 0) refreshModels()
    }
    property bool applyingConversationConfiguration: false
    readonly property var slashCommands: agentHost.compactMode ? [
        {id: "model", label: "/model", description: "Choose model and reasoning effort"},
        {id: "permissions", label: "/permissions", description: "Choose access for the next agent turn"},
        {id: "skills", label: "/skills", description: "Manage skills for this workspace"}
    ] : [
        {id: "new", label: "/new", description: "Start a new conversation"},
        {id: "clear", label: "/clear", description: "Clear this chat into a fresh conversation"},
        {id: "compact", label: "/compact", description: "Compact this conversation's context"},
        {id: "terminal", label: "/terminal", description: "Open a new terminal tab"},
        {id: "diff", label: "/diff", description: "Open the current file diff"},
        {id: "model", label: "/model", description: "Choose model and reasoning effort"},
        {id: "permissions", label: "/permissions", description: "Choose access for the next agent turn"},
        {id: "skills", label: "/skills", description: "Manage skills for this workspace"},
        {id: "plan", label: "/plan", description: "Plan a change without editing files"}
    ].filter(function(command) {
        return command.id !== "compact" || root.providerSupports(root.selectedProvider, "threadCompact")
    })
    readonly property var visibleSlashCommands: filterSlashCommands(prompt ? prompt.text : "")
    property int selectedSkillSuggestion: 0
    readonly property var activeSkillMention: skillMentionAtCursor(
        prompt ? prompt.text : "", prompt ? prompt.cursorPosition : 0)
    // `$` completes skills, `@` completes files and folders of the conversation worktree, and
    // `#` completes other conversations' numbers (ADR 0027).
    readonly property var visibleSkillSuggestions: !activeSkillMention.active ? []
        : activeSkillMention.kind === "file"
            ? (agentHost.fileMentionRevision, agentHost.fileMentionSuggestions(activeSkillMention.query))
            : activeSkillMention.kind === "conversation"
                ? (agentHost.conversations, agentHost.conversationMentionSuggestions(activeSkillMention.query))
                : agentHost.skillSuggestions(activeSkillMention.query)
    property var drafts: ({})
    property string draftScope: ""
    property var questionDrafts: ({})
    property var questionDraftOrder: []
    readonly property int maximumQuestionDrafts: 64
    function questionDraftKey(messageKey) {
        return transcriptScope + "\n" + messageKey
    }
    function questionDraftFor(messageKey) {
        return questionDrafts[questionDraftKey(messageKey)] || ({})
    }
    function saveQuestionDraft(messageKey, answers, questionIndex, otherAnswers) {
        if (!messageKey) return
        const key = questionDraftKey(messageKey)
        const updated = Object.assign({}, questionDrafts)
        updated[key] = {
            answers: Object.assign({}, answers || {}),
            questionIndex: questionIndex,
            otherAnswers: Object.assign({}, otherAnswers || {})
        }
        questionDrafts = updated
        const nextOrder = questionDraftOrder.filter(candidate => candidate !== key)
        nextOrder.push(key)
        while (nextOrder.length > maximumQuestionDrafts) {
            const evicted = nextOrder.shift()
            const bounded = Object.assign({}, questionDrafts)
            delete bounded[evicted]
            questionDrafts = bounded
        }
        questionDraftOrder = nextOrder
    }
    function clearQuestionDraft(messageKey) {
        const key = questionDraftKey(messageKey)
        const updated = Object.assign({}, questionDrafts)
        delete updated[key]
        questionDrafts = updated
        questionDraftOrder = questionDraftOrder.filter(candidate => candidate !== key)
    }
    function saveAndRestoreDraft() {
        drafts[draftScope] = {text: prompt.text}
        agentHost.saveComposerDraft(draftScope, prompt.text)
        draftScope = agentHost.composerScope
        const draft = drafts[draftScope]
        prompt.text = draft ? draft.text : agentHost.composerDraft(draftScope)
    }
    Timer {
        id: composerDraftSaveTimer
        interval: 250
        onTriggered: agentHost.saveComposerDraft(root.draftScope, prompt.text)
    }
    Connections {
        target: agentHost
        function onSteeringAccepted(scope, text) {
            if (scope === root.draftScope) {
                if (prompt.text.trim() === text) prompt.clear()
            } else if (root.drafts[scope] && root.drafts[scope].text.trim() === text) {
                root.drafts[scope].text = ""
            }
        }
        function onAgentAvailableChanged() {
            if (agentHost.agentAvailable)
                root.refreshModels()
        }
        function onModelsChanged() {
            root.modelsLoading = false
            root.applyConversationConfiguration()
        }
        function onConversationConfigurationChanged() { root.applyConversationConfiguration() }
        function onModelConfigurationFinished(error) { if (!error) modelPopup.close() }
        function onSessionChanged() {
            if (!agentHost.sessionRunning && transcript) {
                transcript.streamingViewportPrepared = false
                transcript.streamingTailRequested = false
            }
            if (agentHost.sessionRunning && transcript && transcript.followTail)
                Qt.callLater(root.restoreTranscriptPosition)
        }
        function onComposerScopeChanged() { root.saveAndRestoreDraft() }
        function onRestoringChanged() {
            if (!agentHost.restoring) return
            modelPopup.close()
            permissionMenu.close()
            skillsPopup.close()
            imagePreview.close()
        }
    }

    function modelMatchesId(candidate, modelId) {
        if (!candidate || !modelId)
            return false
        if (candidate.model === modelId || candidate.id === modelId)
            return true
        for (let effort of (candidate.efforts || []))
            if (effort.model === modelId)
                return true
        return false
    }

    function modelForId(modelId) {
        if (!modelId)
            return null
        const preferred = agentHost.conversationProvider || selectedProvider
        let anyMatch = null
        for (let candidate of agentHost.models) {
            if (!modelMatchesId(candidate, modelId)) continue
            if (preferred && candidate.provider === preferred) return candidate
            if (!anyMatch) anyMatch = candidate
        }
        return anyMatch
    }

    function providerTitle(provider) {
        if (provider === "claude") return "Claude"
        if (provider === "cursor") return "Cursor"
        return "Codex"
    }
    function composerModelText() {
        const provider = providerTitle(root.selectedProvider)
        let name = root.modelLabel || root.selectedModel || "Default"
        const prefix = new RegExp("^" + provider + "\\s+", "i")
        name = name.replace(prefix, "")
        if (!name) name = "Default"
        return provider + " · " + name + (root.selectedEffort ? "   " + root.effortLabel(root.selectedEffort) : "")
    }
    function composerModelTooltip() {
        const provider = providerTitle(root.selectedProvider)
        let name = root.modelLabel || root.selectedModel || "Default"
        const prefix = new RegExp("^" + provider + "\\s+", "i")
        name = name.replace(prefix, "")
        const effort = root.selectedEffort ? " · " + root.effortLabel(root.selectedEffort) : ""
        if (root.selectedProvider === "cursor" && (root.selectedModel === "auto" || name.toLowerCase() === "auto"))
            return provider + " · Auto" + effort + " — Cursor chooses the model for this turn"
        return provider + " · " + name + effort
    }

    function publishConversationConfiguration() {
        if (!applyingConversationConfiguration) {
            agentHost.updateConversationConfiguration(selectedModel, selectedEffort, permission, selectedProvider)
        }
    }

    function applyConversationConfiguration() {
        applyingConversationConfiguration = true
        const configuredModel = agentHost.conversationModel || ""
        const configuredProvider = (agentHost.conversationProvider || "").toLowerCase()
        const model = modelForId(configuredModel)
        if (model && (!configuredProvider || model.provider === configuredProvider)) {
            selectModel(model, false)
        } else if (configuredModel) {
            selectedProvider = configuredProvider || selectedProvider || "codex"
            selectedModel = configuredModel
            modelLabel = configuredModel
            efforts = []
            defaultEffort = ""
        } else if (configuredProvider) {
            selectedProvider = configuredProvider
        }
        selectedEffort = agentHost.conversationEffort || (model ? (defaultEffort || selectedEffort) : selectedEffort) || ""
        if (!agentHost.conversationEffort && model) {
            for (let effort of (model.efforts || [])) {
                if (effort.model === configuredModel) {
                    selectedEffort = effort.id
                    break
                }
            }
        }
        permission = agentHost.conversationPermission || "workspace-write"
        let permissionSupported = false
        for (let profile of permissionProfiles) if (profile.id === permission) permissionSupported = true
        if (!permissionSupported) permission = model && model.defaultPermission ? model.defaultPermission : "workspace-write"
        applyingConversationConfiguration = false
    }

    function selectModel(model, persist) {
        if (!model) return
        const changed = selectedProvider !== model.provider || selectedModel !== model.model
        let permissionSupported = false
        for (let profile of model.permissions) if (profile.id === permission) permissionSupported = true
        if (!permissionSupported) permission = model.defaultPermission
        selectedProvider = model.provider
        permissionProfiles = model.permissions
        selectedModel = model.model
        modelLabel = model.displayName
        efforts = model.supportedReasoningEfforts
        defaultEffort = model.defaultReasoningEffort
        if (changed) selectedEffort = defaultEffort
        if (persist !== false) publishConversationConfiguration()
    }

    function focusPrompt() {
        if (agentHost.agentAvailable)
            prompt.forceActiveFocus()
    }

    function useSuggestion(text) {
        prompt.text = text
        prompt.cursorPosition = prompt.length
        prompt.forceActiveFocus()
    }

    // Capability names come from the daemon's provider.runtime_status (ADR 0015).
    function providerSupports(provider, capability) {
        const runtime = agentHost.providerRuntime || {}
        if (runtime.gated !== true) return true
        const capabilities = (runtime.providers || {})[provider] || []
        return capabilities.indexOf(capability) !== -1
    }

    function filterSlashCommands(text) {
        const value = (text || "").trim()
        if (!value.startsWith("/"))
            return []
        const needle = value.substring(1).toLocaleLowerCase()
        return slashCommands.filter(function(command) {
            return command.id.indexOf(needle) !== -1 || command.label.indexOf(value) === 0
        })
    }

    function slashCommandForText(text) {
        const value = (text || "").trim()
        for (let command of slashCommands) {
            if (value === command.label || value.startsWith(command.label + " "))
                return command
        }
        return null
    }

    function isSkillNameCharacter(character) {
        return character.length > 0 && /[A-Za-z0-9_.:-]/.test(character)
    }

    function skillMentionAtCursor(text, cursorPosition) {
        const value = text || ""
        const cursor = Math.max(0, Math.min(cursorPosition, value.length))
        const inactive = {active: false, kind: "", start: cursor, end: cursor, query: ""}
        let fileStart = cursor - 1
        while (fileStart >= 0 && /[^\s@"$]/.test(value.charAt(fileStart))) --fileStart
        if (fileStart >= 0 && value.charAt(fileStart) === "@"
                && (fileStart === 0 || /[\s(\[{'"]/.test(value.charAt(fileStart - 1)))) {
            let fileEnd = cursor
            while (fileEnd < value.length && /[^\s@"$]/.test(value.charAt(fileEnd))) ++fileEnd
            return {active: true, kind: "file", start: fileStart, end: fileEnd,
                    query: value.substring(fileStart + 1, cursor)}
        }
        let numberStart = cursor - 1
        while (numberStart >= 0 && /[^\s#@$]/.test(value.charAt(numberStart))) --numberStart
        if (numberStart >= 0 && value.charAt(numberStart) === "#"
                && (numberStart === 0 || /[\s(\[{'"]/.test(value.charAt(numberStart - 1)))) {
            let numberEnd = cursor
            while (numberEnd < value.length && /[^\s#@$]/.test(value.charAt(numberEnd))) ++numberEnd
            return {active: true, kind: "conversation", start: numberStart, end: numberEnd,
                    query: value.substring(numberStart + 1, cursor)}
        }
        let start = cursor - 1
        while (start >= 0 && isSkillNameCharacter(value.charAt(start))) --start
        if (start < 0 || value.charAt(start) !== "$")
            return inactive
        if (start > 0 && /[A-Za-z0-9_$]/.test(value.charAt(start - 1)))
            return inactive
        let end = cursor
        while (end < value.length && isSkillNameCharacter(value.charAt(end))) ++end
        return {active: true, kind: "skill", start: start, end: end,
                query: value.substring(start + 1, cursor)}
    }

    function applySkillSuggestion(skill) {
        if (!skill || !activeSkillMention.active) return
        // A folder stays open for completion so its children can be picked next.
        const insertion = (skill.mention || ("$" + skill.name)) + (skill.directory ? "" : " ")
        const start = activeSkillMention.start
        prompt.remove(start, activeSkillMention.end)
        prompt.insert(start, insertion)
        prompt.cursorPosition = start + insertion.length
        selectedSkillSuggestion = 0
        root.focusPrompt()
    }

    function applySlashCommand(command) {
        if (command.id === "compact") {
            if (agentHost.sessionRunning) {
                agentHost.steerPrompt("/compact")
                return
            }
            agentHost.submitConfiguredPrompt("/compact", {provider: selectedProvider})
            if (agentHost.sessionRunning) prompt.clear()
            root.focusPrompt()
            return
        }
        prompt.clear()
        if (command.id === "new" || command.id === "clear") {
            agentHost.requestClearSession()
        } else if (command.id === "terminal") {
            agentHost.requestTerminal()
        } else if (command.id === "diff") {
            agentHost.requestCurrentFileDiff()
        } else if (command.id === "model") {
            root.openModelPicker()
            return
        } else if (command.id === "permissions") {
            permissionMenu.open()
        } else if (command.id === "skills") {
            agentHost.refreshSkills()
            skillsPopup.open()
        } else if (command.id === "plan") {
            prompt.text = "/plan "
            prompt.cursorPosition = prompt.text.length
        }
        root.focusPrompt()
    }

    function appendMessage(kind, body, meta, filePath, itemId) {
        messages.append({
            "kind": kind,
            "body": body,
            "imageSources": [],
            "fileStats": [],
            "meta": meta || "",
            "filePath": filePath || "",
            "itemId": itemId || "",
            "subtype": "",
            "command": "",
            "running": false
        })
    }

    function displayMessageBody(kind, text) {
        return text || ""
    }

    ModelPicker {
        id: modelPopup
        viewport: root
        anchorItem: modelButton
        models: agentHost.models
        // Between turns a conversation may move to another provider; not mid-turn.
        efforts: root.efforts
        selectedModel: root.selectedModel
        selectedEffort: root.selectedEffort
        selectedProvider: root.selectedProvider
        defaultEffort: root.defaultEffort
        loading: root.modelsLoading
        available: agentHost.agentAvailable
        effortLabel: root.effortLabel
        configurationRevision: agentHost.conversationConfigurationRevision
        compactWindow: agentHost.conversationCompactWindow
        compactSupported: agentHost.compactWindowSupported
        modelCapacity: Number(root.usageContext.limitTokens) || 0
        applying: agentHost.modelConfigurationApplying
        applyError: agentHost.modelConfigurationError
        blockedReason: {
            if (!agentHost.configurationWritable) return qsTr("Reconnect to the desktop before applying.")
            return ""
        }
        onApplyRequested: function(model, effort, provider, changes) {
            agentHost.applyModelConfiguration(model, effort, provider, changes)
        }
        onRefreshRequested: root.refreshModels()
    }

    UsageLimitsPopover {
        id: usageLimitsPopover
        parent: Overlay.overlay
        workspace: root
        contextUsage: root.usageContext
        limitRows: root.usageLimitRows
        claude: root.usageProvider === "claude"
        sessionUsage: root.usageMatchesProvider ? (root.usageSnapshot.session || {}) : ({})
        accountUsage: agentHost.claudeAccountUsage || ({})
        onAccountRefreshRequested: agentHost.requestClaudeAccountUsage()
        compactEnabled: root.usageCompactEnabled
        function positionPopover() {
            const overlay = Overlay.overlay
            const pos = usageRingButton.mapToItem(overlay, 0, 0)
            x = Math.max(8, Math.min(overlay.width - width - 8,
                                     pos.x + usageRingButton.width - width))
            y = Math.max(8, Math.min(overlay.height - height - 8, pos.y - height - 8))
        }
        onHeightChanged: if (visible) Qt.callLater(positionPopover)
        Connections {
            target: usageLimitsPopover.parent
            function onHeightChanged() { if (usageLimitsPopover.visible) Qt.callLater(usageLimitsPopover.positionPopover) }
            function onWidthChanged() { if (usageLimitsPopover.visible) Qt.callLater(usageLimitsPopover.positionPopover) }
        }
        onAboutToShow: {
            if (claude) agentHost.requestClaudeAccountUsage()
            Qt.callLater(positionPopover)
        }
        onCompactRequested: {
            close()
            agentHost.submitConfiguredPrompt("/compact", {provider: "codex"})
        }
    }

    // The image viewer over the whole window, as the mock opens a capture or an image file.
    Kit.KetLightbox {
        id: imagePreview
        objectName: "imagePreview"
        tools: Kit.KetTheme.LightboxTool.ZoomOut | Kit.KetTheme.LightboxTool.ZoomLevel
               | Kit.KetTheme.LightboxTool.ZoomIn | Kit.KetTheme.LightboxTool.Rotate
               | Kit.KetTheme.LightboxTool.Copy | Kit.KetTheme.LightboxTool.Download
               | Kit.KetTheme.LightboxTool.Print | Kit.KetTheme.LightboxTool.Fullscreen
        readonly property url imageSource: item ? item.source : ""
        // Opens image URLs at `index`; each is captioned with its file name.
        function showImages(sources, index) {
            items = (sources || []).map(source => ({
                source: source.toString(),
                caption: agentHost.imageCaption(source)
            }));
            openAt(index || 0);
        }
        // Opens items that carry their own caption: [{ source, caption }].
        function showItems(list, index) {
            items = list || [];
            openAt(index || 0);
        }
        // A Mermaid diagram opened before its dark rendering was ready takes it once drawn,
        // while the viewer still shows that diagram.
        function refreshMermaid() {
            const shown = item;
            if (!opened || !shown || !shown.diagram)
                return;
            const next = agentHost.mermaidViewerImage(shown.diagram);
            if (!next.source || String(next.source) === String(shown.source))
                return;
            const list = items.slice();
            list[currentIndex] = next;
            items = list;
        }
        onToolTriggered: tool => {
            if (!item)
                return;
            if (tool === Kit.KetTheme.LightboxTool.Copy)
                imageToast.show(agentHost.copyImage(item.source, rotationAngle) ? "Copied the image" : "The image could not be copied");
            else if (tool === Kit.KetTheme.LightboxTool.Download) {
                const saved = agentHost.saveImageToDownloads(item.source);
                imageToast.show(saved ? "Saved " + saved.split("/").pop() + " to Downloads" : "The image could not be saved");
            } else if (tool === Kit.KetTheme.LightboxTool.Print)
                agentHost.printImage(item.source, rotationAngle, item.caption);
            else if (tool === Kit.KetTheme.LightboxTool.Fullscreen)
                agentHost.toggleWindowFullScreen();
        }
    }
    Connections {
        target: agentHost
        function onMermaidChanged() { imagePreview.refreshMermaid() }
    }
    Kit.KetToast {
        id: imageToast
        objectName: "imageToast"
    }

    function openTranscriptFind() {
        if (transcript)
            transcript.openFind()
    }

    function transcriptFindStep(backwards) {
        if (!transcript)
            return
        if (!transcript.findOpen || transcript.findQuery.length === 0) {
            transcript.openFind()
            return
        }
        if (backwards)
            transcript.findPrevious()
        else
            transcript.findNext()
    }

    // Standalone workspaces handle Find here. Hosts with Edit > Find actions (the desktop
    // window) route those to the transcript themselves; a second shortcut for the same
    // sequence would make both ambiguous so that neither fires.
    Shortcut {
        sequences: [StandardKey.Find]
        context: Qt.WindowShortcut
        enabled: !!root.transcript && root.visible && !agentHost.findActionsProvided
        onActivated: root.openTranscriptFind()
    }
    Shortcut {
        sequences: [StandardKey.FindNext]
        context: Qt.WindowShortcut
        enabled: !!root.transcript && root.visible && !agentHost.findActionsProvided
        onActivated: root.transcriptFindStep(false)
    }
    Shortcut {
        sequences: [StandardKey.FindPrevious]
        context: Qt.WindowShortcut
        enabled: !!root.transcript && root.visible && !agentHost.findActionsProvided
        onActivated: root.transcriptFindStep(true)
    }

    function conversationCopyText() {
        return agentHost.transcriptCopyText()
    }

    property var transcriptPositions: ({})
    property string transcriptScope: ""
    property var transcriptAnchor: null
    property var transcript: null
    property var transcriptWheel: null
    property var transcriptScrollBar: null
    property bool restoringTranscript: false
    property bool settlingTranscript: false
    property bool updatingTranscript: false
    property int transcriptUpdateRevision: 0
    property int transcriptInputRevision: 0
    // Shared counter for transcript history request tokens (see TranscriptView).
    property int transcriptHistoryRequestSerial: 0
    // True from a wheel event until the event loop turns over, covering the
    // synchronous Flickable motion that the event itself produces.
    property bool transcriptWheelEventPending: false
    // Test hook: deferred Markdown refreshes that captured a fresh anchor.
    property int transcriptRenderAnchorCaptures: 0
    // Layout trace only: offset at the last frame the trace recorded.
    property real transcriptTracedOffset: 0
    // What the reader saw move: the screen displacement of the row read on the
    // previous frame, accumulated. contentY also moves when rows above are
    // measured or history is prepended, which the reader never sees.
    property string transcriptTracedReaderKey: ""
    property real transcriptTracedReaderY: 0
    property real transcriptTracedReaderOffset: 0
    // Phase of the previous traced frame: the first idle frame after a gesture
    // still carries the gesture's last motion.
    property string transcriptTracedPhase: "idle"

    function traceReaderOffset() {
        const geometry = transcript.geometry
        if (!geometry) return transcript.contentY - transcript.originY
        const previous = geometry.rowForKey(transcriptTracedReaderKey)
        if (previous >= 0)
            transcriptTracedReaderOffset -= (transcript.rowTop(previous) - transcript.contentY)
                                            - transcriptTracedReaderY
        const row = transcript.readingRow()
        transcriptTracedReaderKey = row >= 0 ? geometry.keyAt(row) : ""
        transcriptTracedReaderY = row >= 0 ? transcript.rowTop(row) - transcript.contentY : 0
        return transcriptTracedReaderOffset
    }

    function traceTranscript(event, details) {
        if (!agentHost.layoutTraceEnabled || !transcript) return
        const values = details || {}
        values.contentY = transcript.contentY
        values.originY = transcript.originY
        values.contentHeight = transcript.contentHeight
        values.viewportHeight = transcript.height
        values.followTail = transcript.followTail
        values.moving = transcript.scrolling
        values.restoring = restoringTranscript
        values.settling = settlingTranscript
        values.anchorOffset = transcriptAnchor ? transcriptAnchor.offset : 0
        values.wheelActive = transcriptWheel ? transcriptWheel.active : false
        values.opacity = transcript.opacity
        agentHost.traceLayout(event, values)
    }

    // One presented frame. "frame-presented" is kept verbatim for the existing
    // trace consumers; the two metric events are separate so that the reducer
    // never has to guess which frames it is allowed to difference.
    function traceTranscriptFrame() {
        if (!agentHost.layoutTraceEnabled || !transcript) return
        traceTranscript("frame-presented", {})
        const phase = transcript.tracedGesturePhase()
        const offset = transcript.contentY - transcript.originY
        const previousReaderOffset = transcriptTracedReaderOffset
        const previousReaderKey = transcriptTracedReaderKey
        const readerOffset = traceReaderOffset()
        const previousPhase = transcriptTracedPhase
        transcriptTracedPhase = phase
        // The defect in one record: the reader touched nothing and the line
        // they were reading moved on screen.
        if (phase === "idle" && previousPhase === "idle" && !transcript.followTail
                && Math.abs(readerOffset - previousReaderOffset) > 2)
            traceTranscript("reader-jump", {delta: readerOffset - previousReaderOffset,
                                            key: previousReaderKey})
        // An idle frame that nevertheless moved is the purest form of the defect:
        // the reader touched nothing and the page went somewhere. Skipping those
        // frames would drop that motion out of the path entirely, so idle is
        // silent only while it is genuinely still.
        const moved = Math.abs(offset - transcriptTracedOffset) > 0.5
        if (phase === "idle" && !moved) return
        transcriptTracedOffset = offset
        // M1 and M3: signed position, measured against originY so that a history
        // page prepended above the viewport does not read as motion.
        traceTranscript("gesture-frame", {phase: phase,
                                          direction: transcriptWheel ? transcriptWheel.direction : 0,
                                          offset: offset,
                                          readerOffset: readerOffset})
        // One sample per frame is what the reader sees; a row that enters, leaves
        // and enters again between two frames was never on screen twice.
        const census = transcript.tracedRowCensus()
        traceTranscript("row-visibility", {phase: phase,
                                           keys: census.keys,
                                           collapsedRealized: census.collapsedRealized,
                                           collapsedInViewport: census.collapsedInViewport,
                                           collapsedInSpan: census.collapsedInSpan,
                                           indexSpan: census.indexSpan,
                                           listContentHeight: census.listContentHeight,
                                           geometryTotal: census.geometryTotal,
                                           geometryRows: census.geometryRows,
                                           geometryMeasured: census.geometryMeasured,
                                           geometryMismatches: census.geometryMismatches})
    }

    // When a changed conversation was switched in and hidden to settle, and
    // the longest it may stay hidden.
    property double transcriptSettleStarted: 0
    readonly property int transcriptSettleDeadline: 250
    Timer {
        id: transcriptSettleTimer
        interval: 32
        onTriggered: {
            root.restoreTranscriptPosition()
            if (!running) {
                root.traceTranscript("settle-end", {durationMs: Date.now() - root.transcriptSettleStarted})
                root.settlingTranscript = false
            }
        }
    }

    // Jump to the newest message. From far above, scrolling there would sweep
    // through everything in between and settle row by row as heights arrive, so
    // the transcript is hidden, placed at the bottom and shown again once the
    // rows there have their real heights: the reader sees the bottom, not the trip.
    function jumpToLatest() {
        if (!transcript || !transcript.messageModel || transcript.messageModel.count === 0)
            return
        const far = !!agentHost.historyState.hasNewer
            || transcript.contentBelow() > 2 * transcript.height
        beginTranscriptInput()
        transcript.noteTranscriptInput()
        transcript.cancelFlick()
        transcript.wheelGlideRemaining = 0
        transcript.resetNewMessageCount()
        transcript.followTail = true
        transcript.streamingTailRequested = true
        if (far) {
            settlingTranscript = true
            transcriptJumpFrames = 0
            transcriptJumpTimer.restart()
        }
        if (agentHost.historyState.hasNewer) {
            agentHost.loadLatestMessages()
            return
        }
        restoringTranscript = true
        transcript.positionViewAtEnd()
        restoringTranscript = false
        settleTranscriptTail()
    }
    property int transcriptJumpFrames: 0
    Timer {
        id: transcriptJumpTimer
        interval: 16
        onTriggered: {
            const view = root.transcript
            if (!view || !view.followTail) {
                root.settlingTranscript = false
                return
            }
            ++root.transcriptJumpFrames
            root.restoringTranscript = true
            view.positionViewAtEnd()
            root.restoringTranscript = false
            // Shown once the rows on screen are measured and the newest window has
            // arrived, or after half a second (three when a window is loading).
            // A blank transcript for seconds reads as lost content; past the
            // limit the bottom shows while the newest window is still arriving.
            const loading = !!agentHost.historyState.loading
            const limit = loading ? 60 : 30
            if ((!loading && view.visibleRowsMeasured()) || root.transcriptJumpFrames >= limit) {
                root.traceTranscript("jump-latest-shown", {frames: root.transcriptJumpFrames, loading: loading,
                                                           measured: view.visibleRowsMeasured()})
                root.settlingTranscript = false
                root.settleTranscriptTail()
                return
            }
            restart()
        }
    }
    // Cmd+Down (Ctrl+Down elsewhere). In a text field whose cursor is not at the
    // end, the keys keep their text meaning.
    Shortcut {
        sequences: ["Ctrl+Down"]
        context: Qt.WindowShortcut
        enabled: {
            if (!root.transcript || !root.visible)
                return false
            const focus = root.Window.activeFocusItem
            return !(focus && focus.cursorPosition !== undefined && focus.length !== undefined
                     && focus.cursorPosition < focus.length)
        }
        onActivated: root.jumpToLatest()
    }

    // Pins the bottom once. Heights that arrive later need nothing more: every
    // geometry, header and footer change makes TranscriptView hold the tail.
    function settleTranscriptTail() {
        if (!transcript || !transcript.followTail || transcript.wheelEngaged
                || (transcriptScrollBar && transcriptScrollBar.pressed))
            return
        restoringTranscript = true
        transcript.holdReadingPosition()
        restoringTranscript = false
    }

    function transcriptInputActive() {
        return !!transcript && (transcript.scrolling || transcriptWheelEventPending
                                || transcript.wheelEngaged
                                || (transcriptScrollBar && transcriptScrollBar.pressed))
    }

    // Layout-driven settle point. The C++ TranscriptPolish item invokes this
    // from updatePolish(), i.e. after ListView has committed delegate heights
    // for the frame and before scene sync. Every height/origin/y change of a
    // row requests it, so restoration follows layout rather than a frame count.
    function settleTranscriptLayout() {
        // Row heights reported since the last frame are placed before this one.
        if (transcript)
            transcript.flushRowHeights()
        if (!transcript || restoringTranscript || updatingTranscript)
            return
        restoreTranscriptPosition()
    }

    function beginTranscriptInput() {
        ++transcriptInputRevision
        if (transcript)
            transcript.clearHistoryTrimBlocks()
    }

    function followTranscriptInputMotion(delta) {
        // Credit the physical motion to the anchor, then re-select the interior
        // row for the new viewport.
        if (transcriptAnchor && !transcript.followTail
                && transcriptWheel.direction !== 0)
            transcriptAnchor = Object.assign({}, transcriptAnchor,
                                             {offset: transcriptAnchor.offset + delta, text: undefined})
        captureTranscriptWheelPosition()
    }

    function captureTranscriptPosition() {
        return TranscriptPosition.capture(transcript)
    }

    function captureTranscriptWheelPosition() {
        transcriptAnchor = captureTranscriptPosition()
        return transcriptAnchor
    }

    function restoreTranscriptPosition() {
        restoreTranscriptPositionInternal(false)
    }

    function restoreTranscriptPositionInternal(preserveDuringInput) {
        restoreCapturedTranscriptPosition(transcriptAnchor, preserveDuringInput)
    }

    function restoreCapturedTranscriptPosition(anchor, preserveDuringInput) {
        traceTranscript("restore-request")
        if (!transcript || !transcriptWheel || !transcriptScrollBar || restoringTranscript
                || updatingTranscript || (!preserveDuringInput && (transcript.scrolling
                || transcript.wheelEngaged || transcriptScrollBar.pressed))) return
        restoringTranscript = true
        transcript.forceLayout()
        if (transcript.followTail) {
            transcript.positionViewAtEnd()
            settleTranscriptTail()
        } else if (anchor && messages.count > 0) {
            // Rows sit at exact positions, so restoring is adopting the anchor as
            // the view's reading position; no row has to be materialized first.
            transcript.holdAnchor(anchor)
            traceTranscript("restore-applied", {})
        }
        restoringTranscript = false
    }

    function setSessionMessageModel(model, scope, changed) {
        const activated = transcriptHost.activate(scope || "", model)
        transcriptScope = scope || ""
        transcriptAnchor = transcriptPositions[transcriptScope] || null
        // Models are cached per conversation even though the expensive QML
        // viewport is shared. A clean cached model can restore immediately;
        // hiding the viewport for a settle cycle causes a visible flash on
        // every A -> B -> A switch and is unnecessary when rows did not change.
        if (!changed) {
            settlingTranscript = false
            transcript.followTail = transcriptAnchor ? transcriptAnchor.followTail : true
            restoreTranscriptPosition()
            transcriptAnchor = captureTranscriptPosition()
            return
        }
        settlingTranscript = true
        transcriptSettleStarted = Date.now()
        traceTranscript("settle-begin", {scope: transcriptScope})
        transcriptSettleTimer.restart()
        restoringTranscript = true
        transcriptWheel.enabled = false
        transcriptWheel.direction = 0
        transcript.cancelFlick()
        transcript.followTail = transcriptAnchor ? transcriptAnchor.followTail : true
        transcriptWheel.enabled = true
        restoringTranscript = false
        restoreTranscriptPosition()
        Qt.callLater(root.restoreTranscriptPosition)
    }

    function captureSessionTranscriptPosition() {
        if (transcriptScope && messages.count > 0 && !settlingTranscript)
            transcriptPositions[transcriptScope] = captureTranscriptPosition()
    }

    function beginSessionMessageUpdate() {
        beginTranscriptUpdate()
    }

    function endSessionMessageUpdate() {
        endTranscriptUpdate()
    }

    function releaseSessionTranscript(scope) {
        // Evicted transcripts restart at the tail; do not keep their anchors forever.
        delete transcriptPositions[scope || ""]
        transcriptHost.release(scope || "")
    }

    function warmTranscriptCount() {
        return transcriptHost.cachedTranscripts.length
    }

    function prepareStreamingViewport(item) {
        if (!transcript || !agentHost.sessionRunning || !item || item.kind === "user"
                || transcript.streamingViewportPrepared)
            return
        transcript.streamingViewportPrepared = true
        // Streaming is not a change in reading intent: keep following when
        // already at the bottom, and preserve history reading otherwise.
        transcript.streamingTailRequested = transcript.followTail
    }

    function appendSessionMessage(item) {
        prepareStreamingViewport(item)
        beginTranscriptUpdate()
        messages.append(item)
        endTranscriptUpdate()
    }

    function beginTranscriptUpdate() {
        // A wheel/scrollbar gesture in flight keeps its own row anchor, which
        // tracks physical motion exactly. Otherwise the reading row is captured
        // before the mutation; TranscriptView holds it through the heights that
        // arrive afterwards.
        const gestureOwnsPosition = !settlingTranscript && !restoringTranscript && transcript
            && !transcript.followTail
            && transcriptWheel.direction !== 0 && transcriptInputActive()
        // The view's own reading anchor is current: it is renewed after every
        // move the reader makes and every hold the view makes. Capturing from
        // the screen instead lays out the rows' documents first, so a diagram
        // that finished loading moves the reader's text before the view could
        // hold it, and the capture would adopt the moved position.
        if (!gestureOwnsPosition && !settlingTranscript && !restoringTranscript)
            transcriptAnchor = transcript && !transcript.followTail && transcript.readingAnchor
                ? transcript.readingAnchor : captureTranscriptPosition()
        updatingTranscript = true
    }

    function endTranscriptUpdate() {
        updatingTranscript = false
        // Height notifications during the mutation are deliberately ignored.
        // Always schedule a tail settle, including when immediate restoration
        // is deferred by ListView's residual movement state.
        if (transcript.followTail)
            settleTranscriptTail()
        if (!transcript.followTail
                && transcriptWheel.direction !== 0 && transcriptInputActive()) {
            // Gesture-owned update (see beginTranscriptUpdate).
            agentHost.scheduleTranscriptRestore()
            return
        }
        const updateAnchor = transcriptAnchor
        const revision = ++transcriptUpdateRevision
        const inputRevision = transcriptInputRevision
        const inputWasActive = transcriptInputActive()
        // ListView may revise estimates for recycled delegates after a model
        // update. Restore the captured reading row even during an active
        // flick so those estimate changes cannot move content under the
        // pointer. This does not re-enable tail following or pagination.
        restoreCapturedTranscriptPosition(updateAnchor, inputWasActive)
        // Late delegate materialization is handled by the layout-driven settle
        // (row y / contentHeight change -> polish -> settleTranscriptLayout).
        Qt.callLater(function() {
            if (revision === root.transcriptUpdateRevision
                    && inputRevision === root.transcriptInputRevision)
                root.restoreCapturedTranscriptPosition(updateAnchor, inputWasActive)
        })
    }

    // History pages (older prepends, newer edge-window replacements) can land
    // while a kinetic flick is running. Restoring the reading row would pin it
    // against the flick; TranscriptView instead absorbs the known height
    // shift of a page as it happens, so a kinetic page update only records the
    // reading position and leaves contentY to the flick.
    property bool transcriptPageKinetic: false

    function beginTranscriptPageUpdate() {
        transcriptPageKinetic = !!transcript && transcript.flicking && !transcript.dragging
            && !transcript.followTail && !settlingTranscript && !restoringTranscript
        traceTranscript("page-update-begin", {kinetic: transcriptPageKinetic})
        if (!transcriptPageKinetic) {
            beginTranscriptUpdate()
            return
        }
        transcriptAnchor = captureTranscriptPosition()
        updatingTranscript = true
    }

    function endTranscriptPageUpdate() {
        if (!transcriptPageKinetic) {
            endTranscriptUpdate()
            return
        }
        transcriptPageKinetic = false
        updatingTranscript = false
        // Delegates materialized by the page publish heights in later polish
        // passes; settleTranscriptLayout places them.
        agentHost.scheduleTranscriptRestore()
    }

    function prependSessionMessages(items) {
        beginTranscriptPageUpdate()
        messages.prepend(items)
        endTranscriptPageUpdate()
    }

    function updateSessionMessage(index, item) {
        traceTranscript("delta", {messageIndex: index, characters: (item.body || "").length})
        prepareStreamingViewport(item)
        beginTranscriptUpdate()
        // Session updates address canonical messages; collapsed activities and
        // split Markdown blocks make the displayed row count a different size.
        if (index >= 0 && index < messages.sourceCount())
            messages.set(index, item)
        endTranscriptUpdate()
    }

    function updateSessionMessages(updates) {
        for (const update of updates)
            prepareStreamingViewport(update.message)
        beginTranscriptUpdate()
        for (const update of updates) {
            traceTranscript("delta", {messageIndex: update.index, characters: (update.message.body || "").length})
            if (update.index >= 0 && update.index < messages.sourceCount())
                messages.set(update.index, update.message)
        }
        endTranscriptUpdate()
    }

    function debugMessageSurfaceWidth(index) {
        const delegate = transcript.itemAtIndex(index)
        if (!delegate)
            return -1
        for (let child of delegate.children)
            if (child.objectName === "messageSurface")
                return child.width
        return -1
    }

    function debugMessageBodySelectable(index) {
        const delegate = transcript.itemAtIndex(index)
        if (!delegate)
            return false
        const stack = delegate.children.slice()
        while (stack.length > 0) {
            const item = stack.pop()
            if (item.objectName === "messageBody")
                return item.selectByMouse === true
            for (let child of item.children || [])
                stack.push(child)
        }
        return false
    }

    function prepareBenchmarkMessages(count) {
        transcript.followTail = false
        messages.clear()
        for (let index = 0; index < count; ++index) {
            if (index % 9 === 4) {
                appendMessage("tool", "Reading and analyzing source files", "Tool call · " + index, "")
            } else if (index % 11 === 7) {
                appendMessage("file", "EditorWidget.cpp", "src/editor/EditorWidget.cpp", "src/editor/EditorWidget.cpp")
            } else if (index % 3 === 0) {
                appendMessage("user", "Benchmark message " + index + ": inspect the active worktree.", "You", "")
            } else {
                appendMessage("assistant",
                              "The agent transcript stays virtualized while this response contains enough text to exercise wrapping and GPU-backed scrolling.",
                              "KetPlus Agent", "")
            }
        }
        transcript.positionViewAtBeginning()
    }

    function setBenchmarkScrollProgress(progress) {
        // Programmatic navigation supersedes wheel anchoring and any pending
        // mutation correction.
        beginTranscriptInput()
        if (transcriptWheel)
            transcriptWheel.direction = 0
        transcript.followTail = progress >= 1
        transcript.streamingTailRequested = transcript.followTail
        const maximum = Math.max(0, transcript.contentHeight - transcript.height)
        // Move as navigation, not as layout displacement: adopt the new
        // reading position as the anchor so it is not restored away.
        restoringTranscript = true
        transcript.contentY = maximum * Math.max(0, Math.min(1, progress))
        restoringTranscript = false
        transcript.previousScrollY = transcript.contentY
        captureTranscriptWheelPosition()
    }

    // The failed turn Retry sends again: the latest turn, when it failed and the conversation
    // is idle, with the prompt that opened it. Steers belong to that turn and are not its
    // prompt. A prompt with images or from another agent is not resent, since Retry could not
    // send it as it was, and neither while the composer holds attachments, which a send would
    // carry along, nor while older history is shown instead of the latest turn.
    function findRetryableFailure() {
        const model = agentHost.messageModel
        if (!model || agentHost.sessionRunning || !agentHost.agentAvailable || agentHost.attachments.length > 0 || agentHost.historyState.hasNewer)
            return null
        let errorKey = ""
        for (let i = model.sourceCount() - 1; i >= 0; --i) {
            const row = model.source(i)
            // A turn that ended other than failed needs no retry, whatever errors it showed.
            if (row.subtype === "turnEnd" && row.status !== "failed")
                return null
            if (row.kind === "error") {
                if (errorKey.length === 0)
                    errorKey = String(row.messageKey || "")
                continue
            }
            if (row.kind !== "user" || row.eventType === "turn.steered")
                continue
            const text = String(row.body || "").trim()
            const resendable = text.length > 0 && (row.images || []).length === 0 && row.subtype !== "agentMessage"
            return errorKey.length > 0 && resendable ? {key: errorKey, prompt: text} : null
        }
        return null
    }
    // The source's revision also follows row updates that keep the count.
    readonly property var retryableFailure: {
        if (agentHost.messageModel)
            agentHost.messageModel.sourceRevision
        return findRetryableFailure()
    }
    // Checked again when clicked: the shown banner may be a moment out of date.
    function retryFailedTurn(key) {
        const failure = findRetryableFailure()
        if (!failure || failure.key !== key)
            return
        agentHost.submitConfiguredPrompt(failure.prompt, {model: selectedModel, effort: selectedEffort, permission: permission, provider: selectedProvider})
        revealLatestAfterSubmit()
    }

    function submitCurrentPrompt() {
        if (!agentHost.agentAvailable || agentHost.steeringPending)
            return
        const value = prompt.text.trim()
        if (value.length === 0 && agentHost.attachments.length === 0)
            return
        if (agentHost.sessionRunning) {
            agentHost.steerPrompt(value)
            revealLatestAfterSubmit()
            return
        }
        const slashCommand = root.slashCommandForText(value)
        if (slashCommand && slashCommand.id === "plan" && value.length > slashCommand.label.length) {
            const planPrompt = value.substring(slashCommand.label.length).trim()
            if (planPrompt.length === 0)
                return
            agentHost.submitConfiguredPrompt(planPrompt, {model: selectedModel, effort: selectedEffort, permission: permission, provider: selectedProvider, mode: "plan"})
            if (agentHost.sessionRunning) {
                prompt.clear()
                revealLatestAfterSubmit()
            }
            return
        }
        if (value.startsWith("/") && root.visibleSlashCommands.length > 0) {
            root.applySlashCommand(root.visibleSlashCommands[0])
            return
        }
        agentHost.submitConfiguredPrompt(value, {model: selectedModel, effort: selectedEffort, permission: permission, provider: selectedProvider})
        if (agentHost.sessionRunning) {
            prompt.clear()
            revealLatestAfterSubmit()
        }
    }

    // Sending is a change of reading intent: the reader wants to see the
    // message they sent and its answer, not the history they scrolled to.
    function revealLatestAfterSubmit() {
        if (!transcript || transcript.followTail)
            return
        traceTranscript("submit-reveal-latest", {})
        jumpToLatest()
    }

    readonly property var messages: agentHost.messageModel

    Rectangle {
        anchors.fill: parent
        color: KetTheme.t.pageBg
    }

    Basic.SplitView {
        id: workspaceSplit
        objectName: "agentWorkspaceSplit"
        anchors.fill: parent
        orientation: Qt.Horizontal
        // A drag sets the sidebar's preferred width; keep it in unscaled pixels so a
        // zoom change rescales the user's choice instead of restoring the default.
        onResizingChanged: {
            if (!resizing && sidebar.visible)
                sidebar.unscaledWidth = Math.max(188, Math.min(320, sidebar.width / KetTheme.zoom));
        }
        handle: Item {
            objectName: "sidebarResizeHandle"
            implicitWidth: agentHost.productSidebarVisible && !agentHost.compactMode ? KetTheme.px(6, KetTheme.zoom) : 0
            readonly property color dividerColor: SplitHandle.pressed ? KetTheme.t.accent
                                                 : SplitHandle.hovered ? KetTheme.t.borderStrong : KetTheme.t.panelBorder
            Rectangle {
                objectName: "sidebarDivider"
                visible: parent.implicitWidth > 0
                anchors.horizontalCenter: parent.horizontalCenter
                height: parent.height
                width: 1
                color: parent.dividerColor
            }
            HoverHandler {
                enabled: parent.implicitWidth > 0
                cursorShape: Qt.SplitHCursor
            }
        }

        AgentSidebar {
            id: sidebar
            workspace: root
            visible: agentHost.productSidebarVisible && !agentHost.compactMode
            // Recipe widths follow interface zoom, as the mock's 240 px sidebar does.
            property real unscaledWidth: 240
            function applyWidth() {
                SplitView.preferredWidth = KetTheme.px(unscaledWidth, KetTheme.zoom);
            }
            SplitView.minimumWidth: visible ? KetTheme.px(188, KetTheme.zoom) : 0
            SplitView.maximumWidth: visible ? KetTheme.px(320, KetTheme.zoom) : 0
            Component.onCompleted: applyWidth()
            onUnscaledWidthChanged: applyWidth()
            Connections {
                target: KetTheme
                function onZoomChanged() { sidebar.applyWidth(); }
            }
        }

        Rectangle {
            id: chatPane
            objectName: "chatPane"
            SplitView.fillWidth: true
            SplitView.minimumWidth: KetTheme.px(260, KetTheme.zoom)
            color: KetTheme.t.pageBg

            ColumnLayout {
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width
                spacing: 0

                AgentWorkspaceHeader {
                    workspace: root
                    Layout.fillWidth: true
                    Layout.preferredHeight: implicitHeight
                }

                AgentScopeNotice {
                    workspace: root
                }

                WarmTranscriptHost {
                    id: transcriptHost
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    // Keep the final row clear of the composer even while
                    // the viewport is pinned during streaming or a new send.
                    Layout.bottomMargin: KetTheme.px(20, KetTheme.zoom)
                    initialModel: agentHost.messageModel
                    transcriptFactory: cachedTranscriptFactory
                    onTranscriptActivated: function(view) {
                        root.transcript = view
                        root.transcriptWheel = view.wheelController
                        root.transcriptScrollBar = view.verticalScrollBar
                    }

                    Rectangle {
                        id: transcriptFindBar
                        objectName: "transcriptFindBar"
                        readonly property var view: root.transcript
                        visible: !!view && view.findOpen
                        z: 10
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.topMargin: KetTheme.px(8, KetTheme.zoom)
                        anchors.rightMargin: KetTheme.px(16, KetTheme.zoom)
                        width: Math.min(parent.width - KetTheme.px(32, KetTheme.zoom), KetTheme.px(380, KetTheme.zoom))
                        height: findRow.height + (findStatusText.visible ? findStatusText.height + KetTheme.px(4, KetTheme.zoom) : 0) + KetTheme.px(8, KetTheme.zoom)
                        radius: KetTheme.px(7, KetTheme.zoom)
                        color: KetTheme.t.surfaceRaised
                        border.color: KetTheme.t.borderStrong
                        Accessible.role: Accessible.ToolBar
                        Accessible.name: "Find in conversation"
                        Connections {
                            target: transcriptFindBar.view
                            ignoreUnknownSignals: true
                            function onFindOpened() {
                                findField.forceActiveFocus()
                                findField.selectAll()
                            }
                        }
                        Row {
                            id: findRow
                            x: KetTheme.px(6, KetTheme.zoom)
                            y: KetTheme.px(4, KetTheme.zoom)
                            width: parent.width - KetTheme.px(12, KetTheme.zoom)
                            height: KetTheme.px(30, KetTheme.zoom)
                            spacing: KetTheme.px(4, KetTheme.zoom)
                            Basic.TextField {
                                id: findField
                                objectName: "transcriptFindField"
                                width: parent.width - findCount.width - 3 * KetTheme.px(28, KetTheme.zoom) - 3 * parent.spacing
                                height: KetTheme.px(30, KetTheme.zoom)
                                placeholderText: "Find in conversation"
                                color: KetTheme.t.textMain
                                placeholderTextColor: KetTheme.t.textMuted
                                font.family: KetTheme.fontFamily
                                font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                                background: Rectangle {
                                    color: "transparent"
                                }
                                Accessible.name: "Find in conversation"
                                onTextEdited: if (transcriptFindBar.view) transcriptFindBar.view.setFindQuery(text)
                                Keys.onReturnPressed: event => (event.modifiers & Qt.ShiftModifier) ? transcriptFindBar.view.findPrevious() : transcriptFindBar.view.findNext()
                                Keys.onEnterPressed: event => (event.modifiers & Qt.ShiftModifier) ? transcriptFindBar.view.findPrevious() : transcriptFindBar.view.findNext()
                                Keys.onEscapePressed: transcriptFindBar.view.closeFind()
                            }
                            Text {
                                id: findCount
                                objectName: "transcriptFindCount"
                                anchors.verticalCenter: parent.verticalCenter
                                text: transcriptFindBar.view ? transcriptFindBar.view.findCountText : ""
                                color: KetTheme.t.textMuted
                                font.family: KetTheme.fontFamily
                                font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                                Accessible.name: text
                            }
                            Basic.ToolButton {
                                objectName: "transcriptFindPrevious"
                                width: KetTheme.px(28, KetTheme.zoom)
                                height: KetTheme.px(28, KetTheme.zoom)
                                padding: KetTheme.px(7, KetTheme.zoom)
                                contentItem: Item {
                                    implicitWidth: KetTheme.px(16, KetTheme.zoom)
                                    implicitHeight: implicitWidth
                                    KetIcon {
                                        anchors.centerIn: parent
                                        box: Math.min(parent.width, parent.height)
                                        name: KetTheme.iconForLucide("arrow-up"); color: KetTheme.t.textMain
                                    }
                                }
                                background: Rectangle { color: parent.hovered ? KetTheme.t.surfaceHover : "transparent"; radius: KetTheme.px(4, KetTheme.zoom) }
                                onClicked: transcriptFindBar.view.findPrevious()
                                Accessible.name: "Previous match"
                            }
                            Basic.ToolButton {
                                objectName: "transcriptFindNext"
                                width: KetTheme.px(28, KetTheme.zoom)
                                height: KetTheme.px(28, KetTheme.zoom)
                                padding: KetTheme.px(7, KetTheme.zoom)
                                contentItem: Item {
                                    implicitWidth: KetTheme.px(16, KetTheme.zoom)
                                    implicitHeight: implicitWidth
                                    KetIcon {
                                        anchors.centerIn: parent
                                        box: Math.min(parent.width, parent.height)
                                        name: KetTheme.iconForLucide("arrow-up"); color: KetTheme.t.textMain; rotation: 180
                                    }
                                }
                                background: Rectangle { color: parent.hovered ? KetTheme.t.surfaceHover : "transparent"; radius: KetTheme.px(4, KetTheme.zoom) }
                                onClicked: transcriptFindBar.view.findNext()
                                Accessible.name: "Next match"
                            }
                            Basic.ToolButton {
                                objectName: "transcriptFindClose"
                                width: KetTheme.px(28, KetTheme.zoom)
                                height: KetTheme.px(28, KetTheme.zoom)
                                padding: KetTheme.px(7, KetTheme.zoom)
                                contentItem: Item {
                                    implicitWidth: KetTheme.px(16, KetTheme.zoom)
                                    implicitHeight: implicitWidth
                                    KetIcon {
                                        anchors.centerIn: parent
                                        box: Math.min(parent.width, parent.height)
                                        name: KetTheme.iconForLucide("plus"); color: KetTheme.t.textMain; rotation: 45
                                    }
                                }
                                background: Rectangle { color: parent.hovered ? KetTheme.t.surfaceHover : "transparent"; radius: KetTheme.px(4, KetTheme.zoom) }
                                onClicked: transcriptFindBar.view.closeFind()
                                Accessible.name: "Close find"
                            }
                        }
                        Text {
                            id: findStatusText
                            objectName: "transcriptFindStatus"
                            x: KetTheme.px(12, KetTheme.zoom)
                            y: findRow.y + findRow.height + KetTheme.px(2, KetTheme.zoom)
                            width: parent.width - KetTheme.px(24, KetTheme.zoom)
                            visible: text.length > 0
                            text: transcriptFindBar.view ? transcriptFindBar.view.findStatus : ""
                            color: KetTheme.t.textMuted
                            font.family: KetTheme.fontFamily
                            font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                            elide: Text.ElideRight
                        }
                    }

                    Component {
                        id: cachedTranscriptFactory

                        TranscriptView {
                            shell: root
                            host: agentHost
                            preview: imagePreview
                            settleTimer: transcriptSettleTimer
                        }
                    }
                }

                BackgroundTaskBar {
                    workspace: root
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: Math.min(parent.width - KetTheme.px(40, KetTheme.zoom), root.conversationColumnWidth)
                    Layout.preferredHeight: implicitHeight
                    Layout.bottomMargin: visible ? KetTheme.px(6, KetTheme.zoom) : 0
                }

                AgentComposer {
                    id: composerArea
                    workspace: root
                    modelPopup: root.modelPicker
                    imagePreview: root.imageViewer
                    usageLimitsPopover: root.limitsPopover
                }
            }

        }
    }
}
