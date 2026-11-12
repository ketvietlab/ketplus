// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket

// A page's heading (`.page-heading`): an optional eyebrow, the title, a description and
// right-aligned actions.
Item {
    id: heading

    property string eyebrow
    property string title
    property string description
    default property alias actions: actionRow.data

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    implicitHeight: texts.implicitHeight

    Column {
        id: texts
        width: parent.width - actionRow.width - heading.px(16)
        spacing: heading.px(8)
        Text {
            visible: heading.eyebrow !== ""
            text: heading.eyebrow
            color: KetTheme.t.accentText
            font.family: KetTheme.fontFamily
            font.pixelSize: heading.px(12)
            font.weight: Font.Medium
            font.letterSpacing: heading.px(1.6)
        }
        Text {
            text: heading.title
            color: KetTheme.t.textMain
            font.family: KetTheme.fontFamily
            font.pixelSize: heading.px(30)
            font.weight: Font.DemiBold
        }
        Text {
            width: parent.width
            visible: text !== ""
            wrapMode: Text.Wrap
            text: heading.description
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: heading.px(14)
        }
    }

    Row {
        id: actionRow
        anchors.right: parent.right
        y: heading.px(12)
        spacing: heading.px(8)
    }
}
