// SPDX-License-Identifier: Apache-2.0
import QtQuick
import AX310.App

/// One DSP parameter: what it is called, where it sits in its range, its value.
Item {
    id: slider

    property string label: ""
    property string unit: ""
    property int minimum: 0
    property int maximum: 255
    property int value: 0
    property color accent: Theme.creator

    /// Sized for a fingertip rather than a pointer. A 22px row is comfortable
    /// with a mouse and unusable on the deck's panel.
    property bool touch: false

    signal valueRequested(int value)

    implicitHeight: touch ? 40 : 22

    readonly property real _fraction: maximum > minimum
        ? (value - minimum) / (maximum - minimum) : 0

    Text {
        id: name
        width: slider.touch ? 132 : 92
        anchors.verticalCenter: parent.verticalCenter
        text: slider.label
        color: Theme.inkDim
        font.family: Theme.data
        font.pixelSize: slider.touch ? 15 : 12
        elide: Text.ElideRight
    }

    Item {
        id: rail
        anchors.left: name.right
        anchors.leftMargin: Theme.step
        anchors.right: readout.left
        anchors.rightMargin: Theme.step
        anchors.verticalCenter: parent.verticalCenter
        height: parent.height

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width; height: slider.touch ? 3 : 2
            color: Theme.rule
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width * slider._fraction; height: slider.touch ? 3 : 2
            color: slider.accent
            Behavior on color { ColorAnimation { duration: Theme.mixTransition } }
        }
        Rectangle {
            x: parent.width * slider._fraction - (slider.touch ? 2 : 1)
            anchors.verticalCenter: parent.verticalCenter
            width: slider.touch ? 4 : 3
            height: slider.touch ? 22 : 12
            color: Theme.ink
        }

        MouseArea {
            anchors.fill: parent
            onPressed: mouse => slider._apply(mouse.x)
            onPositionChanged: mouse => { if (pressed) slider._apply(mouse.x) }
        }
    }

    Text {
        id: readout
        width: slider.touch ? 96 : 74
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        horizontalAlignment: Text.AlignRight
        text: slider.value + slider.unit
        color: Theme.ink
        font.family: Theme.data
        font.pixelSize: slider.touch ? 17 : 13
    }

    function _apply(x) {
        let fraction = Math.max(0, Math.min(1, x / rail.width))
        slider.valueRequested(Math.round(minimum + fraction * (maximum - minimum)))
    }
}
