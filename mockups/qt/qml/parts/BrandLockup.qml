// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket

// size is the wordmark's pixel size, including the caller's zoom.
Item {
    id: lockup
    property string edition: "lite"
    property real size: 17
    property bool dark: KetTheme.dark
    property string stem: "ket"
    property color textColor: KetTheme.t.textMain
    readonly property color brandColor: dark ? "#AD97FA" : "#4332C8"
    readonly property real markSize: size * 1.45
    implicitWidth: markSize + markSize * 0.35 + word.implicitWidth
                   + (edition === "pro" ? size * 0.3 + badge.width : 0)
    implicitHeight: Math.max(markSize, word.implicitHeight, badge.height)
    Image {
        width: lockup.markSize; height: width
        anchors.verticalCenter: parent.verticalCenter
        source: "qrc:/qt/qml/KetPlusMock/assets/ketplus-mark-v2.png"
        sourceSize: Qt.size(width * 2, height * 2)
    }
    Text {
        id: word
        x: lockup.markSize * 1.35
        anchors.verticalCenter: parent.verticalCenter
        textFormat: Text.StyledText
        text: lockup.stem + "<font color='" + lockup.brandColor + "'>Plus</font>"
        color: lockup.textColor
        font.family: "KET Sans"
        font.weight: Font.Bold
        font.pixelSize: lockup.size
        font.letterSpacing: -lockup.size * 0.015
    }
    Rectangle {
        id: badge
        visible: lockup.edition === "pro"
        x: word.x + word.implicitWidth + lockup.size * 0.3
        anchors.verticalCenter: word.verticalCenter
        width: label.implicitWidth + label.font.pixelSize * 0.7
        height: label.implicitHeight + label.font.pixelSize * 0.24
        radius: height * 0.25
        color: "transparent"
        border.color: lockup.brandColor
        border.width: 1.5 * KetTheme.zoom
        Text {
            id: label
            anchors.centerIn: parent
            text: "Pro"
            font.family: "KET Sans"
            font.weight: Font.Bold
            font.pixelSize: lockup.size * 0.6
            color: lockup.brandColor
        }
    }
}
