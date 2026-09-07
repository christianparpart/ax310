// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <ax310/Device.hpp>

#include <QString>

namespace ax310::app
{

/// What the interface remembers between runs.
///
/// Kept in the Qt layer rather than the driver, for the reason everything else
/// Qt is: the driver stays free of it. QSettings also means there is no file
/// format to parse and no dependency to add. The driver is handed values and is
/// never asked where they came from.
///
/// The first thing stored here is the effects chain, and it is not a preference
/// in the ordinary sense. The DSP has no read-back, so it cannot be snapshotted
/// and restored across a connect the way the property registers are: without a
/// memory of its own the deck comes up carrying whatever the last program to
/// touch it configured. Remembering it here is the only way to put it back.
class Settings
{
  public:
    /// @param filePath An explicit INI file, or empty for the user's own.
    ///
    /// The parameter exists so a test can point at a temporary file instead of
    /// writing into whoever is running it -- a test that edits real settings is
    /// one that has to be run carefully, which means it stops being run.
    explicit Settings(QString filePath = {});

    /// @return The effects chain as it was left: every effect off, and every
    ///         parameter unset, when nothing has been stored yet.
    [[nodiscard]] EffectState effects() const;

    /// @param state What to remember.
    void setEffects(EffectState const& state);

    /// @return How often the deck's panel should be redrawn, already brought
    ///         inside protocol::MinPanelFps and MaxPanelFps.
    ///
    /// Clamped on the way out as well as on the way in, so a hand-edited file
    /// cannot ask for a rate this driver will not send.
    [[nodiscard]] int panelFps() const;

    /// @param fps How often the panel should be redrawn; clamped before storing.
    void setPanelFps(int fps);

  private:
    QString _filePath;
};

} // namespace ax310::app
