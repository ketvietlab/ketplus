// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket

// The miniature app inside a map node. The Atlas owns the shared preview state.
Rectangle {
    id: preview

    required property var node
    required property int step
    required property bool current
    required property string trail
    required property string savedNote
    signal trailSelected(string name)
    signal advanced()
    signal saved(string note)
    signal activated()

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    implicitHeight: Math.max(px(344), nodeBody.y + nodeBody.implicitHeight + nodeFooter.height + px(12))
    radius: px(9)
    color: KetTheme.t.panelBg
    border.color: current ? KetTheme.t.accentBorder : KetTheme.t.panelBorder
    border.width: current ? 2 : 1

    component Caption: Text {
        color: KetTheme.t.textSecondary
        font.family: KetTheme.fontFamily
        font.pixelSize: preview.px(11)
        wrapMode: Text.WordWrap
    }

    component TrailChoice: KetTreeItem {
        width: parent.width
        iconName: checked ? "check" : "download"
        checked: text === preview.trail
        onClicked: preview.trailSelected(text)
    }

    Item {
        width: parent.width
        height: preview.px(42)
        Row {
            x: preview.px(12)
            anchors.verticalCenter: parent.verticalCenter
            spacing: preview.px(8)
            Text {
                text: String(preview.step + 1).padStart(2, "0")
                color: KetTheme.t.textMuted
                font.family: KetTheme.monoFontFamily
                font.pixelSize: preview.px(12)
            }
            Text {
                text: preview.node.title
                color: KetTheme.t.textMain
                font.family: KetTheme.fontFamily
                font.pixelSize: preview.px(13)
                font.weight: Font.DemiBold
            }
        }
        Rectangle {
            x: 1
            anchors.bottom: parent.bottom
            width: parent.width - 2
            height: 1
            color: KetTheme.t.panelBorder
        }
    }

    Column {
        id: nodeBody
        x: preview.px(12)
        y: preview.px(54)
        width: parent.width - 2 * x
        spacing: preview.px(9)

        Row {
            spacing: preview.px(5)
            KetIcon {
                name: preview.node.icon
                box: preview.px(13)
                color: KetTheme.t.accentText
                anchors.verticalCenter: parent.verticalCenter
            }
            Caption { text: preview.node.context; color: KetTheme.t.accentText }
        }

        Text {
            width: parent.width
            text: preview.node.heading
            color: KetTheme.t.textMain
            font.family: KetTheme.fontFamily
            font.pixelSize: preview.px(17)
            font.weight: Font.DemiBold
            wrapMode: Text.WordWrap
        }

        Loader {
            width: parent.width
            sourceComponent: ({
                trails: trailsBody, note: noteBody, receipt: receiptBody,
                queue: queueBody, synced: receiptBody
            })[preview.node.kind]
        }
    }

    component NoteSummary: Rectangle {
        required property string title
        required property string detail

        width: parent.width
        height: summary.implicitHeight + preview.px(16)
        radius: preview.px(6)
        color: KetTheme.t.panelBgSubtle
        border.color: KetTheme.t.panelBorder
        Column {
            id: summary
            x: preview.px(8)
            y: preview.px(8)
            width: parent.width - 2 * x
            spacing: preview.px(3)
            Caption { width: parent.width; text: title; color: KetTheme.t.textMain; font.weight: Font.DemiBold }
            Caption { width: parent.width; text: detail; maximumLineCount: 2; elide: Text.ElideRight }
        }
    }

    Component {
        id: trailsBody
        Column {
            spacing: preview.px(8)
            Caption { text: "2 routes available offline" }
            TrailChoice { text: "Ember Ridge Loop"; detail: "5.8 km · saved today" }
            TrailChoice { text: "Copper Pine Pass"; detail: "8.2 km · saved" }
            KetButton {
                text: "Open trail"
                iconName: "chevron-right"
                variant: KetTheme.ButtonVariant.Primary
                controlSize: KetTheme.ControlSize.Small
                onClicked: preview.advanced()
            }
        }
    }

    Component {
        id: noteBody
        Column {
            spacing: preview.px(8)
            Caption { width: parent.width; text: preview.trail + " · 09:42" }
            NoteSummary { title: "Category"; detail: "Trail conditions" }
            KetTextArea {
                id: fieldNote
                width: parent.width
                minimumLines: 2
                maximumLines: 2
                text: preview.savedNote
                Accessible.name: "Field observation"
            }
            KetButton {
                text: "Save offline"
                iconName: "check"
                enabled: fieldNote.text.trim() !== ""
                variant: KetTheme.ButtonVariant.Primary
                controlSize: KetTheme.ControlSize.Small
                onClicked: preview.saved(fieldNote.text.trim())
            }
        }
    }

    Component {
        id: receiptBody
        Column {
            spacing: preview.px(9)
            KetBadge { text: preview.node.status; tone: KetTheme.BadgeTone.Success }
            NoteSummary { title: preview.trail; detail: preview.savedNote }
            Caption { width: parent.width; text: preview.node.detail }
            KetButton {
                text: preview.node.action
                iconName: "chevron-right"
                controlSize: KetTheme.ControlSize.Small
                onClicked: preview.advanced()
            }
        }
    }

    Component {
        id: queueBody
        Column {
            spacing: preview.px(9)
            NoteSummary { title: "1 note ready to sync"; detail: preview.trail }
            Caption { width: parent.width; text: "Local copy retained after upload." }
            KetButton {
                text: "Sync now"
                variant: KetTheme.ButtonVariant.Primary
                controlSize: KetTheme.ControlSize.Small
                onClicked: preview.advanced()
            }
        }
    }

    Item {
        id: nodeFooter
        anchors.bottom: parent.bottom
        width: parent.width
        height: preview.px(40)
        Rectangle { x: 1; width: parent.width - 2; height: 1; color: KetTheme.t.panelBorder }
        Caption {
            x: preview.px(12)
            anchors.verticalCenter: parent.verticalCenter
            text: preview.node.role
        }
        KetButton {
            anchors.right: parent.right
            anchors.rightMargin: preview.px(8)
            anchors.verticalCenter: parent.verticalCenter
            text: preview.current ? "Selected" : "Select"
            variant: KetTheme.ButtonVariant.Ghost
            controlSize: KetTheme.ControlSize.Small
            onClicked: preview.activated()
        }
    }
}
