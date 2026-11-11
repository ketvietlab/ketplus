// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket

// Manage tags, from the header's Add tag menu: every label this Mac offers, with its colour.
// Rename, recolour, remove or add; the list is a draft until Save. A renamed label stays on
// the conversations that have it; a removed one leaves them.
KetDialog {
    id: dialog

    // [{label, color}] as the catalog lists them.
    property var tags: []
    property var draft: []
    property int editingColour: -1
    readonly property var palette: ["#4f6bed", "#c2780a", "#7c5cd6", "#3f8f7a", "#2f8f4e", "#d1453b", "#5b6b7f"]
    readonly property string problem: {
        const seen = {}
        for (const entry of draft) {
            const label = entry.label.trim()
            if (label === "")
                return "A tag needs a name."
            if (seen[label.toLowerCase()])
                return "“" + label + "” is listed twice."
            seen[label.toLowerCase()] = true
        }
        return ""
    }

    // renames: {old label: new label} for labels kept under another name.
    signal saved(var tags, var renames)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function update(index, key, value) {
        const next = draft.slice()
        next[index] = Object.assign({}, next[index], { [key]: value })
        draft = next
    }
    function unusedColour() {
        for (const colour of palette)
            if (!draft.some(entry => entry.color === colour))
                return colour
        return palette[draft.length % palette.length]
    }

    title: "Manage tags"
    onAboutToShow: {
        editingColour = -1
        draft = tags.map(entry => ({ label: entry.label, color: entry.color, original: entry.label }))
    }

    Text {
        Layout.fillWidth: true
        bottomPadding: dialog.px(10)
        text: "Tags are shared by every conversation on this Mac. Renaming a tag keeps it on its conversations; removing it takes it off them."
        color: KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: dialog.px(13)
        wrapMode: Text.Wrap
    }

    Column {
        objectName: "manageTagsRows"
        Layout.fillWidth: true
        spacing: dialog.px(6)

        Repeater {
            model: dialog.draft

            Column {
                id: tagRow
                required property var modelData
                required property int index
                width: parent.width
                spacing: dialog.px(6)

                RowLayout {
                    width: parent.width
                    spacing: dialog.px(8)

                    Basic.AbstractButton {
                        objectName: "manageTagsColour_" + tagRow.index
                        Layout.preferredWidth: dialog.px(28)
                        Layout.preferredHeight: dialog.px(28)
                        hoverEnabled: true
                        Accessible.name: "Colour of " + tagRow.modelData.label
                        onClicked: dialog.editingColour = dialog.editingColour === tagRow.index ? -1 : tagRow.index
                        background: Rectangle {
                            radius: dialog.px(6)
                            color: parent.hovered || dialog.editingColour === tagRow.index ? KetTheme.t.surfaceHover : "transparent"
                        }
                        contentItem: Item {
                            Rectangle {
                                anchors.centerIn: parent
                                width: dialog.px(12)
                                height: width
                                radius: width / 2
                                color: tagRow.modelData.color
                            }
                        }
                    }
                    KetTextField {
                        objectName: "manageTagsLabel_" + tagRow.index
                        Layout.fillWidth: true
                        text: tagRow.modelData.label
                        maximumLength: 64
                        invalid: text.trim() === ""
                        onTextEdited: dialog.update(tagRow.index, "label", text)
                    }
                    KetIconButton {
                        objectName: "manageTagsRemove_" + tagRow.index
                        iconName: "trash"
                        text: "Remove " + tagRow.modelData.label
                        onClicked: {
                            const next = dialog.draft.slice()
                            next.splice(tagRow.index, 1)
                            dialog.editingColour = -1
                            dialog.draft = next
                        }
                    }
                }

                Row {
                    visible: dialog.editingColour === tagRow.index
                    leftPadding: dialog.px(36)
                    spacing: dialog.px(4)
                    Repeater {
                        model: dialog.palette
                        Basic.AbstractButton {
                            required property string modelData
                            readonly property bool current: tagRow.modelData.color === modelData
                            width: dialog.px(24)
                            height: width
                            hoverEnabled: true
                            Accessible.name: "Use colour " + modelData
                            onClicked: {
                                dialog.update(tagRow.index, "color", modelData)
                                dialog.editingColour = -1
                            }
                            background: Rectangle {
                                radius: width / 2
                                color: "transparent"
                                border.width: parent.current ? 2 : parent.hovered ? 1 : 0
                                border.color: parent.current ? KetTheme.t.accent : KetTheme.t.panelBorder
                            }
                            contentItem: Item {
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: dialog.px(14)
                                    height: width
                                    radius: width / 2
                                    color: parent.parent.modelData
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    KetButton {
        objectName: "manageTagsAdd"
        Layout.topMargin: dialog.px(8)
        text: "New tag"
        iconName: "plus"
        variant: KetTheme.ButtonVariant.Ghost
        enabled: dialog.draft.length < 32
        onClicked: dialog.draft = dialog.draft.concat([{ label: "New tag", color: dialog.unusedColour(), original: "" }])
    }

    Text {
        visible: dialog.problem !== ""
        Layout.fillWidth: true
        topPadding: dialog.px(6)
        text: dialog.problem
        color: KetTheme.t.danger
        font.family: KetTheme.fontFamily
        font.pixelSize: dialog.px(12)
        wrapMode: Text.Wrap
    }

    actions: [
        KetButton {
            text: "Cancel"
            variant: KetTheme.ButtonVariant.Ghost
            onClicked: dialog.reject()
        },
        KetButton {
            objectName: "manageTagsSave"
            text: "Save"
            variant: KetTheme.ButtonVariant.Primary
            enabled: dialog.problem === ""
            onClicked: {
                const renames = {}
                for (const entry of dialog.draft)
                    if (entry.original !== "" && entry.original !== entry.label.trim())
                        renames[entry.original] = entry.label.trim()
                dialog.saved(dialog.draft.map(entry => ({ label: entry.label.trim(), color: entry.color })), renames)
                dialog.accept()
            }
        }
    ]
}
