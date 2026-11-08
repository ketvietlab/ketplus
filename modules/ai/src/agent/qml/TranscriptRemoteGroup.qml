import Ket
import QtQuick
import Ket 1.0 as Kit
import QtQuick.Controls.Basic as Basic

Column {
    id: root
    objectName: "transcriptRemoteGroup"
    required property var tokens
    required property var host
    required property var view
    required property string groupKey
    required property string detailsData
    required property string detailsRef
    readonly property var details: {
        const value = detailsRef.length > 0 ? host.resolveTranscriptPayload(detailsRef) : detailsData;
        try { return JSON.parse(value || "{}"); } catch (_) { return {}; }
    }
    spacing: KetTheme.px(8, KetTheme.zoom)

    Repeater {
        model: root.details.groupItems || []
        delegate: TranscriptActivityRow {
            required property var modelData
            width: root.width
            tokens: root.tokens
            host: root.host
            stateStore: root.view
            stateKey: JSON.stringify([root.view.cacheScope, modelData.messageKey])
            messageKey: modelData.messageKey || ""
            subtype: modelData.subtype || "generic"
            eventType: modelData.eventType || ""
            title: modelData.title || ""
            summary: modelData.summary || ""
            target: modelData.target || ""
            status: modelData.status || ""
            command: modelData.command || ""
            cwd: modelData.cwd || ""
            commandDescription: modelData.commandDescription || ""
            fileLocation: modelData.fileLocation || ""
            exitCode: modelData.exitCode
            durationMs: modelData.durationMs
            body: modelData.body || ""
            detailsData: JSON.stringify(modelData.details || {})
            payloadData: JSON.stringify(modelData.sanitizedPayload || {})
            detailsRef: ""
            payloadRef: ""
            fileStats: modelData.fileChanges || []
            imageSources: modelData.images || []
            running: !!modelData.running
            compact: true
        }
    }
    Text {
        width: root.width
        visible: !!root.details.loading || !!root.details.error
        text: root.details.error || "Loading activity details…"
        color: root.details.error ? root.tokens.danger : root.tokens.textMuted
        font.family: root.tokens.uiFont
        font.pixelSize: root.tokens.caption
        wrapMode: Text.Wrap
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }
    Kit.KetButton {
        visible: !!root.details.error
        text: "Retry"
        controlSize: Kit.KetTheme.ControlSize.Small
        Accessible.name: "Retry loading activity group"
        onClicked: root.view.runAnchoredMutation(() => {
            root.view.messageModel.setGroupExpanded(root.groupKey, false);
            root.view.messageModel.setGroupExpanded(root.groupKey, true);
        })
    }
}
