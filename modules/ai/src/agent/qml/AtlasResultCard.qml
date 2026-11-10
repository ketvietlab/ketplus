import Ket
import QtQuick
import Ket as Kit

Rectangle {
    id: card
    objectName: "atlasResultCard"
    required property string link
    required property var host

    property var result: ({})
    // "onboarding.ketatlas", the bundle the manifest belongs to.
    readonly property string bundle: String(result.relativePath || "").replace(/\/atlas\.json$/, "").split("/").pop()
    // The mock's Card in its transcript (parts/Transcript.qml): 15 px in, 7 px corners, the
    // panel surface, a 15 px semibold title and a 13 px caption.
    readonly property real padding: Kit.KetTheme.px(15, KetTheme.zoom)
    function refresh() { result = host.inspectAtlasResult(link); }
    Component.onCompleted: refresh()
    onLinkChanged: refresh()
    Connections {
        target: card.host
        function onActivePathChanged() { card.refresh(); }
        function onExecutionBranchChanged() { card.refresh(); }
        function onAtlasResultsChanged() { card.refresh(); }
    }
    implicitHeight: content.implicitHeight + 2 * padding
    radius: Kit.KetTheme.px(7, KetTheme.zoom)
    color: KetTheme.t.panelBg
    border.color: KetTheme.t.panelBorder
    border.width: 1
    Accessible.role: Accessible.Grouping
    Accessible.name: result.title ? "Atlas result · " + result.title : "Atlas result"
    Column {
        id: content
        x: card.padding
        y: card.padding
        width: parent.width - 2 * x
        spacing: Kit.KetTheme.px(8, KetTheme.zoom)
        Text {
            width: parent.width
            // The mock's title: the bundle passed the manifest check and has a preview.
            text: card.result.available ? "Validated preview available" : "Atlas result"
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: KetTheme.t.textMain
            font.family: KetTheme.fontFamily
            font.pixelSize: Kit.KetTheme.px(15, KetTheme.zoom)
            font.weight: Font.DemiBold
        }
        Text {
            width: parent.width
            text: card.result.available
                ? card.bundle + " · " + card.result.screens + (card.result.screens === 1 ? " screen · " : " screens · ")
                    + card.result.flows + (card.result.flows === 1 ? " flow" : " flows")
                : card.result.error || "Atlas unavailable."
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: Kit.KetTheme.px(13, KetTheme.zoom)
        }
        Text {
            width: parent.width
            visible: text.length > 0
            height: visible ? implicitHeight : 0
            text: card.result.viewerError || ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: KetTheme.t.danger
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
        }
        Flow {
            width: parent.width
            spacing: Kit.KetTheme.px(8, KetTheme.zoom)
            Kit.KetButton {
                objectName: "editAtlasResultButton"
                text: "Edit in KétPlus"
                iconName: "edit"
                variant: Kit.KetTheme.ButtonVariant.Primary
                visible: !!card.result.available
                onClicked: { card.refresh(); card.host.openAtlasResult(card.link, false); }
            }
            Kit.KetButton {
                objectName: "openAtlasResultBrowserButton"
                text: "Open in browser"
                iconName: "open"
                visible: !!card.result.available
                onClicked: { card.refresh(); card.host.openAtlasResult(card.link, true); }
            }
            Kit.KetButton {
                text: "Refresh"
                controlSize: Kit.KetTheme.ControlSize.Small
                visible: !card.result.available
                onClicked: card.refresh()
            }
        }
    }
}
