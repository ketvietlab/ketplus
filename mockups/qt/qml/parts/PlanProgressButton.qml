// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket

// Conversation-specific composition: kit button + progress geometry and semantic tokens.
// Usage-ring colours indicate capacity pressure, so they must not colour plan completion.
KetIconButton {
    id: button
    property var run: null
    readonly property real progress: run && run.planCreated ? run.completed / run.tasks.length : 0
    readonly property var ringRecipe: KetTheme.usageRingRecipe(KetTheme.dark, KetTheme.zoom, 0, false)
    readonly property var progressRecipe: KetTheme.progressRecipe(KetTheme.dark, KetTheme.zoom)
    readonly property color arcColor: run && run.status === "complete" ? KetTheme.t.success
        : run && ["blocked", "limit"].includes(run.status) ? KetTheme.t.warning
        : run && run.status === "paused" ? KetTheme.t.textMuted : progressRecipe.fill

    objectName: "conversationPlanButton"
    iconName: "check-all"
    glyphSize: KetTheme.px(12, KetTheme.zoom)
    text: "Plan · " + (run && run.planCreated ? run.completed + "/" + run.tasks.length + " verified · " : "")
        + (run ? run.statusLabel(run.status) : "Not created")

    Canvas {
        id: ring
        anchors.centerIn: parent
        width: KetTheme.px(24, KetTheme.zoom)
        height: width
        readonly property real value: button.progress
        readonly property real stroke: button.ringRecipe.stroke
        readonly property color track: button.progressRecipe.track
        readonly property color arc: button.arcColor
        onValueChanged: requestPaint()
        onStrokeChanged: requestPaint()
        onTrackChanged: requestPaint()
        onArcChanged: requestPaint()
        onWidthChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const radius = (width - stroke) / 2
            ctx.lineWidth = stroke
            ctx.strokeStyle = track
            ctx.beginPath()
            ctx.arc(width / 2, height / 2, radius, 0, 2 * Math.PI)
            ctx.stroke()
            if (value > 0) {
                ctx.strokeStyle = arc
                ctx.lineCap = "round"
                ctx.beginPath()
                ctx.arc(width / 2, height / 2, radius, -Math.PI / 2,
                        -Math.PI / 2 + 2 * Math.PI * Math.min(1, value))
                ctx.stroke()
            }
        }
    }
}
