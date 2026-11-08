import Ket
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

// A new conversation, as the mock's workspace/welcome draws it: the mark, a question, where
// the Agent works, and starters in two columns at the transcript's reading width. It scrolls on
// its own when a short or zoomed viewport cannot show every starter.
Flickable {
    id: emptyState
    required property var workspace
    readonly property real column: Math.min(width - KetTheme.px(48, KetTheme.zoom), KetTheme.px(760, KetTheme.zoom))
    readonly property string place: {
        const repository = agentHost.repositoryName || "";
        const branch = agentHost.executionBranch || "";
        if (repository && branch) return "Your Agent works in " + repository + " on " + branch + ". ";
        if (repository) return "Your Agent works in " + repository + ". ";
        return "";
    }
    objectName: "agentEmptyState"
    clip: true
    contentWidth: width
    contentHeight: welcome.y + welcome.implicitHeight + KetTheme.px(24, KetTheme.zoom)
    interactive: contentHeight > height
    boundsBehavior: Flickable.StopAtBounds
    Basic.ScrollBar.vertical: Basic.ScrollBar { policy: emptyState.interactive ? Basic.ScrollBar.AsNeeded : Basic.ScrollBar.AlwaysOff }

    Column {
        id: welcome
        x: (emptyState.width - width) / 2
        y: KetTheme.px(24, KetTheme.zoom)
        width: emptyState.column
        spacing: KetTheme.px(18, KetTheme.zoom)
        topPadding: KetTheme.px(90, KetTheme.zoom)

        Image {
            anchors.horizontalCenter: parent.horizontalCenter
            width: KetTheme.px(44, KetTheme.zoom)
            height: width
            sourceSize: Qt.size(width * 2, height * 2)
            source: "qrc:/agent/icons/ketplus-mark-" + (KetTheme.dark ? "dark" : "light") + ".svg"
            fillMode: Image.PreserveAspectFit
            Accessible.name: "KétPlus Pro"
        }
        Text {
            objectName: "agentEmptyStateTitle"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: agentHost.agentAvailable ? "What will you create today?"
                                           : "Codex is not available on this Mac."
            color: KetTheme.t.textMain
            wrapMode: Text.Wrap
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.px(26, KetTheme.zoom)
            font.weight: Font.DemiBold
            Accessible.role: Accessible.Heading
        }
        Text {
            objectName: "agentEmptyStatePlace"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: agentHost.agentAvailable
                  ? emptyState.place + "Ask anything or start from an idea."
                  : "Install or reconnect Codex to continue. Your workspace and drafts are preserved."
            color: KetTheme.t.textSecondary
            wrapMode: Text.Wrap
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.px(14, KetTheme.zoom)
        }
        Grid {
            id: starters
            // Two columns, as in the mock; one when the workspace is too narrow for both.
            readonly property bool paired: emptyState.column >= KetTheme.px(480, KetTheme.zoom)
            anchors.horizontalCenter: parent.horizontalCenter
            columns: paired ? 2 : 1
            spacing: KetTheme.px(10, KetTheme.zoom)
            topPadding: KetTheme.px(12, KetTheme.zoom)
            Repeater {
                // The title becomes the prompt.
                model: [
                    { icon: "layers", title: "Explain this repository",
                      caption: "Architecture, modules and entry points" },
                    { icon: "compare", title: "Review my changes",
                      caption: "Review diffs, find issues and suggest fixes" },
                    { icon: "palette", title: "Design a new screen",
                      caption: "Start an Atlas from a prompt" },
                    { icon: "terminal", title: "Fix the failing tests",
                      caption: "Run the tests, then fix what fails" }
                ]
                delegate: KetSuggestion {
                    required property var modelData
                    objectName: "workspaceSuggestion"
                    width: starters.paired ? (emptyState.column - starters.spacing) / 2 : emptyState.column
                    text: modelData.title
                    caption: modelData.caption
                    iconName: modelData.icon
                    enabled: agentHost.agentAvailable
                    onClicked: emptyState.workspace.useSuggestion(modelData.title)
                }
            }
        }
    }
}
