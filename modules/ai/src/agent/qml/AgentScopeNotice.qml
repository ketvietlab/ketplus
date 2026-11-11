import Ket
import QtQuick
import Ket as Kit
import QtQuick.Layouts

Kit.KetBanner {
    id: notice
    required property var workspace
    readonly property var state: agentHost.presentationState
    readonly property bool folderMissing: state.folder === "missing"
    readonly property bool historyFailed: state.history === "error"
    readonly property bool connectionUnavailable: state.connection === "unavailable"
    readonly property bool shown: folderMissing || historyFailed || connectionUnavailable
    title: folderMissing ? "Worktree folder was deleted" : historyFailed ? "Earlier messages could not be loaded" : "Agent connection unavailable"
    readonly property string detail: folderMissing ? "The agent keeps working from the repository's main worktree. If it recreates a worktree, the conversation moves there." : historyFailed ? "Visible messages remain available. Retry from the history control." : "Your draft and conversation are preserved until the Agent reconnects."

    objectName: "agentScopeNotice"
    visible: shown
    Layout.fillWidth: true
    Layout.leftMargin: Math.max(KetTheme.px(20, KetTheme.zoom), (parent.width - KetTheme.px(792, KetTheme.zoom)) / 2 + KetTheme.px(20, KetTheme.zoom))
    Layout.rightMargin: Layout.leftMargin
    Layout.topMargin: shown ? KetTheme.px(12, KetTheme.zoom) : 0
    Layout.preferredHeight: shown ? implicitHeight : 0
    tone: historyFailed ? Kit.KetTheme.BannerTone.Danger : folderMissing ? Kit.KetTheme.BannerTone.Warning : Kit.KetTheme.BannerTone.Info
    message: detail
    actionText: folderMissing ? "Choose worktree" : ""
    onActionTriggered: agentHost.requestExecutionWorktree()
}
