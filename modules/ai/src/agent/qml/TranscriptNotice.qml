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
    // A banner's action, as KetBanner's: Retry on the latest failed turn.
    property string actionText
    signal actionTriggered()
    readonly property bool failed: status === "failed"
    readonly property bool lineOnly: subtype === "event" || subtype === "turnEnd"
    // The mock's turn end: one quiet line, its status in bold and the details after it.
    readonly property bool turnEnd: subtype === "turnEnd"
    readonly property string turnLabel: failed ? "Failed" : status === "completed" ? "Completed" : "Stopped"
    readonly property var recipe: Kit.KetTheme.bannerRecipe(KetTheme.dark, KetTheme.zoom, failed ? Kit.KetTheme.BannerTone.Danger : Kit.KetTheme.BannerTone.Info)
    implicitHeight: content.implicitHeight + 2 * recipe.paddingBlock
    radius: lineOnly ? 0 : recipe.radius
    color: lineOnly ? "transparent" : recipe.background
    border.color: lineOnly ? "transparent" : recipe.border
    Accessible.role: failed ? Accessible.AlertMessage : Accessible.StaticText
    Accessible.name: title + (message.length ? ". " + message : "")
    RowLayout {
        id: content
        x: notice.turnEnd ? 0 : notice.recipe.paddingInline
        y: notice.recipe.paddingBlock
        width: parent.width - 2 * x
        spacing: notice.recipe.spacing
        Kit.KetIcon {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: notice.turnEnd ? Math.max(0, (turnLine.lineHeight - notice.recipe.iconSize) / 2) : 0
            objectName: "transcriptNoticeIcon"
            // A failure reads as the kit's danger banner, its turn end as the mock's
            // circle-x; the other notices keep their own symbol.
            name: notice.failed && !notice.lineOnly ? notice.recipe.icon : Kit.KetTheme.iconForLucide(notice.turnEnd ? (notice.status === "completed" ? "circle-check" : notice.failed ? "circle-x" : "circle-stop") : notice.subtype === "reconnecting" ? "refresh-cw" : notice.subtype === "compaction" ? "archive" : "info")
            box: notice.recipe.iconSize
            color: notice.failed ? (notice.lineOnly ? KetTheme.t.danger : notice.recipe.text) : notice.turnEnd && notice.status === "completed" ? KetTheme.t.success : KetTheme.t.textMuted
        }
        Text {
            id: turnLine
            objectName: "transcriptTurnEnd"
            readonly property real lineHeight: fontMetrics.height
            visible: notice.turnEnd
            Layout.fillWidth: true
            text: "<b>" + notice.turnLabel + "</b>" + (notice.message.length ? "&nbsp;&nbsp;" + notice.message.replace(/&/g, "&amp;").replace(/</g, "&lt;") : "")
            textFormat: Text.StyledText
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.px(13, KetTheme.zoom)
            wrapMode: Text.Wrap
            FontMetrics {
                id: fontMetrics
                font: turnLine.font
            }
        }
        ColumnLayout {
            visible: !notice.turnEnd
            Layout.fillWidth: true
            spacing: notice.recipe.messageGap
            Text {
                objectName: "transcriptNoticeTitle"
                Layout.fillWidth: true
                text: notice.title
                textFormat: Text.PlainText
                color: notice.failed ? notice.recipe.text : KetTheme.t.textSecondary
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
                color: notice.failed ? notice.recipe.text : KetTheme.t.textMuted
                font.family: KetTheme.fontFamily
                font.pixelSize: notice.recipe.messagePixels
                selectionColor: KetTheme.t.accentSubtle
                selectedTextColor: KetTheme.t.accentText
            }
        }
        Kit.KetButton {
            objectName: "transcriptNoticeAction"
            Layout.alignment: Qt.AlignVCenter
            visible: notice.actionText.length > 0 && !notice.lineOnly
            text: notice.actionText
            iconName: "refresh"
            controlSize: Kit.KetTheme.ControlSize.Small
            onClicked: notice.actionTriggered()
        }
    }
}
