// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket

// Product copy using the same text tokens and density as the Teams mock.
Text {
    color: KetTheme.t.textMain
    font.family: KetTheme.fontFamily
    font.pixelSize: KetTheme.px(13, KetTheme.zoom)
    textFormat: Text.PlainText
    wrapMode: Text.Wrap
    Accessible.name: text
    Accessible.role: Accessible.StaticText
}
