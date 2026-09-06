// SPDX-License-Identifier: Apache-2.0
import QtQuick
import AX310.App

/// One of the deck panel's big touch targets.
Item {
    id: tile

    property string label: ""
    property string glyph: ""

    /// Reserved rather than omitted: the control exists on the device but cannot
    /// be driven yet, so the slot is held and made visibly inert. Re-cutting the
    /// row later would move every other tile and invalidate the muscle memory
    /// built in the meantime.
    property bool pending: false

    /// Why it is inert, in two words. Shown only when pending.
    property string note: "not mapped"

    /// Whether this tile is the state the panel is currently in. A tile that
    /// opens a page stays lit while that page is open, so the way back is
    /// visibly the same control.
    property bool active: false

    property color accent: Theme.creator

    signal activated()

    Rectangle {
        anchors.fill: parent
        color: tile.active ? tile.accent
             : mouse.pressed && !tile.pending ? Qt.lighter(Theme.chassis, 1.5)
             : "transparent"
        Behavior on color { ColorAnimation { duration: 90 } }
    }

    Rectangle {
        width: 1
        height: parent.height
        anchors.right: parent.right
        color: Theme.rule
    }

    Column {
        anchors.centerIn: parent
        spacing: Theme.stepSmall
        opacity: tile.pending ? 0.45 : 1.0

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: tile.glyph
            color: tile.pending ? Theme.inkDim : tile.active ? Theme.ground : tile.accent
            font.family: Theme.data
            font.pixelSize: 22
            Behavior on color { ColorAnimation { duration: Theme.mixTransition } }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: tile.label
            color: tile.pending ? Theme.inkDim : tile.active ? Theme.ground : Theme.ink
            font.family: Theme.legend
            font.pixelSize: 16
            font.letterSpacing: 1.9
            font.capitalization: Font.AllUppercase
        }
    }

    Text {
        visible: tile.pending
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.step
        text: tile.note
        color: Theme.inkDim
        font.family: Theme.data
        font.pixelSize: 9
        font.letterSpacing: 0.8
        font.capitalization: Font.AllUppercase
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        enabled: !tile.pending
        onClicked: tile.activated()
    }
}
