import Ket
import QtQuick
import QtQuick.Controls.Basic as Basic

Grid {
    id: root

    required property var sources

    property var host: null
    property int maximumVisible: 3
    property bool showUnavailable: true
    property string unavailableText: "Image unavailable"
    readonly property int imageCount: sources ? sources.length : 0
    readonly property int visibleCount: Math.min(imageCount, maximumVisible)
    readonly property int remainingCount: imageCount - visibleCount
    readonly property int tileCount: visibleCount + (remainingCount > 0 ? 1 : 0)
    signal expanded()
    property bool largePreview: false
    readonly property real thumbnailSide: KetTheme.px(largePreview ? 360 : 120, KetTheme.zoom)
    readonly property real thumbnailHeight: KetTheme.px(largePreview ? 225 : 78, KetTheme.zoom)
    readonly property real gap: KetTheme.px(8, KetTheme.zoom)
    readonly property real preferredWidth: tileCount * thumbnailSide + Math.max(0, tileCount - 1) * gap
    readonly property real bubblePreferredWidth: preferredWidth

    signal imageClicked(url source)

    visible: imageCount > 0
    columns: Math.max(1, Math.min(tileCount,
                     Math.floor((width + gap) / (thumbnailSide + gap))))
    columnSpacing: gap
    rowSpacing: gap
    // Fixed cells reserve space before decoding and while delegates are recreated.
    // Source aspect ratio never changes transcript geometry; the viewer uses the original.
    readonly property real cellSide: Math.min(thumbnailSide, Math.max(0, width))
    readonly property real cellHeight: cellSide * thumbnailHeight / thumbnailSide
    height: tileCount > 0 ? Math.ceil(tileCount / columns) * (cellHeight + gap) - gap : 0

    Repeater {
        model: root.sources ? root.sources.slice(0, root.maximumVisible) : []

        delegate: Rectangle {
            id: imageFrame
            required property int index
            required property string modelData
            objectName: "messageAttachmentFrame"
            activeFocusOnTab: visible
            Accessible.role: Accessible.Button
            Accessible.name: "Open image " + (index + 1) + " of " + root.imageCount
            Accessible.description: modelData.split("/").pop()
            Accessible.onPressAction: root.imageClicked(modelData)
            Keys.onReturnPressed: root.imageClicked(modelData)
            Keys.onSpacePressed: root.imageClicked(modelData)
            visible: attachmentImage.status === Image.Loading
                     || attachmentImage.status === Image.Ready
                     || root.showUnavailable
            width: root.cellSide
            height: root.cellHeight
            radius: KetTheme.px(8, KetTheme.zoom)
            color: KetTheme.t.interactiveBg
            border.width: 1
            border.color: imageHover.hovered || activeFocus ? KetTheme.t.colorFocus : KetTheme.t.panelBorder
            clip: true

            Image {
                id: attachmentImage
                objectName: "messageAttachmentImage"
                anchors.fill: parent
                anchors.margins: 1
                source: imageFrame.modelData
                fillMode: Image.PreserveAspectCrop
                horizontalAlignment: Image.AlignHCenter
                autoTransform: true
                asynchronous: true
                cache: false
                sourceSize.width: Math.ceil(root.cellSide * 2)
                sourceSize.height: Math.ceil(root.cellHeight * 2)
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - KetTheme.px(8, KetTheme.zoom)
                visible: root.showUnavailable && attachmentImage.status === Image.Error
                objectName: "messageAttachmentUnavailable"
                text: root.unavailableText
                color: KetTheme.t.textMuted
                font.family: KetTheme.fontFamily
                font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
            }

            HoverHandler {
                id: imageHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: {
                    if (attachmentImage.status !== Image.Error)
                        root.imageClicked(attachmentImage.source);
                }
            }
        }
    }
    Basic.Button {
        id: moreButton
        objectName: "expandMessageImages"
        visible: root.remainingCount > 0
        width: root.cellSide
        height: root.cellHeight
        text: "+" + root.remainingCount
        Accessible.name: "Show " + root.remainingCount + " more images"
        onClicked: {
            root.maximumVisible = 2147483647;
            root.expanded();
        }
        contentItem: Text {
            text: moreButton.text
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: KetTheme.px(8, KetTheme.zoom)
            color: moreButton.hovered ? KetTheme.t.interactiveHover : KetTheme.t.interactiveBg
            border.width: 1
            border.color: moreButton.activeFocus ? KetTheme.t.colorFocus : KetTheme.t.panelBorder
        }
    }

}
