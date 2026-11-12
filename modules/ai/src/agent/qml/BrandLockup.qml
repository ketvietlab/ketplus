// SPDX-License-Identifier: MPL-2.0
import Ket
import QtQuick

// The approved ketPlus / ketPlus Pro lockup, ported from mockups/qt/qml/parts/BrandLockup.qml:
// the mark at 1.45× the wordmark pixel size, a 0.12× mark gap, `ketPlus` in KÉT Sans Bold 700
// with "Plus" in the accent-text blue, and for Pro a framed "Pro" badge in the same blue.
// `size` is the wordmark's pixel size, including the caller's zoom.
Item {
    id: lockup
    property string edition: "pro"
    property real size: 17
    property color textColor: KetTheme.t.textMain
    readonly property color brandColor: KetTheme.t.accentText
    readonly property real markSize: size * 1.45
    readonly property string productName: edition === "pro" ? "ketPlus Pro" : "ketPlus"
    implicitWidth: markSize * 1.12 + word.implicitWidth
                   + (edition === "pro" ? size * 0.3 + badge.width : 0)
    implicitHeight: Math.max(markSize, word.implicitHeight, badge.height)
    Accessible.role: Accessible.StaticText
    Accessible.name: productName

    FontMetrics { id: wordMetrics; font: word.font }
    Image {
        objectName: "brandLockupMark"
        width: lockup.markSize; height: width
        anchors.verticalCenter: parent.verticalCenter
        source: "qrc:/agent/icons/ketplus-mark.png"
        sourceSize: Qt.size(width * 2, height * 2)
        smooth: true
        mipmap: true
    }
    Text {
        id: word
        objectName: "brandLockupWordmark"
        x: lockup.markSize * 1.12
        anchors.verticalCenter: parent.verticalCenter
        textFormat: Text.StyledText
        text: "ket<font color='" + lockup.brandColor + "'>Plus</font>"
        color: lockup.textColor
        font.family: "KET Sans"
        font.weight: Font.Bold
        font.pixelSize: lockup.size
        font.letterSpacing: -lockup.size * 0.015
        Accessible.ignored: true
    }
    Rectangle {
        id: badge
        objectName: "brandLockupBadge"
        visible: lockup.edition === "pro"
        x: word.x + word.implicitWidth + lockup.size * 0.3
        // Centred on the wordmark's x-height.
        y: word.y + word.baselineOffset - wordMetrics.tightBoundingRect("x").height / 2 - height / 2
        width: label.implicitWidth + label.font.pixelSize * 0.7
        height: label.implicitHeight + label.font.pixelSize * 0.24
        radius: height * 0.25
        color: "transparent"
        border.color: lockup.brandColor
        border.width: 1.5 * KetTheme.zoom
        border.pixelAligned: false
        antialiasing: true
        Text {
            id: label
            anchors.centerIn: parent
            text: "Pro"
            font.family: "KET Sans"
            font.weight: Font.Bold
            font.pixelSize: lockup.size * 0.6
            color: lockup.brandColor
            Accessible.ignored: true
        }
    }
}
