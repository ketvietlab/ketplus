// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket

// Design mock, not kit code: the notched step slider the phone draws (KetStepSlider with
// `sliderNotched` in ket-ios), at desktop size. A pill holds a track with one dot per step;
// the chosen step is a ringed thumb and the track fills up to it, so every step can be seen
// before it is chosen. It replaces ket::StepSlider here only to be judged; the product gets
// it as a notched variant in ket-qt first. Same API as KetStepSlider.
Column {
    id: picker

    property string label
    property var steps: []
    property int currentIndex: 0
    readonly property string currentStep: steps.length > 0 ? steps[currentIndex] : ""
    readonly property var recipe: KetTheme.sliderRecipe(KetTheme.dark, KetTheme.zoom,
                                                        false, false, true, false)
    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    // The phone's 62/24/31/9/44/4, scaled to a desktop row.
    readonly property real pillHeight: px(36)
    readonly property real trackHeight: px(14)
    readonly property real inset: px(18)
    readonly property real dot: px(6)
    readonly property real thumb: px(26)
    readonly property real ring: px(3)

    function centre(index) {
        const track = Math.max(0, notches.width - inset * 2)
        return steps.length > 1 ? inset + track * index / (steps.length - 1) : notches.width / 2
    }
    function nearest(x) {
        const track = Math.max(1, notches.width - inset * 2)
        const fraction = Math.min(1, Math.max(0, (x - inset) / track))
        return Math.round(fraction * Math.max(0, steps.length - 1))
    }

    width: px(KetTheme.t.space16 * 4)
    spacing: 0

    Item {
        width: picker.width
        height: labelText.implicitHeight + picker.recipe.headerGap
        Text {
            id: labelText
            text: picker.label
            color: picker.recipe.label
            font.family: KetTheme.fontFamily
            font.pixelSize: picker.recipe.labelPixels
            font.weight: picker.recipe.labelWeight
        }
        Text {
            anchors.right: parent.right
            text: picker.currentStep
            color: picker.recipe.value
            font.family: KetTheme.fontFamily
            font.pixelSize: picker.recipe.labelPixels
        }
    }

    Rectangle {
        id: notches
        objectName: "notchedStepSlider"
        width: picker.width
        height: picker.pillHeight
        radius: height / 2
        color: Qt.rgba(picker.recipe.track.r, picker.recipe.track.g, picker.recipe.track.b, KetTheme.dark ? 0.16 : 0.35)
        activeFocusOnTab: true
        Accessible.role: Accessible.Slider
        Accessible.name: picker.label
        Accessible.description: picker.currentStep
        Keys.onLeftPressed: picker.currentIndex = Math.max(0, picker.currentIndex - 1)
        Keys.onRightPressed: picker.currentIndex = Math.min(picker.steps.length - 1, picker.currentIndex + 1)

        readonly property real start: picker.inset - picker.trackHeight / 2

        // The track and its fill reach half their height past the end notches, so the end dots
        // sit inside the round caps.
        Rectangle {
            x: notches.start
            y: (parent.height - height) / 2
            width: Math.max(0, parent.width - notches.start * 2)
            height: picker.trackHeight
            radius: height / 2
            color: picker.recipe.track
        }
        Rectangle {
            x: notches.start
            y: (parent.height - height) / 2
            width: Math.max(0, picker.centre(picker.currentIndex) - notches.start + picker.trackHeight / 2)
            height: picker.trackHeight
            radius: height / 2
            color: picker.recipe.fill
            Behavior on width { NumberAnimation { duration: 160; easing.type: Easing.OutQuad } }
        }
        Repeater {
            model: picker.steps.length
            Item {
                required property int index
                readonly property bool chosen: index === picker.currentIndex
                width: picker.thumb
                height: picker.thumb
                x: picker.centre(index) - width / 2
                y: (notches.height - height) / 2
                Rectangle {
                    anchors.fill: parent
                    visible: parent.chosen
                    radius: width / 2
                    color: picker.recipe.handle
                    border.width: picker.ring
                    border.color: picker.recipe.fill
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: picker.dot
                    height: picker.dot
                    radius: width / 2
                    color: parent.chosen ? picker.recipe.fill : picker.recipe.rangeLabel
                }
            }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onPressed: mouse => { notches.forceActiveFocus(); picker.currentIndex = picker.nearest(mouse.x) }
            onPositionChanged: mouse => { if (pressed) picker.currentIndex = picker.nearest(mouse.x) }
        }
    }

    Item {
        width: picker.width
        height: first.implicitHeight + picker.recipe.labelsGap
        Text {
            id: first
            x: picker.px(6)
            y: picker.recipe.labelsGap
            text: picker.steps.length > 0 ? picker.steps[0] : ""
            color: picker.recipe.rangeLabel
            font.family: KetTheme.fontFamily
            font.pixelSize: picker.recipe.rangePixels
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: picker.px(6)
            y: picker.recipe.labelsGap
            text: picker.steps.length > 1 ? picker.steps[picker.steps.length - 1] : ""
            color: picker.recipe.rangeLabel
            font.family: KetTheme.fontFamily
            font.pixelSize: picker.recipe.rangePixels
        }
    }
}
