import QtQuick
import Ket as Kit

Kit.KetUsageRing {
    id: ring
    objectName: "usageLimitsButton"
    property var tokens: ({})
    property real progress: 0
    // Compatibility inputs for callers; the shared recipe owns their presentation.
    property color trackColor: "transparent"
    property color fillColor: "transparent"
    property color hoverColor: "transparent"
    value: Math.max(0, Math.min(1, progress))
    Accessible.name: "Context and usage limits"
}
