// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Shapes
import AX310.App

/// One track, drawn the way the hardware presents it.
///
/// The physical control is a knob inside an LED ring, so a vertical fader would
/// misrepresent the instrument. Four concentric arcs, outermost first:
///
///   - the **other mix**, thin and in that mix's own colour, so both mixes are
///     readable per track without switching between them;
///   - the **active mix**, thick, in that mix's colour;
///   - the **live meter**, with a peak-hold tick that decays.
///
/// The meter is genuinely per track: the deck reports a stereo pair for each and
/// this shows the louder side. It was removed for a while on the belief that no
/// per-track signal existed, which came from reading only the first pair -- the
/// microphone -- and a live microphone hears whatever is played into the room.
///
/// The level reads as a percentage in the middle. The deck has 21 steps and one
/// step is exactly 5%, so every value shown is reachable and nothing rounds.
Item {
    id: gauge

    /// This track's level in the mix being edited, 0..100.
    property int level: 0

    /// The same track's level in the mix that is not being edited, 0..100.
    property int otherLevel: 0

    /// Live signal for this track, 0..1.
    property real meter: 0

    /// Peak hold for this track, 0..1.
    property real peak: 0

    /// The colour of the mix being edited.
    property color accent: Theme.creator

    /// The colour of the mix that is not being edited.
    ///
    /// Its own colour rather than a dimmed accent: the outer arc exists so both
    /// mixes are readable per track, and an arc in the colour of the mix it is
    /// not showing says nothing a person can use.
    property color otherAccent: Theme.audience

    /// What the deck prints under this knob.
    property string label: ""

    /// Emitted while dragging. The parent decides whether to believe it.
    /// @param percent The level asked for.
    signal levelRequested(int percent)

    // The design the arcs are drawn against: a 112px dial, then a band under it
    // holding the gap and the legend. Everything else is this times _unit.
    readonly property int _designDial: 112
    readonly property int _designLegend: 20

    implicitWidth: _designDial
    implicitHeight: (_designDial + _designLegend) * _unit

    // A knob's usable travel, not a full circle: 270 degrees with the gap at the
    // bottom, which is where a physical indicator's dead zone sits.
    readonly property int arcStart: 135
    readonly property int arcSweep: 270

    // Scale from the width alone.
    //
    // Subtracting the legend's measured height from the gauge's own would make
    // the legend an input to the scale that sets the legend's pixel size. Qt
    // calls that a binding loop, refuses to settle it, and leaves whichever value
    // it saw first -- rings sized by an accident of evaluation order, and one
    // warning per gauge per repaint. Height follows from the width instead of
    // back into it.
    readonly property real _unit: width / _designDial

    Item {
        id: dial
        width: gauge._designDial * gauge._unit
        height: width
        anchors.horizontalCenter: parent.horizontalCenter

        Shape {
            anchors.fill: parent
            // The triangulating renderer, not the curve renderer.
            //
            // The deck's frames are grabbed from a window the compositor never
            // maps, and each such grab builds a throwaway RHI. CurveRenderer is
            // shader-based and keeps GPU resources across frames, so it draws the
            // first grab and then nothing at all -- the panel went out with the
            // numerals and legends intact and every arc missing. GeometryRenderer
            // triangulates on the CPU and survives every grab.
            preferredRendererType: Shape.GeometryRenderer

            // The other mix: present but quiet.
            ShapePath {
                strokeColor: Theme.rule
                strokeWidth: 2.5 * gauge._unit
                fillColor: "transparent"
                capStyle: ShapePath.FlatCap
                PathAngleArc {
                    centerX: dial.width / 2; centerY: dial.height / 2
                    radiusX: 52 * gauge._unit; radiusY: 52 * gauge._unit
                    startAngle: gauge.arcStart; sweepAngle: gauge.arcSweep
                }
            }
            ShapePath {
                strokeColor: Qt.alpha(gauge.otherAccent, 0.55)
                strokeWidth: 2.5 * gauge._unit
                fillColor: "transparent"
                capStyle: ShapePath.FlatCap
                PathAngleArc {
                    centerX: dial.width / 2; centerY: dial.height / 2
                    radiusX: 52 * gauge._unit; radiusY: 52 * gauge._unit
                    startAngle: gauge.arcStart
                    sweepAngle: gauge.arcSweep * gauge.otherLevel / 100
                }
                Behavior on strokeColor { ColorAnimation { duration: Theme.mixTransition } }
            }

            // The signal on this track.
            ShapePath {
                strokeColor: Theme.rule
                strokeWidth: 4.5 * gauge._unit
                fillColor: "transparent"
                capStyle: ShapePath.FlatCap
                PathAngleArc {
                    centerX: dial.width / 2; centerY: dial.height / 2
                    radiusX: 31 * gauge._unit; radiusY: 31 * gauge._unit
                    startAngle: gauge.arcStart; sweepAngle: gauge.arcSweep
                }
            }
            ShapePath {
                strokeColor: gauge.meter > 0.96 ? Theme.clip : Qt.alpha(gauge.accent, 0.85)
                strokeWidth: 4.5 * gauge._unit
                fillColor: "transparent"
                capStyle: ShapePath.FlatCap
                PathAngleArc {
                    centerX: dial.width / 2; centerY: dial.height / 2
                    radiusX: 31 * gauge._unit; radiusY: 31 * gauge._unit
                    startAngle: gauge.arcStart
                    sweepAngle: gauge.arcSweep * gauge.meter
                }
            }
            // Peak hold: a short tick left at the loudest recent moment, which is
            // what makes a meter readable rather than merely animated.
            ShapePath {
                strokeColor: gauge.peak > 0.96 ? Theme.clip : gauge.accent
                strokeWidth: 4.5 * gauge._unit
                fillColor: "transparent"
                capStyle: ShapePath.FlatCap
                PathAngleArc {
                    centerX: dial.width / 2; centerY: dial.height / 2
                    radiusX: 31 * gauge._unit; radiusY: 31 * gauge._unit
                    startAngle: gauge.arcStart + gauge.arcSweep * gauge.peak
                    sweepAngle: gauge.peak > 0.02 ? 2.5 : 0
                }
            }

            // The mix being edited.
            ShapePath {
                strokeColor: Theme.rule
                strokeWidth: 8 * gauge._unit
                fillColor: "transparent"
                capStyle: ShapePath.FlatCap
                PathAngleArc {
                    centerX: dial.width / 2; centerY: dial.height / 2
                    radiusX: 43 * gauge._unit; radiusY: 43 * gauge._unit
                    startAngle: gauge.arcStart; sweepAngle: gauge.arcSweep
                }
            }
            ShapePath {
                strokeColor: gauge.accent
                strokeWidth: 8 * gauge._unit
                fillColor: "transparent"
                capStyle: ShapePath.FlatCap
                PathAngleArc {
                    centerX: dial.width / 2; centerY: dial.height / 2
                    radiusX: 43 * gauge._unit; radiusY: 43 * gauge._unit
                    startAngle: gauge.arcStart
                    sweepAngle: gauge.arcSweep * gauge.level / 100
                }
                Behavior on strokeColor { ColorAnimation { duration: Theme.mixTransition } }
            }

        }

        // "100%" is four glyphs where a bare number is two, so the numeral
        // carries the size and the unit sits small beside it.
        Row {
            anchors.centerIn: parent
            spacing: 1
            Text {
                text: gauge.level
                color: Theme.ink
                font.family: Theme.data
                font.pixelSize: 22 * gauge._unit
                font.weight: Font.Medium
                anchors.baseline: unit.baseline
            }
            Text {
                id: unit
                text: "%"
                color: Theme.inkDim
                font.family: Theme.data
                font.pixelSize: 10 * gauge._unit
            }
        }

        MouseArea {
            anchors.fill: parent
            property int startY: 0
            property int startLevel: 0
            onPressed: mouse => { startY = mouse.y; startLevel = gauge.level }
            onPositionChanged: mouse => {
                // One hardware step is 5%, so travel is quantised to it -- a
                // drag cannot ask for a level the deck could not hold.
                let steps = Math.round((startY - mouse.y) / (6 * gauge._unit))
                let wanted = Math.max(0, Math.min(100, startLevel + steps * 5))
                if (wanted !== gauge.level)
                    gauge.levelRequested(wanted)
            }
        }
    }

    Text {
        id: legend
        anchors.top: dial.bottom
        anchors.topMargin: Theme.stepTiny * gauge._unit
        anchors.horizontalCenter: parent.horizontalCenter
        text: gauge.label
        color: Theme.inkDim
        font.family: Theme.legend
        font.pixelSize: Math.round(12 * gauge._unit)
        font.letterSpacing: 1.6
        font.capitalization: Font.AllUppercase
    }
}
