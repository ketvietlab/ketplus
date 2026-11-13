// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket
import "ScheduleRules.js" as Rules

KetPopover {
    id: popup
    parent: Basic.Overlay.overlay
    property int mode: 0
    property bool more: false
    property bool tried: false
    property var applied: null
    readonly property var draft: Object.assign({}, repeatFields.draft, {
        mode: mode, rule: mode === 1 ? 0 : Math.max(1, repeatFields.selection),
        date: timeFields.date, time: mode === 1 ? timeFields.time : repeatFields.time,
        zone: mode === 1 ? timeFields.zone : repeatFields.zone,
        newConversation: newEachRun.checked, busy: busyPicker.currentIndex, missed: missedPicker.currentIndex
    })
    readonly property string problem: mode === 0 ? "" : Rules.error(Object.assign({}, draft, { name: "Schedule", prompt: "First message" }))
    signal configured(var plan)
    title: "Schedule first message"
    width: Math.min(KetTheme.px(480, KetTheme.zoom), parent ? parent.width - KetTheme.px(24, KetTheme.zoom) : KetTheme.px(480, KetTheme.zoom))
    function prepare(plan) {
        applied = plan
        // No applied draft always starts at Send now, regardless of the last preview.
        mode = plan ? plan.mode : 0; tried = false; more = false
        timeFields.reset(); timeFields.scheduled = true; repeatFields.reset(); repeatFields.selection = 1
        if (plan) { timeFields.date = plan.date; timeFields.time = plan.time; repeatFields.selection = plan.rule || 1; repeatFields.time = plan.time; repeatFields.days = plan.days || [1,2,3,4,5]; repeatFields.hours = plan.hours || "4"; timeFields.setZone(plan.zone); repeatFields.setZone(plan.zone) }
        busyPicker.currentIndex = plan ? plan.busy : 0; missedPicker.currentIndex = plan ? plan.missed : 0
        newEachRun.checked = plan ? !!plan.newConversation : false
    }
    readonly property point anchorCorner: {
        if (!anchorItem || !parent) return Qt.point(0, 0)
        for (let item = anchorItem; item; item = item.parent) { item.x; item.y; item.width; item.height; item.scale; item.rotation }
        return parent.mapFromItem(anchorItem, anchorItem.width, 0)
    }
    onAnchorCornerChanged: if (visible) place()
    function place() {
        if (!anchorItem || !parent) return
        x = Math.max(KetTheme.px(8, KetTheme.zoom), Math.min(parent.width - width - KetTheme.px(8, KetTheme.zoom), anchorCorner.x - width))
        y = Math.max(KetTheme.px(8, KetTheme.zoom), anchorCorner.y - height - recipe.offset)
    }
    function geometryVisible() { return visible && parent && x >= 0 && y >= 0 && x + width <= parent.width + 1 && y + height <= parent.height + 1 }
    function sendNowSelected() { return mode === 0 && !atFields.visible && !repeatSection.visible }
    function preview(kind) {
        mode = kind === "at" ? 1 : 2; repeatFields.selection = 1
        if (kind === "weekly") repeatFields.selection = 3
        more = kind === "more"
    }
    function apply() {
        tried = true
        if (problem) return false
        configured(mode === 0 ? null : draft); close(); return true
    }
    Flickable {
        id: scroll
        width: parent.width
        height: Math.min(contentHeight, Math.max(KetTheme.px(180, KetTheme.zoom), (popup.parent ? popup.parent.height : 900) - KetTheme.px(210, KetTheme.zoom)))
        contentHeight: fields.implicitHeight; clip: true; boundsBehavior: Flickable.StopAtBounds
        Basic.ScrollBar.vertical: Basic.ScrollBar {}
        Column {
            id: fields
            width: scroll.width - KetTheme.px(12, KetTheme.zoom); spacing: KetTheme.px(10, KetTheme.zoom)
            KetRadio { text: "Send now"; checked: popup.mode === 0; onClicked: popup.mode = 0 }
            KetRadio { text: "At a time"; checked: popup.mode === 1; onClicked: popup.mode = 1 }
            Column {
                id: atFields
                visible: popup.mode === 1; width: parent.width; spacing: KetTheme.px(8, KetTheme.zoom)
                // Reuse the same date/time validation and presets as scheduled team start.
                TeamStartFields { id: timeFields; width: parent.width; showStartChoice: false; startHint: "The first message will be sent at "; scheduled: true; tried: popup.tried }
            }
            KetRadio { text: "Repeat"; checked: popup.mode === 2; onClicked: popup.mode = 2 }
            Column {
                id: repeatSection
                width: parent.width; visible: popup.mode === 2; spacing: KetTheme.px(8, KetTheme.zoom)
                ScheduleRepeatFields { id: repeatFields; width: parent.width; allowOff: false; selection: 1 }
                KetCheckbox { id: newEachRun; text: "New conversation each run" }
            }
            KetButton { visible: popup.mode > 0; text: popup.more ? "Less" : "More"; variant: KetTheme.ButtonVariant.Ghost; onClicked: popup.more = !popup.more }
            Column {
                width: parent.width; visible: popup.more && popup.mode > 0; spacing: KetTheme.px(8, KetTheme.zoom)
                KetFieldRow { width: parent.width; label: "When busy"; KetSelect { id: busyPicker; width: parent.width; model: ["Queue until finished", "Skip this run"] } }
                KetFieldRow { width: parent.width; label: "When missed"; KetSelect { id: missedPicker; width: parent.width; model: ["Catch up once when the computer wakes", "Skip"] } }
            }
            ScheduleText { width: parent.width; visible: popup.tried && text !== ""; text: popup.problem; color: KetTheme.t.danger; Accessible.role: Accessible.AlertMessage }
        }
    }
    footer: [
        KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: popup.close() },
        KetButton { text: popup.mode === 0 ? "Use Send now" : "Set schedule"; variant: KetTheme.ButtonVariant.Primary; onClicked: popup.apply() }
    ]
}
