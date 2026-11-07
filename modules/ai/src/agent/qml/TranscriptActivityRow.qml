import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import "TranscriptRenderers.js" as Renderers

Column {
    id: root

    required property var tokens
    required property var host
    required property string messageKey
    required property var stateStore
    required property string stateKey
    required property string subtype
    required property string eventType
    required property string title
    required property string summary
    required property string target
    required property string status
    required property string command
    required property string cwd
    // What the agent said a command is for; the row reads it first and the
    // command beneath it, the way the phone shows a command.
    property string commandDescription: ""
    // The file a read row names, absolute with an optional `:line`; clicking the
    // title opens it in the editor there.
    property string fileLocation: ""
    required property var exitCode
    required property var durationMs
    required property string body
    required property string detailsData
    required property string payloadData
    required property string detailsRef
    required property string payloadRef
    required property var fileStats
    required property var imageSources
    required property bool running

    // A group may contain many edits. Opening the group must not expand every
    // file list and repeat long paths before the reader asks for one edit.
    property bool expanded: subtype === "imageView" || subtype === "imageGeneration"
    property bool commandExpanded: false
    function restoreExpansion() {
        const saved = stateStore.activityState(stateKey);
        expanded = saved === undefined ? subtype === "imageView" || subtype === "imageGeneration" : saved.expanded;
        gallery.maximumVisible = saved === undefined ? 3 : saved.imageLimit;
        saveExpansion();
    }
    function saveExpansion() {
        stateStore.rememberActivityState(stateKey, {
            expanded: expanded,
            imageLimit: gallery.maximumVisible,
            status: status
        });
    }
    function setExpanded(value) {
        if (!hasDetails)
            return;
        expanded = value;
        if (!value)
            commandExpanded = false;
        saveExpansion();
    }
    onMessageKeyChanged: commandExpanded = false
    onStateKeyChanged: restoreTimer.restart()
    Component.onCompleted: restoreTimer.restart()
    onStatusChanged: restoreTimer.restart()
    Timer {
        id: restoreTimer
        interval: 0
        onTriggered: root.restoreExpansion()
    }
    activeFocusOnTab: hasDetails || opensFile
    Accessible.role: hasDetails || opensFile ? Accessible.Button : Accessible.StaticText
    Accessible.name: displayTitle + ", " + status
    Accessible.description: opensFile ? "Open in editor" : hasDetails ? (expanded ? "Collapse activity details" : "Expand activity details") : ""
    Accessible.onPressAction: opensFile ? openFile() : toggleDetails()
    Keys.onSpacePressed: toggleDetails()
    Keys.onReturnPressed: opensFile ? openFile() : toggleDetails()
    Keys.onRightPressed: setExpanded(true)
    Keys.onLeftPressed: setExpanded(false)
    readonly property var details: parseObject(detailsRef.length > 0 && expanded ? host.resolveTranscriptPayload(detailsRef) : detailsData)
    readonly property var payload: parseObject(payloadRef.length > 0 && expanded ? host.resolveTranscriptPayload(payloadRef) : payloadData)
    readonly property var files: parseArray(fileStats)
    readonly property var images: parseArray(imageSources)
    readonly property string commandOutput: command.length > 0 ? String(details.output || "") : ""
    readonly property bool browserActivity: subtype === "ketplusBrowser"
    readonly property bool browserOpenAction: /^https?:\/\//.test(target) &&
                                               /browser|opened/i.test(displayTitle) &&
                                               /open/i.test(displayTitle)
    readonly property bool hasDetails: !browserActivity && (subtype === "command" || subtype === "fileChange" || subtype === "imageView" || subtype === "imageGeneration" || target.length > 0 || summary.length > 0 || detailsRef.length > 0 || payloadRef.length > 0 || Object.keys(payload).length > 0)
    readonly property bool describedCommand: command.length > 0 && commandDescription.length > 0
    readonly property string singleFileTitle: compact && subtype === "fileChange" && files.length === 1
        ? (files[0].operation || "Edited") + " " + String(files[0].destination || files[0].path || "file").split(/[\\/]/).pop() + (files[0].diffTrimmed ? " · Diff trimmed" : "")
        : ""
    readonly property string displayTitle: (singleFileTitle.length > 0 ? singleFileTitle : describedCommand ? commandDescription : title.length > 0 ? title : (running ? "Working" : "Activity")) + (status === "failed" ? " · Failed" : status === "interrupted" ? " · Interrupted" : "")

    signal imageClicked(url source)
    // Inside an expanded group: no icon and a shorter header, so members read as a plain
    // list under the group's rail.
    property bool compact: false
    // Where the title starts; details and images line up under it.
    readonly property real contentInset: compact ? 0 : Metrics.px(25, tokens)

    function parseObject(text) {
        try {
            return JSON.parse(text || "{}");
        } catch (error) {
            return {};
        }
    }
    function parseArray(text) {
        // Model roles arrive as QVariantList sequences; legacy callers may
        // still pass JSON strings.
        if (text !== undefined && text !== null && typeof text !== "string")
            return text;
        try {
            return JSON.parse(text || "[]");
        } catch (error) {
            return [];
        }
    }
    readonly property bool opensFile: fileLocation.length > 0 && !!host
    function openFile() {
        host.openRelativeFile(fileLocation)
    }
    function toggleDetails() {
        if (hasDetails)
            setExpanded(!expanded);
    }
    function iconSource() {
        return "qrc:/agent/icons/" + Renderers.renderer(subtype).icon + ".svg";
    }
    function detailText() {
        return Renderers.detail(root);
    }

    width: parent ? parent.width : implicitWidth
    spacing: Metrics.px(7, tokens)

    Item {
        id: activityHeader
        objectName: "transcriptActivityHeader"
        width: parent.width
        height: Math.max(Metrics.px(root.describedCommand ? 22 : 32, tokens), activityTitle.implicitHeight)
        // Height of one title line; the icon and chevron centre on the first line.
        FontMetrics {
            id: titleMetrics
            font: activityTitle.font
        }
        readonly property real titleTop: root.expanded ? 0 : Math.max(0, (height - activityTitle.implicitHeight) / 2)
        readonly property real firstLineCenter: titleTop + titleMetrics.height / 2
        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.color: root.activeFocus ? root.tokens.focus : "transparent"
            radius: Metrics.px(4, tokens)
        }

        TintedIcon {
            id: activityIcon
            visible: !root.compact && root.subtype !== "command"
            width: root.compact || root.subtype === "command" ? 0 : Metrics.px(16, tokens)
            height: Metrics.px(16, tokens)
            anchors.left: parent.left
            y: Math.round(activityHeader.firstLineCenter - height / 2)
            source: root.iconSource()
            color: root.running ? root.tokens.accentText : root.tokens.textMuted
        }

        Text {
            id: activityTitle
            objectName: "transcriptActivityTitle"
            anchors.left: activityIcon.right
            anchors.leftMargin: root.compact || root.subtype === "command" ? 0 : Metrics.px(9, tokens)
            anchors.right: browserOpen.left
            anchors.rightMargin: Metrics.px(8, tokens)
            y: activityHeader.titleTop
            // Middle elision only applies to a single line, so a collapsed
            // multi-line command folds onto one; a command is never markup.
            text: root.expanded && root.subtype === "command" && !root.describedCommand
                  ? (root.running ? "Running command" : "Ran command") + (root.status === "failed" ? " · Failed" : root.status === "interrupted" ? " · Interrupted" : "")
                  : root.expanded ? root.displayTitle : root.displayTitle.replace(/\s*\n\s*/g, " ")
            textFormat: Text.PlainText
            color: root.status === "failed" ? root.tokens.danger : titleHover.hovered && root.opensFile ? root.tokens.accentText : root.tokens.textSecondary
            font.underline: titleHover.hovered && root.opensFile
            font.family: root.subtype === "command" && !root.describedCommand ? root.tokens.monoFont : root.tokens.uiFont
            font.pixelSize: root.tokens.body
            font.weight: Font.Medium
            wrapMode: root.expanded ? Text.WrapAnywhere : Text.NoWrap
            elide: root.expanded ? Text.ElideNone : Text.ElideMiddle
            maximumLineCount: root.expanded ? 100000 : 1
            HoverHandler {
                id: titleHover
                cursorShape: root.opensFile ? Qt.PointingHandCursor : Qt.ArrowCursor
            }
        }

        Text {
            id: detailChevron
            objectName: "transcriptActivityChevron"
            anchors.right: parent.right
            y: Math.round(activityHeader.firstLineCenter - height / 2)
            width: root.hasDetails ? Metrics.px(18, tokens) : 0
            visible: root.hasDetails
            text: root.expanded ? "⌄" : "›"
            color: root.tokens.textMuted
            font.family: root.tokens.uiFont
            font.pixelSize: root.tokens.title
            horizontalAlignment: Text.AlignHCenter
        }

        Basic.Button {
            id: browserOpen
            objectName: "transcriptBrowserOpen"
            visible: root.browserOpenAction
            anchors.right: detailChevron.left
            anchors.rightMargin: visible ? Metrics.px(8, tokens) : 0
            y: Math.round(activityHeader.firstLineCenter - height / 2)
            width: visible ? Metrics.px(46, tokens) : 0
            height: Metrics.px(26, tokens)
            text: "Open"
            background: Item {}
            contentItem: Text {
                text: browserOpen.text
                color: root.tokens.accentText
                font.family: root.tokens.uiFont
                font.pixelSize: root.tokens.caption
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            Accessible.name: "Open " + root.target
            onClicked: root.host.openRelativeFile(root.target)
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }

        Rectangle {
            id: runningShine
            objectName: "toolActivityShine"
            visible: root.running && !root.compact
            anchors.verticalCenter: activityIcon.verticalCenter
            width: Metrics.px(4, tokens)
            height: Metrics.px(4, tokens)
            radius: Metrics.px(2, tokens)
            color: root.tokens.accent
        }

        TapHandler {
            enabled: root.visible && root.enabled && (root.hasDetails || root.opensFile)
            // Clicking toggles without taking focus, so no focus outline appears;
            // keyboard focus still shows it. A read row's title opens its file
            // instead, and the chevron still toggles.
            onTapped: function (point) {
                if (root.browserOpenAction && point.position.x >= browserOpen.x)
                    return
                if (root.opensFile && (!root.hasDetails || point.position.x < detailChevron.x - Metrics.px(4, root.tokens)))
                    root.openFile()
                else
                    root.toggleDetails()
            }
        }
        HoverHandler {
            cursorShape: root.hasDetails ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
    }

    Row {
        objectName: "transcriptActivityTarget"
        width: parent.width - root.contentInset
        x: root.contentInset
        visible: root.expanded && !root.browserOpenAction && root.subtype !== "command" && root.subtype !== "fileChange" && !root.describedCommand && root.target.length > 0
        spacing: Metrics.px(8, tokens)
        Text {
            width: parent.width - (openTarget.visible ? openTarget.width + parent.spacing : 0)
            text: root.target
            textFormat: Text.PlainText
            color: root.tokens.textMuted
            font.family: root.tokens.uiFont
            font.pixelSize: root.tokens.caption
            elide: Text.ElideMiddle
        }
        Basic.Button {
            id: openTarget
            visible: /^https?:\/\//.test(root.target)
            text: "Open"
            height: Metrics.px(26, tokens)
            horizontalPadding: Metrics.px(8, tokens)
            verticalPadding: 0
            contentItem: Text {
                text: openTarget.text
                color: root.tokens.accentText
                font.family: root.tokens.uiFont
                font.pixelSize: root.tokens.caption
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: openTarget.hovered ? root.tokens.surfaceHover : "transparent"
                radius: Metrics.px(5, tokens)
            }
            Accessible.name: "Open " + root.target
            onClicked: root.host.openRelativeFile(root.target)
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
        }
    }

    Text {
        objectName: "transcriptBrowserError"
        width: parent.width - root.contentInset
        x: root.contentInset
        visible: root.browserActivity && (root.status === "failed" || root.status === "interrupted")
        text: visible ? Renderers.browserError(root) : ""
        textFormat: Text.PlainText
        wrapMode: Text.WrapAnywhere
        color: root.tokens.danger
        font.family: root.tokens.uiFont
        font.pixelSize: root.tokens.body
    }

    // Visual evidence remains visible independently of the technical receipt.
    Column {
        width: parent.width - root.contentInset
        x: root.contentInset
        visible: root.images.length > 0
        spacing: Metrics.px(6, tokens)
        MessageImageGallery {
            id: gallery
            objectName: "transcriptActivityGallery"
            host: root.host
            width: Math.min(parent.width, preferredWidth)
            enabled: root.visible && root.enabled
            sources: root.images
            tokens: root.tokens
            maximumVisible: 3
            onExpanded: root.saveExpansion()
            showUnavailable: true
            unavailableText: root.images.some(function (source) {
                return String(source).indexOf("ketplus-media-unavailable:") === 0;
            }) ? "Screenshot unavailable" : "Image unavailable"
            onImageClicked: function (source) {
                root.imageClicked(source);
            }
        }
    }

    Column {
        objectName: "transcriptActivityDetails"
        width: parent.width - root.contentInset
        x: root.contentInset
        visible: root.expanded && !root.browserActivity
        spacing: root.subtype === "fileChange" ? 2 : 6

        Item {
            id: commandInputRow
            objectName: "transcriptCommandInputRow"
            width: parent.width
            height: commandInputText.height
            visible: root.subtype === "command" && root.command.length > 0
            readonly property real buttonGap: Metrics.px(8, tokens)
            readonly property bool needsExpansion: root.command.indexOf("\n") >= 0 ||
                                                   commandMeasure.implicitWidth > width - Metrics.px(84, tokens)
            FontMetrics {
                id: commandMetrics
                font: commandInputText.font
            }
            Text {
                id: commandMeasure
                visible: false
                text: root.command.replace(/\s*\n\s*/g, " ")
                font: commandInputText.font
                wrapMode: Text.NoWrap
            }
            TextEdit {
                id: commandInputText
                objectName: "transcriptCommandInput"
                width: parent.width - (commandMore.visible ? commandMore.width + commandInputRow.buttonGap : 0)
                height: root.commandExpanded ? contentHeight : commandMetrics.height
                clip: true
                text: root.command
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                persistentSelection: true
                wrapMode: root.commandExpanded ? TextEdit.WrapAnywhere : TextEdit.NoWrap
                color: root.tokens.textSecondary
                selectionColor: root.tokens.accent
                selectedTextColor: "#ffffff"
                font.family: root.tokens.monoFont
                font.pixelSize: root.tokens.body
                Keys.onShortcutOverride: event => event.accepted = (event.matches(StandardKey.Copy) && selectedText.length > 0) || event.matches(StandardKey.SelectAll)
                // Export the exact selection; Qt rich fragments can prepend a list paragraph.
                Keys.onPressed: event => {
                    if (event.matches(StandardKey.Copy) && selectedText.length > 0) {
                        host.copySelectedText(selectedText);
                        event.accepted = true;
                    }
                }
            }
            Basic.Button {
                id: commandMore
                objectName: "transcriptCommandShowMore"
                visible: commandInputRow.needsExpansion
                anchors.right: parent.right
                y: 0
                width: Metrics.px(76, tokens)
                height: commandMetrics.height
                text: root.commandExpanded ? "Show less" : "Show more"
                background: Item {}
                contentItem: Text {
                    text: commandMore.text
                    color: root.tokens.accentText
                    font.family: root.tokens.uiFont
                    font.pixelSize: root.tokens.caption
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                }
                Accessible.name: text + " command input"
                onClicked: {
                    // This is a reader action, so keep the command's first line in view
                    // instead of following the transcript tail as the row grows.
                    root.stateStore.followTail = false;
                    root.stateStore.streamingTailRequested = false;
                    root.stateStore.runAnchoredMutation(() => root.commandExpanded = !root.commandExpanded);
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }
        }

        TextEdit {
            objectName: "transcriptActivityDetailText"
            // Claim Copy and Select All before the Edit menu can: a read-only TextEdit does not,
            // so macOS fired the menu item and the selected text never reached the clipboard.
            Keys.onShortcutOverride: event => event.accepted = (event.matches(StandardKey.Copy) && selectedText.length > 0) || event.matches(StandardKey.SelectAll)
            // Export the exact selection; Qt rich fragments can prepend a list paragraph.
            Keys.onPressed: event => {
                if (event.matches(StandardKey.Copy) && selectedText.length > 0) {
                    host.copySelectedText(selectedText);
                    event.accepted = true;
                }
            }
            width: parent.width
            height: visible ? contentHeight : 0
            visible: text.length > 0 && root.subtype !== "fileChange"
            text: root.expanded ? root.detailText() : ""
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            persistentSelection: true
            wrapMode: TextEdit.WrapAnywhere
            color: root.tokens.textMuted
            selectionColor: root.tokens.accent
            selectedTextColor: "#ffffff"
            font.family: root.subtype === "command" || root.subtype === "generic" ? root.tokens.monoFont : root.tokens.uiFont
            font.pixelSize: root.tokens.body
        }

        Rectangle {
            id: commandOutputFrame
            objectName: "transcriptCommandOutput"
            width: parent.width
            height: visible ? Math.min(Metrics.px(300, tokens), commandOutputFlick.contentHeight + Metrics.px(16, tokens)) : 0
            visible: root.expanded && root.commandOutput.length > 0
            color: root.tokens.panelBackground || root.tokens.surfaceRaised
            border.color: root.tokens.border
            border.width: 1
            radius: Metrics.px(7, tokens)
            clip: true
            Flickable {
                id: commandOutputFlick
                anchors.fill: parent
                anchors.margins: Metrics.px(8, tokens)
                clip: true
                contentWidth: width
                contentHeight: commandOutputText.contentHeight
                onContentHeightChanged: if (contentY + height >= contentHeight - 24) contentY = Math.max(0, contentHeight - height)
                TextEdit {
                    id: commandOutputText
                    // Claim Copy and Select All before the Edit menu can: a read-only TextEdit does not,
                    // so macOS fired the menu item and the selected text never reached the clipboard.
                    Keys.onShortcutOverride: event => event.accepted = (event.matches(StandardKey.Copy) && selectedText.length > 0) || event.matches(StandardKey.SelectAll)
                    // Export the exact selection; Qt rich fragments can prepend a list paragraph.
                    Keys.onPressed: event => {
                        if (event.matches(StandardKey.Copy) && selectedText.length > 0) {
                            host.copySelectedText(selectedText);
                            event.accepted = true;
                        }
                    }
                    width: commandOutputFlick.width
                    text: root.commandOutput
                    textFormat: TextEdit.PlainText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    color: root.tokens.textMuted
                    selectionColor: root.tokens.accent
                    selectedTextColor: "#ffffff"
                    font.family: root.tokens.monoFont
                    font.pixelSize: root.tokens.body
                }
            }
        }

        Repeater {
            model: root.subtype === "fileChange" ? root.files : []
            delegate: Item {
                id: fileRow
                required property var modelData
                objectName: "changedFileRow"
                width: parent.width
                height: Metrics.px(24, tokens)
                // A rename acts on where the file went; the source path is gone by design.
                readonly property string targetPath: modelData.destination || modelData.path || ""
                // Deleted or moved since the change: drawn in the danger colour and inert.
                readonly property bool missing: {
                    root.host.pathStateRevision;
                    return root.host.pathMissing(targetPath);
                }
                activeFocusOnTab: root.expanded && !missing
                Accessible.role: Accessible.Link
                Accessible.name: (modelData.path || "Changed file") + (missing ? ", no longer exists" : "")
                Accessible.onPressAction: if (!missing) root.host.requestFileDiff(targetPath)
                Keys.onReturnPressed: if (!missing) root.host.requestFileDiff(targetPath)
                Text {
                    id: fileLabel
                    objectName: "changedFileLabel"
                    anchors.left: parent.left
                    anchors.right: fileCounts.left
                    anchors.rightMargin: Metrics.px(8, tokens)
                    anchors.verticalCenter: parent.verticalCenter
                    text: (fileRow.modelData.operation ? fileRow.modelData.operation + " " : "") + root.host.displayFilePath(fileRow.modelData.path || "") + (fileRow.modelData.destination ? " → " + root.host.displayFilePath(fileRow.modelData.destination) : "")
                    color: fileRow.missing ? root.tokens.danger : root.tokens.textSecondary
                    font.family: root.tokens.uiFont
                    font.pixelSize: root.tokens.body
                    elide: Text.ElideMiddle
                }
                Row {
                    id: fileCounts
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Metrics.px(7, tokens)
                    visible: fileRow.modelData.known === true
                    Text {
                        objectName: "fileAdditions"
                        text: "+" + (fileRow.modelData.added || 0)
                        color: root.tokens.positive
                        font.family: root.tokens.monoFont
                        font.pixelSize: root.tokens.body
                    }
                    Text {
                        objectName: "fileDeletions"
                        text: "-" + (fileRow.modelData.removed || 0)
                        color: root.tokens.danger
                        font.family: root.tokens.monoFont
                        font.pixelSize: root.tokens.body
                    }
                }

                MouseArea {
                    id: fileMouse
                    objectName: "changedFileMouseArea"
                    anchors.fill: parent
                    enabled: root.visible && root.enabled
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: fileRow.missing ? Qt.ArrowCursor : Qt.PointingHandCursor
                    onClicked: function (mouse) {
                        if (mouse.button === Qt.RightButton)
                            fileContextMenu.popup();
                        else if (!fileRow.missing)
                            root.host.requestFileDiff(fileRow.targetPath);
                    }
                }

                Basic.Menu {
                    id: fileContextMenu
                    objectName: "changedFileContextMenu"
                    padding: Metrics.px(5, tokens)
                    margins: 8
                    font.family: root.tokens.uiFont
                    font.pixelSize: root.tokens.body
                    background: Rectangle {
                        implicitWidth: Metrics.px(190, tokens)
                        implicitHeight: Metrics.px(112, tokens)
                        color: root.tokens.panelBackground || root.tokens.surfaceRaised
                        border.color: root.tokens.borderStrong || root.tokens.border
                        border.width: 1
                        radius: Metrics.px(7, tokens)
                    }
                    Basic.MenuItem {
                        objectName: "copyChangedFilePathAction"
                        text: "Copy Path"
                        onTriggered: root.host.copyText(root.host.absoluteFilePath(fileRow.targetPath))
                    }
                    Basic.MenuItem {
                        objectName: "revealChangedFileAction"
                        text: root.host.revealFileActionText()
                        enabled: !fileRow.missing
                        onTriggered: root.host.revealFile(fileRow.targetPath)
                    }
                    Basic.MenuItem {
                        objectName: "openChangedFileAction"
                        text: "Open File"
                        enabled: !fileRow.missing
                        onTriggered: root.host.openRelativeFile(fileRow.targetPath)
                    }
                }
                KetToolTip {
                    tokens: root.tokens
                    visible: fileHover.hovered
                    text: root.host.displayFilePath(fileRow.modelData.path || "") + (fileRow.missing ? " — no longer exists" : "")
                }
                HoverHandler {
                    id: fileHover
                }
            }
        }

        Basic.Button {
            id: copyDetailsButton
            objectName: "copyActivityDetails"
            visible: root.subtype !== "fileChange" && (root.command.length > 0 || root.detailText().length > 0)
            text: root.subtype === "command" ? "Copy command" : "Copy details"
            Accessible.name: text
            padding: Metrics.px(6, tokens)
            contentItem: Text {
                text: copyDetailsButton.text
                color: root.tokens.textSecondary
                font.family: root.tokens.uiFont
                font.pixelSize: root.tokens.body
            }
            background: Rectangle {
                color: copyDetailsButton.hovered ? root.tokens.interactiveHover : "transparent"
                border.color: copyDetailsButton.activeFocus ? root.tokens.focus : root.tokens.border
                radius: Metrics.px(4, tokens)
            }
            onClicked: root.host.copyText(root.subtype === "command" ? root.command : root.detailText())
        }
        Basic.Button {
            id: openResource
            objectName: "openActivityResource"
            readonly property string resource: Renderers.resource(root)
            visible: resource.length > 0
            text: "Open resource"
            Accessible.name: text + ": " + resource
            padding: Metrics.px(6, tokens)
            onClicked: root.host.openRelativeFile(resource)
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
            contentItem: Text {
                text: openResource.text
                color: root.tokens.textSecondary
                font.family: root.tokens.uiFont
                font.pixelSize: root.tokens.body
            }
            background: Rectangle {
                color: openResource.hovered ? root.tokens.interactiveHover : "transparent"
                border.color: openResource.activeFocus ? root.tokens.focus : root.tokens.border
                radius: Metrics.px(4, tokens)
            }
        }
    }
}
