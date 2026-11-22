// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import KetPlusMock
import KetPlusMock.Native

// Add member… with Developer: how many developers to add at once (a select box, never a
// slider), and one row per developer with its name and lane. Names and lanes continue
// the team's own ("Developer 3", "Developer 4"…) and never repeat one the team has; each row
// can be edited before Add. Every new developer uses the agent chosen once in the dialog.
// A lane is a name only: members are not given paths.
Column {
    id: rows
    property string language: "en"
    function copy(source) { return Mock.teamText(language, source) }
    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    // The names and lanes the team already has.
    property var takenNames: []
    property var takenLanes: []
    // How many more developers the team may have.
    property int room: 2
    property bool tried: false

    // [{ name, lane }], one per developer to add.
    property var entries: []
    readonly property int count: entries.length
    readonly property bool valid: entries.every((e, i) => e.name.trim() !== "" && e.lane.trim() !== ""
        && !duplicate(i))

    function nextFree(taken, from) {
        let n = from
        while (taken.indexOf("Developer " + n) >= 0)
            ++n
        return n
    }
    function resize(wanted) {
        const next = entries.slice(0, wanted)
        let n = 1
        const used = takenNames.concat(takenLanes)
        while (next.length < wanted) {
            n = nextFree(used.concat(next.map(e => e.name)), n)
            next.push({ name: "Developer " + n, lane: "Developer " + n })
            ++n
        }
        entries = next
    }
    function edit(index, key, value) {
        const next = entries.slice()
        next[index] = Object.assign({}, next[index], { [key]: value })
        entries = next
    }
    function duplicate(index) {
        const e = entries[index]
        return takenNames.indexOf(e.name.trim()) >= 0 || takenLanes.indexOf(e.lane.trim()) >= 0
            || entries.some((o, i) => i !== index && (o.name.trim() === e.name.trim() || o.lane.trim() === e.lane.trim()))
    }

    spacing: px(12)
    Component.onCompleted: resize(1)

    KetFieldRow {
        width: parent.width
        label: rows.copy("Developers to add")
        help: rows.room > 0 ? rows.copy("Each one starts as a new conversation with the agent below.")
                            : rows.copy("The team has the most developers a team has.")
        KetSelect {
            objectName: "addDevelopersCount"
            width: parent.width
            enabled: rows.room > 1
            model: Array.from({ length: Math.max(1, rows.room) }, (_, i) => i === 0 ? rows.copy("1 developer") : (i + 1) + rows.copy(" developers"))
            currentIndex: Math.max(0, rows.count - 1)
            onActivated: index => rows.resize(index + 1)
        }
    }
    Repeater {
        model: rows.entries
        Column {
            id: row
            required property var modelData
            required property int index
            width: rows.width
            spacing: rows.px(6)
            KetFieldRow {
                width: parent.width
                label: rows.count > 1 ? rows.copy("Developer ") + (row.index + 1) : rows.copy("Name and lane")
                error: rows.tried && rows.duplicate(row.index) ? rows.copy("A member or lane of the team already has this name.")
                     : rows.tried && (row.modelData.name.trim() === "" || row.modelData.lane.trim() === "")
                       ? rows.copy("Name the developer and its lane.") : ""
                Column {
                    width: parent.width
                    spacing: rows.px(6)
                    Row {
                        width: parent.width
                        spacing: rows.px(6)
                        KetTextField {
                            objectName: "addDeveloperName_" + row.index
                            width: (parent.width - parent.spacing) / 2
                            placeholderText: rows.copy("Name")
                            text: row.modelData.name
                            onTextEdited: rows.edit(row.index, "name", text)
                        }
                        KetTextField {
                            objectName: "addDeveloperLane_" + row.index
                            width: (parent.width - parent.spacing) / 2
                            placeholderText: rows.copy("Lane")
                            text: row.modelData.lane
                            onTextEdited: rows.edit(row.index, "lane", text)
                        }
                    }
                }
            }
        }
    }
}
