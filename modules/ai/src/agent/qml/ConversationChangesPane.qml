import Ket
import QtQuick
import QtQuick.Controls.Basic as Basic

// The details pane's Changes tab (mock DetailsPane, changes): the worktree's uncommitted files
// as the kit changed-files card, each opening its diff, and Open Source Control.
Rectangle {
    id: pane

    readonly property var files: changesHost.files

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    color: KetTheme.t.pageBg

    Basic.ScrollView {
        id: scroll
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth

        Column {
            x: pane.px(16)
            y: pane.px(16)
            width: scroll.availableWidth - 2 * x
            bottomPadding: pane.px(16)
            spacing: pane.px(14)

            KetChangedFiles {
                objectName: "conversationChangedFiles"
                visible: pane.files.length > 0
                width: parent.width
                files: pane.files
                note: changesHost.note
                onFileActivated: index => changesHost.activateFile(index)
                footer: KetButton {
                    objectName: "changesOpenSourceControl"
                    text: "Open Source Control"
                    controlSize: KetTheme.ControlSize.Small
                    onClicked: changesHost.openSourceControl()
                }
            }
            Text {
                objectName: "conversationChangesEmpty"
                visible: pane.files.length === 0
                width: parent.width
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: qsTr("No uncommitted changes in this worktree.")
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: pane.px(13)
            }
            KetButton {
                visible: pane.files.length === 0
                text: "Open Source Control"
                controlSize: KetTheme.ControlSize.Small
                onClicked: changesHost.openSourceControl()
            }
        }
    }
}
