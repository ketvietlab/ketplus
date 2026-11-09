import Ket
import QtQuick
import Ket as Kit

// A file's changes under its open step, as the mock's expanded edit draws them with the kit's
// diff lines. A created or deleted file's diff is its content. The lines are bounded, so a
// large rewrite never builds thousands of rows; the full diff opens in the diff viewer.
Column {
    id: fileDiff
    required property string diff
    required property string operation
    required property string path
    required property var host
    readonly property int lineLimit: 400
    readonly property var allLines: {
        if (diff.length === 0)
            return [];
        if (operation === "Created" || operation === "Deleted") {
            const kind = operation === "Created" ? "add" : "remove";
            const content = diff.endsWith("\n") ? diff.slice(0, -1) : diff;
            return content.split("\n").map((text, index) => ({ kind: kind, number: index + 1, text: text }));
        }
        return diffLines.parseUnified(diff);
    }

    objectName: "transcriptFileDiff"
    spacing: KetTheme.px(6, KetTheme.zoom)
    visible: allLines.length > 0

    Kit.KetDiffView {
        id: diffLines
        objectName: "transcriptFileDiffLines"
        width: parent.width
        clip: true
        lines: fileDiff.allLines.slice(0, fileDiff.lineLimit)
    }
    Kit.KetButton {
        id: fullDiff
        objectName: "transcriptFullDiff"
        variant: Kit.KetTheme.ButtonVariant.Ghost
        controlSize: Kit.KetTheme.ControlSize.Small
        visible: fileDiff.allLines.length > fileDiff.lineLimit
        text: (fileDiff.allLines.length - fileDiff.lineLimit) + " more lines · Open the full diff"
        Accessible.name: text
        padding: KetTheme.px(6, KetTheme.zoom)
        contentItem: Text {
            text: fullDiff.text
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
        }
        background: Rectangle {
            color: fullDiff.hovered ? KetTheme.t.interactiveHover : "transparent"
            border.color: fullDiff.activeFocus ? KetTheme.t.colorFocus : KetTheme.t.panelBorder
            radius: KetTheme.px(4, KetTheme.zoom)
        }
        onClicked: fileDiff.host.requestFileDiff(fileDiff.path)
    }
}
