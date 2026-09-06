// SPDX-License-Identifier: Apache-2.0
import QtQuick
import AX310.App

/// The six tracks, side by side. Shared by both screens.
///
/// Holds the levels for both mixes and the meter ballistics, so the desktop
/// window and the deck's panel show the same state without either owning it.
Item {
    id: mixer

    /// Which mix is being edited, as an ax310::MixId int.
    property int mix: 0

    /// How big a ring is. The panel wants them thumb-sized; the desktop does not.
    property int gaugeSize: 112

    /// Levels per mix, 0..100 each. Populated from the deck on connect.
    property var levels: [[0, 0, 0, 0, 0, 0], [0, 0, 0, 0, 0, 0]]

    /// Smoothed signal per track, 0..1, and the peak held for each.
    property var meters: [0, 0, 0, 0, 0, 0]
    property var peaks: [0, 0, 0, 0, 0, 0]

    readonly property color accent: Theme.mixColor(mixer.mix)

    implicitHeight: strip.implicitHeight
    implicitWidth: strip.implicitWidth

    Row {
        id: strip
        anchors.centerIn: parent
        spacing: 0

        Repeater {
            model: ax310Device ? ax310Device.trackNames : []

            RingGauge {
                required property int index
                required property string modelData

                width: mixer.gaugeSize
                label: modelData
                accent: mixer.accent
                level: mixer.levels[mixer.mix][index]
                meter: mixer.meters[index]
                peak: mixer.peaks[index]
                otherLevel: mixer.levels[mixer.mix === 0 ? 1 : 0][index]

                onLevelRequested: percent => {
                    // Applied locally first so a drag stays smooth, then sent.
                    // The deck emits nothing for our own writes, so the optimism
                    // is not laziness -- there is no echo to wait for.
                    let next = mixer.levels.slice()
                    next[mixer.mix] = next[mixer.mix].slice()
                    next[mixer.mix][index] = percent
                    mixer.levels = next
                    if (ax310Device)
                        ax310Device.setLevel(mixer.mix, index, percent)
                }
            }
        }
    }

    // Read the current state rather than waiting for it to change. QML loaded
    // after a connect would otherwise sit at zero until somebody moved a knob.
    Component.onCompleted: {
        if (!ax310Device)
            return
        let next = [[], []]
        for (let mix = 0; mix < 2; ++mix)
            for (let knob = 0; knob < 6; ++knob)
                next[mix].push(ax310Device.levelPercent(mix, knob))
        mixer.levels = next
    }

    Connections {
        target: ax310Device
        enabled: ax310Device !== null

        function onLevelChanged(mix, knob, percent) {
            let next = mixer.levels.slice()
            next[mix] = next[mix].slice()
            next[mix][knob] = percent
            mixer.levels = next
        }

        function onAudioMetersChanged(levels) {
            mixer.targets = levels
        }
    }

    /// Where the meters are heading, straight off the wire.
    property var targets: [0, 0, 0, 0, 0, 0]

    // Broadcast ballistics rather than a plain binding. A meter that follows the
    // signal exactly is unreadable: it flickers on transients and collapses
    // between syllables. Fast attack keeps transients visible, slow release keeps
    // the bar where the eye can find it, and the peak tick marks the loudest
    // recent moment and decays away from it.
    /// @param current Where the meter is.
    /// @param target Where the signal is.
    /// @return The next value: fast toward a rise, slow away from one.
    function _step(current, target) {
        return current + (target - current) * (target > current ? 0.75 : 0.10)
    }

    /// @param peak The mark currently held.
    /// @param level The meter now.
    /// @return The mark, moved up instantly or decayed slightly.
    function _hold(peak, level) {
        return level > peak ? level : Math.max(level, peak - 0.012)
    }

    Timer {
        interval: 33
        running: true
        repeat: true
        onTriggered: {
            let nextMeters = []
            let nextPeaks = []
            for (let i = 0; i < 6; ++i) {
                let value = mixer._step(mixer.meters[i], (mixer.targets[i] || 0) / 100)
                nextMeters.push(value)
                nextPeaks.push(mixer._hold(mixer.peaks[i], value))
            }
            mixer.meters = nextMeters
            mixer.peaks = nextPeaks
        }
    }
}
