import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket as Kit
import "ChromeMetrics.js" as Metrics

Rectangle {
    id: attachmentChip
    required property var workspace
    required property var imagePreview
    readonly property var root: workspace
    readonly property var recipe: Kit.KetTheme.attachmentChipRecipe(Kit.KetTheme.dark, Kit.KetTheme.zoom, attachmentChipHover.hovered)
    objectName: "composerAttachmentChip"
    required property var modelData
    required property int index
    width: recipe.maxWidth
    height: recipe.height
    radius: recipe.radius
    color: recipe.background
    border.width: modelData.image && attachmentChipHover.hovered ? 1 : 0
    border.color: root.t.focus
    Accessible.role: modelData.image ? Accessible.Button : Accessible.StaticText
    Accessible.name: modelData.image ? "Preview " + (modelData.name || "image") : (modelData.name || "Attachment")
    Accessible.onPressAction: attachmentChip.previewAttachment()
    function previewAttachment() {
        if (!modelData.image || !modelData.url)
            return;
        imagePreview.imageSource = modelData.url;
        imagePreview.open();
    }
    HoverHandler {
        id: attachmentChipHover
        enabled: !!modelData.image
    }
    RowLayout {
        id: chipRow
        anchors.fill: parent
        anchors.leftMargin: recipe.paddingInline
        anchors.rightMargin: recipe.paddingInline
        spacing: recipe.spacing
        Image {
            objectName: "composerAttachmentImage"
            visible: modelData.image
            source: modelData.image ? modelData.url : ""
            sourceSize.width: 56
            sourceSize.height: 56
            Layout.preferredWidth: recipe.iconSize
            Layout.preferredHeight: recipe.iconSize
            fillMode: Image.PreserveAspectCrop
        }
        Text {
            Layout.fillWidth: true
            text: modelData.name
            elide: Text.ElideMiddle
            color: recipe.text
            font.pixelSize: recipe.fontPixels
        }
        Basic.ToolButton {
            id: removeAttachmentButton
            objectName: "removeAttachmentButton"
            z: 2
            contentItem: Kit.KetIcon {
                name: Kit.KetTheme.iconForLucide("x")
                box: attachmentChip.recipe.iconSize
                color: attachmentChip.recipe.text
            }
            Layout.preferredWidth: Metrics.px(24, root.t)
            Layout.preferredHeight: Metrics.px(24, root.t)
            onClicked: agentHost.removeAttachment(index)
            KetToolTip {
                tokens: root.t
                visible: removeAttachmentButton.hovered
                text: "Remove attachment"
            }
        }
    }
    MouseArea {
        id: attachmentPreviewArea
        objectName: "composerAttachmentPreviewArea"
        z: 1
        anchors.fill: parent
        anchors.rightMargin: removeAttachmentButton.width + chipRow.spacing + recipe.paddingInline
        enabled: !!modelData.image
        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: attachmentChip.previewAttachment()
    }
    KetToolTip {
        tokens: root.t
        visible: modelData.image && attachmentChipHover.hovered && !removeAttachmentButton.hovered
        text: "Click to preview"
    }
}
