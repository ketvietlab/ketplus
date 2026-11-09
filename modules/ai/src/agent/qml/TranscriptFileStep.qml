import Ket
import QtQuick
import Ket as Kit

// One file of a multi-file edit inside an open group, as the mock lists them: the operation,
// then the path and its line counts. Clicking opens the file's diff, as the card's rows did.
Kit.KetActivityRow {
    id: step
    required property var file
    required property var host

    readonly property string targetPath: file.destination || file.path || ""
    readonly property bool missing: {
        host.pathStateRevision;
        return host.pathMissing(targetPath);
    }
    readonly property string counts: file.known === true
        ? [file.added > 0 ? "+" + file.added : "", file.removed > 0 ? "−" + file.removed : ""].filter(part => part.length > 0).join(" ")
        : ""
    objectName: "transcriptFileStep"
    text: file.operation || "Edited"
    // As a one-file edit's line: the path as the agent named it.
    detail: String(file.path || "file") + (file.destination ? " → " + file.destination : "") + (counts.length > 0 ? "  " + counts : "") + (file.diffTrimmed ? " · Diff trimmed" : "")
    iconName: ({ Edited: "edit", Created: "file-code", Deleted: "trash", Moved: "arrow-right" })[file.operation] || "edit"
    // The kit's detail has no width; this line elides the path's middle within the step.
    contentItem: Item {}
    // A deleted file is missing by design; only a file that still exists opens its diff.
    Accessible.name: text + " " + (file.path || "file") + (missing ? ", no longer exists" : "")
    onClicked: if (!missing) host.requestFileDiff(targetPath)

    Kit.KetIcon {
        id: stepIcon
        anchors.left: parent.left
        anchors.leftMargin: step.leftPadding
        anchors.verticalCenter: parent.verticalCenter
        name: step.iconName
        box: step.recipe.iconSize
        color: step.recipe.text
    }
    Text {
        id: stepTitle
        objectName: "transcriptFileStepTitle"
        anchors.left: stepIcon.right
        anchors.leftMargin: step.recipe.spacing
        anchors.verticalCenter: parent.verticalCenter
        text: step.text
        textFormat: Text.PlainText
        color: step.recipe.text
        font.family: KetTheme.fontFamily
        font.pixelSize: step.recipe.fontPixels
        maximumLineCount: 1
    }
    Text {
        objectName: "transcriptFileStepDetail"
        anchors.left: stepTitle.right
        anchors.leftMargin: step.recipe.spacing
        anchors.right: parent.right
        anchors.rightMargin: step.rightPadding
        anchors.baseline: stepTitle.baseline
        text: step.detail
        textFormat: Text.PlainText
        color: step.recipe.detail
        font.family: KetTheme.fontFamily
        font.pixelSize: step.recipe.detailPixels
        elide: Text.ElideMiddle
        maximumLineCount: 1
    }

    // Right-click keeps the card's file actions; the left button stays with the row.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onClicked: fileMenu.popup()
    }
    TranscriptFileMenu {
        id: fileMenu
        host: step.host
        targetPath: step.targetPath
        missing: step.missing
    }
    KetToolTip {
        visible: stepHover.hovered
        text: step.host.displayFilePath(step.file.path || "") + (step.file.destination ? " → " + step.host.displayFilePath(step.file.destination) : "") + (step.missing ? " — no longer exists" : "")
    }
    HoverHandler {
        id: stepHover
    }
}
