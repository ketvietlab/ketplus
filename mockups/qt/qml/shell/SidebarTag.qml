// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket

// A tag on a sidebar row (concept). KetTag tints its colour at 13% on both themes; on the
// dark sidebar that chip is as dark as the row, its edge is lost and the label looks
// indented from the titles' edge. Here the dark chip is tinted more, has a hairline of its
// colour and a label lightened from it, so the chip's own left edge is what lines up.
// After approval this becomes a theme-aware ket::tagRecipe in ket-qt.
Rectangle {
    id: tag

    property string text
    property color tagColor: "transparent"
    property bool overflow: false
    property real maxWidth: KetTheme.px(96, KetTheme.zoom)
    readonly property bool dark: KetTheme.dark
    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function mix(a, b, t) {
        return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1)
    }

    implicitWidth: Math.min(label.implicitWidth + 2 * px(6), maxWidth)
    implicitHeight: px(18)
    radius: px(4)
    color: overflow ? KetTheme.t.surfaceHover
                    : Qt.rgba(tagColor.r, tagColor.g, tagColor.b, dark ? 0.30 : 0.13)
    border.width: dark ? 1 : 0
    border.color: overflow ? KetTheme.t.sidebarBorder
                           : Qt.rgba(tagColor.r, tagColor.g, tagColor.b, 0.55)
    Accessible.name: text

    Text {
        id: label
        anchors.centerIn: parent
        width: Math.min(implicitWidth, tag.maxWidth - 2 * tag.px(6))
        text: tag.text
        elide: Text.ElideRight
        color: tag.overflow ? KetTheme.t.textSecondary
               : tag.dark ? tag.mix(tag.tagColor, Qt.rgba(1, 1, 1, 1), 0.72)
               : KetTheme.t.textSecondary
        font.family: KetTheme.fontFamily
        font.pixelSize: tag.px(11)
        font.weight: Font.Medium
    }
}
