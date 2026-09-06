// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls
import AX310.App

/// The desktop window.
///
/// Deliberately thin: the six tracks are a MixerView, shared with the deck's own
/// panel, so the two screens cannot disagree about what the device is doing.
ApplicationWindow {
    id: window

    width: 1000
    height: 840
    minimumWidth: 860
    minimumHeight: 620
    visible: true
    title: "AX310"
    color: Theme.chassis

    readonly property int mix: ax310Device ? ax310Device.selectedMix : 0
    readonly property color accent: Theme.mixColor(window.mix)
    readonly property bool online: ax310Device ? ax310Device.isConnected : false

    // ── status ───────────────────────────────────────────────────────────

    Item {
        id: statusbar
        width: parent.width
        height: 52

        Row {
            anchors.left: parent.left
            anchors.leftMargin: Theme.step
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.step

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "AX310"
                color: Theme.ink
                font.family: Theme.legend
                font.pixelSize: 17
                font.letterSpacing: 2.7
                font.capitalization: Font.AllUppercase
            }

            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 7
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Theme.iconDot
                    color: window.online ? window.accent : Theme.inkDim
                    font.family: Theme.data
                    font.pixelSize: 11
                    Behavior on color { ColorAnimation { duration: Theme.mixTransition } }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    // Says what is true, and what to do about it when it is not.
                    text: window.online ? "live · 48 kHz · 24-bit" : "no deck found — plug one in"
                    color: Theme.inkDim
                    font.family: Theme.data
                    font.pixelSize: 11
                }
            }

        }

        // The control that changes what every ring means, so it sits where the
        // eye lands last and nothing else competes with it.
        Row {
            anchors.right: parent.right
            anchors.rightMargin: Theme.step
            anchors.verticalCenter: parent.verticalCenter
            spacing: 0

            Repeater {
                model: ["Creator", "Audience"]

                Rectangle {
                    required property int index
                    required property string modelData

                    width: 96
                    height: 28
                    color: window.mix === index
                        ? (index === 1 ? Theme.audience : Theme.creator) : "transparent"
                    border.color: Theme.rule
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: Theme.mixTransition } }

                    Text {
                        anchors.centerIn: parent
                        text: modelData
                        color: window.mix === index ? Theme.ground : Theme.inkDim
                        font.family: Theme.legend
                        font.pixelSize: 12
                        font.letterSpacing: 1.7
                        font.capitalization: Font.AllUppercase
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: if (ax310Device) ax310Device.selectMix(index)
                    }
                }
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width; height: 1
            color: Theme.rule
        }
    }

    // ── the tracks ───────────────────────────────────────────────────────

    MixerView {
        id: mixer
        anchors.top: statusbar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 172
        mix: window.mix
        gaugeSize: 128
    }

    // ── microphone processing ────────────────────────────────────────────

    Rectangle {
        id: effects
        anchors.top: mixer.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 296
        color: "transparent"

        Rectangle {
            anchors.top: parent.top
            width: parent.width; height: 1
            color: Theme.rule
        }

        Text {
            id: effectsTitle
            anchors.top: parent.top
            anchors.topMargin: Theme.step
            anchors.left: parent.left
            anchors.leftMargin: Theme.step
            text: "Microphone processing"
            color: Theme.inkDim
            font.family: Theme.legend
            font.pixelSize: 12
            font.letterSpacing: 1.7
            font.capitalization: Font.AllUppercase
        }

        EffectsView {
            anchors.top: effectsTitle.bottom
            anchors.topMargin: Theme.stepSmall
            anchors.left: parent.left
            anchors.leftMargin: Theme.step
            anchors.right: parent.right
            anchors.rightMargin: Theme.step
            accent: window.accent
        }
    }

    // ── what the deck is showing ─────────────────────────────────────────

    Item {
        anchors.top: effects.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        Rectangle {
            anchors.top: parent.top
            width: parent.width; height: 1
            color: Theme.rule
        }

        Text {
            id: previewTitle
            anchors.top: parent.top
            anchors.topMargin: Theme.step
            anchors.left: parent.left
            anchors.leftMargin: Theme.step
            text: "On the deck"
            color: Theme.inkDim
            font.family: Theme.legend
            font.pixelSize: 12
            font.letterSpacing: 1.7
            font.capitalization: Font.AllUppercase
        }

        // A live preview of the panel, at the scale it is actually sent. It
        // replaces a labelled empty rectangle that had been standing in for one,
        // and it means the deck's screen can be worked on without the deck.
        // Named so a test can find it and click through it: the preview is live,
        // not a picture, and a scale transform that stopped mapping input would
        // leave it looking right and ignoring every click.
        Item {
            id: preview
            objectName: "deckPreview"
            anchors.top: previewTitle.bottom
            anchors.topMargin: Theme.stepSmall
            anchors.horizontalCenter: parent.horizontalCenter
            width: 800 * preview.factor
            height: 480 * preview.factor
            clip: true

            // Not called `scale`: Item already has one, and shadowing it makes
            // the container scale itself as well as its contents.
            readonly property real factor: 0.42

            ScreenUI {
                transform: Scale { xScale: preview.factor; yScale: preview.factor }
            }

            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.color: Theme.rule
                border.width: 1
            }
        }
    }
}
