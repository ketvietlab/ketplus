import Ket
import QtQuick
import QtQuick.Layouts
import Ket as Kit

Rectangle {
    id: notice

    property string title
    property string message
    property string status
    property string subtype
    readonly property bool failed: status === "failed"
    readonly property bool lineOnly: subtype === "event" || subtype === "turnEnd"
    readonly property var recipe: Kit.KetTheme.bannerRecipe(KetTheme.dark, KetTheme.zoom, failed ? Kit.KetTheme.BannerTone.Danger : Kit.KetTheme.BannerTone.Info)
    implicitHeight: content.implicitHeight + 2 * recipe.paddingBlock
    radius: lineOnly ? 0 : recipe.radius
    color: lineOnly ? "transparent" : recipe.background
    border.color: lineOnly ? "transparent" : recipe.border
    Accessible.role: failed ? Accessible.AlertMessage : Accessible.StaticText
    Accessible.name: title + (message.length ? ". " + message : "")
    RowLayout {
        id: content
        x: notice.recipe.paddingInline
        y: notice.recipe.paddingBlock
        width: parent.width - 2 * x
        spacing: notice.recipe.spacing
        Kit.KetIcon {
            Layout.alignment: Qt.AlignTop
            name: Kit.KetTheme.iconForLucide(notice.failed ? "alert-circle" : notice.subtype === "turnEnd" ? (notice.status === "completed" ? "circle-check" : "circle-stop") : notice.subtype === "reconnecting" ? "refresh-cw" : notice.subtype === "compaction" ? "archive" : "info")
            box: notice.recipe.iconSize
            color: notice.failed ? KetTheme.t.danger : KetTheme.t.textMuted
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: notice.recipe.messageGap
            Text {
                objectName: "transcriptNoticeTitle"
                Layout.fillWidth: true
                text: notice.title
                textFormat: Text.PlainText
                color: notice.failed ? KetTheme.t.danger : KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: notice.recipe.titlePixels
                font.weight: notice.recipe.titleWeight
                wrapMode: Text.Wrap
            }
            TextEdit {
                objectName: "transcriptNoticeBody"
                Layout.fillWidth: true
                visible: text.length > 0
                text: notice.message
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                color: KetTheme.t.textMuted
                font.family: KetTheme.fontFamily
                font.pixelSize: notice.recipe.messagePixels
                selectionColor: KetTheme.t.accentSubtle
                selectedTextColor: KetTheme.t.accentText
            }
        }
    }
}
