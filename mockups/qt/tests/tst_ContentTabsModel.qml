// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtTest
import "../qml/parts"

TestCase {
    name: "ContentTabsModel"
    ContentTabsModel { id: session }
    Component { id: storeFactory; ContentTabsStore {} }
    function init() { session.tabs = []; session.currentId = ""; session.terminalSerial = 0 }
    function test_reopen_document_selects_existing_tab() {
        const id = session.open("file", "a.cpp", "a.cpp", "source")
        session.newTerminal()
        session.open("file", "a.cpp", "a.cpp", "replacement")
        compare(session.tabs.length, 2)
        compare(session.currentId, id)
        compare(session.current.body, "source")
        session.open("diff", "a.cpp", "a.cpp · diff", "patch")
        compare(session.tabs.length, 3)
    }
    function test_terminals_keep_independent_drafts_and_outputs() {
        const first = session.newTerminal()
        session.patch(first, { draft: "build" })
        const second = session.newTerminal()
        session.patch(second, { draft: "test" })
        session.submit(second)
        session.select(first)
        compare(session.current.draft, "build")
        verify(!session.current.body.includes("$ test"))
        session.select(second)
        compare(session.current.draft, "")
        verify(session.current.body.includes("$ test"))
        verify(session.current.body.includes("not executed"))
    }
    function test_closing_inactive_tab_keeps_selection() {
        const first = session.newTerminal()
        const second = session.newTerminal()
        session.close(first)
        compare(session.currentId, second)
        session.close(second)
        compare(session.current, null)
        compare(session.currentId, "")
        const third = session.newTerminal()
        verify(third !== first && third !== second)
    }
    function test_closing_active_tab_selects_neighbour() {
        const first = session.newTerminal()
        const second = session.newTerminal()
        const third = session.newTerminal()
        session.select(second); session.close(second)
        compare(session.currentId, third)
        session.close(third)
        compare(session.currentId, first)
    }
    function test_plan_tab_is_singleton_and_can_reopen() {
        session.open("plan", "conversation-plan", "Plan", "")
        session.open("plan", "conversation-plan", "Plan", "")
        compare(session.tabs.length, 1)
        session.close(session.currentId)
        session.open("plan", "conversation-plan", "Plan", "")
        compare(session.tabs.length, 1)
    }
    function test_conversations_keep_independent_open_tabs() {
        const store = createTemporaryObject(storeFactory, this)
        const first = store.get("4821")
        const terminal = first.newTerminal()
        first.patch(terminal, { draft: "build" })
        const second = store.get("4822")
        compare(second.tabs.length, 0)
        second.newTerminal()
        compare(store.get("4821").current.draft, "build")
        compare(second.current.draft, "")
    }
}
