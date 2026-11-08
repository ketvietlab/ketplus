import QtQuick
import Ket
import QtQuick.Layouts

Item {
    id: status
    required property var workspace
    objectName: "workingIndicator"
    implicitHeight: visible ? KetTheme.px(44, KetTheme.zoom) : 0
    Accessible.role: Accessible.StaticText
    Accessible.name: agentHost.sessionStatus || "Agent is working"
    readonly property bool animationAllowed: visible && agentHost.sessionRunning
        && agentHost.workingMotionEnabled && !agentHost.restoring
        && Window.window !== null && Window.window.visible
        && Window.window.visibility !== Window.Minimized

    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: KetTheme.px(9, KetTheme.zoom)

        KetActivityIndicator {
            id: workingIcon
            objectName: "workingAnimationIcon"
            Layout.preferredWidth: KetTheme.px(16, KetTheme.zoom)
            Layout.preferredHeight: KetTheme.px(16, KetTheme.zoom)
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
            font.pixelSize: KetTheme.activityRowRecipe(workspace.t.dark, workspace.t.zoomFactor, false, true, false).fontPixels
        }
    }
}
