// SPDX-License-Identifier: Apache-2.0
#include <QBuffer>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickView>
#include <QThreadPool>
#include <QTimer>
#include <cstdio>
#include <memory>

#include "DeviceBridge.hpp"

using namespace Qt::StringLiterals;

namespace
{
/// Frame interval for the screen mirror, ~30 FPS.
constexpr int RenderIntervalMs = 33;

/// JPEG quality for the frames pushed to the deck.
constexpr int FrameQuality = 80;

/// The deck's screen, in pixels.
constexpr int ScreenWidth = 800;
constexpr int ScreenHeight = 480;
} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription("AX310 Driver & Demo");
    parser.addHelpOption();
    QCommandLineOption const showScreenOption(QStringList() << "s" << "show-screen",
                                              "Show the QML window on the desktop for debugging");
    parser.addOption(showScreenOption);
    QCommandLineOption const verboseOption(QStringList() << "v" << "verbose",
                                           "Log every HID report and decoded event");
    parser.addOption(verboseOption);
    QCommandLineOption const brightnessOption(
        QStringList() << "l" << "led-brightness", "Set knob LED ring brightness, 0..100", "percent");
    parser.addOption(brightnessOption);
    parser.process(app);

    bool const showScreen = parser.isSet(showScreenOption);
    auto const logLevel = parser.isSet(verboseOption) ? ax310::LogLevel::Debug : ax310::LogLevel::Info;

    // The driver's enums live in Qt-free headers, so moc never sees them and they
    // must be handed to the meta-object system by hand -- otherwise a queued
    // emission from the worker thread has no metatype to marshal through.
    ax310::app::registerDeviceMetaTypes();

    // The bridge constructs the driver's collaborators and injects them; it is
    // also the only object here that knows the driver exists.
    auto const device = std::make_unique<ax310::app::DeviceBridge>(logLevel);
    device->start();

    // Applied once the worker has had a chance to bring the deck up. There is no
    // "ready" signal to hang this off yet, which is why it waits on a timer --
    // it exists so the brightness hypothesis can be tested on the hardware.
    if (parser.isSet(brightnessOption))
    {
        bool isNumber = false;
        auto const percent = parser.value(brightnessOption).toInt(&isNumber);
        if (!isNumber)
        {
            std::puts("--led-brightness wants a number from 0 to 100");
            return 2;
        }

        QTimer::singleShot(
            2000, device.get(), [bridge = device.get(), percent] { bridge->setKnobLedBrightness(percent); });
    }

    // 1. Desktop UI (Main.qml)
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("ax310Device", device.get());
    QUrl const mainUrl(u"qrc:/qt/qml/AX310/App/gui/Main.qml"_s);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.load(mainUrl);

    // 2. Hardware Screen UI (ScreenUI.qml) via QQuickView for easy grabbing
    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(ScreenWidth, ScreenHeight);
    view.rootContext()->setContextProperty("ax310Device", device.get());
    QUrl const screenUrl(u"qrc:/qt/qml/AX310/App/gui/ScreenUI.qml"_s);
    view.setSource(screenUrl);

    // Shown only when asked for. Otherwise the window is *created* and never
    // mapped: grabWindow() on an unmapped window renders the scene through Qt's
    // software rasteriser into an image, which is the only thing this window is
    // for. Posted mouse events still reach it, so the deck's touch screen keeps
    // working.
    //
    // Hiding it by moving it to -2000,-2000 was the previous attempt. Under
    // Wayland a client does not get to say where its windows go -- the request is
    // ignored -- so the deck's panel turned up on the desktop as a window that
    // could not be clicked and would not go away.
    if (showScreen)
        view.show();
    else
        view.create();

    // Grab the window and push it to the deck's screen.
    QTimer renderTimer;
    QObject::connect(&renderTimer, &QTimer::timeout, [&view, bridge = device.get()]() {
        if (!bridge->isConnected())
            return;

        // The whole window rather than its root item. QQuickItem::grabToImage()
        // needs a window the compositor has actually put on screen, and this one
        // deliberately never is; QQuickWindow::grabWindow() renders the scene
        // itself and hands the pixels straight back.
        //
        // It comes back at whatever size the display's device pixel ratio
        // imposes -- an 800x480 window on a 125% desktop grabs at 1000x600 -- so
        // the resize below is not optional. Asking the old item grab for a target
        // size did not avoid that either: it takes the size in logical pixels and
        // multiplies by the same ratio.
        QImage const frame = view.grabWindow();
        if (frame.isNull())
            return;

        QThreadPool::globalInstance()->start([bridge, frame]() {
            // Resized here rather than at the grab, because the grab's idea of
            // the size is the display's and the deck's is fixed.
            auto scaled = frame;
            if (scaled.width() != ScreenWidth || scaled.height() != ScreenHeight)
            {
                bridge->noteGrabResized(frame.width(), frame.height());
                scaled = frame.scaled(
                    ScreenWidth, ScreenHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
            scaled.setDevicePixelRatio(1.0);

            QByteArray jpegData;
            QBuffer buffer(&jpegData);
            buffer.open(QIODevice::WriteOnly);
            scaled.save(&buffer, "JPG", FrameQuality);
            bridge->sendFrame(scaled.width(), scaled.height(), jpegData);
        });
    });
    renderTimer.start(RenderIntervalMs);

    // Map hardware screen touches to synthetic mouse events on the view.
    QObject::connect(device.get(),
                     &ax310::app::DeviceBridge::screenTouched,
                     [&view](int x, int y, ax310::TouchPhase phase) {
                         QPointF const pos(x, y);

                         // Press, then moves, then release -- the sequence Qt
                         // expects. Posting a fresh press for every report, which
                         // is what this did, made a tap register for a single
                         // frame while a drag held the button down.
                         auto type = QEvent::MouseMove;
                         auto buttons = Qt::MouseButtons { Qt::LeftButton };
                         switch (phase)
                         {
                             case ax310::TouchPhase::Pressed: type = QEvent::MouseButtonPress; break;
                             case ax310::TouchPhase::Moved: type = QEvent::MouseMove; break;
                             case ax310::TouchPhase::Released:
                                 type = QEvent::MouseButtonRelease;
                                 buttons = Qt::NoButton;
                                 break;
                         }

                         // postEvent takes ownership and deletes the event once delivered.
                         auto* mouseEvent =
                             new QMouseEvent(type, pos, pos, Qt::LeftButton, buttons, Qt::NoModifier);
                         QCoreApplication::postEvent(&view, mouseEvent);
                     });

    return QGuiApplication::exec();
}
