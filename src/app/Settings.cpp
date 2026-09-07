// SPDX-License-Identifier: Apache-2.0
#include "Settings.hpp"

#include <ax310/Protocol.hpp>

#include <QSettings>
#include <QVariantList>

#include <memory>
#include <utility>

namespace ax310::app
{

namespace
{
    /// Where the user's own settings live, when no explicit file was given.
    constexpr auto Organisation = "ax310";
    constexpr auto Application = "ax310";

    /// @param path An explicit file, or empty for the user's own.
    /// @return The store to read or write.
    [[nodiscard]] std::unique_ptr<QSettings> openStore(QString const& path)
    {
        if (path.isEmpty())
            return std::make_unique<QSettings>(
                QSettings::IniFormat, QSettings::UserScope, Organisation, Application);

        return std::make_unique<QSettings>(path, QSettings::IniFormat);
    }

    /// @param command One of protocol::EffectEnables.
    /// @return Its key, named by the command's own number.
    ///
    /// The number rather than the name, so renaming an enumerator in this project
    /// does not silently orphan somebody's stored setting.
    [[nodiscard]] QString enabledKey(protocol::FramedCommand command)
    {
        return QStringLiteral("effects/enabled/%1").arg(std::to_underlying(command), 2, 16, QLatin1Char('0'));
    }

    /// @param parameter Which parameter.
    /// @return Its key, named by the parameter's own number.
    ///
    /// One key each rather than a single positional list. A list has to be the
    /// length this build expects or every value lands on the wrong parameter, and
    /// it cannot say "this one was chosen and that one was not" -- which is the
    /// distinction that keeps a stored file from re-imposing a whole effect
    /// configuration nobody asked for.
    [[nodiscard]] QString parameterKey(protocol::Parameter parameter)
    {
        return QStringLiteral("effects/parameter/%1").arg(std::to_underlying(parameter));
    }

    constexpr auto PanelFpsKey = "panel/framesPerSecond";
} // namespace

Settings::Settings(QString filePath): _filePath { std::move(filePath) }
{
}

EffectState Settings::effects() const
{
    auto const store = openStore(_filePath);

    EffectState state;
    for (std::size_t index = 0; index < protocol::EffectEnables.size(); ++index)
        state.enabled[index] = store->value(enabledKey(protocol::EffectEnables[index]), false).toBool();

    // Read one at a time, so a key this build does not know is skipped and a key
    // it knows that nobody has written stays unset. A parameter nobody chose must
    // arrive unset rather than as a default: the bodies the driver edits from are
    // captured vendor payloads, and writing one back puts that effect's whole
    // configuration on the deck.
    for (auto const& info: protocol::Parameters)
    {
        auto const key = parameterKey(info.id);
        if (store->contains(key))
            state.parameters[protocol::indexOf(info.id)] = store->value(key).toInt();
    }

    return state;
}

void Settings::setEffects(EffectState const& state)
{
    auto const store = openStore(_filePath);

    for (std::size_t index = 0; index < protocol::EffectEnables.size(); ++index)
        store->setValue(enabledKey(protocol::EffectEnables[index]), state.enabled[index]);

    for (auto const& info: protocol::Parameters)
    {
        auto const& chosen = state.parameters[protocol::indexOf(info.id)];
        if (chosen)
            store->setValue(parameterKey(info.id), *chosen);
    }
}

int Settings::panelFps() const
{
    auto const store = openStore(_filePath);
    return protocol::clampPanelFps(store->value(PanelFpsKey, protocol::DefaultPanelFps).toInt());
}

void Settings::setPanelFps(int fps)
{
    auto const store = openStore(_filePath);
    store->setValue(PanelFpsKey, protocol::clampPanelFps(fps));
}

} // namespace ax310::app
