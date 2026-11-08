import Ket
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Item {
    id: emptyState
    required property var workspace
    objectName: "agentEmptyState"

    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width - KetTheme.px(48, KetTheme.zoom), KetTheme.px(520, KetTheme.zoom))
        spacing: KetTheme.px(10, KetTheme.zoom)

        Image {
            anchors.horizontalCenter: parent.horizontalCenter
            width: KetTheme.px(46, KetTheme.zoom)
            height: KetTheme.px(46, KetTheme.zoom)
            source: "qrc:/agent/icons/ketplus-mark-" + (KetTheme.dark ? "dark" : "light") + ".svg"
            fillMode: Image.PreserveAspectFit
            Accessible.name: "KétPlus Pro"
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "YOUR WORKSPACE"
            color: KetTheme.t.textMuted
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
            font.weight: Font.Bold
            font.letterSpacing: 0.7
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: agentHost.agentAvailable ? "Start with an idea."
                                           : "Codex is not available on this Mac."
            color: KetTheme.t.textMain
            wrapMode: Text.Wrap
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.markdownRecipe(KetTheme.dark, KetTheme.zoom).heading2Pixels
            font.weight: Font.DemiBold
            Accessible.role: Accessible.Heading
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: agentHost.agentAvailable
                  ? "Explore code, solve problems, and work with your Agent to bring ideas to life."
                  : "Install or reconnect Codex to continue. Your workspace and drafts are preserved."
            color: KetTheme.t.textMuted
            wrapMode: Text.Wrap
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
        }
        Column {
            width: parent.width
            topPadding: KetTheme.px(6, KetTheme.zoom)
            spacing: KetTheme.px(6, KetTheme.zoom)
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
                    height: KetTheme.px(50, KetTheme.zoom)
                    enabled: agentHost.agentAvailable
                    padding: 0
                    onClicked: workspace.useSuggestion(modelData.title)
                    Accessible.name: modelData.title + ". " + modelData.detail
                    contentItem: RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: KetTheme.px(13, KetTheme.zoom)
                        anchors.rightMargin: KetTheme.px(13, KetTheme.zoom)
                        spacing: KetTheme.px(11, KetTheme.zoom)
                        KetIcon {
                            Layout.preferredWidth: KetTheme.px(17, KetTheme.zoom)
                            Layout.preferredHeight: KetTheme.px(17, KetTheme.zoom)
                            name: KetTheme.iconForLucide(("qrc:/agent/icons/" + suggestion.modelData.icon).toString().split("/").pop().replace(/\.svg$/, ""))
                            color: KetTheme.t.textSecondary
                            box: KetTheme.px(17, KetTheme.zoom)
                        }
                        Column {
                            Layout.fillWidth: true
                            spacing: KetTheme.px(2, KetTheme.zoom)
                            Text {
                                width: parent.width
                                text: suggestion.modelData.title
                                color: KetTheme.t.textMain
                                elide: Text.ElideRight
                                font.family: KetTheme.fontFamily
                                font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                                font.weight: Font.DemiBold
                            }
                            Text {
                                width: parent.width
                                text: suggestion.modelData.detail
                                color: KetTheme.t.textMuted
                                elide: Text.ElideRight
                                font.family: KetTheme.fontFamily
                                font.pixelSize: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
                            }
                        }
                        Text {
                            text: "↗"
                            color: KetTheme.t.textMuted
                            font.family: KetTheme.fontFamily
                            font.pixelSize: KetTheme.markdownRecipe(KetTheme.dark, KetTheme.zoom).bodyPixels
                        }
                    }
                    background: Rectangle {
                        radius: KetTheme.px(7, KetTheme.zoom)
                        color: suggestion.hovered ? KetTheme.t.surfaceHover
                                                  : KetTheme.t.interactiveBg
                        border.color: suggestion.visualFocus ? KetTheme.t.colorFocus
                                                             : KetTheme.t.panelBorder
                    }
                }
            }
        }
    }
}
