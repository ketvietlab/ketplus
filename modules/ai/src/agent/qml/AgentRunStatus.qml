import QtQuick
import Ket
import QtQuick.Layouts

Item {
    id: status
    required property var workspace
    objectName: "workingIndicator"
    implicitHeight: visible ? KetTheme.px(44, KetTheme.zoom) : 0
    Accessible.role: Accessible.StaticText
    Accessible.name: agentHost.runStatus || "Agent is working"
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
            running: status.animationAllowed
        }
        Text {
            objectName: "workingStatusLabel"
            Layout.fillWidth: true
            text: agentHost.runStatus || "Agent is working…"
            color: KetTheme.t.textSecondary
            elide: Text.ElideRight
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.activityRowRecipe(KetTheme.dark, KetTheme.zoom, false, true, false).fontPixels
        }
    }
}
