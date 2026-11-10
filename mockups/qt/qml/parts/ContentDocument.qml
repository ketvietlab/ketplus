// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as Basic
import Ket
import KetPlusMock

ColumnLayout {
    id: view
    required property var session
    readonly property var document: session.current
    function px(n) { return KetTheme.px(n, KetTheme.zoom) }
    spacing: 0
    Text {
        Layout.fillWidth: true
        Layout.margins: view.px(12)
        text: view.document ? (view.document.kind === "terminal" ? view.document.title + " · conversation worktree" : view.document.path) : ""
        color: KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: view.px(12)
        elide: Text.ElideMiddle
    }
    Basic.ScrollView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        Basic.TextArea {
            text: view.document ? view.document.body : ""
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.NoWrap
            color: KetTheme.t.textMain
            selectionColor: KetTheme.t.accentSubtle
            font.family: KetTheme.monoFontFamily
            font.pixelSize: view.px(14)
            padding: view.px(12)
            background: null
        }
    }
    KetTextField {
        visible: !!view.document && view.document.kind === "terminal"
        Layout.fillWidth: true
        Layout.margins: view.px(12)
        placeholderText: "Type a demo command…"
        text: view.document ? view.document.draft : ""
        font.family: KetTheme.monoFontFamily
        onTextEdited: view.session.patch(view.document.id, { draft: text })
        onAccepted: view.session.submit(view.document.id)
    }
}
