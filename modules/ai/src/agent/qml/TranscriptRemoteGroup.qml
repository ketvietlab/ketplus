import Ket
import QtQuick
import Ket as Kit
import QtQuick.Controls.Basic as Basic

Column {
    id: root
    objectName: "transcriptRemoteGroup"

    required property var host
    required property var view
    required property string groupKey
    required property string detailsData
    required property string detailsRef
    // The workspace's lightbox, which a screenshot in a grouped row opens like any other.
    required property var preview
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
            onImageClicked: function (source) {
                const all = images.map(image => String(image));
                const index = all.indexOf(source.toString());
                root.preview.showImages(index < 0 ? [source] : all, Math.max(0, index));
            }
        }
    }
    Text {
        width: root.width
        visible: !!root.details.loading || !!root.details.error
        text: root.details.error || "Loading activity details…"
        color: root.details.error ? KetTheme.t.danger : KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
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
