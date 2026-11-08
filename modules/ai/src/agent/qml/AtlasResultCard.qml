import Ket
import QtQuick
import Ket as Kit

Rectangle {
    id: card
    objectName: "atlasResultCard"
    required property string link
    required property var host

    property var result: ({})
    readonly property var recipe: Kit.KetTheme.updateCardRecipe(KetTheme.dark, KetTheme.zoom, false)
    function refresh() { result = host.inspectAtlasResult(link); }
    Component.onCompleted: refresh()
    onLinkChanged: refresh()
    Connections {
        target: card.host
        function onActivePathChanged() { card.refresh(); }
        function onExecutionBranchChanged() { card.refresh(); }
        function onAtlasResultsChanged() { card.refresh(); }
    }
    implicitHeight: content.implicitHeight + 2 * recipe.paddingBlock
    radius: recipe.radius
    color: recipe.background
    border.color: recipe.border
    border.width: 1
    Accessible.role: Accessible.Grouping
    Accessible.name: result.title || "Atlas result"
    Column {
        id: content
        x: card.recipe.paddingInline
        y: card.recipe.paddingBlock
        width: parent.width - 2 * x
        spacing: Kit.KetTheme.px(6, KetTheme.zoom)
        Text {
            width: parent.width
            text: card.result.title || "Atlas result"
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: card.recipe.title
            font.family: KetTheme.fontFamily
            font.pixelSize: card.recipe.titlePixels
            font.weight: card.recipe.titleWeight
        }
        Text {
            width: parent.width
            text: card.result.available
                ? card.result.screens + (card.result.screens === 1 ? " screen · " : " screens · ")
                    + card.result.flows + (card.result.flows === 1 ? " flow · " : " flows · ") + card.result.relativePath
                : card.result.error || "Atlas unavailable."
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: card.recipe.detail
            font.family: KetTheme.fontFamily
            font.pixelSize: card.recipe.detailPixels
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
            spacing: Kit.KetTheme.px(6, KetTheme.zoom)
            Kit.KetButton {
                objectName: "editAtlasResultButton"
                text: "Edit in KétPlus"
                controlSize: Kit.KetTheme.ControlSize.Small
                visible: !!card.result.available
                onClicked: { card.refresh(); card.host.openAtlasResult(card.link, false); }
            }
            Kit.KetButton {
                objectName: "openAtlasResultBrowserButton"
                text: "Open in browser"
                controlSize: Kit.KetTheme.ControlSize.Small
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
