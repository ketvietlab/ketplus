// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import KetPlusMock

// The conversation's work report: what the agent reported at the end of its turns,
// merged by item key into one ledger. The agent reports structure and status through the
// `report` tool; the app counts, so the numbers always match the items. `report` is
// { title, turn, groups: [{ title, items: [{ key, title, status, change, owner, evidence,
// remaining, next, verified }] }] }. Status is done, active, waiting, blocked or cancelled.
// At the end of a turn the card closes the transcript; while the next turn runs it shows
// that turn's live activity, and the details pane shows the whole ledger (`ledger: true`).
Rectangle {
    id: card

    required property var report
    property bool running: false
    property string activity
    property bool ledger: false
    property var openGroups: report.groups.map(group => group.items.some(item => item.change !== ""
                                                                         || item.status === "active"
                                                                         || item.status === "blocked"))
    property string openItem
    property bool showClosed: true

    signal openRequested()

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function counted(items) { return items.filter(item => item.status !== "cancelled") }
    function done(items) { return counted(items).filter(item => item.status === "done").length }
    readonly property var allItems: report.groups.reduce((all, group) => all.concat(group.items), [])
    readonly property int total: counted(allItems).length
    readonly property int finished: done(allItems)
    readonly property bool needsYou: allItems.some(item => item.status !== "done"
                                                   && item.status !== "cancelled"
                                                   && item.owner === "You")
    function statusIcon(status) {
        return status === "done" ? "success" : status === "active" ? "loading"
            : status === "blocked" ? "error" : status === "cancelled" ? "close" : "clock"
    }
    function statusColor(status) {
        return status === "done" ? KetTheme.t.success : status === "active" ? KetTheme.t.accentText
            : status === "blocked" ? KetTheme.t.danger : KetTheme.t.textMuted
    }
    function statusLabel(status) {
        return ({ done: "Done", active: "In progress", waiting: "Waiting", blocked: "Blocked",
                  cancelled: "Cancelled" })[status]
    }
    function groupStatus(items) {
        const open = counted(items)
        if (open.length > 0 && open.every(item => item.status === "done"))
            return "done"
        if (open.some(item => item.status === "blocked"))
            return "blocked"
        return open.some(item => item.status === "active") ? "active" : "waiting"
    }

    width: parent ? parent.width : implicitWidth
    // In the details pane the pane is the surface: no frame or inset of its own.
    implicitHeight: layout.implicitHeight + (ledger ? 0 : px(32))
    radius: ledger ? 0 : px(7)
    color: ledger ? "transparent" : KetTheme.t.panelBg
    border.color: ledger ? "transparent" : KetTheme.t.panelBorder
    Accessible.role: Accessible.Grouping
    Accessible.name: report.title + ", " + finished + " of " + total + " done"

    Column {
        id: layout
        x: card.ledger ? 0 : card.px(16)
        y: x
        width: parent.width - 2 * x
        spacing: card.px(12)

        // The report's title and the conversation's state.
        Item {
            width: parent.width
            height: Math.max(titleRow.implicitHeight, badge.implicitHeight)
            Row {
                id: titleRow
                width: parent.width - badge.width - card.px(12)
                anchors.verticalCenter: parent.verticalCenter
                spacing: card.px(8)
                KetIcon { name: "bot"; box: card.px(16); color: KetTheme.t.textSecondary; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    width: parent.width - card.px(24)
                    anchors.verticalCenter: parent.verticalCenter
                    text: card.report.title
                    elide: Text.ElideRight
                    color: KetTheme.t.textMain
                    font.family: KetTheme.fontFamily
                    font.pixelSize: card.px(15)
                    font.weight: Font.DemiBold
                }
            }
            KetBadge {
                id: badge
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: card.running ? "Running" : card.finished === card.total ? "All done"
                    : card.needsYou ? "Needs you" : "Turn " + card.report.turn
                tone: card.running ? KetTheme.BadgeTone.Accent
                    : card.finished === card.total ? KetTheme.BadgeTone.Success
                    : card.needsYou ? KetTheme.BadgeTone.Warning : KetTheme.BadgeTone.Neutral
            }
        }

        // Counted by the app from the items.
        Item {
            width: parent.width
            height: Math.max(bar.implicitHeight, count.implicitHeight)
            KetProgressBar {
                id: bar
                anchors.left: parent.left
                anchors.right: count.left
                anchors.rightMargin: card.px(12)
                anchors.verticalCenter: parent.verticalCenter
                from: 0
                to: Math.max(1, card.total)
                value: card.finished
                Accessible.name: "Report progress"
            }
            Row {
                id: count
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: card.px(8)
                Repeater {
                    model: [card.finished + "/" + card.total,
                            Math.round(100 * card.finished / Math.max(1, card.total)) + "%"]
                    Text {
                        required property string modelData
                        text: modelData
                        color: KetTheme.t.textSecondary
                        font.family: KetTheme.fontFamily
                        font.pixelSize: card.px(13)
                        font.features: { "tnum": 1 }
                    }
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: KetTheme.t.panelBorder }

        // The details pane can hide finished and cancelled items.
        KetSegmentedControl {
            visible: card.ledger
            options: ["Open", "All"]
            currentIndex: card.showClosed ? 1 : 0
            onActivated: index => card.showClosed = index === 1
        }

        Repeater {
            model: card.report.groups
            Column {
                id: group
                required property var modelData
                required property int index
                readonly property bool open: card.openGroups[index] === true
                readonly property var shown: modelData.items.filter(item => card.showClosed
                                                 || (item.status !== "done" && item.status !== "cancelled"))
                visible: shown.length > 0
                width: parent.width
                spacing: card.px(2)

                // A group: its state, title and count; it opens and closes its items.
                Rectangle {
                    width: parent.width
                    height: card.px(36)
                    radius: card.px(5)
                    color: groupArea.containsMouse ? KetTheme.t.surfaceHover : "transparent"
                    border.color: groupArea.activeFocus ? KetTheme.t.focusBorder : "transparent"
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: group.modelData.title + ", " + card.done(group.modelData.items)
                                     + " of " + card.counted(group.modelData.items).length + " done"
                    Keys.onSpacePressed: groupArea.toggle()
                    Keys.onReturnPressed: groupArea.toggle()
                    Row {
                        x: card.px(6)
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: card.px(10)
                        KetIcon { name: group.open ? "chevron-down" : "chevron-right"; box: card.px(14); color: KetTheme.t.textMuted; anchors.verticalCenter: parent.verticalCenter }
                        KetIcon {
                            readonly property string state: card.groupStatus(group.modelData.items)
                            name: card.statusIcon(state)
                            box: card.px(16)
                            color: card.statusColor(state)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: group.modelData.title
                            color: KetTheme.t.textMain
                            font.family: KetTheme.fontFamily
                            font.pixelSize: card.px(14)
                            font.weight: Font.Medium
                        }
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: card.px(8)
                        anchors.verticalCenter: parent.verticalCenter
                        text: card.done(group.modelData.items) + "/" + card.counted(group.modelData.items).length
                        color: KetTheme.t.textSecondary
                        font.family: KetTheme.fontFamily
                        font.pixelSize: card.px(13)
                        font.features: { "tnum": 1 }
                    }
                    MouseArea {
                        id: groupArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        function toggle() {
                            const next = card.openGroups.slice()
                            next[group.index] = !group.open
                            card.openGroups = next
                        }
                        onClicked: toggle()
                    }
                }

                // The group's items, each opening its evidence and next step.
                Repeater {
                    model: group.open ? group.shown : []
                    Column {
                        id: entry
                        required property var modelData
                        readonly property bool expanded: card.openItem === modelData.key
                        width: parent.width
                        spacing: 0

                        Rectangle {
                            width: parent.width
                            height: card.px(32)
                            radius: card.px(5)
                            color: entry.expanded ? KetTheme.t.panelBgSubtle
                                : itemArea.containsMouse ? KetTheme.t.surfaceHover : "transparent"
                            border.color: itemArea.activeFocus ? KetTheme.t.focusBorder : "transparent"
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: entry.modelData.title + ", " + card.statusLabel(entry.modelData.status)
                            Keys.onSpacePressed: itemArea.toggle()
                            Keys.onReturnPressed: itemArea.toggle()
                            Row {
                                x: card.px(30)
                                width: parent.width - x - owner.width - card.px(16)
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: card.px(8)
                                KetIcon {
                                    name: card.statusIcon(entry.modelData.status)
                                    box: card.px(14)
                                    color: card.statusColor(entry.modelData.status)
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Text {
                                    width: Math.min(implicitWidth, parent.width - card.px(22) - (change.visible ? change.width + card.px(8) : 0))
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: entry.modelData.title
                                    elide: Text.ElideRight
                                    color: entry.modelData.status === "cancelled" ? KetTheme.t.textMuted : KetTheme.t.textMain
                                    font.strikeout: entry.modelData.status === "cancelled"
                                    font.family: KetTheme.fontFamily
                                    font.pixelSize: card.px(13)
                                }
                                KetBadge {
                                    id: change
                                    visible: entry.modelData.change !== ""
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: entry.modelData.change === "new" ? "New" : "Updated"
                                    tone: KetTheme.BadgeTone.Info
                                }
                            }
                            Text {
                                id: owner
                                anchors.right: parent.right
                                anchors.rightMargin: card.px(8)
                                anchors.verticalCenter: parent.verticalCenter
                                text: entry.modelData.status === "done" || entry.modelData.status === "cancelled"
                                      ? card.statusLabel(entry.modelData.status) : entry.modelData.owner
                                color: entry.modelData.owner === "You" && entry.modelData.status !== "done"
                                       ? KetTheme.t.warning : KetTheme.t.textMuted
                                font.family: KetTheme.fontFamily
                                font.pixelSize: card.px(12)
                            }
                            MouseArea {
                                id: itemArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                function toggle() { card.openItem = entry.expanded ? "" : entry.modelData.key }
                                onClicked: toggle()
                            }
                        }

                        // Evidence, what remains, the next step and what was verified.
                        Column {
                            visible: entry.expanded
                            x: card.px(52)
                            width: parent.width - x - card.px(8)
                            topPadding: card.px(4)
                            bottomPadding: card.px(10)
                            spacing: card.px(6)
                            Repeater {
                                model: [["Result", entry.modelData.evidence], ["Remaining", entry.modelData.remaining],
                                        ["Next", entry.modelData.next ? entry.modelData.next + " · " + entry.modelData.owner : ""]]
                                Row {
                                    required property var modelData
                                    visible: modelData[1] !== undefined && modelData[1] !== ""
                                    width: parent.width
                                    spacing: card.px(12)
                                    Text {
                                        width: card.px(72)
                                        text: parent.modelData[0]
                                        color: KetTheme.t.textMuted
                                        font.family: KetTheme.fontFamily
                                        font.pixelSize: card.px(12)
                                    }
                                    Text {
                                        width: parent.width - card.px(84)
                                        wrapMode: Text.Wrap
                                        text: parent.modelData[1] || ""
                                        color: KetTheme.t.textSecondary
                                        font.family: KetTheme.fontFamily
                                        font.pixelSize: card.px(12)
                                    }
                                }
                            }
                            Row {
                                visible: (entry.modelData.verified || []).length > 0
                                width: parent.width
                                spacing: card.px(12)
                                Text {
                                    width: card.px(72)
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "Verified"
                                    color: KetTheme.t.textMuted
                                    font.family: KetTheme.fontFamily
                                    font.pixelSize: card.px(12)
                                }
                                Row {
                                    spacing: card.px(6)
                                    Repeater {
                                        model: entry.modelData.verified || []
                                        KetBadge { required property string modelData; text: modelData; tone: KetTheme.BadgeTone.Success }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: KetTheme.t.panelBorder }

        // While a turn runs: its live activity. Otherwise the next step and who takes it.
        Item {
            readonly property var next: card.allItems.find(item => item.status === "active" || item.status === "blocked")
                                        || card.allItems.find(item => item.status === "waiting")
            width: parent.width
            height: card.px(20)
            Row {
                width: parent.width - when.width - card.px(12)
                anchors.verticalCenter: parent.verticalCenter
                spacing: card.px(8)
                KetActivityIndicator { visible: card.running; anchors.verticalCenter: parent.verticalCenter }
                KetIcon { visible: !card.running && !card.ledger; name: "chevron-right"; box: card.px(14); color: KetTheme.t.textMuted; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    width: parent.width - card.px(24)
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: card.running ? card.activity
                        : parent.parent.next ? "Next · " + parent.parent.next.next + " · " + parent.parent.next.owner
                        : "Nothing left to do"
                    color: KetTheme.t.textSecondary
                    font.family: KetTheme.fontFamily
                    font.pixelSize: card.px(13)
                }
            }
            Text {
                id: when
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: card.running ? "Now" : card.ledger ? "Updated in turn " + card.report.turn : ""
                color: KetTheme.t.textMuted
                font.family: KetTheme.fontFamily
                font.pixelSize: card.px(12)
            }
            MouseArea {
                visible: !card.ledger && !card.running
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: card.openRequested()
            }
        }
    }
}
