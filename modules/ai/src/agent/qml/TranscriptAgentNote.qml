import Ket
import QtQuick
import Ket as Kit

// Shared compact card for incoming and outgoing agent/team messages, and closed questions.
Rectangle {
    id: note
    required property string label
    required property string details
    required property string message
    required property string state
    property string iconName: "send"
    property color iconColor: state === "failed" ? KetTheme.t.danger : KetTheme.t.accentText

    objectName: "transcriptAgentNote"
    implicitHeight: noteRow.implicitHeight + KetTheme.px(20, KetTheme.zoom)
    radius: KetTheme.px(6, KetTheme.zoom)
    color: KetTheme.t.panelBgSubtle
    border.color: KetTheme.t.panelBorder
    Accessible.role: Accessible.StaticText
    Accessible.name: label + ". " + details + (message.length > 0 ? ". " + message : "")

    function escaped(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/\n/g, "<br>");
    }

    property bool externallyManagedExpansion: false
    property bool expanded: false
    property bool hasExtraContent: false
    property bool expandBody: true
    readonly property int previewLines: 4
    readonly property bool overlong: body.contentHeight > previewHeight + 1 || hasExtraContent
    readonly property real previewHeight: noteMetrics.height * previewLines
    signal toggleRequested

    Column {
        id: noteRow
        x: KetTheme.px(14, KetTheme.zoom)
        y: KetTheme.px(10, KetTheme.zoom)
        width: parent.width - 2 * x
        spacing: KetTheme.px(6, KetTheme.zoom)
        Row {
            width: parent.width
            spacing: KetTheme.px(8, KetTheme.zoom)
            Kit.KetIcon {
                name: note.iconName
                box: KetTheme.px(14, KetTheme.zoom)
                color: note.iconColor
            }
            Text {
                objectName: "transcriptAgentNoteText"
                width: parent.width - KetTheme.px(22, KetTheme.zoom)
                wrapMode: Text.Wrap
                text: "<b>" + note.escaped(note.label) + "</b>" + (note.details ? "&nbsp;&nbsp;" + note.escaped(note.details) : "")
                textFormat: Text.StyledText
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: KetTheme.px(13, KetTheme.zoom)
            }
        }
        TextEdit {
            id: body
            objectName: "transcriptAgentNoteBody"
            width: parent.width
            height: note.expanded && note.expandBody ? contentHeight : Math.min(contentHeight, note.previewHeight)
            clip: true
            text: note.message
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.Wrap
            readOnly: true
            selectByMouse: true
            persistentSelection: true
            color: KetTheme.t.textSecondary
            selectionColor: KetTheme.t.accent
            selectedTextColor: KetTheme.t.actionPrimaryInk
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.px(13, KetTheme.zoom)
        }
        Kit.KetButton {
            objectName: "transcriptAgentNoteExpand"
            visible: note.overlong
            height: visible ? implicitHeight : 0
            text: note.expanded ? "Collapse" : "Expand"
            Accessible.name: text + " " + note.label
            variant: Kit.KetTheme.ButtonVariant.Ghost
            controlSize: Kit.KetTheme.ControlSize.Small
            onClicked: {
                if (note.externallyManagedExpansion)
                    note.toggleRequested();
                else
                    note.expanded = !note.expanded;
            }
        }
    }
    FontMetrics {
        id: noteMetrics
        font.family: KetTheme.fontFamily
        font.pixelSize: KetTheme.px(13, KetTheme.zoom)
    }
}
