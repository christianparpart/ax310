// SPDX-License-Identifier: Apache-2.0
pragma Singleton

import QtQuick

/// The one place a colour, a face or a duration is decided.
///
/// Before this, every value was an inline literal and the two screens used two
/// unrelated palettes -- so nothing could be changed in one place, and nothing
/// agreed with anything else.
QtObject {
    id: theme

    // ── Colour ───────────────────────────────────────────────────────────
    //
    // Taken from the deck itself: its LED rings glow blue for the creator mix
    // and orange for the audience mix, so the hardware already has a colour
    // language for the single most important piece of state. These two are
    // information, not accent -- they say which mix you are in.

    readonly property color ground:   "#0E1113"   // near-black, faint blue-green: unlit glass
    readonly property color chassis:  "#171B1E"
    readonly property color rule:     "#262C30"   // dividers, and the unlit part of a ring
    readonly property color ink:      "#E6EAEC"
    readonly property color inkDim:   "#7E888E"
    readonly property color creator:  "#2FA8FF"
    readonly property color audience: "#FF9130"
    readonly property color clip:     "#FF4D5A"   // peak only, never decoration

    /// @param mix An ax310::MixId, as an int.
    /// @return The colour that mix is shown in.
    function mixColor(mix) {
        return mix === 1 ? theme.audience : theme.creator
    }

    // ── Type ─────────────────────────────────────────────────────────────
    //
    // Bundled rather than named, because the panel is rendered to a JPEG and
    // pushed to hardware: its appearance cannot depend on what a host machine
    // happens to have installed. Loading them here means every entry point --
    // the application, the tests, the screenshot tool -- gets them by importing
    // the theme, with no C++ to duplicate.

    readonly property FontLoader legendFont: FontLoader {
        source: "fonts/BarlowCondensed-SemiBold.ttf"
    }
    readonly property FontLoader legendBoldFont: FontLoader {
        source: "fonts/BarlowCondensed-Bold.ttf"
    }
    readonly property FontLoader dataFont: FontLoader {
        source: "fonts/JetBrainsMonoNF-Regular.ttf"
    }
    readonly property FontLoader dataMediumFont: FontLoader {
        source: "fonts/JetBrainsMonoNF-Medium.ttf"
    }
    readonly property FontLoader dataBoldFont: FontLoader {
        source: "fonts/JetBrainsMonoNF-Bold.ttf"
    }

    /// Condensed grotesque for legends: it reads as silkscreen on equipment, and
    /// condensed is what keeps six track names legible under six rings.
    readonly property string legend: legendFont.name

    /// Monospace for every numeral and all interface text. The bundled copy is a
    /// Nerd Font subset, so the icons below come from the same file -- no icon
    /// library, and identical rendering inside the JPEG sent to the deck.
    readonly property string data: dataFont.name

    // ── Icons, from the bundled Nerd Font ────────────────────────────────

    readonly property string iconMute:    ""
    readonly property string iconMonitor: ""
    readonly property string iconSwitch:  ""
    readonly property string iconEffects: ""
    readonly property string iconDualMix: ""
    readonly property string iconDot:     ""

    // ── Rhythm ───────────────────────────────────────────────────────────

    readonly property int stepTiny: 4
    readonly property int stepSmall: 8
    readonly property int step: 16
    readonly property int stepLarge: 24

    /// How long a mix switch takes to cross-fade. The one orchestrated moment in
    /// the interface; everything else stays still.
    readonly property int mixTransition: 220
}
