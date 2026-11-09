import Ket
import QtQuick
import QtQuick.Controls.Basic as Basic

// Recovery actions for one changed file. A missing file can still be copied to find it again.
Basic.Menu {
    id: fileMenu
    required property var host
    required property string targetPath
    required property bool missing

    objectName: "changedFileContextMenu"
    padding: KetTheme.px(5, KetTheme.zoom)
    margins: 8
    font.family: KetTheme.fontFamily
    font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
    background: Rectangle {
        implicitWidth: KetTheme.px(190, KetTheme.zoom)
        implicitHeight: KetTheme.px(112, KetTheme.zoom)
        color: KetTheme.t.panelBg || KetTheme.t.surfaceRaised
        border.color: KetTheme.t.borderStrong || KetTheme.t.panelBorder
        border.width: 1
        radius: KetTheme.px(7, KetTheme.zoom)
    }
    Basic.MenuItem {
        objectName: "copyChangedFilePathAction"
        text: "Copy Path"
        onTriggered: fileMenu.host.copyText(fileMenu.host.absoluteFilePath(fileMenu.targetPath))
    }
    Basic.MenuItem {
        objectName: "revealChangedFileAction"
        text: fileMenu.host.revealFileActionText()
        enabled: !fileMenu.missing
        onTriggered: fileMenu.host.revealFile(fileMenu.targetPath)
    }
    Basic.MenuItem {
        objectName: "openChangedFileAction"
        text: "Open File"
        enabled: !fileMenu.missing
        onTriggered: fileMenu.host.openRelativeFile(fileMenu.targetPath)
    }
}
