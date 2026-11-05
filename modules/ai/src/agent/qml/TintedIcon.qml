import QtQuick
import QtQuick.Effects

Item {
    id: root
    property url source
    property color color: "white"

    // Lucide icons load through the lucide image provider, which fits the stroke to the size the
    // icon is shown at. Other sources load unchanged.
    readonly property string lucidePrefix: "qrc:/agent/icons/"
    readonly property url resolvedSource: {
        const text = root.source.toString()
        if (!text.startsWith(root.lucidePrefix) || !text.endsWith(".svg"))
            return root.source
        if (root.width <= 0)
            return ""
        return "image://lucide/" + text.slice(root.lucidePrefix.length, -4) + "/"
            + Math.round(root.width * 2) / 2
    }

    Image {
        id: sourceImage
        anchors.fill: parent
        source: root.resolvedSource
        sourceSize.width: width
        sourceSize.height: height
        fillMode: Image.PreserveAspectFit
        visible: false
        smooth: true
    }

    MultiEffect {
        anchors.fill: parent
        source: sourceImage
        colorization: 1.0
        colorizationColor: root.color
    }
}
