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

    // Sized for the deck's own panel, which is what these are for: 800x480 across
    // a few inches, read at arm's length across a desk rather than at a monitor's
    // distance. The tile is 136 tall and the glyph and legend together used barely
    // a third of it, which spent the space on nothing and made both hard to pick
    // out. The glyph leads, because the icon is what is recognised at a glance and
    // the word underneath is what confirms it.
    Column {
        anchors.centerIn: parent
        spacing: Theme.stepSmall

        // Reserved rather than absent, so it has to stay legible while reading as
        // inert -- a tile nobody can read is not obviously a tile that is off.
        opacity: tile.pending ? 0.6 : 1.0

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: tile.glyph
            color: tile.pending ? Theme.inkDim : tile.active ? Theme.ground : tile.accent
            font.family: Theme.data
            font.pixelSize: 40
            Behavior on color { ColorAnimation { duration: Theme.mixTransition } }
        }

        Text {
            // Held inside the tile: the labels are set in a condensed face and fit
            // today, but a longer one would otherwise run out over its neighbours
            // rather than be cut short.
            width: tile.width - Theme.step * 2
            anchors.horizontalCenter: parent.horizontalCenter
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: tile.label
            color: tile.pending ? Theme.inkDim : tile.active ? Theme.ground : Theme.ink
            font.family: Theme.legend
            font.pixelSize: 21
            font.letterSpacing: 2.2
            font.capitalization: Font.AllUppercase
        }
    }

    Text {
        visible: tile.pending
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.stepSmall
        text: tile.note
        color: Theme.inkDim
        font.family: Theme.data
        font.pixelSize: 12
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
