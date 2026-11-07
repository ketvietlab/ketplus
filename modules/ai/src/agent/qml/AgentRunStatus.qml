import "ChromeMetrics.js" as Metrics
import QtQuick
import Ket
import QtQuick.Layouts

Item {
    id: status
    required property var workspace
    objectName: "workingIndicator"
    implicitHeight: visible ? Metrics.px(44, workspace.t) : 0
    Accessible.role: Accessible.StaticText
    Accessible.name: agentHost.sessionStatus || "Agent is working"
    readonly property bool animationAllowed: visible && agentHost.sessionRunning
        && agentHost.workingMotionEnabled && !agentHost.restoring
        && Window.window !== null && Window.window.visible && Window.window.active
        && Window.window.visibility !== Window.Minimized

    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: Metrics.px(9, workspace.t)

        KetActivityIndicator {
            id: workingIcon
            objectName: "workingAnimationIcon"
            Layout.preferredWidth: Metrics.px(16, workspace.t)
            Layout.preferredHeight: Metrics.px(16, workspace.t)
            color: workspace.t.accentHover
            running: status.animationAllowed
        }
        Text {
            objectName: "workingStatusLabel"
            Layout.fillWidth: true
            text: agentHost.sessionStatus || "Agent is working…"
            color: workspace.t.textSecondary
            elide: Text.ElideRight
            font.family: workspace.t.uiFont
            font.pixelSize: workspace.t.reading
        }
    }
}
