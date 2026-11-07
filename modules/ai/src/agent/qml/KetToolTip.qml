import QtQuick
import Ket as Kit

// Retain the product call sites while rendering the shared tooltip recipe.
Kit.KetToolTip {
    objectName: "ketToolTip"
    property var tokens
    readonly property color backgroundColor: recipe.background
    readonly property color labelColor: recipe.text
    readonly property real cornerRadius: recipe.radius
}
