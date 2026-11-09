import Ket
import QtQuick
import Ket as Kit

// The mock's note for a message this conversation sent to another agent: who it went to, its
// state, and the start of what it said.
Rectangle {
    id: note
    required property string label
    required property string details
    required property string message
    required property string state

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

    Row {
        id: noteRow
        x: KetTheme.px(14, KetTheme.zoom)
        y: KetTheme.px(10, KetTheme.zoom)
        width: parent.width - 2 * x
        spacing: KetTheme.px(8, KetTheme.zoom)
        Kit.KetIcon {
            y: Math.max(0, (noteMetrics.height - height) / 2)
            name: "send"
            box: KetTheme.px(14, KetTheme.zoom)
            color: note.state === "failed" ? KetTheme.t.danger : KetTheme.t.accentText
        }
        Text {
            objectName: "transcriptAgentNoteText"
            width: parent.width - KetTheme.px(22, KetTheme.zoom)
            wrapMode: Text.Wrap
            // The text is another agent's, so every part is escaped for StyledText.
            text: "<b>" + note.escaped(note.label) + "</b>&nbsp;&nbsp;" + note.escaped(note.details) + (note.message.length > 0 ? "<br>" + note.escaped(note.message) : "")
            textFormat: Text.StyledText
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.px(13, KetTheme.zoom)
            FontMetrics {
                id: noteMetrics
                font.family: KetTheme.fontFamily
                font.pixelSize: KetTheme.px(13, KetTheme.zoom)
            }
        }
    }
}
