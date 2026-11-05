import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls.Basic as Basic

Grid {
    id: root

    required property var sources
    required property var tokens
    property var host: null
    property int maximumVisible: 3
    property bool showUnavailable: true
    property string unavailableText: "Image unavailable"
    readonly property int imageCount: sources ? sources.length : 0
    readonly property int visibleCount: Math.min(imageCount, maximumVisible)
    readonly property int remainingCount: imageCount - visibleCount
    readonly property int tileCount: visibleCount + (remainingCount > 0 ? 1 : 0)
    signal expanded()
    readonly property real thumbnailSide: 88
    readonly property real gap: 6
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
    height: tileCount > 0 ? Math.ceil(tileCount / columns) * (cellSide + gap) - gap : 0

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
            height: width
            radius: 9
            color: root.tokens.interactiveBackground
            border.width: 1
            border.color: imageHover.hovered || activeFocus ? root.tokens.focus : root.tokens.border
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
                sourceSize.width: 176
                sourceSize.height: 176
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - Metrics.px(8, tokens)
                visible: root.showUnavailable && attachmentImage.status === Image.Error
                objectName: "messageAttachmentUnavailable"
                text: root.unavailableText
                color: root.tokens.textMuted
                font.family: root.tokens.uiFont
                font.pixelSize: root.tokens.body
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
        height: width
        text: "+" + root.remainingCount
        Accessible.name: "Show " + root.remainingCount + " more images"
        onClicked: {
            root.maximumVisible = 2147483647;
            root.expanded();
        }
        contentItem: Text {
            text: moreButton.text
            color: root.tokens.textSecondary
            font.family: root.tokens.uiFont
            font.pixelSize: root.tokens.body
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 9
            color: moreButton.hovered ? root.tokens.interactiveHover : root.tokens.interactiveBackground
            border.width: 1
            border.color: moreButton.activeFocus ? root.tokens.focus : root.tokens.border
        }
    }

}
