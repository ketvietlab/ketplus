// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import KetPlusMock
import KetPlusMock.Native

// A ```mermaid fence, as the app shows it: its source by default, and the diagram once the
// reader switches to Mermaid. The diagram opens in the image viewer. The mock's diagrams
// are rendered ahead of time (tools/render_mermaid.py); an unknown one stays source only.
Item {
    id: block

    required property var window
    property string source
    // Source by default, as in the app; --open=mermaid starts on the diagram for screenshots.
    property bool rendered: Mock.initialPanel === "mermaid"
    readonly property string diagram: Fixtures.mermaidImage(source, KetTheme.dark)
    readonly property var recipe: KetTheme.codeBlockRecipe(KetTheme.dark, KetTheme.zoom)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    implicitHeight: rendered && diagram !== "" ? frame.height + 2 * recipe.margin
                                               : code.implicitHeight

    KetCodeBlock {
        id: code
        visible: !block.rendered || block.diagram === ""
        width: parent.width
        code: block.source
        language: "mermaid"
    }

    Rectangle {
        id: frame
        visible: block.rendered && block.diagram !== ""
        y: block.recipe.margin
        width: parent.width
        height: picture.height + 2 * block.px(16) + switcher.height
        radius: block.px(6)
        color: block.recipe.background
        border.color: block.recipe.border

        Image {
            id: picture
            anchors.horizontalCenter: parent.horizontalCenter
            y: switcher.height + block.px(16)
            width: Math.min(implicitWidth, parent.width - 2 * block.px(16))
            height: implicitWidth > 0 ? width * implicitHeight / implicitWidth : 0
            source: block.rendered ? block.diagram : ""
            fillMode: Image.PreserveAspectFit
            smooth: true
            mipmap: true
            Accessible.role: Accessible.Graphic
            Accessible.name: "Mermaid diagram"

            HoverHandler { cursorShape: Qt.PointingHandCursor }
            // The viewer is dark in both themes, so it shows the dark rendering.
            TapHandler {
                onTapped: block.window.openImages([{ source: Fixtures.mermaidImage(block.source, true),
                                                     caption: "Mermaid diagram" }], 0)
            }
        }
    }

    // In the corner of a diagram; beside the copy button of the source.
    MermaidSwitch {
        id: switcher
        visible: block.diagram !== ""
        rendered: block.rendered && block.diagram !== ""
        x: parent.width - width - block.px(4) - (rendered ? 0 : block.px(30))
        y: block.recipe.margin + block.px(5)
        z: 2
        onModeRequested: value => block.rendered = value
    }
}
