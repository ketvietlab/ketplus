import Ket
import QtQuick
import Ket as Kit

// ADR 0106: the conversation's work report as the turn ended it (mock TurnReportCard). The
// agent reports items through work_report and the daemon merges them by key. This card groups
// them and computes every count, the progress and the state, so the numbers always match the
// items. `report` is the daemon ledger: { title, turn, items: [{ key, group, title, status,
// evidence, remaining, next: { action, owner }, verified, created_turn, updated_turn }] }.
Rectangle {
    id: card

    property var report: ({})
    // The details pane shows the whole ledger without a frame of its own.
    property bool ledger: false
    property string openItem
    property bool showClosed: true
    readonly property var items: report && report.items ? report.items : []
    readonly property int turn: report && report.turn ? report.turn : 0
    readonly property var groups: {
        const order = []
        const byTitle = {}
        for (const item of items) {
            const title = item.group || "Work"
            if (!byTitle[title]) {
                byTitle[title] = { title: title, items: [] }
                order.push(byTitle[title])
            }
            byTitle[title].items.push(item)
        }
        return order
    }
    // Groups with new, changed, active or blocked items open; finished groups stay closed.
    property var closedGroups: ({})
    readonly property int total: counted(items).length
    readonly property int finished: done(items)
    readonly property bool needsYou: items.some(item => !closed(item) && item.next && item.next.owner === "user")

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function closed(item) { return item.status === "done" || item.status === "cancelled" }
    function counted(list) { return list.filter(item => item.status !== "cancelled") }
    function done(list) { return counted(list).filter(item => item.status === "done").length }
    function change(item) {
        if (item.created_turn === turn)
            return "new"
        return item.updated_turn === turn ? "updated" : ""
    }
    function owner(name) {
        return name === "user" ? qsTr("You") : name === "agent" ? qsTr("Agent")
            : name ? "@agent/" + name : ""
    }
    function check(name) { return name.charAt(0).toUpperCase() + name.slice(1) }
    function statusIcon(status) {
        return status === "done" ? "success" : status === "active" ? "loading"
            : status === "blocked" ? "error" : status === "cancelled" ? "close" : "clock"
    }
    function statusColor(status) {
        return status === "done" ? KetTheme.t.success : status === "active" ? KetTheme.t.accentText
            : status === "blocked" ? KetTheme.t.danger : KetTheme.t.textMuted
    }
    function statusLabel(status) {
        return ({ done: qsTr("Done"), active: qsTr("In progress"), waiting: qsTr("Waiting"),
                  blocked: qsTr("Blocked"), cancelled: qsTr("Cancelled") })[status] || ""
    }
    // A group whose items were all cancelled is cancelled, not waiting.
    function groupStatus(list) {
        const open = counted(list)
        if (open.length === 0)
            return "cancelled"
        if (open.every(item => item.status === "done"))
            return "done"
        if (open.some(item => item.status === "blocked"))
            return "blocked"
        return open.some(item => item.status === "active") ? "active" : "waiting"
    }
    function opensByDefault(list) {
        return list.some(item => change(item) !== "" || item.status === "active" || item.status === "blocked")
    }
    function groupOpen(group) {
        const choice = closedGroups[group.title]
        return choice === undefined ? opensByDefault(group.items) : !choice
    }
    function toggleGroup(group) {
        const next = Object.assign({}, closedGroups)
        next[group.title] = groupOpen(group)
        closedGroups = next
    }

    objectName: "transcriptWorkReport"
    implicitHeight: layout.implicitHeight + (ledger ? 0 : px(32))
    radius: ledger ? 0 : px(7)
    color: ledger ? "transparent" : KetTheme.t.panelBg
    border.color: ledger ? "transparent" : KetTheme.t.panelBorder
    Accessible.role: Accessible.Grouping
    Accessible.name: (report.title || qsTr("Work report")) + ", " + qsTr("%1 of %2 done").arg(finished).arg(total)

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
                Kit.KetIcon { name: "bot"; box: card.px(16); color: KetTheme.t.textSecondary; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    width: parent.width - card.px(24)
                    anchors.verticalCenter: parent.verticalCenter
                    text: card.report.title || qsTr("Work report")
                    elide: Text.ElideRight
                    color: KetTheme.t.textMain
                    font.family: KetTheme.fontFamily
                    font.pixelSize: card.px(15)
                    font.weight: Font.DemiBold
                }
            }
            Kit.KetBadge {
                id: badge
                readonly property bool allDone: card.total > 0 && card.finished === card.total
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: allDone ? qsTr("All done") : card.needsYou ? qsTr("Needs you") : qsTr("Turn %1").arg(card.turn)
                tone: allDone ? KetTheme.BadgeTone.Success
                    : card.needsYou ? KetTheme.BadgeTone.Warning : KetTheme.BadgeTone.Neutral
            }
        }

        // Counted from the items, never from the agent.
        Item {
            width: parent.width
            height: Math.max(bar.implicitHeight, count.implicitHeight)
            Kit.KetProgressBar {
                id: bar
                anchors.left: parent.left
                anchors.right: count.left
                anchors.rightMargin: card.px(12)
                anchors.verticalCenter: parent.verticalCenter
                from: 0
                to: Math.max(1, card.total)
                value: card.finished
                Accessible.name: qsTr("Report progress")
            }
            Row {
                id: count
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: card.px(8)
                Repeater {
                    model: card.total === 0 ? [qsTr("Nothing counted")]
                         : [card.finished + "/" + card.total, Math.round(100 * card.finished / card.total) + "%"]
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

        Kit.KetSegmentedControl {
            visible: card.ledger
            options: [qsTr("Open"), qsTr("All")]
            currentIndex: card.showClosed ? 1 : 0
            onActivated: index => card.showClosed = index === 1
        }

        Repeater {
            model: card.groups
            Column {
                id: group
                required property var modelData
                readonly property bool open: card.groupOpen(modelData)
                readonly property var shown: modelData.items.filter(item => card.showClosed || !card.closed(item))
                visible: shown.length > 0
                width: parent.width
                spacing: card.px(2)

                // A group: its state, title and count; it opens and closes its items.
                Rectangle {
                    id: groupRow
                    readonly property string state: card.groupStatus(group.modelData.items)
                    readonly property int counted: card.counted(group.modelData.items).length
                    width: parent.width
                    height: card.px(36)
                    radius: card.px(5)
                    color: groupArea.containsMouse ? KetTheme.t.surfaceHover : "transparent"
                    border.color: groupRow.activeFocus ? KetTheme.t.focusBorder : "transparent"
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: group.modelData.title + ", " + card.statusLabel(state)
                                     + (counted > 0 ? ", " + qsTr("%1 of %2 done").arg(card.done(group.modelData.items)).arg(counted) : "")
                    Accessible.description: group.open ? qsTr("Expanded") : qsTr("Collapsed")
                    Accessible.onPressAction: card.toggleGroup(group.modelData)
                    Keys.onSpacePressed: card.toggleGroup(group.modelData)
                    Keys.onReturnPressed: card.toggleGroup(group.modelData)
                    Row {
                        x: card.px(6)
                        width: parent.width - x - groupCount.width - card.px(20)
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: card.px(10)
                        Kit.KetIcon { name: group.open ? "chevron-down" : "chevron-right"; box: card.px(14); color: KetTheme.t.textMuted; anchors.verticalCenter: parent.verticalCenter }
                        Kit.KetIcon {
                            name: card.statusIcon(groupRow.state)
                            box: card.px(16)
                            color: card.statusColor(groupRow.state)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            id: groupTitle
                            width: Math.min(implicitWidth, parent.width - card.px(50))
                            anchors.verticalCenter: parent.verticalCenter
                            elide: Text.ElideRight
                            text: group.modelData.title
                            color: KetTheme.t.textMain
                            font.family: KetTheme.fontFamily
                            font.pixelSize: card.px(14)
                            font.weight: Font.Medium
                        }
                    }
                    Text {
                        id: groupCount
                        anchors.right: parent.right
                        anchors.rightMargin: card.px(8)
                        anchors.verticalCenter: parent.verticalCenter
                        text: groupRow.counted > 0 ? card.done(group.modelData.items) + "/" + groupRow.counted : qsTr("Cancelled")
                        color: KetTheme.t.textSecondary
                        font.family: KetTheme.fontFamily
                        font.pixelSize: card.px(13)
                        font.features: { "tnum": 1 }
                    }
                    Kit.KetToolTip { visible: groupArea.containsMouse && groupTitle.truncated; text: group.modelData.title }
                    MouseArea {
                        id: groupArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: card.toggleGroup(group.modelData)
                    }
                }

                // The group's items, each opening its evidence and next step.
                Repeater {
                    model: group.open ? group.shown : []
                    Column {
                        id: entry
                        required property var modelData
                        readonly property bool expanded: card.openItem === modelData.key
                        readonly property string change: card.change(modelData)
                        readonly property var next: modelData.next || null
                        width: parent.width
                        spacing: 0

                        Rectangle {
                            id: itemRow
                            width: parent.width
                            height: card.px(32)
                            radius: card.px(5)
                            color: entry.expanded ? KetTheme.t.panelBgSubtle
                                : itemArea.containsMouse ? KetTheme.t.surfaceHover : "transparent"
                            border.color: itemRow.activeFocus ? KetTheme.t.focusBorder : "transparent"
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: entry.modelData.title + ", " + card.statusLabel(entry.modelData.status)
                                             + (!card.closed(entry.modelData) && entry.next ? ", " + card.owner(entry.next.owner) : "")
                            Accessible.description: entry.expanded ? qsTr("Expanded") : qsTr("Collapsed")
                            Accessible.onPressAction: itemArea.toggle()
                            Keys.onSpacePressed: itemArea.toggle()
                            Keys.onReturnPressed: itemArea.toggle()
                            Row {
                                x: card.px(30)
                                width: parent.width - x - ownerText.width - card.px(16)
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: card.px(8)
                                Kit.KetIcon {
                                    name: card.statusIcon(entry.modelData.status)
                                    box: card.px(14)
                                    color: card.statusColor(entry.modelData.status)
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Text {
                                    id: itemTitle
                                    width: Math.min(implicitWidth, parent.width - card.px(22) - (changeBadge.visible ? changeBadge.width + card.px(8) : 0))
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: entry.modelData.title
                                    elide: Text.ElideRight
                                    color: entry.modelData.status === "cancelled" ? KetTheme.t.textMuted : KetTheme.t.textMain
                                    font.strikeout: entry.modelData.status === "cancelled"
                                    font.family: KetTheme.fontFamily
                                    font.pixelSize: card.px(13)
                                }
                                Kit.KetBadge {
                                    id: changeBadge
                                    visible: entry.change !== ""
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: entry.change === "new" ? qsTr("New") : qsTr("Updated")
                                    tone: KetTheme.BadgeTone.Info
                                }
                            }
                            Text {
                                id: ownerText
                                anchors.right: parent.right
                                anchors.rightMargin: card.px(8)
                                anchors.verticalCenter: parent.verticalCenter
                                text: card.closed(entry.modelData) || !entry.next
                                      ? card.statusLabel(entry.modelData.status) : card.owner(entry.next.owner)
                                color: !card.closed(entry.modelData) && entry.next && entry.next.owner === "user"
                                       ? KetTheme.t.warning : KetTheme.t.textMuted
                                font.family: KetTheme.fontFamily
                                font.pixelSize: card.px(12)
                            }
                            Kit.KetToolTip { visible: itemArea.containsMouse && itemTitle.truncated; text: entry.modelData.title }
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
                                model: [[qsTr("Result"), entry.modelData.evidence || ""],
                                        [qsTr("Remaining"), entry.modelData.remaining || ""],
                                        [qsTr("Next"), entry.next ? entry.next.action + " · " + card.owner(entry.next.owner) : ""]]
                                Row {
                                    required property var modelData
                                    visible: modelData[1] !== ""
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
                                        textFormat: Text.PlainText
                                        text: parent.modelData[1]
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
                                    text: qsTr("Verified")
                                    color: KetTheme.t.textMuted
                                    font.family: KetTheme.fontFamily
                                    font.pixelSize: card.px(12)
                                }
                                Flow {
                                    width: parent.width - card.px(84)
                                    spacing: card.px(6)
                                    Repeater {
                                        model: entry.modelData.verified || []
                                        Kit.KetBadge { required property string modelData; text: card.check(modelData); tone: KetTheme.BadgeTone.Success }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle { visible: footer.visible; width: parent.width; height: 1; color: KetTheme.t.panelBorder }

        // The next step and who takes it.
        Item {
            id: footer
            readonly property var next: card.items.find(item => (item.status === "active" || item.status === "blocked") && item.next)
                                        || card.items.find(item => item.status === "waiting" && item.next)
            visible: card.ledger || !!next || (card.total > 0 && card.finished === card.total)
            width: parent.width
            height: card.px(20)
            Text {
                width: parent.width - when.width - card.px(12)
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: parent.next ? qsTr("Next · %1 · %2").arg(parent.next.next.action).arg(card.owner(parent.next.next.owner))
                                  : card.total > 0 && card.finished === card.total ? qsTr("Nothing left to do") : ""
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: card.px(13)
            }
            Text {
                id: when
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: card.ledger ? qsTr("Updated in turn %1").arg(card.turn) : ""
                color: KetTheme.t.textMuted
                font.family: KetTheme.fontFamily
                font.pixelSize: card.px(12)
            }
        }
    }
}
