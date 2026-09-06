// SPDX-License-Identifier: Apache-2.0
import QtQuick
import AX310.App

/// What the deck shows on its own 5" panel.
///
/// Rendered offscreen at 800x480, encoded as JPEG and pushed over USB, so it is
/// never composited by a desktop and never scaled. It is also read at arm's
/// length in a dim room while its owner is doing something else, which is why it
/// carries fewer things than the desktop window and carries them larger.
Rectangle {
    id: root

    width: 800
    height: 480
    color: Theme.ground

    /// Which mix the deck is monitoring.
    readonly property int mix: ax310Device ? ax310Device.selectedMix : 0
    readonly property color accent: Theme.mixColor(root.mix)

    /// Whether the body shows the effects rather than the tracks.
    ///
    /// A page rather than a separate screen: the tile row stays where it is and
    /// the Effects tile lights up, so the way back is the control that got you
    /// here. Re-cutting the row for a back button would move every other tile.
    ///
    /// Kept on the bridge rather than here, because this file is instantiated
    /// twice -- once for the frames the deck receives, and again as the preview
    /// inside the desktop window. Two copies of the state would let the preview
    /// show a different page from the deck it claims to be showing.
    property bool showingEffects: ax310Device ? ax310Device.panelShowsEffects : _localPage

    /// Where the page is kept when there is no bridge to keep it on.
    property bool _localPage: false

    // The header takes the mix's own colour, so which mix you are in is legible
    // from across a desk -- the same signal the deck's LED rings send.
    Rectangle {
        id: header
        width: parent.width
        height: 64
        color: Qt.alpha(root.accent, 0.14)
        Behavior on color { ColorAnimation { duration: Theme.mixTransition } }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width; height: 2
            color: root.accent
            Behavior on color { ColorAnimation { duration: Theme.mixTransition } }
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.stepLarge
            anchors.verticalCenter: parent.verticalCenter
            text: root.mix === 1 ? "Audience mix" : "Creator mix"
            color: root.accent
            font.family: Theme.legend
            font.pixelSize: 22
            font.letterSpacing: 3.2
            font.capitalization: Font.AllUppercase
            Behavior on color { ColorAnimation { duration: Theme.mixTransition } }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: Theme.stepLarge
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.stepSmall

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: Theme.iconDot
                color: ax310Device && ax310Device.isConnected ? root.accent : Theme.inkDim
                font.family: Theme.data
                font.pixelSize: 11
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "48k · 24-bit"
                color: Theme.inkDim
                font.family: Theme.data
                font.pixelSize: 13
            }
        }
    }

    MixerView {
        id: mixer
        anchors.top: header.bottom
        anchors.bottom: tiles.top
        anchors.left: parent.left
        anchors.right: parent.right
        visible: !root.showingEffects
        mix: root.mix
        gaugeSize: 133
    }

    EffectsView {
        // Named so a test can ask whether its rows fit the band it was given.
        objectName: "effectsPage"
        anchors.top: header.bottom
        anchors.topMargin: Theme.stepSmall
        anchors.bottom: tiles.top
        anchors.bottomMargin: Theme.stepSmall
        anchors.left: parent.left
        anchors.leftMargin: Theme.stepLarge
        anchors.right: parent.right
        anchors.rightMargin: Theme.stepLarge
        visible: root.showingEffects
        accent: root.accent
        touch: true
    }

    Rectangle {
        anchors.bottom: tiles.top
        width: parent.width; height: 1
        color: Theme.rule
    }

    Row {
        id: tiles
        anchors.bottom: parent.bottom
        width: parent.width
        height: 136
        spacing: 0

        ActionTile {
            width: root.width / 5; height: parent.height
            glyph: Theme.iconSwitch; label: "Switch mix"; accent: root.accent
            onActivated: if (ax310Device) ax310Device.selectMix(root.mix === 0 ? 1 : 0)
        }
        ActionTile {
            width: root.width / 5; height: parent.height
            glyph: Theme.iconMute; label: "Mute mic"; accent: root.accent
            onActivated: if (ax310Device) ax310Device.setLevel(root.mix, 0, 0)
        }
        ActionTile {
            width: root.width / 5; height: parent.height
            glyph: Theme.iconMonitor; label: "Monitor"; accent: root.accent
            pending: true
        }
        ActionTile {
            width: root.width / 5; height: parent.height
            glyph: Theme.iconEffects; label: "Effects"; accent: root.accent
            active: root.showingEffects
            onActivated: {
                if (ax310Device)
                    ax310Device.panelShowsEffects = !root.showingEffects
                else
                    root._localPage = !root._localPage
            }
        }
        // The register is known -- 0x21 takes 0x80 for Single and 0x00 for Dual --
        // but the same address also selects which knob rings light, and which of
        // the two it is doing has never been settled on the hardware. Driving it
        // on a guess would leave somebody's deck in a state they did not ask for,
        // so the tile says what is true instead.
        ActionTile {
            width: root.width / 5; height: parent.height
            glyph: Theme.iconDualMix; label: "Dual mix"; accent: root.accent
            pending: true
            note: "unverified"
        }
    }
}
