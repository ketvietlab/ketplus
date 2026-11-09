import Ket
import Ket as Kit
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import "TranscriptRenderers.js" as Renderers

Column {
    id: root

    readonly property var filesRecipe: Kit.KetTheme.changedFilesRecipe(KetTheme.dark, KetTheme.zoom)

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
    // The mock's step line: a short title, then what the step acted on ("Read" and its path,
    // a command's description and the command, or "Ran" and an undescribed command).
    readonly property var titleParts: {
        if (subtype === "command" && command.length > 0)
            return [describedCommand ? commandDescription : running ? "Running" : "Ran", command.replace(/\s*\n\s*/g, " ")];
        // "Edited" and its path with the line counts; a move names both ends.
        if (subtype === "fileChange" && files.length === 1) {
            const file = files[0];
            const where = file.destination ? String(file.path || "") + " → " + file.destination : String(file.path || "file");
            const counts = file.known ? [file.added > 0 ? "+" + file.added : "", file.removed > 0 ? "−" + file.removed : ""].filter(part => part.length > 0).join(" ") : "";
            return [file.operation || "Edited", where + (counts.length > 0 ? "  " + counts : "") + (file.diffTrimmed ? " · Diff trimmed" : "")];
        }
        const text = singleFileTitle.length > 0 ? singleFileTitle : title.length > 0 ? title : (running ? "Working" : "Activity");
        const split = text.indexOf(": ");
        return split > 0 ? [text.slice(0, split), text.slice(split + 2).replace(/\s*\n\s*/g, " ")] : [text, ""];
    }
    readonly property string headline: titleParts[0]
    readonly property string headDetail: titleParts[1]
    // What the title cannot carry: a failure with its exit code and duration, or an interruption.
    readonly property string statusLine: {
        if (status === "interrupted")
            return "Interrupted";
        if (status !== "failed")
            return "";
        const known = value => value !== undefined && value !== null && value !== "";
        const code = Number(exitCode);
        const ms = Number(durationMs);
        const tenths = Math.round(ms / 100) / 10;
        const seconds = Math.round(ms / 1000);
        const duration = ms < 1000 ? Math.round(ms) + " ms" : tenths < 60 ? tenths.toFixed(1) + " s" : Math.floor(seconds / 60) + "m " + String(seconds % 60).padStart(2, "0") + "s";
        return "Failed" + (known(exitCode) && !isNaN(code) && code !== 0 ? " · exit code " + code : "")
            + (known(durationMs) && ms > 0 ? " · " + duration : "");
    }

    signal imageClicked(url source)
    // Inside an expanded group: members read as a list under the group's rail, each with its
    // kind's icon as in the mock.
    property bool compact: false
    // Where the title starts; details and images line up under it.
    readonly property real contentInset: KetTheme.px(25, KetTheme.zoom)

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
    spacing: KetTheme.px(7, KetTheme.zoom)

    TranscriptActivityHeader { activity: root; width: parent.width }

    Text {
        objectName: "transcriptActivityStatus"
        visible: root.statusLine.length > 0
        x: root.contentInset
        width: parent.width - x
        text: root.statusLine
        color: root.status === "failed" ? KetTheme.t.danger : KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: KetTheme.px(12, KetTheme.zoom)
        elide: Text.ElideRight
    }

    Row {
        objectName: "transcriptActivityTarget"
        width: parent.width - root.contentInset
        x: root.contentInset
        visible: root.expanded && !root.browserOpenAction && root.subtype !== "command" && root.subtype !== "fileChange" && !root.describedCommand && root.target.length > 0
        spacing: KetTheme.px(8, KetTheme.zoom)
        Text {
            width: parent.width - (openTarget.visible ? openTarget.width + parent.spacing : 0)
            text: root.target
            textFormat: Text.PlainText
            color: KetTheme.t.textMuted
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
            elide: Text.ElideMiddle
        }
        Basic.Button {
            id: openTarget
            visible: /^https?:\/\//.test(root.target)
            text: "Open"
            height: KetTheme.px(26, KetTheme.zoom)
            horizontalPadding: KetTheme.px(8, KetTheme.zoom)
            verticalPadding: 0
            contentItem: Text {
                text: openTarget.text
                color: KetTheme.t.accentText
                font.family: KetTheme.fontFamily
                font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: openTarget.hovered ? KetTheme.t.surfaceHover : "transparent"
                radius: KetTheme.px(5, KetTheme.zoom)
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
        color: KetTheme.t.danger
        font.family: KetTheme.fontFamily
        font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
    }

    // Visual evidence remains visible independently of the technical receipt.
    Column {
        width: parent.width - root.contentInset
        x: root.contentInset
        visible: root.images.length > 0
        spacing: KetTheme.px(6, KetTheme.zoom)
        MessageImageGallery {
            id: gallery
            objectName: "transcriptActivityGallery"
            largePreview: root.subtype === "imageGeneration"
            host: root.host
            width: Math.min(parent.width, preferredWidth)
            enabled: root.visible && root.enabled
            sources: root.images

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
            readonly property real buttonGap: KetTheme.px(8, KetTheme.zoom)
            readonly property bool needsExpansion: root.command.indexOf("\n") >= 0 ||
                                                   commandMeasure.implicitWidth > width - KetTheme.px(84, KetTheme.zoom)
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
                color: KetTheme.t.textSecondary
                selectionColor: KetTheme.t.accent
                selectedTextColor: KetTheme.t.actionPrimaryInk
                font.family: agentHost.monoFontFamily
                font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
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
                width: KetTheme.px(76, KetTheme.zoom)
                height: commandMetrics.height
                text: root.commandExpanded ? "Show less" : "Show more"
                background: Item {}
                contentItem: Text {
                    text: commandMore.text
                    color: KetTheme.t.accentText
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
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
            color: KetTheme.t.textMuted
            selectionColor: KetTheme.t.accent
            selectedTextColor: KetTheme.t.actionPrimaryInk
            font.family: root.subtype === "command" || root.subtype === "generic" ? agentHost.monoFontFamily : KetTheme.fontFamily
            font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
        }

        Rectangle {
            id: commandOutputFrame
            objectName: "transcriptCommandOutput"
            width: parent.width
            height: visible ? Math.min(KetTheme.px(300, KetTheme.zoom), commandOutputFlick.contentHeight + KetTheme.px(16, KetTheme.zoom)) : 0
            visible: root.expanded && root.commandOutput.length > 0
            color: KetTheme.t.panelBg || KetTheme.t.surfaceRaised
            border.color: KetTheme.t.panelBorder
            border.width: 1
            radius: KetTheme.px(7, KetTheme.zoom)
            clip: true
            Flickable {
                id: commandOutputFlick
                anchors.fill: parent
                anchors.margins: KetTheme.px(8, KetTheme.zoom)
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
                    color: KetTheme.t.textMuted
                    selectionColor: KetTheme.t.accent
                    selectedTextColor: KetTheme.t.actionPrimaryInk
                    font.family: agentHost.monoFontFamily
                    font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                }
            }
        }

        TranscriptChangedFiles {
            activity: root
            width: parent.width
            visible: root.subtype === "fileChange" && root.files.length > 0
            height: visible ? implicitHeight : 0
        }

        Kit.KetButton {
            id: copyDetailsButton
            objectName: "copyActivityDetails"
            variant: Kit.KetTheme.ButtonVariant.Ghost
            controlSize: Kit.KetTheme.ControlSize.Small
            visible: root.subtype !== "fileChange" && (root.command.length > 0 || root.detailText().length > 0)
            text: root.subtype === "command" ? "Copy command" : "Copy details"
            Accessible.name: text
            padding: KetTheme.px(6, KetTheme.zoom)
            contentItem: Text {
                text: copyDetailsButton.text
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
            }
            background: Rectangle {
                color: copyDetailsButton.hovered ? KetTheme.t.interactiveHover : "transparent"
                border.color: copyDetailsButton.activeFocus ? KetTheme.t.colorFocus : KetTheme.t.panelBorder
                radius: KetTheme.px(4, KetTheme.zoom)
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
            padding: KetTheme.px(6, KetTheme.zoom)
            onClicked: root.host.openRelativeFile(resource)
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
            contentItem: Text {
                text: openResource.text
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
            }
            background: Rectangle {
                color: openResource.hovered ? KetTheme.t.interactiveHover : "transparent"
                border.color: openResource.activeFocus ? KetTheme.t.colorFocus : KetTheme.t.panelBorder
                radius: KetTheme.px(4, KetTheme.zoom)
            }
        }
    }
}
