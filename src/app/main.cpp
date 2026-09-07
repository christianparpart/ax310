// SPDX-License-Identifier: Apache-2.0
#include "DeviceBridge.hpp"
#include "Settings.hpp"

#include <ax310/IConsole.hpp>

#include <QBuffer>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickView>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <optional>

using namespace Qt::StringLiterals;

namespace
{
/// JPEG quality for the frames pushed to the deck.
constexpr int FrameQuality = 80;

/// The deck's screen, in pixels.
constexpr int ScreenWidth = 800;
constexpr int ScreenHeight = 480;

/// How often the event loop looks to see whether a signal asked it to stop.
constexpr int QuitPollIntervalMs = 100;

/// Whether the scene graph drew through the GPU rather than the software
/// rasteriser. Written once the loop has finished, read by the sanitiser hook
/// below after main has returned.
std::atomic<bool> usedGpuRenderer { false };

/// Reports a command line this program will not run, and shows what a good one
/// looks like.
///
/// Both go to the diagnostic stream: the complaint, so a caller piping ordinary
/// output still sees why nothing happened, and the usage after it, because the
/// list of options is the answer to the question every one of these raises. The
/// list is short enough that the complaint stays on screen above it.
///
/// @param console Where it goes.
/// @param parser The parser, for its usage text.
/// @param problem What is wrong with the command line, as a sentence.
/// @return EXIT_FAILURE, so a caller can `return refuse(...)`.
[[nodiscard]] int refuse(ax310::IConsole& console, QCommandLineParser const& parser, QString const& problem)
{
    writeErrorLine(console, "error: {}", problem.toStdString());
    console.writeError("\n");
    console.writeError(parser.helpText().toStdString());
    return EXIT_FAILURE;
}

/// Set from a signal handler, read by the event loop.
///
/// A handler may set a flag and almost nothing else, so this is the whole of
/// what happens in signal context; the quit that follows is an ordinary one on
/// the main thread.
std::atomic<bool> quitRequested { false };

extern "C" void onTerminationSignal(int /*signalNumber*/)
{
    quitRequested.store(true);
}
} // namespace

/// Answers LeakSanitizer's exit check, which is skipped when the interface drew
/// through the GPU. Inert in a build with no sanitiser: nothing calls it.
///
/// Putting a window on a real display loads the graphics driver, and that stack
/// leaks: 25884 bytes across twelve records at every exit, byte for byte the
/// same on each run and whether or not the deck's panel window is mapped. Three
/// records are inside libdbus; the other nine are in mappings that carry no
/// module name at all, so a suppression file has nothing to match on. Not one of
/// the twelve holds a frame of ours.
///
/// What places them outside this project is the same session run with
/// QT_QUICK_BACKEND=software: the same deck opened, the same frames grabbed, the
/// same shutdown sequence sent, and nothing reported at all.
///
/// So the check is skipped only for the case that was measured. A headless run
/// -- every test binary, and this one under the software rasteriser -- keeps it,
/// which is where this project's own code is exercised. Only the exit check is
/// affected: ASan and UBSan stay on throughout, and this run reports no error of
/// any other class.
///
/// The name is the sanitiser runtime's, not ours, which is what both exemptions
/// below are for.
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming,cert-dcl37-c,cert-dcl51-cpp)
extern "C" int __lsan_is_turned_off()
{
    return usedGpuRenderer.load() ? 1 : 0;
}

int main(int argc, char* argv[])
{
    ax310::SystemConsole console;
    QGuiApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription("AX310 Driver & Demo");

    // Qt's addHelpOption() and process() are both avoided here, because both
    // answer -h by calling ::exit(). That walks past every destructor between
    // there and the top of main -- including ~QGuiApplication, which is what
    // releases the platform theme the desktop environment loaded during
    // construction. Answering `ax310_app --help` that way ends in a LeakSanitizer
    // report of somebody else's memory, and so does every mistyped option.
    //
    // The cost is Qt's --help-all, which lists its own generic options and has no
    // public accessor other than the one that exits. Those options keep working;
    // QGuiApplication has already taken them out of the arguments by this point.
    QCommandLineOption const helpOption(QStringList() << "h" << "help",
                                        "Displays help on commandline options.");
    parser.addOption(helpOption);
    QCommandLineOption const showScreenOption(QStringList() << "s" << "show-screen",
                                              "Show the QML window on the desktop for debugging");
    parser.addOption(showScreenOption);
    QCommandLineOption const minimisedOption(
        QStringList() << "m" << "start-minimised",
        "Start with the window minimised to the taskbar rather than on screen");
    parser.addOption(minimisedOption);
    QCommandLineOption const verboseOption(QStringList() << "v" << "verbose",
                                           "Log every HID report and decoded event");
    parser.addOption(verboseOption);
    QCommandLineOption const brightnessOption(
        QStringList() << "l" << "led-brightness", "Set knob LED ring brightness, 0..100", "percent");
    parser.addOption(brightnessOption);

    if (!parser.parse(QCoreApplication::arguments()))
        return refuse(console, parser, parser.errorText());

    if (parser.isSet(helpOption))
    {
        // helpText() ends in a newline of its own.
        ax310::write(console, "{}", parser.helpText().toStdString());
        return EXIT_SUCCESS;
    }

    // This program takes options and nothing else, so anything left over is a
    // mistake -- most often a misspelt option that lost its dashes. Qt's parser
    // collects those as positional arguments and says nothing about them, which
    // means a typo would otherwise open the deck and start a session.
    if (auto const leftovers = parser.positionalArguments(); !leftovers.isEmpty())
        return refuse(console,
                      parser,
                      leftovers.size() == 1
                          ? u"Unexpected argument '%1'."_s.arg(leftovers.front())
                          : u"Unexpected arguments: %1."_s.arg(leftovers.join(u", ")));

    // Every value is validated here, above the bridge, because opening the deck
    // is not a step to take on the way to rejecting a command line. start() hands
    // the connect to a worker thread, so a check placed below it races that
    // thread, and which one wins decides whether a mistyped argument reaches the
    // hardware at all.
    //
    // The range is checked here too, and not only whether the text is a number.
    // The driver clamps, so without this the message below would promise a limit
    // that nothing enforced: --led-brightness 500 would be accepted and quietly
    // become 100.
    std::optional<int> ledBrightness;
    if (parser.isSet(brightnessOption))
    {
        bool isNumber = false;
        auto const percent = parser.value(brightnessOption).toInt(&isNumber);
        if (!isNumber || percent < 0 || percent > 100)
            return refuse(console,
                          parser,
                          u"--led-brightness wants a number from 0 to 100, not '%1'."_s.arg(
                              parser.value(brightnessOption)));
        ledBrightness = percent;
    }

    bool const showScreen = parser.isSet(showScreenOption);
    bool const startMinimised = parser.isSet(minimisedOption);
    auto const logLevel = parser.isSet(verboseOption) ? ax310::LogLevel::Debug : ax310::LogLevel::Info;

    // The driver's enums live in Qt-free headers, so moc never sees them and they
    // must be handed to the meta-object system by hand -- otherwise a queued
    // emission from the worker thread has no metatype to marshal through.
    ax310::app::registerDeviceMetaTypes();

    // The bridge constructs the driver's collaborators and injects them; it is
    // also the only object here that knows the driver exists.
    auto const device = std::make_unique<ax310::app::DeviceBridge>(logLevel);

    // What the deck comes up as is a decision, not an inheritance. The DSP has no
    // read-back, so the snapshot that protects the property registers cannot
    // protect the effects chain -- remembering it here is the only way to put it
    // back, and until something is remembered the default is every effect off.
    ax310::app::Settings settings;
    device->setEffectState(settings.effects());
    device->start();

    // Applied once the worker has had a chance to bring the deck up. There is no
    // "ready" signal to hang this off yet, which is why it waits on a timer --
    // it exists so the brightness hypothesis can be tested on the hardware.
    if (ledBrightness)
        QTimer::singleShot(2000, device.get(), [bridge = device.get(), percent = *ledBrightness] {
            bridge->setKnobLedBrightness(percent);
        });

    // Ctrl+C is how a program run from a terminal is stopped, and letting the
    // process simply die leaves the deck initialised: disconnect() is what sends
    // the shutdown sequence and puts back the settings the handshake overwrote.
    // Terminating that way also skips every destructor, which is a leak report
    // rather than a clean exit under a sanitised build.
    std::signal(SIGINT, onTerminationSignal);
    std::signal(SIGTERM, onTerminationSignal);

    QTimer quitPoll;
    QObject::connect(&quitPoll, &QTimer::timeout, [] {
        if (quitRequested.load())
            QGuiApplication::quit();
    });
    quitPoll.start(QuitPollIntervalMs);

    // 1. Desktop UI (Main.qml)
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("ax310Device", device.get());

    // Created hidden, then asked for minimised below -- rather than letting the
    // QML show it and minimising afterwards, which puts the window on screen
    // before taking it away again. It still gets a taskbar entry, so it is
    // restored the way any other minimised window is.
    if (startMinimised)
        engine.setInitialProperties({ { u"visible"_s, false } });

    QUrl const mainUrl(u"qrc:/qt/qml/AX310/App/gui/Main.qml"_s);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.load(mainUrl);

    auto* const mainWindow = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0));
    if (startMinimised && mainWindow != nullptr)
        mainWindow->showMinimized();

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

    // Which backend the scene graph settled on, taken from the scene graph itself
    // rather than from QQuickWindow::graphicsApi() -- that one answers with the
    // API that was *asked* for, and reports the GPU even under the software
    // rasteriser. Connected before the loop starts, because this is emitted the
    // first time either window renders, and read after the loop has finished.
    auto const noteRenderer = [](QQuickWindow* window) {
        if (window == nullptr)
            return;

        // Direct, because it arrives on the render thread and the answer belongs
        // to that thread.
        QObject::connect(
            window,
            &QQuickWindow::sceneGraphInitialized,
            window,
            [window] {
                auto const* const renderer = window->rendererInterface();
                if (renderer != nullptr && renderer->graphicsApi() != QSGRendererInterface::Software)
                    usedGpuRenderer.store(true);
            },
            Qt::DirectConnection);
    };
    noteRenderer(mainWindow);
    noteRenderer(&view);

    // Grab the window and push it to the deck's screen.
    //
    // One frame in flight at a time. Encoding and sending take longer than the
    // interval whenever the machine is busy, and starting a second frame anyway
    // put two of them on the bus at once: the deck reassembles chunks into one
    // image, so it received a frame made of two, and a switch that should have
    // repainted the panel left it showing the colour it had. Dropping the grab
    // is the right answer for a live view -- the frame it would have carried is
    // already out of date by the time the previous one lands.
    auto const busy = std::make_shared<std::atomic<bool>>(false);

    QTimer renderTimer;
    QObject::connect(&renderTimer, &QTimer::timeout, [&view, busy, bridge = device.get()]() {
        if (!bridge->isConnected())
            return;

        if (busy->exchange(true))
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
        {
            busy->store(false);
            return;
        }

        QThreadPool::globalInstance()->start([bridge, frame, busy]() {
            // Resized here rather than at the grab, because the grab's idea of
            // the size is the display's and the deck's is fixed.
            auto scaled = frame;
            if (scaled.width() != ScreenWidth || scaled.height() != ScreenHeight)
            {
                bridge->noteGrabResized(frame.width(), frame.height());
                scaled =
                    frame.scaled(ScreenWidth, ScreenHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
            scaled.setDevicePixelRatio(1.0);

            QByteArray jpegData;
            QBuffer buffer(&jpegData);
            buffer.open(QIODevice::WriteOnly);
            scaled.save(&buffer, "JPG", FrameQuality);
            bridge->sendFrame(scaled.width(), scaled.height(), jpegData);
            busy->store(false);
        });
    });
    // Capped in the driver rather than here, so no stored value and no hand-edited
    // file can ask for a rate that costs a core and a share of the USB bus.
    renderTimer.start(1000 / settings.panelFps());

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

    auto const exitCode = QGuiApplication::exec();

    // Stored on the way out, so the next run starts where this one ended rather
    // than with everything off again.
    settings.setEffects(device->effectState());

    // Said once at the end, because which backend drew the deck's frames is not
    // decided here: the environment chooses it, and a component can render
    // perfectly under the software rasteriser and not at all through the RHI.
    device->log(u"debug"_s,
                usedGpuRenderer.load() ? u"the scene graph drew through the GPU"_s
                                       : u"the scene graph drew through the software rasteriser"_s);

    return exitCode;
}
