// SPDX-License-Identifier: MPL-2.0
import QtQuick

// Per-conversation fixture documents. No real file writes or shell processes.
QtObject {
    property var tabs: []
    property string currentId: ""
    property int terminalSerial: 0
    readonly property var current: tabs.find(t => t.id === currentId) || null
    function open(kind, path, title, body) {
        const id = kind + ":" + path
        if (!tabs.some(t => t.id === id))
            tabs = tabs.concat([{ id: id, kind: kind, path: path, title: title,
                body: body || "", draft: "" }])
        currentId = id
        return id
    }
    function newTerminal() {
        ++terminalSerial
        return open("terminal", String(terminalSerial), "Terminal " + terminalSerial,
                    "Terminal " + terminalSerial + " · mock session\nNo shell process is running.\n")
    }
    function select(id) { if (tabs.some(t => t.id === id)) currentId = id }
    function close(id) {
        const index = tabs.findIndex(t => t.id === id)
        if (index < 0) return
        const next = tabs.filter(t => t.id !== id)
        if (currentId === id) currentId = next.length ? next[Math.min(index, next.length - 1)].id : ""
        tabs = next
    }
    function patch(id, values) {
        tabs = tabs.map(t => t.id === id ? Object.assign({}, t, values) : t)
    }
    function submit(id) {
        const item = tabs.find(t => t.id === id)
        if (!item || item.kind !== "terminal" || !item.draft.trim()) return
        patch(id, { body: item.body + "\n$ " + item.draft + "\nDemo output · command was not executed.\n", draft: "" })
    }
}
