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
    // The mock's event: its text on a divider line, the detail centred under it.
    readonly property bool eventLine: subtype === "event" || subtype === "compaction"
    // The mock's reconnect: one quiet line after an indicator.
    readonly property bool reconnecting: subtype === "reconnecting"
    readonly property bool lineOnly: eventLine || reconnecting || subtype === "turnEnd"
    // The mock's turn end: one quiet line, its status in bold and the details after it.
    readonly property bool turnEnd: subtype === "turnEnd"
    readonly property string turnLabel: failed ? "Failed" : status === "completed" ? "Completed" : "Stopped"
    readonly property var recipe: Kit.KetTheme.bannerRecipe(KetTheme.dark, KetTheme.zoom, failed ? Kit.KetTheme.BannerTone.Danger : Kit.KetTheme.BannerTone.Info)
    implicitHeight: eventLine ? eventColumn.implicitHeight : reconnecting ? reconnectRow.implicitHeight : content.implicitHeight + 2 * recipe.paddingBlock
    radius: lineOnly ? 0 : recipe.radius
    color: lineOnly ? "transparent" : recipe.background
    border.color: lineOnly ? "transparent" : recipe.border
    Accessible.role: failed ? Accessible.AlertMessage : Accessible.StaticText
    Accessible.name: title + (message.length ? ". " + message : "")
    Column {
        id: eventColumn
        objectName: "transcriptEventLine"
        visible: notice.eventLine
        width: parent.width
        spacing: KetTheme.px(4, KetTheme.zoom)
        Kit.KetDayDivider {
            objectName: "transcriptEventDivider"
            width: parent.width
            text: notice.title
        }
        Text {
            objectName: "transcriptEventDetail"
            visible: notice.message.length > 0
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: notice.message
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: KetTheme.t.textMuted
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.px(12, KetTheme.zoom)
        }
    }
    Row {
        id: reconnectRow
        objectName: "transcriptReconnectLine"
        visible: notice.reconnecting
        width: parent.width
        spacing: KetTheme.px(8, KetTheme.zoom)
        // Still, as in the mock: a journal row must not hold the shared animation clock.
        Kit.KetActivityIndicator {
            id: reconnectIndicator
            // Centred on the first line when a long message wraps.
            y: Math.max(0, (reconnectMetrics.height - height) / 2)
            color: KetTheme.t.warning
            previewTime: 0
        }
        Text {
            id: reconnectText
            objectName: "transcriptReconnectText"
            width: reconnectRow.width - reconnectIndicator.width - reconnectRow.spacing
            text: notice.message.length > 0 ? notice.message : notice.title
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.px(13, KetTheme.zoom)
            FontMetrics {
                id: reconnectMetrics
                font: reconnectText.font
            }
        }
    }
    RowLayout {
        id: content
        visible: !notice.eventLine && !notice.reconnecting
        x: notice.turnEnd ? 0 : notice.recipe.paddingInline
        y: notice.recipe.paddingBlock
        width: parent.width - 2 * x
        // The turn's end is the mock's status line: a 14 px icon 8 px before its label.
        spacing: notice.turnEnd ? KetTheme.px(8, KetTheme.zoom) : notice.recipe.spacing
        Kit.KetIcon {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: notice.turnEnd ? Math.max(0, (turnLine.lineHeight - box) / 2) : 0
            objectName: "transcriptNoticeIcon"
            // A failure reads as the kit's danger banner, its turn end as the mock's
            // circle-x; the other notices keep their own symbol.
            name: notice.failed && !notice.lineOnly ? notice.recipe.icon : Kit.KetTheme.iconForLucide(notice.turnEnd ? (notice.status === "completed" ? "circle-check" : notice.failed ? "circle-x" : "circle-stop") : notice.subtype === "reconnecting" ? "refresh-cw" : notice.subtype === "compaction" ? "archive" : "info")
            box: notice.turnEnd ? KetTheme.px(14, KetTheme.zoom) : notice.recipe.iconSize
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
