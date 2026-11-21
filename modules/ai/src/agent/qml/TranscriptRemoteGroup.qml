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
    readonly property var groupItems: details.groupItems || []
    spacing: KetTheme.px(8, KetTheme.zoom)

    onGroupItemsChanged: syncSteps()
    Component.onCompleted: syncSteps()

    // Each new page re-parses the whole group. Steps are matched to their rows by key, so a
    // step that arrives adds one row and a step that changes updates its own; the rows already
    // shown are neither destroyed nor laid out again.
    ListModel {
        id: steps
    }
    function stepKey(item, index) {
        return item && item.messageKey ? String(item.messageKey) : "#" + index;
    }
    function syncSteps() {
        const items = groupItems;
        let index = 0;
        for (; index < items.length; ++index) {
            const key = stepKey(items[index], index);
            const data = JSON.stringify(items[index] || {});
            let found = -1;
            for (let probe = index; probe < steps.count; ++probe) {
                if (steps.get(probe).key === key) {
                    found = probe;
                    break;
                }
            }
            if (found < 0) {
                steps.insert(index, { key: key, stepData: data });
                continue;
            }
            if (found > index)
                steps.remove(index, found - index);
            if (steps.get(index).stepData !== data)
                steps.setProperty(index, "stepData", data);
        }
        if (steps.count > index)
            steps.remove(index, steps.count - index);
    }

    Repeater {
        model: steps
        delegate: TranscriptActivityRow {
            required property string stepData
            readonly property var modelData: JSON.parse(stepData)
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
        objectName: "transcriptRemoteGroupStatus"
        width: root.width
        // Reading the next page of an open group leaves its shown steps in place.
        visible: (!!root.details.loading && steps.count === 0) || !!root.details.error
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
