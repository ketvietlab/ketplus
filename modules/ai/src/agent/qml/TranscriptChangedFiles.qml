import Ket
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket 1.0 as Kit

// Product file identities and recovery actions over the kit ChangedFiles recipe.
Rectangle {
    id: card
    required property var activity
    readonly property var tokens: activity.tokens
    readonly property var recipe: activity.filesRecipe
    implicitHeight: list.implicitHeight + 2 * recipe.paddingBlock
    color: recipe.background
    border.color: recipe.border
    border.width: 1
    radius: recipe.radius
    Accessible.role: Accessible.Grouping
    Accessible.name: activity.files.length + " files changed"
    Column {
        id: list
        x: card.recipe.paddingInline
        y: card.recipe.paddingBlock
        width: parent.width - 2 * x
        Text {
            width: parent.width
            text: activity.files.length === 1 ? "1 file changed" : activity.files.length + " files changed"
            color: card.recipe.heading
            font.family: card.tokens.uiFont
            font.pixelSize: card.recipe.titlePixels
            font.weight: card.recipe.titleWeight
            bottomPadding: card.recipe.paddingBlock
        }
        Repeater {
            model: activity.subtype === "fileChange" ? activity.files : []
            delegate: Item {
                id: fileRow
                required property var modelData
                objectName: "changedFileRow"
                width: parent.width
                height: fileLabel.implicitHeight + 2 * activity.filesRecipe.rowPaddingBlock
                Rectangle {
                    anchors.fill: parent
                    color: fileMouse.containsMouse ? activity.filesRecipe.rowHover : activity.filesRecipe.background
                }
                // A rename acts on where the file went; the source path is gone by design.
                readonly property string targetPath: modelData.destination || modelData.path || ""
                // Deleted or moved since the change: drawn in the danger colour and inert.
                readonly property bool missing: {
                    activity.host.pathStateRevision;
                    return activity.host.pathMissing(targetPath);
                }
                activeFocusOnTab: activity.expanded && !missing
                Accessible.role: Accessible.Link
                Accessible.name: (modelData.path || "Changed file") + (missing ? ", no longer exists" : "")
                Accessible.onPressAction: if (!missing)
                    activity.host.requestFileDiff(targetPath)
                Keys.onReturnPressed: if (!missing)
                    activity.host.requestFileDiff(targetPath)
                Text {
                    id: fileLabel
                    objectName: "changedFileLabel"
                    anchors.left: parent.left
                    anchors.right: fileCounts.left
                    anchors.rightMargin: KetTheme.px(8, KetTheme.zoom)
                    anchors.verticalCenter: parent.verticalCenter
                    text: (fileRow.modelData.operation ? fileRow.modelData.operation + " " : "") + activity.host.displayFilePath(fileRow.modelData.path || "") + (fileRow.modelData.destination ? " → " + activity.host.displayFilePath(fileRow.modelData.destination) : "")
                    color: fileRow.missing ? activity.tokens.danger : activity.filesRecipe.name
                    font.family: activity.tokens.uiFont
                    font.pixelSize: activity.tokens.body
                    elide: Text.ElideMiddle
                }
                Row {
                    id: fileCounts
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: KetTheme.px(7, KetTheme.zoom)
                    visible: fileRow.modelData.known === true
                    Text {
                        objectName: "fileAdditions"
                        text: "+" + (fileRow.modelData.added || 0)
                        color: activity.filesRecipe.added
                        font.family: activity.tokens.uiFont
                        font.pixelSize: activity.tokens.body
                    }
                    Text {
                        objectName: "fileDeletions"
                        text: "-" + (fileRow.modelData.removed || 0)
                        color: activity.filesRecipe.removed
                        font.family: activity.tokens.uiFont
                        font.pixelSize: activity.tokens.body
                    }
                }

                MouseArea {
                    id: fileMouse
                    objectName: "changedFileMouseArea"
                    anchors.fill: parent
                    enabled: activity.visible && activity.enabled
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: fileRow.missing ? Qt.ArrowCursor : Qt.PointingHandCursor
                    onClicked: function (mouse) {
                        if (mouse.button === Qt.RightButton)
                            fileContextMenu.popup();
                        else if (!fileRow.missing)
                            activity.host.requestFileDiff(fileRow.targetPath);
                    }
                }

                Basic.Menu {
                    id: fileContextMenu
                    objectName: "changedFileContextMenu"
                    padding: KetTheme.px(5, KetTheme.zoom)
                    margins: 8
                    font.family: activity.tokens.uiFont
                    font.pixelSize: activity.tokens.body
                    background: Rectangle {
                        implicitWidth: KetTheme.px(190, KetTheme.zoom)
                        implicitHeight: KetTheme.px(112, KetTheme.zoom)
                        color: activity.tokens.panelBackground || activity.tokens.surfaceRaised
                        border.color: activity.tokens.borderStrong || activity.tokens.border
                        border.width: 1
                        radius: KetTheme.px(7, KetTheme.zoom)
                    }
                    Basic.MenuItem {
                        objectName: "copyChangedFilePathAction"
                        text: "Copy Path"
                        onTriggered: activity.host.copyText(activity.host.absoluteFilePath(fileRow.targetPath))
                    }
                    Basic.MenuItem {
                        objectName: "revealChangedFileAction"
                        text: activity.host.revealFileActionText()
                        enabled: !fileRow.missing
                        onTriggered: activity.host.revealFile(fileRow.targetPath)
                    }
                    Basic.MenuItem {
                        objectName: "openChangedFileAction"
                        text: "Open File"
                        enabled: !fileRow.missing
                        onTriggered: activity.host.openRelativeFile(fileRow.targetPath)
                    }
                }
                KetToolTip {
                    objectName: "ketToolTip"
                    visible: fileHover.hovered
                    text: activity.host.displayFilePath(fileRow.modelData.path || "") + (fileRow.missing ? " — no longer exists" : "")
                }
                HoverHandler {
                    id: fileHover
                }
            }
        }
    }
}
