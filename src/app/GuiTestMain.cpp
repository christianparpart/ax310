// SPDX-License-Identifier: Apache-2.0
/// Entry point for the headless rendering tests.
///
/// Qt Quick needs a QGuiApplication and a scene graph before anything can be
/// rendered, and both have to exist before the first test runs -- so this owns
/// them rather than Catch2's default main.
///
/// The two environment settings are what make this work without a display or a
/// GPU, which is the entire point: `offscreen` gives a windowing system that
/// renders nowhere, and the `software` scene graph rasterises on the CPU. A CI
/// runner has neither a screen nor a driver, and neither does a session where the
/// deck's owner is away from the machine.

#include <catch2/catch_session.hpp>

#include <QGuiApplication>
#include <QQuickWindow>

#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[])
{
    // Pinning the software scene graph is what lets these run with no display and
    // no driver -- and it also hides one whole class of defect, because the deck's
    // frames are not produced that way. They come from a window the compositor
    // never maps, grabbed through the RHI, where a component can render perfectly
    // under QPainter and not at all. The native run exists for those, and needs a
    // real display; it skips itself where there is none, which includes CI.
    if (qEnvironmentVariableIsSet("AX310_TEST_NATIVE_BACKEND"))
    {
        // Gated on a Wayland or X11 session specifically, rather than on "a
        // display" in the abstract. The defect this run exists to catch was found
        // on Wayland, and nobody has established that the same throwaway-RHI
        // behaviour shows up under another windowing system -- so on Windows or
        // macOS this skips rather than pretending to have checked something.
        if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && qEnvironmentVariableIsEmpty("DISPLAY"))
        {
            std::puts("no Wayland or X11 session, so the native-backend rendering tests are skipped");
            return 4;
        }
    }
    else
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("QT_QUICK_BACKEND", "software");

        // Deterministic frames: without this the renderer is free to skip or
        // coalesce updates, and a grab can catch a half-drawn scene.
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }

    QGuiApplication const application { argc, argv };

    Catch::Session session;
    if (auto const failure = session.applyCommandLine(argc, argv); failure != 0)
        return failure;

    return session.run();
}
