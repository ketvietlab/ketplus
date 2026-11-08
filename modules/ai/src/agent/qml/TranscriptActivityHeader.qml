import Ket
import QtQuick
import Ket 1.0 as Kit

Kit.KetActivityRow {
    id: activityHeader
    required property var activity
    readonly property var tokens: activity.tokens
    objectName: "transcriptActivityHeader"
    width: parent.width
    height: Math.max(recipe.height, activityTitle.implicitHeight)
    contentItem: Item {}
    activeFocusOnTab: false
    Accessible.name: activity.displayTitle
    Accessible.description: activity.hasDetails ? (activity.expanded ? "Collapse details" : "Expand details") : ""
    // Height of one title line; the icon and chevron centre on the first line.
    FontMetrics {
        id: titleMetrics
        font: activityTitle.font
    }
    readonly property real titleTop: activity.expanded ? 0 : Math.max(0, (height - activityTitle.implicitHeight) / 2)
    readonly property real firstLineCenter: titleTop + titleMetrics.height / 2
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: activity.activeFocus ? activity.tokens.focus : "transparent"
        radius: activityHeader.recipe.radius
    }

    KetIcon {
        id: activityIcon
        visible: !activity.compact
        width: activity.compact ? 0 : activityHeader.recipe.iconSize
        height: activityHeader.recipe.iconSize
        anchors.left: parent.left
        anchors.leftMargin: activity.compact ? 0 : activityHeader.recipe.paddingInline
        y: Math.round(activityHeader.firstLineCenter - height / 2)
        name: KetTheme.iconForLucide((activity.iconSource()).toString().split("/").pop().replace(/\.svg$/, ""))
        color: activity.running ? activity.tokens.accentText : activity.tokens.textMuted
        box: activity.compact ? 0 : activityHeader.recipe.iconSize
    }

    Text {
        id: activityTitle
        objectName: "transcriptActivityTitle"
        anchors.left: activityIcon.right
        anchors.leftMargin: activity.compact ? 0 : activityHeader.recipe.spacing
        anchors.right: browserOpen.left
        anchors.rightMargin: KetTheme.px(8, KetTheme.zoom)
        y: activityHeader.titleTop
        // Middle elision only applies to a single line, so a collapsed
        // multi-line command folds onto one; a command is never markup.
        text: activity.expanded && activity.subtype === "command" && !activity.describedCommand ? (activity.running ? "Running command" : "Ran command") + (activity.status === "failed" ? " · Failed" : activity.status === "interrupted" ? " · Interrupted" : "") : activity.expanded ? activity.displayTitle : activity.displayTitle.replace(/\s*\n\s*/g, " ")
        textFormat: Text.PlainText
        color: activity.status === "failed" ? activity.tokens.danger : titleHover.hovered && activity.opensFile ? activity.tokens.accentText : activity.tokens.textSecondary
        font.underline: titleHover.hovered && activity.opensFile
        font.family: activity.subtype === "command" && !activity.describedCommand ? activity.tokens.monoFont : activity.tokens.uiFont
        font.pixelSize: activityHeader.recipe.fontPixels
        font.weight: Font.Normal
        wrapMode: activity.expanded ? Text.WrapAnywhere : Text.NoWrap
        elide: activity.expanded ? Text.ElideNone : Text.ElideMiddle
        maximumLineCount: activity.expanded ? 100000 : 1
        HoverHandler {
            id: titleHover
            cursorShape: activity.opensFile ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
    }

    Kit.KetIcon {
        id: detailChevron
        objectName: "transcriptActivityChevron"
        anchors.right: parent.right
        y: Math.round(activityHeader.firstLineCenter - height / 2)
        width: activity.hasDetails ? KetTheme.px(18, KetTheme.zoom) : 0
        visible: activity.hasDetails
        name: activity.expanded ? "chevron-down" : "chevron-right"
        box: activityHeader.recipe.chevronSize
        color: activity.tokens.textMuted
    }

    Kit.KetButton {
        id: browserOpen
        objectName: "transcriptBrowserOpen"
        visible: activity.browserOpenAction
        anchors.right: detailChevron.left
        anchors.rightMargin: visible ? KetTheme.px(8, KetTheme.zoom) : 0
        y: Math.round(activityHeader.firstLineCenter - height / 2)
        width: visible ? KetTheme.px(46, KetTheme.zoom) : 0
        height: KetTheme.px(26, KetTheme.zoom)
        text: "Open"
        variant: Kit.KetTheme.ButtonVariant.Ghost
        controlSize: Kit.KetTheme.ControlSize.Small
        background: Item {}
        contentItem: Text {
            text: browserOpen.text
            color: activity.tokens.accentText
            font.family: activity.tokens.uiFont
            font.pixelSize: activity.tokens.caption
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        Accessible.name: "Open " + activity.target
        onClicked: activity.host.openRelativeFile(activity.target)
        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }
    }

    Rectangle {
        id: runningShine
        objectName: "toolActivityShine"
        visible: activity.running && !activity.compact
        anchors.verticalCenter: activityIcon.verticalCenter
        width: KetTheme.px(4, KetTheme.zoom)
        height: KetTheme.px(4, KetTheme.zoom)
        radius: KetTheme.px(2, KetTheme.zoom)
        color: activity.tokens.accent
    }

    TapHandler {
        enabled: activity.visible && activity.enabled && (activity.hasDetails || activity.opensFile)
        // Clicking toggles without taking focus, so no focus outline appears;
        // keyboard focus still shows it. A read row's title opens its file
        // instead, and the chevron still toggles.
        onTapped: function (point) {
            if (activity.browserOpenAction && point.position.x >= browserOpen.x)
                return;
            if (activity.opensFile && (!activity.hasDetails || point.position.x < detailChevron.x - KetTheme.px(4, KetTheme.zoom)))
                activity.openFile();
            else
                activity.toggleDetails();
        }
    }
    HoverHandler {
        cursorShape: activity.hasDetails ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
}
