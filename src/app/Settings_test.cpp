// SPDX-License-Identifier: Apache-2.0
/// What the interface remembers between runs, and what it refuses to remember.

#include <app/Settings.hpp>
#include <ax310/Protocol.hpp>

#include <QSettings>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <utility>

using namespace ax310;

namespace
{

/// A settings file of its own, so a test never writes into whoever runs it.
struct SettingsFixture
{
    QTemporaryDir directory;
    QString path { directory.filePath("ax310.ini") };
};

} // namespace

TEST_CASE("a settings file nobody has written leaves every effect off", "[settings]")
{
    SettingsFixture const fixture;
    REQUIRE(fixture.directory.isValid());

    auto const state = app::Settings { fixture.path }.effects();

    // The default has to be silence rather than whatever the deck was left in:
    // the DSP has no read-back, so an unprocessed signal is the only starting
    // point this project can define without guessing somebody's taste.
    CHECK(std::ranges::none_of(state.enabled, [](bool on) { return on; }));

    // Every parameter unset, which is the difference that matters. A parameter
    // arriving with a value nobody chose is written to the deck on the next
    // connect, and because a parameter write sends its whole framed body, that
    // puts a captured effect configuration back on the hardware.
    CHECK(std::ranges::none_of(state.parameters,
                               [](auto const& chosen) { return chosen.has_value(); }));
}

TEST_CASE("the effects chain comes back the way it was left", "[settings]")
{
    SettingsFixture const fixture;
    REQUIRE(fixture.directory.isValid());

    EffectState wanted;
    wanted.enabled.front() = true;
    wanted.enabled.back() = true;

    // Two chosen and the rest left alone, so the round trip has to carry which
    // is which and not merely the numbers.
    wanted.parameters[protocol::indexOf(protocol::Parameter::ReverbDecay)] = 90;
    wanted.parameters[protocol::indexOf(protocol::Parameter::CompressorRatio)] = 4;

    app::Settings { fixture.path }.setEffects(wanted);

    auto const restored = app::Settings { fixture.path }.effects();
    CHECK(restored.enabled == wanted.enabled);
    CHECK(restored.parameters == wanted.parameters);
}

TEST_CASE("a frame rate outside what the driver will send is brought inside it", "[settings]")
{
    SettingsFixture const fixture;
    REQUIRE(fixture.directory.isValid());

    app::Settings settings { fixture.path };

    settings.setPanelFps(100000);
    CHECK(settings.panelFps() == protocol::MaxPanelFps);

    settings.setPanelFps(0);
    CHECK(settings.panelFps() == protocol::MinPanelFps);

    settings.setPanelFps(protocol::DefaultPanelFps);
    CHECK(settings.panelFps() == protocol::DefaultPanelFps);
}

TEST_CASE("a frame rate written straight into the file is capped when read", "[settings]")
{
    SettingsFixture const fixture;
    REQUIRE(fixture.directory.isValid());

    // Written past the setter, the way somebody editing the file by hand would.
    // A cap enforced only on the way in is not a cap: the number that costs a
    // core and a share of the USB bus is the one that gets used, not the one that
    // was typed into a dialog.
    {
        QSettings store { fixture.path, QSettings::IniFormat };
        store.setValue("panel/framesPerSecond", 10000);
    }

    CHECK(app::Settings { fixture.path }.panelFps() == protocol::MaxPanelFps);
}

TEST_CASE("a parameter this build does not know is left where it lies", "[settings]")
{
    SettingsFixture const fixture;
    REQUIRE(fixture.directory.isValid());

    // A key from a build that knew a parameter this one does not. Keyed by the
    // parameter's own number rather than by position, an unknown key is simply
    // not read -- where a positional block would shift every value after it onto
    // the wrong parameter.
    {
        QSettings store { fixture.path, QSettings::IniFormat };
        store.setValue("effects/parameter/99", 7);
        store.setValue(QStringLiteral("effects/parameter/%1")
                           .arg(std::to_underlying(protocol::Parameter::ReverbDamp)),
                       33);
    }

    auto const state = app::Settings { fixture.path }.effects();
    CHECK(state.parameters[protocol::indexOf(protocol::Parameter::ReverbDamp)] == 33);
    CHECK(std::ranges::count_if(state.parameters,
                                [](auto const& chosen) { return chosen.has_value(); })
          == 1);
}
