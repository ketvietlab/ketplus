// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket

// KetMarkdown with Mermaid: a ```mermaid fence becomes a MermaidBlock, the rest reads as
// KetMarkdown does. The app draws Mermaid in its own transcript; the kit has no diagrams.
Column {
    id: view

    required property var window
    property string markdown

    signal linkActivated(string link)

    // Prose and Mermaid fences in order: [{ mermaid: false, text }, { mermaid: true, text }].
    readonly property var parts: {
        const result = []
        const fence = /^```mermaid[ \t]*\n([\s\S]*?)^```[ \t]*$/gm
        let last = 0
        let match
        while ((match = fence.exec(markdown)) !== null) {
            if (match.index > last)
                result.push({ mermaid: false, text: markdown.slice(last, match.index) })
            result.push({ mermaid: true, text: match[1].replace(/\n$/, "") })
            last = fence.lastIndex
        }
        if (last < markdown.length)
            result.push({ mermaid: false, text: markdown.slice(last) })
        return result
    }

    Repeater {
        model: view.parts

        Loader {
            required property var modelData
            width: view.width
            sourceComponent: modelData.mermaid ? diagram : prose

            Component {
                id: prose
                KetMarkdown {
                    width: view.width
                    markdown: modelData.text
                    onLinkActivated: link => view.linkActivated(link)
                }
            }

            Component {
                id: diagram
                MermaidBlock {
                    width: view.width
                    window: view.window
                    source: modelData.text
                }
            }
        }
    }
}
