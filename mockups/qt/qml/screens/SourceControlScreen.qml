// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// Source Control: the repository's changes and staged changes, and the selected file's diff.
// Git commands stay in the terminal.
Item {
    id: screen

    required property var window
    property string selected: "NavigationRail.qml"
    property int mode: 0 // 0 inline, 1 side by side (shown inline in the mock)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    component Section: Item {
        property string title
        property int count
        property bool open: true
        width: parent.width
        height: screen.px(34)
        Basic.AbstractButton {
            anchors.fill: parent
            onClicked: parent.open = !parent.open
            background: null
            contentItem: Item {
                KetIcon { x: screen.px(12); anchors.verticalCenter: parent.verticalCenter; name: open ? "chevron-down" : "chevron-right"; box: screen.px(12); color: KetTheme.t.textMuted }
                Text { x: screen.px(30); anchors.verticalCenter: parent.verticalCenter; text: title; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13); font.weight: Font.Medium }
                KetBadge { anchors.right: parent.right; anchors.rightMargin: screen.px(12); anchors.verticalCenter: parent.verticalCenter; text: count }
            }
        }
    }

    // Changed files as the Explorer's tree: folders from their paths, a folder with a single
    // subfolder and no files compacted into "src / agent / qml", files under their folder.
    property var closedFolders: []
    function treeRows(files, section) {
        const root = { dirs: {}, files: [] }
        for (const file of files) {
            let node = root
            for (const part of file.path.split("/"))
                node = node.dirs[part] = node.dirs[part] || { dirs: {}, files: [] }
            node.files.push(file)
        }
        const rows = []
        const walk = (node, depth, path) => {
            for (const name of Object.keys(node.dirs).sort()) {
                let child = node.dirs[name]
                let label = name
                let key = path + "/" + name
                while (child.files.length === 0 && Object.keys(child.dirs).length === 1) {
                    const only = Object.keys(child.dirs)[0]
                    label += " / " + only
                    key += "/" + only
                    child = child.dirs[only]
                }
                const open = !screen.closedFolders.includes(section + key)
                rows.push({ folder: true, name: label, depth: depth, key: section + key, open: open })
                if (open)
                    walk(child, depth + 1, key)
            }
            for (const file of node.files)
                rows.push({ folder: false, name: file.name, depth: depth, status: file.status || "M" })
        }
        walk(root, 0, "")
        return rows
    }

    component ChangeTree: Column {
        property var files: []
        property string section
        width: parent.width
        Repeater {
            model: screen.treeRows(parent.files, parent.section)
            KetFileRow {
                required property var modelData
                width: parent.width
                text: modelData.name
                folder: modelData.folder
                depth: modelData.depth
                status: modelData.folder ? "" : modelData.status
                checked: modelData.folder && modelData.open
                active: !modelData.folder && screen.selected === modelData.name
                onClicked: {
                    if (modelData.folder)
                        screen.closedFolders = modelData.open
                            ? screen.closedFolders.concat([modelData.key])
                            : screen.closedFolders.filter(key => key !== modelData.key)
                    else
                        screen.selected = modelData.name
                }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: screen.px(248) // as wide as the Explorer
            color: KetTheme.t.sidebarBg

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                KetPaneHeader {
                    Layout.fillWidth: true
                    title: "SOURCE CONTROL"
                    KetIconButton { controlSize: KetTheme.ControlSize.Small; iconName: "refresh"; text: "Refresh"; onClicked: screen.window.notify("Status refreshed") }
                }

                Column {
                    Layout.fillWidth: true
                    Layout.margins: screen.px(14)
                    Row {
                        spacing: screen.px(8)
                        KetIcon { name: "folder-open"; box: screen.px(14); color: KetTheme.t.textSecondary; anchors.verticalCenter: parent.verticalCenter }
                        Text { text: screen.window.repository; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(14); font.weight: Font.Medium }
                    }
                    Text { leftPadding: screen.px(22); text: "develop"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
                }

                Column {
                    Layout.fillWidth: true
                    Section { id: changes; title: "Changes"; count: Fixtures.changedFiles.length }
                    ChangeTree { visible: changes.open; files: Fixtures.changedFiles; section: "changes" }
                    Section { id: staged; title: "Staged Changes"; count: Fixtures.stagedFiles.length }
                    ChangeTree { visible: staged.open; files: Fixtures.stagedFiles; section: "staged" }
                }

                Item { Layout.fillHeight: true }

                Column {
                    Layout.fillWidth: true
                    Layout.margins: screen.px(14)
                    spacing: screen.px(10)
                    Text { text: "Run Git commands in the terminal"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(12) }
                    KetButton { text: "Open terminal"; iconName: "terminal"; onClicked: screen.window.notify("Terminal opened in develop") }
                }
            }

            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: KetTheme.t.panelBorder }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                spacing: 0
                KetTabBar {
                    Layout.fillWidth: true
                    KetTabButton { text: screen.selected + "  ·  Working tree"; iconName: "compare"; width: implicitWidth }
                }
                KetButton {
                    text: "Open file"
                    iconName: "arrow-up-right"
                    variant: KetTheme.ButtonVariant.Ghost
                    controlSize: KetTheme.ControlSize.Small
                    onClicked: screen.window.showScreen("editor")
                }
                Item { width: screen.px(8) }
            }

            KetBreadcrumb {
                Layout.fillWidth: true
                path: ["modules", "ai", "src", "agent", "qml"]
                KetSegmentedControl {
                    options: ["Inline", "Side by side"]
                    currentIndex: screen.mode
                    onActivated: index => screen.mode = index
                }
            }

            Basic.ScrollView {
                id: scroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth

                Column {
                    width: scroll.availableWidth
                    Item {
                        width: parent.width
                        height: screen.px(46)
                        Text { x: screen.px(20); anchors.verticalCenter: parent.verticalCenter; text: "Working tree · develop"; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
                        Row {
                            anchors.right: parent.right
                            anchors.rightMargin: screen.px(20)
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: screen.px(8)
                            Text { text: "+28"; color: KetTheme.t.success; font.family: KetTheme.monoFontFamily; font.pixelSize: screen.px(13) }
                            Text { text: "−9"; color: KetTheme.t.danger; font.family: KetTheme.monoFontFamily; font.pixelSize: screen.px(13) }
                        }
                    }
                    KetDiffView {
                        id: diff
                        width: parent.width
                        lines: parseUnified(Fixtures.sampleDiff)
                    }
                    Item {
                        width: parent.width
                        height: screen.px(48)
                        Rectangle { width: parent.width; height: 1; color: KetTheme.t.panelBorder }
                        Row {
                            x: screen.px(20)
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: screen.px(8)
                            KetIcon { name: "check"; box: screen.px(12); color: KetTheme.t.textMuted; anchors.verticalCenter: parent.verticalCenter }
                            Text { text: "End of changes"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
                        }
                        KetButton {
                            anchors.right: parent.right
                            anchors.rightMargin: screen.px(12)
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Open full file"
                            iconName: "arrow-up-right"
                            variant: KetTheme.ButtonVariant.Ghost
                            onClicked: screen.window.showScreen("editor")
                        }
                    }
                }
            }
        }
    }
}
