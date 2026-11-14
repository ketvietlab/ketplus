// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Layouts
import Ket

// A failure/skip notification composed from the kit toast and its public content slot.
KetToast {
    id: toast
    property string scheduleId: "ci"
    signal openRequested(string id)
    contentItem: RowLayout {
        spacing: KetTheme.px(8, KetTheme.zoom)
        Accessible.role: Accessible.AlertMessage
        Accessible.name: toast.text
        KetIcon { name: "error"; box: KetTheme.px(16, KetTheme.zoom); color: toast.recipe.text }
        Text {
            Layout.maximumWidth: Math.max(KetTheme.px(180, KetTheme.zoom), Math.min(KetTheme.px(650, KetTheme.zoom), (toast.parent ? toast.parent.width : 900) - KetTheme.px(220, KetTheme.zoom)))
            text: toast.text; wrapMode: Text.Wrap
            color: toast.recipe.text; font.family: KetTheme.fontFamily; font.pixelSize: KetTheme.px(13, KetTheme.zoom)
        }
        KetButton { text: "Open schedule"; onClicked: { toast.close(); toast.openRequested(toast.scheduleId) } }
    }
}
