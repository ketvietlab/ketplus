import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: notice
    required property var workspace
    readonly property var state: agentHost.presentationState
    readonly property bool folderMissing: state.folder === "missing"
    readonly property bool historyFailed: state.history === "error"
    readonly property bool connectionUnavailable: state.connection === "unavailable"
    readonly property bool shown: folderMissing || historyFailed || connectionUnavailable
    readonly property string title: folderMissing ? "Worktree folder was deleted"
                                      : historyFailed ? "Earlier messages could not be loaded"
                                                      : "Agent connection unavailable"
    readonly property string detail: folderMissing
        ? "The agent keeps working from the repository's main worktree. If it recreates a worktree, the conversation moves there."
        : historyFailed ? "Visible messages remain available. Retry from the history control."
                        : "Your draft and conversation are preserved until the Agent reconnects."

    objectName: "agentScopeNotice"
    visible: shown
    Layout.fillWidth: true
    Layout.leftMargin: Math.max(Metrics.px(20, workspace.t), (parent.width - Metrics.px(792, workspace.t)) / 2 + Metrics.px(20, workspace.t))
    Layout.rightMargin: Math.max(Metrics.px(20, workspace.t), (parent.width - Metrics.px(792, workspace.t)) / 2 + Metrics.px(20, workspace.t))
    Layout.topMargin: shown ? Metrics.px(12, workspace.t) : 0
    Layout.preferredHeight: shown ? noticeCopy.implicitHeight + Metrics.px(22, workspace.t) : 0
    radius: Metrics.px(7, workspace.t)
    color: folderMissing ? workspace.t.interactiveBackground : workspace.t.surfaceRaised
    border.color: historyFailed ? workspace.t.danger
                                : folderMissing ? workspace.t.warning : workspace.t.borderStrong
    Accessible.role: Accessible.AlertMessage
    Accessible.name: title + ". " + detail

    RowLayout {
        anchors.fill: parent
        anchors.margins: Metrics.px(11, workspace.t)
        spacing: Metrics.px(10, workspace.t)
        Rectangle {
            Layout.preferredWidth: Metrics.px(8, workspace.t)
            Layout.preferredHeight: Metrics.px(8, workspace.t)
            radius: Metrics.px(4, workspace.t)
            color: notice.historyFailed ? workspace.t.danger
                                        : notice.folderMissing ? workspace.t.warning
                                                               : workspace.t.textMuted
        }
        Column {
            id: noticeCopy
            Layout.fillWidth: true
            spacing: Metrics.px(3, workspace.t)
            Text {
                width: parent.width
                text: notice.title
                color: workspace.t.textMain
                font.family: workspace.t.uiFont
                font.pixelSize: workspace.t.body
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                text: notice.detail + (notice.folderMissing ? ' <a href="worktree">Choose worktree</a>' : '')
                textFormat: Text.RichText
                linkColor: workspace.t.accentText
                onLinkActivated: agentHost.requestExecutionWorktree()
                property string hoveredLink: ""
                onLinkHovered: function(link) { hoveredLink = link }
                HoverHandler {
                    cursorShape: parent.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                }
                color: workspace.t.textMuted
                wrapMode: Text.Wrap
                font.family: workspace.t.uiFont
                font.pixelSize: workspace.t.caption
            }
        }
    }
}
