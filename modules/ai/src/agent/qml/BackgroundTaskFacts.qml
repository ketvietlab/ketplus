import QtQuick
import Ket
import QtQuick.Layouts

// The opened managed job's facts, one a line: when it was queued or created, its timeout,
// a verified cleanup, and what happens when it ends. A turn that ends on a live job waits
// on it, so the reader learns here that the agent continues with its result, or, for a
// job stopping toward cancellation, that it does not (ADR 0127).
// Legacy tasks have none of these fields and show nothing.
GridLayout {
    id: facts
    required property var task
    required property var taskBar
    readonly property var rows: task ? lines(task) : []

    function duration(ms) {
        const seconds = Math.max(0, Math.round(ms / 1000));
        if (seconds < 60)
            return seconds + "s";
        const minutes = Math.floor(seconds / 60);
        if (minutes < 60)
            return minutes + "m" + (seconds % 60 ? " " + (seconds % 60 < 10 ? "0" : "") + seconds % 60 + "s" : "");
        return Math.floor(minutes / 60) + "h" + (minutes % 60 ? " " + minutes % 60 + "m" : "");
    }
    function lines(task) {
        if (task.schema_version === undefined)
            return [];
        const result = [];
        const state = taskBar.state(task);
        const live = taskBar.live(task);
        const created = Number(task.created_at || 0);
        if (live && created > 0)
            result.push([state === "queued" ? "Queued" : "Created", duration(taskBar.now - created) + " ago" + (state === "queued" ? " · not started" : "")]);
        const timeout = Number(task.timeout_ms || 0);
        if (live && timeout > 0)
            result.push(["Timeout", duration(timeout)]);
        // The strip states best-effort cleanup and the row's note unverified cleanup; only a
        // verified cleanup is said here.
        if (task.cleanup_status === "verified")
            result.push(["Cleanup", "Verified"]);
        if (taskBar.attention(task))
            result.push(["Next", "Only you can acknowledge its cleanup; the agent will not start it again"]);
        else if (state === "stopping" && task.terminal_target === "cancelled")
            // A cancelled job's completion wake is suppressed (ADR 0127).
            result.push(["When it ends", "It ends as cancelled; the agent is not woken"]);
        else if (live)
            result.push(["When it ends", "The agent gets its result and continues this conversation"]);
        return result;
    }

    objectName: "backgroundTaskFacts"
    visible: rows.length > 0
    columns: 2
    columnSpacing: KetTheme.px(8, KetTheme.zoom)
    rowSpacing: KetTheme.px(2, KetTheme.zoom)
    Accessible.role: Accessible.List
    Accessible.name: "Task details"

    Repeater {
        model: facts.rows.length * 2
        Text {
            required property int index
            readonly property bool name: index % 2 === 0
            Layout.alignment: Qt.AlignTop
            Layout.fillWidth: !name
            Layout.preferredWidth: name ? KetTheme.px(84, KetTheme.zoom) : -1
            text: facts.rows[Math.floor(index / 2)][name ? 0 : 1]
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: name ? KetTheme.t.textMuted : KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
        }
    }
}
