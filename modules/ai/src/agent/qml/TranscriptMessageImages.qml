import QtQuick

// A transcript row's thumbnails: their expansion survives delegate reuse, a click opens the
// viewer and a height change keeps the reader's place.
MessageImageGallery {
    id: gallery

    required property Item row
    readonly property string expansionKey: JSON.stringify(["images", row.view.cacheScope, row.messageKey])

    function restoreImages() {
        const saved = row.view.activityState(expansionKey);
        maximumVisible = saved === undefined ? 3 : saved.imageLimit;
    }

    // Only the instance holding the row's images answers to the name.
    objectName: imageCount > 0 ? "messageImageGallery" : ""
    width: Math.min(parent.width, preferredWidth)
    host: row.host
    visible: !row.activityRow && imageCount > 0
    onExpansionKeyChanged: restoreImages()
    Component.onCompleted: restoreImages()
    onExpanded: row.view.rememberActivityState(expansionKey, {
        imageLimit: maximumVisible
    })
    // Thumbnail rows have fixed geometry; adding/removing attachments
    // still preserves the reading anchor.
    onHeightChanged: {
        if (row.view.followTail || row.shell.transcript !== row.view)
            return;
        row.host.scheduleTranscriptRestore();
    }
    onImageClicked: function (source) {
        const all = sources.map(image => image.toString());
        row.preview.showImages(all, Math.max(0, all.indexOf(source.toString())));
    }
}
