// SPDX-License-Identifier: Apache-2.0
import QtQuick
import AX310.App

/// The effects the driver can write, and their parameters.
///
/// Shared by the desktop window and the deck's panel, so the two cannot disagree
/// about which effects exist or what a slider is worth. The list is built from
/// the driver's parameter table rather than written out here: a parameter located
/// later becomes one row in `protocol::Parameters` and appears on both screens
/// with no QML change at all.
Item {
    id: view

    /// The colour of the mix being edited.
    property color accent: Theme.creator

    /// Sized for a fingertip rather than a pointer. The deck's panel is touched;
    /// the desktop window is not.
    property bool touch: false

    /// Effects with at least one writable parameter. One whose registers are
    /// still unknown is absent rather than shown as an empty tab -- a tab that
    /// opens onto nothing reads as a broken interface, not an honest gap.
    readonly property var groups: {
        let seen = []
        let rows = ax310Device ? ax310Device.parameters : []
        for (let i = 0; i < rows.length; ++i)
            if (seen.indexOf(rows[i].effectName) === -1)
                seen.push(rows[i].effectName)
        return seen
    }

    /// Which effect's parameters are shown.
    property string current: groups.length > 0 ? groups[0] : ""

    implicitHeight: tabs.height + rows.anchors.topMargin + rows.height

    /// How many parameters the effect being shown has.
    readonly property int _rowCount: {
        let rows = ax310Device ? ax310Device.parameters : []
        let count = 0
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].effectName === view.current)
                ++count
        return count
    }

    /// The height to give each row, or zero for "whatever the slider asks for".
    ///
    /// On the panel the rows are fitted to the space rather than given a fixed
    /// height. The tallest effect has five parameters today and a sixth would
    /// slide under the tile row -- silent clipping on a screen nobody is looking
    /// at directly, which is the worst place for it. The desktop window has room
    /// to spare and lets the slider size itself.
    readonly property int _rowHeight: {
        if (!view.touch || view.height <= 0 || view._rowCount <= 0)
            return 0
        let free = view.height - tabs.height - rows.anchors.topMargin
                 - ((view._rowCount - 1) * rows.spacing)
        return Math.max(30, Math.floor(free / view._rowCount))
    }

    Row {
        id: tabs
        anchors.left: parent.left
        anchors.right: parent.right
        height: view.touch ? 40 : 32
        spacing: 2

        Repeater {
            model: view.groups

            Item {
                required property string modelData
                width: view.touch ? view.width / Math.max(1, view.groups.length)
                                  : label.width + Theme.stepLarge
                height: parent.height

                Text {
                    id: label
                    anchors.centerIn: parent
                    text: modelData
                    color: view.current === modelData ? Theme.ink : Theme.inkDim
                    font.family: Theme.legend
                    font.pixelSize: view.touch ? 16 : 13
                    font.letterSpacing: 1.5
                    font.capitalization: Font.AllUppercase
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width; height: 2
                    color: view.current === modelData ? view.accent : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.mixTransition } }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: view.current = modelData
                }
            }
        }
    }

    Column {
        id: rows
        anchors.top: tabs.bottom
        anchors.topMargin: view.touch ? Theme.stepSmall : Theme.step
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: view.touch ? Theme.stepSmall : Theme.stepSmall + 5

        Repeater {
            model: ax310Device ? ax310Device.parameters : []

            ParamSlider {
                required property var modelData

                // Collapsed rather than merely hidden, so the column does not
                // reserve the space of every effect at once.
                visible: modelData.effectName === view.current
                height: !visible ? 0 : view._rowHeight > 0 ? view._rowHeight : implicitHeight
                width: parent.width
                touch: view.touch
                label: modelData.name
                unit: modelData.unit
                minimum: modelData.minimum
                maximum: modelData.maximum
                accent: view.accent
                value: ax310Device ? ax310Device.parameterValue(modelData.id) : 0

                onValueRequested: wanted => {
                    if (ax310Device)
                        ax310Device.setParameter(modelData.id, wanted)
                }

                Connections {
                    target: ax310Device
                    enabled: ax310Device !== null
                    function onParameterChanged(id, updated) {
                        if (id === modelData.id)
                            value = updated
                    }
                }
            }
        }
    }
}
