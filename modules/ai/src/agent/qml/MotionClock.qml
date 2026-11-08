import QtQuick
import Ket

// Product effects that are not themselves a kit indicator share its stepped clock.
QtObject {
    id: clock
    property bool running: false
    property bool held: false
    readonly property real time: held ? KetTheme.animationClock : 0
    function sync() {
        if (running === held) return
        held = running
        KetTheme.holdAnimationClock(held)
    }
    onRunningChanged: sync()
    Component.onCompleted: sync()
    Component.onDestruction: if (held) KetTheme.holdAnimationClock(false)
}
