// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import KetPlusMock

// One step inside an activity group, as the journal records it: a command, a file read,
// a code search, an edit, a browser or MCP tool, a web search, an image, reasoning or a
// wait. The row toggles its details: what the step was for, the command and its output,
// the diff, captures, web results or the error.
//
// `step` = { kind, title, detail, status, description, command, cwd, output, outputLines,
//            exitCode, duration, diff, images, results, prompt, error, live, expanded }
// status is "completed" (default), "running", "failed" or "interrupted".
Column {
    id: item

    required property var step
    required property var window
    readonly property string status: step.status || "completed"
    readonly property bool failed: status === "failed"
    readonly property bool running: status === "running"
    readonly property bool hasDetails: !!(step.description || step.command || step.output
                                          || step.diff || step.results || step.prompt
                                          || step.error || (step.images && step.images.length))
    readonly property var icons: ({
        command: "terminal", read: "file-text", search: "search", list: "view-list",
        edit: "edit", create: "file-code", delete: "trash", move: "arrow-right",
        browser: "globe", web: "globe", mcp: "package", image: "image", generate: "sparkles",
        thinking: "sparkles", wait: "clock", tool: "sliders", skill: "layers", agent: "send",
        subagent: "bot", task: "terminal", plan: "map"
    })

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    width: parent ? parent.width : 0
    spacing: px(6)

    KetActivityRow {
        id: row
        width: parent.width
        text: item.step.title
        detail: item.step.detail || ""
        iconName: item.running ? "loading" : item.failed ? "error" : (item.icons[item.step.kind] || "sliders")
        expandable: item.hasDetails
        checked: !!item.step.expanded
    }

    // Status the title cannot carry: a failure, an interruption or a running step.
    Text {
        visible: item.failed || item.status === "interrupted" || item.running
        leftPadding: item.px(30)
        text: item.failed ? (item.step.exitCode !== undefined ? "Failed · exit code " + item.step.exitCode : "Failed")
                            + (item.step.duration ? " · " + item.step.duration : "")
            : item.running ? "Running" + (item.step.duration ? " · " + item.step.duration : "")
            : "Interrupted"
        color: item.failed ? KetTheme.t.danger : KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: item.px(12)
    }

    Column {
        visible: item.hasDetails && (row.checked || item.running)
        x: item.px(30)
        width: parent.width - x
        spacing: item.px(8)

        Text {
            visible: !!item.step.description
            width: parent.width
            wrapMode: Text.Wrap
            text: item.step.description || ""
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: item.px(13)
        }

        // The command with its working directory, then its output on one surface.
        Rectangle {
            visible: !!(item.step.command || item.step.output)
            width: parent.width
            implicitHeight: terminal.implicitHeight + item.px(20)
            radius: item.px(8)
            color: KetTheme.t.panelBgSubtle
            border.color: KetTheme.t.panelBorder

            Column {
                id: terminal
                x: item.px(12)
                y: item.px(10)
                width: parent.width - 2 * x
                spacing: item.px(6)

                Text {
                    visible: !!item.step.cwd
                    text: item.step.cwd || ""
                    color: KetTheme.t.textMuted
                    font.family: KetTheme.monoFontFamily
                    font.pixelSize: item.px(14)
                }
                Text {
                    visible: !!item.step.command
                    width: parent.width
                    wrapMode: Text.WrapAnywhere
                    text: "$ " + (item.step.command || "")
                    color: KetTheme.t.textMain
                    font.family: KetTheme.monoFontFamily
                    font.pixelSize: item.px(14)
                }
                Rectangle {
                    visible: !!item.step.command && !!item.step.output
                    width: parent.width
                    height: 1
                    color: KetTheme.t.panelBorder
                }
                Text {
                    visible: !!item.step.output
                    width: parent.width
                    wrapMode: Text.WrapAnywhere
                    text: item.step.output || ""
                    color: item.failed ? KetTheme.t.danger : KetTheme.t.textSecondary
                    font.family: KetTheme.monoFontFamily
                    font.pixelSize: item.px(14)
                    lineHeight: 1.45
                }
                Row {
                    spacing: item.px(10)
                    visible: !!item.step.outputLines || item.running
                    KetActivityIndicator { visible: item.running; anchors.verticalCenter: parent.verticalCenter }
                    Text {
                        text: item.running ? "Streaming output" : "Show all " + item.step.outputLines + " lines"
                        color: item.running ? KetTheme.t.textMuted : KetTheme.t.accentText
                        font.family: KetTheme.fontFamily
                        font.pixelSize: item.px(12)
                        TapHandler { enabled: !item.running; onTapped: item.window.notify("Opened the full output") }
                    }
                }
            }
        }

        KetDiffView {
            id: diff
            visible: !!item.step.diff
            clip: true
            width: parent.width
            lines: item.step.diff ? parseUnified(item.step.diff) : []
        }

        // A generated image reads at size; captures and viewed images are thumbnails.
        Rectangle {
            visible: item.step.kind === "generate" && !!(item.step.images && item.step.images.length)
            width: Math.min(parent.width, item.px(360))
            height: width * 0.625
            radius: item.px(10)
            clip: true
            color: KetTheme.t.panelBgSubtle
            border.color: KetTheme.t.panelBorder
            Image {
                anchors.fill: parent
                anchors.margins: 1
                source: item.step.images && item.step.images.length ? item.step.images[0].source : ""
                fillMode: Image.PreserveAspectCrop
            }
            TapHandler { onTapped: item.window.notify("Opened the generated image") }
        }

        KetGallery {
            visible: item.step.kind !== "generate" && !!(item.step.images && item.step.images.length)
            images: item.step.images || []
            onActivated: index => item.window.notify("Opened " + item.step.images[index].caption)
        }

        // An image the agent generated, at reading size with its revised prompt.
        Text {
            visible: !!item.step.prompt
            width: parent.width
            wrapMode: Text.Wrap
            text: "<b>Prompt</b>&nbsp;&nbsp;" + (item.step.prompt || "")
            textFormat: Text.StyledText
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: item.px(13)
        }

        // Web results: the pages a search returned or opened.
        Column {
            visible: !!item.step.results
            width: parent.width
            spacing: item.px(4)
            Repeater {
                model: item.step.results || []
                Text {
                    required property var modelData
                    width: parent.width
                    elide: Text.ElideRight
                    text: "<a href=\"" + modelData.url + "\">" + modelData.title + "</a>&nbsp;&nbsp;<font color=\""
                          + KetTheme.t.textMuted + "\">" + modelData.url.replace(/^https?:\/\//, "") + "</font>"
                    textFormat: Text.StyledText
                    linkColor: KetTheme.t.accentText
                    font.family: KetTheme.fontFamily
                    font.pixelSize: item.px(13)
                    onLinkActivated: link => item.window.notify("Opened " + link)
                }
            }
        }

        Text {
            visible: !!item.step.error
            width: parent.width
            wrapMode: Text.Wrap
            text: item.step.error || ""
            color: KetTheme.t.danger
            font.family: KetTheme.monoFontFamily
            font.pixelSize: item.px(14)
        }
    }
}
