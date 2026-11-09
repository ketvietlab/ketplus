import Ket
import QtQuick
import Ket as Kit

Kit.KetActivityRow {
    id: activityHeader
    required property var activity

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
    readonly property real titleTop: Math.max(0, (height - activityTitle.implicitHeight) / 2)
    readonly property real firstLineCenter: titleTop + titleMetrics.height / 2
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: activity.activeFocus ? KetTheme.t.colorFocus : "transparent"
        radius: activityHeader.recipe.radius
    }

    KetIcon {
        id: activityIcon
        width: activityHeader.recipe.iconSize
        height: activityHeader.recipe.iconSize
        anchors.left: parent.left
        anchors.leftMargin: activityHeader.recipe.paddingInline
        y: Math.round(activityHeader.firstLineCenter - height / 2)
        name: activity.status === "failed" ? KetTheme.iconForLucide("circle-x") : KetTheme.iconForLucide((activity.iconSource()).toString().split("/").pop().replace(/\.svg$/, ""))
        color: activity.running ? KetTheme.t.accentText : activityHeader.recipe.text
        box: activityHeader.recipe.iconSize
    }

    Text {
        id: activityTitle
        objectName: "transcriptActivityTitle"
        anchors.left: activityIcon.right
        anchors.leftMargin: activityHeader.recipe.spacing
        y: activityHeader.titleTop
        width: Math.min(implicitWidth, browserOpen.x - x - KetTheme.px(8, KetTheme.zoom))
        // One line, as the mock's step: the title, then what it acted on beside it.
        text: activity.headline
        textFormat: Text.PlainText
        color: titleHover.hovered && activity.opensFile ? KetTheme.t.accentText : activityHeader.recipe.text
        font.underline: titleHover.hovered && activity.opensFile
        font.family: KetTheme.fontFamily
        font.pixelSize: activityHeader.recipe.fontPixels
        font.weight: Font.Normal
        elide: Text.ElideRight
        maximumLineCount: 1
        HoverHandler {
            id: titleHover
            cursorShape: activity.opensFile ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
    }

    Text {
        id: activityDetail
        objectName: "transcriptActivityDetail"
        anchors.left: activityTitle.right
        anchors.leftMargin: visible ? activityHeader.recipe.spacing : 0
        anchors.right: browserOpen.left
        anchors.rightMargin: KetTheme.px(8, KetTheme.zoom)
        anchors.baseline: activityTitle.baseline
        visible: text.length > 0
        // A command or a path is never markup; the middle keeps both of its ends.
        text: activity.headDetail
        textFormat: Text.PlainText
        color: activityHeader.recipe.detail
        font.family: KetTheme.fontFamily
        font.pixelSize: activityHeader.recipe.detailPixels
        elide: Text.ElideMiddle
        maximumLineCount: 1
    }

    Kit.KetIcon {
        id: detailChevron
        objectName: "transcriptActivityChevron"
        anchors.right: parent.right
        y: Math.round(activityHeader.firstLineCenter - height / 2)
        width: activity.hasDetails ? KetTheme.px(18, KetTheme.zoom) : 0
        visible: activity.hasDetails
        name: activity.expanded ? "chevron-up" : "chevron-down"
        box: activityHeader.recipe.chevronSize
        color: activityHeader.recipe.text
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
            color: KetTheme.t.accentText
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
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
        visible: activity.running
        anchors.verticalCenter: activityIcon.verticalCenter
        width: KetTheme.px(4, KetTheme.zoom)
        height: KetTheme.px(4, KetTheme.zoom)
        radius: KetTheme.px(2, KetTheme.zoom)
        color: KetTheme.t.accent
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
