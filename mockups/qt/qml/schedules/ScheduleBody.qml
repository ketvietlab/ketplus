// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket

// Keep the dialog footer reachable at the mock's 1024 × 640 minimum, as Teams does.
Flickable {
    id: body
    default property alias fields: column.data
    Layout.fillWidth: true
    Layout.fillHeight: true
    implicitHeight: column.height
    contentHeight: column.height
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    Basic.ScrollBar.vertical: Basic.ScrollBar {
        policy: body.contentHeight > body.height ? Basic.ScrollBar.AlwaysOn : Basic.ScrollBar.AlwaysOff
    }
    Column {
        id: column
        width: body.width - (body.contentHeight > body.height ? KetTheme.px(14, KetTheme.zoom) : 0)
        spacing: KetTheme.px(14, KetTheme.zoom)
    }
}
