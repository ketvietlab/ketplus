// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket

// A tag on a sidebar row (concept). Every tag is the same neutral grey: in the list, colour
// belongs to a conversation's state alone (working, needs input, failed, unread), and a
// coloured tag beside "Needs input" or "Reply failed" competed with it. The chip is drawn
// from the text colour at a low alpha with a hairline, so its edge stays visible on the
// dark sidebar and on a selected row, and that edge is what lines up with the titles.
// After approval this becomes a neutral variant of ket::tagRecipe in ket-qt.
Rectangle {
    id: tag

    property string text
    property real maxWidth: KetTheme.px(96, KetTheme.zoom)
    readonly property color ink: KetTheme.t.textSecondary
    // The width a label takes as a chip, for a row that counts how many fit.
    function widthOf(advance) { return Math.min(advance + 2 * px(6), maxWidth) }
    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    implicitWidth: widthOf(label.implicitWidth)
    implicitHeight: px(18)
    radius: px(4)
    // A filled chip with a faint edge, not an outlined button.
    color: Qt.rgba(ink.r, ink.g, ink.b, KetTheme.dark ? 0.20 : 0.12)
    border.width: 1
    border.color: Qt.rgba(ink.r, ink.g, ink.b, KetTheme.dark ? 0.16 : 0.10)
    Accessible.name: text

    Text {
        id: label
        anchors.centerIn: parent
        width: Math.min(implicitWidth, tag.maxWidth - 2 * tag.px(6))
        text: tag.text
        elide: Text.ElideRight
        color: tag.ink
        font.family: KetTheme.fontFamily
        font.pixelSize: tag.px(11)
        font.weight: Font.Medium
    }
}
