// SPDX-License-Identifier: Apache-2.0
/// Headless rendering tests for the interface.
///
/// These render real QML through the real scene graph and look at the pixels
/// that come out. They deliberately assert **invariants** rather than comparing
/// against a stored reference image: font hinting, antialiasing and Qt's own
/// rasteriser all differ between machines and versions, so a reference
/// comparison fails for reasons that have nothing to do with the interface. What
/// is checked instead is what a person would notice -- the panel is the size the
/// deck demands, it is not blank, its background is the colour it should be, and
/// it changes when the device state changes.

#include <ax310/Protocol.hpp>

#include <BridgeHarness.hpp>

#include <QColor>
#include <QEventLoop>
#include <QImage>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <map>
#include <memory>
#include <vector>

using namespace ax310;

namespace
{

/// The deck's panel, which is the one size the hardware accepts.
constexpr int PanelWidth = 800;
constexpr int PanelHeight = 480;

/// Everything a rendered screen needs, with no hardware behind it.
///
/// The connected fake and the settling come from the shared harness; what is
/// added here is the rendering, which only these cases want.
struct RenderHarness: tests::BridgeHarness
{
    /// Renders one QML file at the given size and returns the pixels.
    ///
    /// @param source The QML to load.
    /// @param width Pixel width.
    /// @param height Pixel height.
    /// @return What was drawn.
    [[nodiscard]] QImage render(QString const& source, int width, int height)
    {
        QQuickView view;
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.rootContext()->setContextProperty("ax310Device", &bridge);
        view.resize(width, height);
        view.setSource(QUrl(source));

        // A QML error otherwise shows up as an unexplained "3 == 1"; the messages
        // say which binding failed and on which line, which is the whole
        // difference between a useful failure and a puzzle.
        for (auto const& error: view.errors())
            UNSCOPED_INFO("QML: " << error.toString().toStdString());

        REQUIRE(view.status() == QQuickView::Ready);
        view.show();

        return view.grabWindow();
    }

    /// Renders a QML file whose root is a Window rather than an Item.
    ///
    /// `Main.qml` is rooted in ApplicationWindow, which a QQuickView cannot host
    /// -- a view *is* the window. Splitting the desktop interface so its content
    /// is an Item and the window is a thin shell would let both go through
    /// render() and is the better shape; until then this drives the engine the
    /// way the application does.
    ///
    /// @param source The QML to load.
    /// @param width Pixel width.
    /// @param height Pixel height.
    /// @return What was drawn.
    [[nodiscard]] QImage renderWindow(QString const& source, int width, int height)
    {
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("ax310Device", &bridge);
        engine.load(QUrl(source));

        REQUIRE_FALSE(engine.rootObjects().isEmpty());
        auto* const window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
        REQUIRE(window != nullptr);

        window->resize(width, height);
        window->show();

        return window->grabWindow();
    }

    /// @return The deck's panel, as the deck would receive it.
    [[nodiscard]] QImage renderPanel()
    {
        return render("qrc:/qt/qml/AX310/App/gui/ScreenUI.qml", PanelWidth, PanelHeight);
    }
};

/// @param image The rendering to inspect.
/// @param hue The hue to look for, in degrees.
/// @param tolerance How far off that hue still counts.
/// @return How many reasonably saturated pixels sit near that hue.
[[nodiscard]] std::size_t pixelsNearHue(QImage const& image, int hue, int tolerance = 26)
{
    std::size_t found = 0;
    for (int y = 0; y < image.height(); y += 2)
    {
        for (int x = 0; x < image.width(); x += 2)
        {
            auto const colour = QColor::fromRgb(image.pixel(x, y)).toHsv();
            if (colour.saturation() < 90 || colour.value() < 60)
                continue;
            auto const difference = std::abs(colour.hue() - hue);
            if (std::min(difference, 360 - difference) <= tolerance)
                ++found;
        }
    }

    return found;
}

/// @param image The rendering to inspect.
/// @return How many distinct colours it contains, capped so a busy screen does
///         not cost a full histogram.
[[nodiscard]] std::size_t distinctColours(QImage const& image, std::size_t cap = 64)
{
    std::map<QRgb, int> seen;
    for (int y = 0; y < image.height() && seen.size() < cap; y += 4)
        for (int x = 0; x < image.width() && seen.size() < cap; x += 4)
            ++seen[image.pixel(x, y)];

    return seen.size();
}

} // namespace

TEST_CASE("the panel renders at exactly the size the deck accepts", "[gui][render]")
{
    RenderHarness harness;
    auto const panel = harness.renderPanel();

    // A frame of any other size is accepted by the deck and then shown as
    // nothing, so a wrong size looks exactly like a dead screen. This is the
    // check that would have caught the device-pixel-ratio bug, where a grab at
    // 125% scaling came back 1000x600.
    REQUIRE_FALSE(panel.isNull());
    CHECK(panel.width() == PanelWidth);
    CHECK(panel.height() == PanelHeight);
}

TEST_CASE("the panel draws something rather than a blank frame", "[gui][render]")
{
    RenderHarness harness;
    auto const panel = harness.renderPanel();

    // A QML error leaves a window that renders one flat colour, and the deck
    // would happily display it. More than a handful of colours means the scene
    // graph actually ran.
    CHECK(distinctColours(panel) > 3);
}

TEST_CASE("the panel stays dark enough for a dim room", "[gui][render]")
{
    RenderHarness harness;
    auto const panel = harness.renderPanel();

    // This panel sits on a desk beside a screen somebody is watching, often in
    // the dark, so a bright frame is a real defect rather than a taste question.
    //
    // It is also the check that catches accidental translucency. An earlier
    // version of this test asserted every pixel was fully opaque, which can
    // never fail -- grabWindow() composites onto an opaque window, so the alpha
    // is 255 whatever the scene did. Measured instead: this frame averages well
    // under 80, and giving the root `opacity: 0.5` takes a background pixel from
    // 57 to 149 because it composites against white.
    double total = 0;
    int counted = 0;
    for (int y = 0; y < panel.height(); y += 3)
    {
        for (int x = 0; x < panel.width(); x += 3)
        {
            total += qGray(panel.pixel(x, y));
            ++counted;
        }
    }

    REQUIRE(counted > 0);
    auto const mean = total / counted;
    UNSCOPED_INFO("mean luminance " << mean);
    CHECK(mean < 110.0);
}

TEST_CASE("the interface renders with no device present", "[gui][render]")
{
    RenderHarness harness;
    harness.transport.presentDevice(protocol::descriptorFor(DeviceMode::Control).productId, {});

    // A deck that is unplugged must still produce a frame. Rendering nothing, or
    // crashing, is how a disconnect turns into a black panel nobody can explain.
    auto const panel = harness.renderPanel();
    REQUIRE_FALSE(panel.isNull());
    CHECK(panel.size() == QSize(PanelWidth, PanelHeight));
}

TEST_CASE("the desktop window renders", "[gui][render]")
{
    RenderHarness harness;
    auto const window = harness.renderWindow("qrc:/qt/qml/AX310/App/gui/Main.qml", 900, 650);

    REQUIRE_FALSE(window.isNull());
    CHECK(distinctColours(window) > 3);
}

TEST_CASE("the panel is drawn in the colour of the mix being edited", "[gui][render][mix]")
{
    RenderHarness harness;
    harness.connectWithLevels({ 15, 8, 0, 20, 18, 13 }, { 12, 8, 0, 11, 20, 9 });

    // The deck's own rings glow blue for the creator mix and orange for the
    // audience mix, and the interface says the same thing. This is the design's
    // central claim, so it is the one worth pinning: switching the mix must
    // visibly repaint the screen, not merely change a label.
    auto const hueOf = [](MixId mix) {
        auto const& colour = protocol::MixRingColours[indexOf(mix)];
        return QColor(colour.red, colour.green, colour.blue).hue();
    };
    auto const creatorHue = hueOf(MixId::Creator);
    auto const audienceHue = hueOf(MixId::Audience);

    auto const creator = harness.renderPanel();
    auto const creatorBlue = pixelsNearHue(creator, creatorHue);
    auto const creatorOrange = pixelsNearHue(creator, audienceHue);

    harness.bridge.selectMix(1);
    RenderHarness::settle();

    auto const audience = harness.renderPanel();
    auto const audienceBlue = pixelsNearHue(audience, creatorHue);
    auto const audienceOrange = pixelsNearHue(audience, audienceHue);

    UNSCOPED_INFO("creator: " << creatorBlue << " blue, " << creatorOrange << " orange");
    UNSCOPED_INFO("audience: " << audienceBlue << " blue, " << audienceOrange << " orange");

    // A margin rather than a bare comparison. The other mix is on the screen too,
    // in its own colour and by design, so "more blue than orange" would still
    // hold if its arc grew to rival the one it sits beside. Three to one is what
    // a thin reference ring against a thick one looks like; it does not pin how
    // bright that ring is, which no pixel count measures.
    CHECK(creatorBlue > creatorOrange * 3);
    CHECK(audienceOrange > audienceBlue * 3);
}

TEST_CASE("the mix that is not being edited is drawn in its own colour", "[gui][render][mix]")
{
    RenderHarness harness;

    // Every track carries a level in both mixes, so every gauge has an outer arc
    // to draw. The two sets differ per track, so the arcs cannot coincide.
    harness.connectWithLevels({ 15, 8, 14, 20, 18, 13 }, { 6, 16, 4, 11, 9, 20 });

    auto const hueOf = [](MixId mix) {
        auto const& colour = protocol::MixRingColours[indexOf(mix)];
        return QColor(colour.red, colour.green, colour.blue).hue();
    };

    // The thin outer arc is the other mix, and drawing it in the *edited* mix's
    // colour is what made it useless: both arcs then said the same thing and a
    // person had to switch to read the other mix. So each screen has to carry
    // both hues, not just the one it is in.
    auto const creator = harness.renderPanel();
    auto const creatorOther = pixelsNearHue(creator, hueOf(MixId::Audience));
    UNSCOPED_INFO("the creator panel carries " << creatorOther << " audience-coloured pixels");
    CHECK(creatorOther > 100);

    harness.bridge.selectMix(1);
    RenderHarness::settle();

    auto const audience = harness.renderPanel();
    auto const audienceOther = pixelsNearHue(audience, hueOf(MixId::Creator));
    UNSCOPED_INFO("the audience panel carries " << audienceOther << " creator-coloured pixels");
    CHECK(audienceOther > 100);
}

TEST_CASE("the interface and the deck's rings agree on the mix colours", "[gui][mix]")
{
    // Two palettes for one piece of state is how they came to disagree: a mix
    // switch lit the rings the deck's blue while the panel showed a different
    // one. The driver's table is the source, and this is what keeps the QML from
    // drifting away from it again.
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import AX310.App
        QtObject {
            property color creator: Theme.creator
            property color audience: Theme.audience
        }
    )",
                      QUrl("qrc:/mix-colours.qml"));

    INFO(component.errorString().toStdString());
    REQUIRE(component.isReady());

    std::unique_ptr<QObject> const values { component.create() };
    REQUIRE(values != nullptr);

    auto const wanted = [](MixId mix) {
        auto const& colour = protocol::MixRingColours[indexOf(mix)];
        return QColor(colour.red, colour.green, colour.blue);
    };

    CHECK(values->property("creator").value<QColor>() == wanted(MixId::Creator));
    CHECK(values->property("audience").value<QColor>() == wanted(MixId::Audience));
}

TEST_CASE("a track's level reaches the ring that shows it", "[gui][render][level]")
{
    RenderHarness harness;

    // Every track silent against every track at full: the accent-coloured arcs
    // are what differ, so the second frame must carry visibly more of them.
    harness.connectWithLevels({ 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 });
    auto const silent = pixelsNearHue(harness.renderPanel(), QColor(0x2F, 0xA8, 0xFF).hue());

    for (auto const knob: AllKnobs)
        harness.bridge.setLevel(0, static_cast<int>(indexOf(knob)), 100);
    RenderHarness::settle();

    auto const loud = pixelsNearHue(harness.renderPanel(), QColor(0x2F, 0xA8, 0xFF).hue());

    UNSCOPED_INFO("silent " << silent << " lit pixels, full " << loud);
    CHECK(loud > silent * 2);
}

TEST_CASE("levels appear when the deck connects after the view exists", "[gui][render][level]")
{
    // The application creates its windows and *then* the worker connects, so the
    // interface is alive before there is anything to show. It must pick the
    // levels up when they arrive rather than only reading them once at startup.
    RenderHarness harness;

    for (auto const knob: AllKnobs)
        harness.transport.setRegister(protocol::levelAddressOf(protocol::Property::CreatorMixLevels, knob),
                                      { std::uint8_t { 20 } });

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.rootContext()->setContextProperty("ax310Device", &harness.bridge);
    view.resize(800, 480);
    view.setSource(QUrl("qrc:/qt/qml/AX310/App/gui/ScreenUI.qml"));
    REQUIRE(view.status() == QQuickView::Ready);
    view.show();

    auto const before = pixelsNearHue(view.grabWindow(), QColor(0x2F, 0xA8, 0xFF).hue());

    // Only now does the deck turn up.
    harness.bridge.start();
    RenderHarness::settle();

    auto const after = pixelsNearHue(view.grabWindow(), QColor(0x2F, 0xA8, 0xFF).hue());

    UNSCOPED_INFO("lit pixels before connect " << before << ", after " << after);
    CHECK(after > before * 2);
}

TEST_CASE("levels are shown when the deck connected before the view existed", "[gui][render][level]")
{
    // main.cpp starts the worker and only then loads the QML, so a connect can
    // finish before anything is listening. The interface must ask for the state
    // rather than rely on having heard it change.
    RenderHarness harness;

    for (auto const knob: AllKnobs)
        harness.transport.setRegister(protocol::levelAddressOf(protocol::Property::CreatorMixLevels, knob),
                                      { std::uint8_t { 20 } });

    harness.bridge.start();
    RenderHarness::settle();

    // Every signal has already been emitted by now, and heard by nobody.
    auto const panel = harness.renderPanel();
    auto const lit = pixelsNearHue(panel, QColor(0x2F, 0xA8, 0xFF).hue());

    RenderHarness quiet;
    quiet.bridge.start();
    RenderHarness::settle();
    auto const silent = pixelsNearHue(quiet.renderPanel(), QColor(0x2F, 0xA8, 0xFF).hue());

    UNSCOPED_INFO("full " << lit << " lit pixels, silent " << silent);
    CHECK(lit > silent * 2);
}

namespace
{

/// Where a MessageCollector puts what Qt says. A message handler is a plain
/// function pointer with nowhere to hang state, so the state lives here.
std::vector<QString>* collected = nullptr;

/// Captures everything Qt reports for as long as it is alive.
///
/// Qt writes QML diagnostics to the message handler and then carries on, so a
/// binding loop, a failed assignment or a reference to a property that does not
/// exist all leave the interface running and the tests green. This is what turns
/// them into failures.
class MessageCollector
{
  public:
    MessageCollector(): _previous { qInstallMessageHandler(&MessageCollector::handle) }
    {
        collected = &_messages;
    }

    ~MessageCollector()
    {
        qInstallMessageHandler(_previous);
        collected = nullptr;
    }

    MessageCollector(MessageCollector const&) = delete;
    MessageCollector& operator=(MessageCollector const&) = delete;
    MessageCollector(MessageCollector&&) = delete;
    MessageCollector& operator=(MessageCollector&&) = delete;

    /// @return Everything reported, warnings upward, in order.
    [[nodiscard]] std::vector<QString> const& messages() const noexcept { return _messages; }

  private:
    static void handle(QtMsgType type, QMessageLogContext const& /*context*/, QString const& text)
    {
        if (collected != nullptr && type != QtDebugMsg && type != QtInfoMsg)
            collected->push_back(text);
    }

    QtMessageHandler _previous = nullptr;
    std::vector<QString> _messages;
};

} // namespace

TEST_CASE("rendering either screen reports nothing to Qt's message handler", "[gui][render]")
{
    // A QML defect is not a crash. A binding loop, an assignment Qt cannot make,
    // a property that does not exist -- each prints a line and lets the interface
    // carry on with that one binding quietly dead, which is precisely how the
    // ring gauges came to be sized by evaluation order while every test passed
    // and the application printed the same warning thirty times a second.
    //
    // Both screens are rendered under one collector, because a warning raised by
    // a shared component must fail whichever screen provoked it.
    MessageCollector const collector;

    RenderHarness harness;
    harness.connectWithLevels({ 20, 15, 0, 12, 8, 4 }, { 4, 8, 12, 0, 15, 20 });
    (void) harness.renderPanel();
    harness.bridge.setPanelShowsEffects(true);
    (void) harness.renderPanel();
    (void) harness.renderWindow("qrc:/qt/qml/AX310/App/gui/Main.qml", 1000, 840);

    for (auto const& message: collector.messages())
        UNSCOPED_INFO("Qt said: " << message.toStdString());

    CHECK(collector.messages().empty());
}

TEST_CASE("the panel keeps its arcs after the first frame", "[.native][gui][render]")
{
    // The deck's frames are grabbed from a window the compositor never maps, and
    // every such grab builds a throwaway RHI. A Shape drawn by the shader-based
    // CurveRenderer holds GPU resources across frames, so it draws the first grab
    // and nothing afterwards: the deck received the numerals, the legends and the
    // tiles with every ring arc missing, from the second frame until the
    // application was restarted.
    //
    // Only this run can see that. The rest of the suite pins the software scene
    // graph so it can run without a display, and under QPainter both renderers
    // draw the arcs -- which is exactly how this reached the hardware with the
    // whole suite green.
    RenderHarness harness;
    harness.connectWithLevels({ 20, 20, 20, 20, 20, 20 }, { 20, 20, 20, 20, 20, 20 });

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.rootContext()->setContextProperty("ax310Device", &harness.bridge);
    view.resize(PanelWidth, PanelHeight);
    view.setSource(QUrl("qrc:/qt/qml/AX310/App/gui/ScreenUI.qml"));
    REQUIRE(view.status() == QQuickView::Ready);

    // Created and never shown, which is what the application does.
    view.create();
    RenderHarness::settle();

    auto const hue = QColor(0x2F, 0xA8, 0xFF).hue();
    std::vector<std::size_t> lit;
    std::size_t sampled = 0;
    for (int frame = 0; frame < 4; ++frame)
    {
        auto const grab = view.grabWindow();
        REQUIRE_FALSE(grab.isNull());
        sampled = static_cast<std::size_t>(grab.width() / 2) * static_cast<std::size_t>(grab.height() / 2);
        lit.push_back(pixelsNearHue(grab, hue));
        RenderHarness::settle(60);
    }

    for (std::size_t frame = 0; frame < lit.size(); ++frame)
        UNSCOPED_INFO("frame " << frame << ": " << lit[frame] << " of " << sampled << " sampled");

    // Two checks, because there are two ways to fail. The floor says the arcs
    // were drawn at all; it is a fraction of the frame rather than a count
    // because the grab arrives at the display's device pixel ratio, and six full
    // rings come to around 5% of it. Expressed as a count this test would pass on
    // a 2x display and fail on a 1x one.
    REQUIRE(sampled > 0);
    REQUIRE(lit.front() * 50 > sampled);

    // And the ratio says they were still drawn afterwards, which is the defect
    // this exists for: the first frame was whole and every frame after it kept
    // only the header and the tile glyphs -- an eighth of the colour.
    for (auto const count: lit)
        CHECK(count * 10 > lit.front() * 9);
}

TEST_CASE("the effects page fits the panel and shows every parameter", "[gui][render][effects]")
{
    RenderHarness harness;
    harness.connectWithLevels({ 15, 8, 0, 20, 18, 13 }, { 12, 8, 0, 11, 20, 9 });

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.rootContext()->setContextProperty("ax310Device", &harness.bridge);
    view.resize(PanelWidth, PanelHeight);
    view.setSource(QUrl("qrc:/qt/qml/AX310/App/gui/ScreenUI.qml"));
    REQUIRE(view.status() == QQuickView::Ready);
    view.show();

    auto const mixer = view.grabWindow();
    harness.bridge.setPanelShowsEffects(true);
    RenderHarness::settle(120);
    auto const effects = view.grabWindow();

    // A different page, not the same one with a lit tile.
    REQUIRE(effects.size() == mixer.size());
    CHECK(effects != mixer);
    CHECK(distinctColours(effects) > 3);

    // And its rows fit the band between the header and the tile row.
    //
    // Nothing in QML clips by default, so an effect with one parameter more than
    // fits draws its last row straight over the tiles -- and on a screen nobody
    // looks at directly, a control sliding under another one is invisible until
    // somebody reaches for it. The rows are sized to the space for that reason,
    // and this is the assertion that the sizing works: what the page wants is no
    // more than what it was given.
    auto* const page = view.rootObject()->findChild<QQuickItem*>("effectsPage");
    REQUIRE(page != nullptr);
    UNSCOPED_INFO("the effects page wants " << page->implicitHeight() << " of " << page->height()
                                            << " available");
    CHECK(page->implicitHeight() <= page->height());
    CHECK(page->height() > 0);
}

TEST_CASE("the desktop's preview shows the page the deck is on", "[gui][render][effects]")
{
    // The desktop window renders its own copy of the panel under a heading that
    // says "on the deck". The application renders another one for the deck
    // itself, in a separate view with a separate engine, so the page cannot be
    // kept in the QML: two copies of it would let the preview sit under that
    // heading showing a page the deck is not on. It lives on the bridge, which is
    // the one object both engines share.
    RenderHarness harness;
    harness.connectWithLevels({ 15, 8, 0, 20, 18, 13 }, { 12, 8, 0, 11, 20, 9 });

    auto const tracks = harness.renderWindow("qrc:/qt/qml/AX310/App/gui/Main.qml", 1000, 840);

    harness.bridge.setPanelShowsEffects(true);
    RenderHarness::settle(120);
    auto const effects = harness.renderWindow("qrc:/qt/qml/AX310/App/gui/Main.qml", 1000, 840);

    REQUIRE_FALSE(tracks.isNull());
    REQUIRE(effects.size() == tracks.size());
    CHECK(effects != tracks);
}

TEST_CASE("a touch on the panel lands on the tile it looks like", "[gui][render][touch]")
{
    // The deck reports a touch in its own screen coordinates and the application
    // passes them through verbatim, so where a tile *looks* and where it *is* are
    // the same question -- and it is answerable without the deck.
    //
    // The tile row is the bottom 136 pixels, five tiles of 160, so their centres
    // are at x = 80, 240, 400, 560, 720 and y = 412.
    RenderHarness harness;
    harness.connectWithLevels({ 15, 8, 0, 20, 18, 13 }, { 12, 8, 0, 11, 20, 9 });

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.rootContext()->setContextProperty("ax310Device", &harness.bridge);
    view.resize(PanelWidth, PanelHeight);
    view.setSource(QUrl("qrc:/qt/qml/AX310/App/gui/ScreenUI.qml"));
    REQUIRE(view.status() == QQuickView::Ready);
    view.show();

    constexpr int TileRow = 412;

    REQUIRE(view.rootObject() != nullptr);
    CHECK(view.rootObject()->width() == PanelWidth);
    CHECK(view.rootObject()->height() == PanelHeight);

    /// @param name The objectName the tile carries.
    /// @param x Where the deck would report a touch.
    /// @param y The same.
    /// @return Whether that tile's rectangle contains that point.
    auto const occupies = [&view](char const* name, int x, int y) {
        auto* const tile = view.rootObject()->findChild<QQuickItem*>(name);
        if (tile == nullptr)
        {
            UNSCOPED_INFO(name << " is not in the scene at all");
            return false;
        }

        auto const corner = tile->mapToScene(QPointF(0, 0));
        QRectF const box { corner, QSizeF { tile->width(), tile->height() } };
        UNSCOPED_INFO(name << " occupies " << box.x() << "," << box.y() << " " << box.width() << "x"
                           << box.height() << ", asked about " << x << "," << y);
        return box.contains(QPointF(x, y));
    };

    // The coordinate question, on every platform. This is the half that matters:
    // if a tile moves, the deck's touches land somewhere else and nobody notices
    // until they reach for one.
    CHECK(occupies("switchMixTile", 80, TileRow));
    CHECK(occupies("muteMicTile", 240, TileRow));
    CHECK(occupies("monitorTile", 400, TileRow));
    CHECK(occupies("effectsTile", 560, TileRow));
    CHECK(occupies("dualMixTile", 720, TileRow));

#ifndef _WIN32
    // And that a press there reaches the control, which needs the platform to
    // deliver a synthetic pointer event.
    //
    // Not on Windows. Under the offscreen platform there, Qt Quick accepts the
    // event and delivers it nowhere: the scene is provably right -- the window is
    // visible, the root object is exactly 800x480, and childAt() finds an item at
    // the tile's centre -- and a sent press still changes nothing. Posting does
    // not work either. So the geometry above is what runs everywhere, and this
    // runs where delivery works rather than being deleted or quietly skipped.
    auto const tap = [&view](int x, int y) {
        QPointF const at(x, y);
        QMouseEvent press {
            QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier
        };
        QMouseEvent release {
            QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton, Qt::NoModifier
        };
        QCoreApplication::sendEvent(&view, &press);
        QCoreApplication::sendEvent(&view, &release);
        RenderHarness::settle(120);
    };

    SECTION("the Effects tile opens the effects page, and closes it again")
    {
        REQUIRE_FALSE(harness.bridge.panelShowsEffects());
        tap(560, TileRow);
        CHECK(harness.bridge.panelShowsEffects());
        tap(560, TileRow);
        CHECK_FALSE(harness.bridge.panelShowsEffects());
    }

    SECTION("the Switch mix tile switches the mix")
    {
        REQUIRE(harness.bridge.selectedMix() == 0);
        tap(80, TileRow);
        CHECK(harness.bridge.selectedMix() == 1);
    }

    SECTION("the Mute mic tile mutes the microphone")
    {
        REQUIRE(harness.bridge.levelPercent(0, 0) == 75);
        tap(240, TileRow);
        CHECK(harness.bridge.levelPercent(0, 0) == 0);
    }

    SECTION("a tile marked as not mapped does nothing when touched")
    {
        // Monitor is reserved rather than operable. If `pending` ever stops
        // disabling the tile, this is a control that appears to work and does
        // not -- which is worse than an obviously missing one.
        auto const mix = harness.bridge.selectedMix();
        tap(400, TileRow);
        CHECK_FALSE(harness.bridge.panelShowsEffects());
        CHECK(harness.bridge.selectedMix() == mix);
    }
#endif
}

#ifndef _WIN32
TEST_CASE("the desktop's preview of the deck can be operated", "[gui][render][touch]")
{
    // Not on Windows, and unlike the tile geometry above there is no portable
    // half to keep: the whole claim here is that a scale transform maps *input*
    // as well as pixels, and Qt Quick's offscreen platform on Windows delivers no
    // synthetic pointer events at all. A version of this that ran there would be
    // asserting something it had not tested.
    // The preview is a live ScreenUI under a scale transform, so it is not only a
    // picture: a click lands on whatever the deck's own finger would have hit.
    //
    // That is worth having and worth pinning. It is what lets the panel be worked
    // on -- and its controls exercised -- from a machine with no deck attached,
    // and it is the reason the two copies have to agree about which page they are
    // showing. A transform that stopped mapping input would leave the preview
    // looking right and quietly ignoring every click.
    RenderHarness harness;
    harness.connectWithLevels({ 15, 8, 0, 20, 18, 13 }, { 12, 8, 0, 11, 20, 9 });

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("ax310Device", &harness.bridge);
    engine.load(QUrl("qrc:/qt/qml/AX310/App/gui/Main.qml"));
    REQUIRE_FALSE(engine.rootObjects().isEmpty());

    auto* const window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    REQUIRE(window != nullptr);
    window->resize(1000, 840);
    window->show();
    RenderHarness::settle(200);

    auto* const preview = window->findChild<QQuickItem*>("deckPreview");
    REQUIRE(preview != nullptr);
    auto const factor = preview->property("factor").toReal();
    REQUIRE(factor > 0.0);

    // Panel coordinates, scaled and mapped -- the same tile centres the panel's
    // own touch test uses, so the two cannot drift apart.
    auto const tapPanel = [&](int x, int y) {
        auto const at = preview->mapToScene(QPointF(x * factor, y * factor));
        UNSCOPED_INFO("panel " << x << "," << y << " -> window " << at.x() << "," << at.y());
        QMouseEvent press {
            QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier
        };
        QMouseEvent release {
            QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton, Qt::NoModifier
        };
        QCoreApplication::sendEvent(window, &press);
        QCoreApplication::sendEvent(window, &release);
        RenderHarness::settle(150);
    };

    constexpr int TileRow = 412;

    REQUIRE_FALSE(harness.bridge.panelShowsEffects());
    tapPanel(560, TileRow);
    CHECK(harness.bridge.panelShowsEffects());

    REQUIRE(harness.bridge.selectedMix() == 0);
    tapPanel(80, TileRow);
    CHECK(harness.bridge.selectedMix() == 1);
}
#endif

TEST_CASE("monitoring is the microphone's level in the creator mix", "[gui][monitor]")
{
    // Captured from the vendor software: its monitor widget writes 0x27 -- the
    // creator block's base, which is the Mic track -- and nothing else. There is
    // no separate register, so this is the whole feature.
    RenderHarness harness;
    harness.connectWithLevels({ 15, 8, 0, 20, 18, 13 }, { 12, 8, 0, 11, 20, 9 });

    REQUIRE(harness.bridge.micMonitor());
    REQUIRE(harness.bridge.levelPercent(0, 0) == 75);

    harness.bridge.setMicMonitor(false);
    CHECK_FALSE(harness.bridge.micMonitor());
    CHECK(harness.bridge.levelPercent(0, 0) == 0);

    // The audience still hears you: turning your own monitoring off is not a mute.
    CHECK(harness.bridge.levelPercent(1, 0) == 60);

    // And it comes back where it was, not at full.
    harness.bridge.setMicMonitor(true);
    CHECK(harness.bridge.micMonitor());
    CHECK(harness.bridge.levelPercent(0, 0) == 75);
}

TEST_CASE("the panel's Monitor tile is operable now", "[gui][render][monitor]")
{
    RenderHarness harness;
    harness.connectWithLevels({ 15, 8, 0, 20, 18, 13 }, { 12, 8, 0, 11, 20, 9 });

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.rootContext()->setContextProperty("ax310Device", &harness.bridge);
    view.resize(PanelWidth, PanelHeight);
    view.setSource(QUrl("qrc:/qt/qml/AX310/App/gui/ScreenUI.qml"));
    REQUIRE(view.status() == QQuickView::Ready);
    view.show();

    // It shipped inert, with "not mapped" under it, because the register was
    // thought to be missing. It never was.
    auto* const tile = view.rootObject()->findChild<QQuickItem*>("monitorTile");
    REQUIRE(tile != nullptr);
    CHECK_FALSE(tile->property("pending").toBool());
    CHECK(tile->property("active").toBool());
}
