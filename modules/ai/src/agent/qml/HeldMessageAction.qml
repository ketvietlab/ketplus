import Ket
import QtQuick
import Ket as Kit

Column {
    id: root
    objectName: "heldMessageAction"

    required property string messageId
    readonly property var state: agentHost.heldMessageStates[messageId] || ({})
    readonly property bool pending: !!state.pending
    spacing: Kit.KetTheme.px(4, KetTheme.zoom)
    Component.onCompleted: if (messageId) agentHost.requestHeldMessageState(messageId)
    onMessageIdChanged: if (messageId) agentHost.requestHeldMessageState(messageId)

    Text {
        width: parent.width
        text: root.state.error || (root.state.state === "failed" ? (root.state.reason || "Unable to receive this message.")
            : root.state.can_release
            ? "1 held message · continuing may start an agent turn and use provider quota."
            : root.state.state === "queued" ? "Authorized · waiting for the current turn to finish."
            : root.state.state === "releasing" ? "Receiving the authorized message…"
            : root.state.state === "delivered" || root.state.state === "answered" ? "Message received."
            : root.state.state === "expired" ? "This message has expired."
            : root.pending ? "Checking message status…" : "")
        visible: text.length > 0
        height: visible ? implicitHeight : 0
        color: root.state.error ? KetTheme.t.danger : KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
        wrapMode: Text.Wrap
    }
    Kit.KetButton {
        objectName: "receiveHeldMessageButton"
        visible: !!root.state.can_release || !!root.state.error
        enabled: !root.pending
        text: root.pending ? "Receiving…" : root.state.error ? "Refresh status"
            : root.state.state === "failed" ? "Retry receiving" : "Receive and continue"
        controlSize: Kit.KetTheme.ControlSize.Small
        onClicked: root.state.error ? agentHost.requestHeldMessageState(root.messageId)
                                   : agentHost.releaseHeldMessage(root.messageId)
    }
    Timer {
        objectName: "heldMessageRefreshTimer"
        interval: 1000
        repeat: true
        running: root.visible && root.Window.window !== null && root.Window.window.visible
                 && root.Window.window.visibility !== Window.Minimized && !root.pending &&
                 (root.state.state === "queued" || root.state.state === "releasing")
        onTriggered: agentHost.requestHeldMessageState(root.messageId)
    }
}
