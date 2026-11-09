import Ket
import QtQuick
import Ket as Kit

// The mock's note above a turn's end when history retention reduced its detail: what went,
// and the way to the setting that decides it.
Rectangle {
    id: note
    required property string message
    required property var host

    objectName: "transcriptTrimmedDetail"
    implicitHeight: noteColumn.implicitHeight + KetTheme.px(20, KetTheme.zoom)
    radius: KetTheme.px(6, KetTheme.zoom)
    color: KetTheme.t.panelBgSubtle
    border.color: KetTheme.t.panelBorder
    Accessible.role: Accessible.StaticText
    Accessible.name: "Detail trimmed. " + message

    Column {
        id: noteColumn
        x: KetTheme.px(14, KetTheme.zoom)
        y: KetTheme.px(10, KetTheme.zoom)
        width: parent.width - 2 * x
        spacing: KetTheme.px(8, KetTheme.zoom)
        Row {
            width: parent.width
            spacing: KetTheme.px(8, KetTheme.zoom)
            Kit.KetIcon {
                anchors.verticalCenter: parent.verticalCenter
                name: "archive"
                box: KetTheme.px(14, KetTheme.zoom)
                color: KetTheme.t.textMuted
            }
            Text {
                objectName: "transcriptTrimmedDetailText"
                width: parent.width - KetTheme.px(22, KetTheme.zoom)
                wrapMode: Text.Wrap
                // The message is the desktop's own wording, but still escaped for StyledText.
                text: "<b>Detail trimmed</b>&nbsp;&nbsp;" + note.message.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
                textFormat: Text.StyledText
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: KetTheme.px(13, KetTheme.zoom)
            }
        }
        Kit.KetButton {
            objectName: "transcriptStorageSettings"
            x: KetTheme.px(22, KetTheme.zoom)
            text: "Storage settings"
            variant: Kit.KetTheme.ButtonVariant.Ghost
            controlSize: Kit.KetTheme.ControlSize.Small
            onClicked: note.host.requestSettings("storage")
        }
    }
}
