import "ChromeMetrics.js" as Metrics
import QtQuick
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

        TintedIcon {
            id: workingIcon
            objectName: "workingAnimationIcon"
            Layout.preferredWidth: Metrics.px(16, workspace.t)
            Layout.preferredHeight: Metrics.px(16, workspace.t)
            source: "qrc:/agent/icons/zap.svg"
            color: workspace.t.accentHover
            SequentialAnimation on opacity {
                objectName: "workingOpacityAnimator"
                running: status.animationAllowed
                loops: Animation.Infinite
                // Animator-only groups run on the scene graph render thread.
                OpacityAnimator { from: 1; to: 0.56; duration: 520; easing.type: Easing.InOutQuad }
                OpacityAnimator { from: 0.56; to: 1; duration: 520; easing.type: Easing.InOutQuad }
                onStopped: workingIcon.opacity = 1
            }
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
