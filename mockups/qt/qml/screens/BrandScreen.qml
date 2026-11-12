// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Shapes
import QtQuick.Layouts
import Ket
import KetPlusMock

Basic.ScrollView {
    id: screen
    clip: true
    contentWidth: availableWidth
    ColumnLayout {
        width: screen.availableWidth
        spacing: 0
        Repeater {
            model: [false, true]
            Rectangle {
                id: panel
                required property bool modelData
                Layout.fillWidth: true
                implicitHeight: specimens.implicitHeight + 48
                color: modelData ? "#17171C" : "#FFFFFF"
                ColumnLayout {
                    id: specimens
                    x: 24; y: 24
                    spacing: 20
                    Text {
                        text: "Brand · " + (panel.modelData ? "Dark" : "Light") + " · Lite / iOS & Pro"
                        color: panel.modelData ? "#F5F5F7" : "#202024"
                        font.family: KetTheme.fontFamily; font.pixelSize: 18
                    }
                    Repeater {
                        model: [17, 32, 64]
                        RowLayout {
                            required property int modelData
                            spacing: 32
                            BrandLockup { size: modelData; dark: panel.modelData; textColor: panel.modelData ? "#F5F5F7" : "#202024" }
                            BrandLockup { edition: "pro"; size: modelData; dark: panel.modelData; textColor: panel.modelData ? "#F5F5F7" : "#202024" }
                        }
                    }
                    RowLayout {
                        spacing: 24
                        Repeater {
                            model: [16, 32, 64, 128]
                            Image {
                                required property int modelData
                                source: "qrc:/qt/qml/KetPlusMock/assets/ketplus-mark-v2.png"
                                Layout.preferredWidth: modelData; Layout.preferredHeight: modelData
                                sourceSize: Qt.size(modelData * 2, modelData * 2)
                            }
                        }
                        Repeater {
                            model: ["#FFFFFF", "#17171C"]
                            Item {
                                required property string modelData
                                width: 60; height: 60
                                Shape {
                                    anchors.fill: parent
                                    ShapePath {
                                        fillColor: modelData
                                        strokeColor: panel.modelData ? "#45454D" : "#D9D9E0"
                                        strokeWidth: 0.5
                                        PathSvg { path: "M30 0 C54 0 60 6 60 30 C60 54 54 60 30 60 C6 60 0 54 0 30 C0 6 6 0 30 0 Z" }
                                    }
                                }
                                Image { anchors.fill: parent; anchors.margins: 4; source: "qrc:/qt/qml/KetPlusMock/assets/ketplus-mark-v2.png"; sourceSize: Qt.size(1024, 1024) }
                            }
                        }
                        Text { text: "iOS · 60 pt"; color: panel.modelData ? "#F5F5F7" : "#202024"; font.pixelSize: 13 }
                    }
                }
            }
        }
    }
}
