import QtQuick
import Ket

// Compatibility adapter for semantic source names used by existing transcript delegates.
// Két selects the optical master and tints it directly; no per-icon MultiEffect layer.
Item {
    id: root
    property url source
    property color color: KetTheme.t.textMain
    implicitWidth: KetTheme.px(16, KetTheme.zoom)
    implicitHeight: implicitWidth
    readonly property string symbolName: {
        const value = source.toString()
        return value.slice(value.lastIndexOf("/") + 1).replace(/\.svg$/, "")
    }
    KetIcon {
        anchors.centerIn: parent
        name: KetTheme.iconForLucide(root.symbolName)
        box: Math.min(root.width, root.height)
        color: root.color
    }
}
