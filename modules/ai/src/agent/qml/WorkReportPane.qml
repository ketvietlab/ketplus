import Ket
import QtQuick
import QtQuick.Controls.Basic as Basic

// ADR 0106: the details pane's Report tab (mock DetailsPane, report). The whole ledger of the
// active conversation, every item by group, with Open and All filters.
Rectangle {
    id: pane

    readonly property var report: reportHost.report
    readonly property bool hasItems: !!report && !!report.items && report.items.length > 0
    // Open groups and the open item belong to one conversation; another starts from the
    // defaults, even when its groups and keys have the same names.
    readonly property string conversationId: reportHost.conversationId
    onConversationIdChanged: {
        ledgerCard.closedGroups = Object.create(null)
        ledgerCard.openItem = ""
    }

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    color: KetTheme.t.panelBg

    Basic.ScrollView {
        id: scroll
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth

        Column {
            x: pane.px(16)
            y: pane.px(16)
            width: scroll.availableWidth - 2 * x
            bottomPadding: pane.px(16)

            TranscriptWorkReport {
                id: ledgerCard
                objectName: "workReportLedger"
                visible: pane.hasItems
                width: parent.width
                ledger: true
                report: pane.hasItems ? pane.report : ({})
            }
            Text {
                objectName: "workReportEmpty"
                visible: !pane.hasItems
                width: parent.width
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: reportHost.loading ? qsTr("Loading the work report…")
                    : reportHost.message.length > 0 ? reportHost.message
                    : qsTr("No work report yet. When the agent records its work, the report appears here and updates as the conversation goes on.")
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: pane.px(13)
            }
        }
    }
}
