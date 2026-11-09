import Ket
import QtQuick
import Ket as Kit

// One file of a multi-file edit inside an open group, as the mock lists them: "Edited", its
// path and its line counts, each with its operation's icon. Activating it opens the diff.
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
    // A deleted file is missing by design; only a file that still exists opens its diff.
    Accessible.name: text + " " + (file.path || "file") + (missing ? ", no longer exists" : "")
    onClicked: if (!missing)
        host.requestFileDiff(targetPath)
}
