import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Item {
    id: emptyState
    required property var workspace
    objectName: "agentEmptyState"

    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width - Metrics.px(48, workspace.t), Metrics.px(520, workspace.t))
        spacing: Metrics.px(10, workspace.t)

        Image {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Metrics.px(46, workspace.t)
            height: Metrics.px(46, workspace.t)
            source: "qrc:/agent/icons/ketplus-mark-" + (workspace.t.dark ? "dark" : "light") + ".svg"
            fillMode: Image.PreserveAspectFit
            Accessible.name: "KétPlus Pro"
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "YOUR WORKSPACE"
            color: workspace.t.textMuted
            font.family: workspace.t.uiFont
            font.pixelSize: workspace.t.micro
            font.weight: Font.Bold
            font.letterSpacing: 0.7
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: agentHost.agentAvailable ? "Start with an idea."
                                           : "Codex is not available on this Mac."
            color: workspace.t.textMain
            wrapMode: Text.Wrap
            font.family: workspace.t.uiFont
            font.pixelSize: workspace.t.heading
            font.weight: Font.DemiBold
            Accessible.role: Accessible.Heading
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: agentHost.agentAvailable
                  ? "Explore code, solve problems, and work with your Agent to bring ideas to life."
                  : "Install or reconnect Codex to continue. Your workspace and drafts are preserved."
            color: workspace.t.textMuted
            wrapMode: Text.Wrap
            font.family: workspace.t.uiFont
            font.pixelSize: workspace.t.body
        }
        Column {
            width: parent.width
            topPadding: Metrics.px(6, workspace.t)
            spacing: Metrics.px(6, workspace.t)
            Repeater {
                model: [
                    { icon: "file-code.svg", title: "Explore the codebase",
                      detail: "Understand the structure and where to start" },
                    { icon: "copy.svg", title: "Review changes",
                      detail: "Review diffs, find issues, and suggest fixes" },
                    { icon: "bot.svg", title: "Design with Atlas",
                      detail: "Turn a brief into interactive screens" }
                ]
                delegate: Basic.Button {
                    id: suggestion
                    required property var modelData
                    objectName: "workspaceSuggestion"
                    width: parent.width
                    height: Metrics.px(50, workspace.t)
                    enabled: agentHost.agentAvailable
                    padding: 0
                    onClicked: workspace.useSuggestion(modelData.title)
                    Accessible.name: modelData.title + ". " + modelData.detail
                    contentItem: RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Metrics.px(13, workspace.t)
                        anchors.rightMargin: Metrics.px(13, workspace.t)
                        spacing: Metrics.px(11, workspace.t)
                        TintedIcon {
                            Layout.preferredWidth: Metrics.px(17, workspace.t)
                            Layout.preferredHeight: Metrics.px(17, workspace.t)
                            source: "qrc:/agent/icons/" + suggestion.modelData.icon
                            color: workspace.t.textSecondary
                        }
                        Column {
                            Layout.fillWidth: true
                            spacing: Metrics.px(2, workspace.t)
                            Text {
                                width: parent.width
                                text: suggestion.modelData.title
                                color: workspace.t.textMain
                                elide: Text.ElideRight
                                font.family: workspace.t.uiFont
                                font.pixelSize: workspace.t.body
                                font.weight: Font.DemiBold
                            }
                            Text {
                                width: parent.width
                                text: suggestion.modelData.detail
                                color: workspace.t.textMuted
                                elide: Text.ElideRight
                                font.family: workspace.t.uiFont
                                font.pixelSize: workspace.t.micro
                            }
                        }
                        Text {
                            text: "↗"
                            color: workspace.t.textMuted
                            font.family: workspace.t.uiFont
                            font.pixelSize: workspace.t.reading
                        }
                    }
                    background: Rectangle {
                        radius: Metrics.px(7, workspace.t)
                        color: suggestion.hovered ? workspace.t.surfaceHover
                                                  : workspace.t.interactiveBackground
                        border.color: suggestion.visualFocus ? workspace.t.focus
                                                             : workspace.t.border
                    }
                }
            }
        }
    }
}
