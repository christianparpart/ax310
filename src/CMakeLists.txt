# SPDX-License-Identifier: Apache-2.0
# The driver library carries its own build description; this file only knows the
# application, which is the layer allowed to depend on Qt.
add_subdirectory(ax310)
add_subdirectory(tools)

# The Qt layer lives in a library rather than straight in the executable, so the
# interface can be rendered by something other than the application -- which is
# what makes headless GUI tests possible at all. ax310_app becomes a main() that
# links it.
add_library(ax310_gui STATIC
    app/DeviceBridge.cpp
    app/DeviceBridge.hpp
)

target_link_libraries(ax310_gui PUBLIC
    ax310
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Concurrent
)

set_target_properties(ax310_gui PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
    POSITION_INDEPENDENT_CODE ON
)

# Theme.qml is a singleton; without this the QML engine will not treat it as one.
set_source_files_properties(gui/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(ax310_gui
    URI "AX310.App"
    VERSION 1.0
    # The engine searches :/qt/qml for compiled-in modules. Left at the default
    # the resources land at :/AX310/App and whether the module resolves comes down
    # to the Qt version -- it did locally on 6.11 and did not in CI on 6.10, with
    # "module AX310.App is not installed" at every import.
    RESOURCE_PREFIX "/qt/qml"
    # No plugin, because there is nothing for one to do: every type in this module
    # is a QML file, and the only C++ object the interface uses is a context
    # property. A plugin would exist solely to be loaded, and loading it out of a
    # static library is what broke in CI.
    NO_PLUGIN
    QML_FILES
        gui/Theme.qml
        gui/RingGauge.qml
        gui/ParamSlider.qml
        gui/ActionTile.qml
        gui/MixerView.qml
        gui/EffectsView.qml
        gui/Main.qml
        gui/ScreenUI.qml
    # Bundled because the panel is rendered to a JPEG and pushed to hardware: its
    # appearance cannot depend on what a host machine happens to have installed.
    # The Nerd Font is subset from 2.2 MB a weight to under 80 KB -- it keeps the
    # Latin set and the six icons the interface draws, and drops several thousand
    # glyphs for tools this project will never mention.
    RESOURCES
        gui/fonts/BarlowCondensed-SemiBold.ttf
        gui/fonts/BarlowCondensed-Bold.ttf
        gui/fonts/JetBrainsMonoNF-Regular.ttf
        gui/fonts/JetBrainsMonoNF-Medium.ttf
        gui/fonts/JetBrainsMonoNF-Bold.ttf
)

# Regenerates the screenshots in README.md. Renders the same QML the application
# does, with no deck and no display, so the documentation can be brought back into
# agreement with the interface by anyone with a checkout.
add_executable(ax310_screenshot app/Screenshot.cpp)
target_link_libraries(ax310_screenshot PRIVATE ax310_gui)
ax310_place_runtime_dlls(ax310_screenshot)
set_target_properties(ax310_screenshot PROPERTIES AUTOMOC ON)

add_executable(ax310_app app/main.cpp)
target_link_libraries(ax310_app PRIVATE ax310_gui)
ax310_place_runtime_dlls(ax310_app)
set_target_properties(ax310_app PROPERTIES AUTOMOC ON)


# Qt generates the QML cache loader, and at -O3 GCC reports a -Wnull-dereference
# false positive inside QHash from it. It is generated code we neither wrote nor
# can fix, so it alone is exempt from -Werror; every first-party source is not.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(
        "${CMAKE_CURRENT_BINARY_DIR}/.rcc/qmlcache/ax310_gui_qmlcache_loader.cpp"
        TARGET_DIRECTORY ax310_gui
        PROPERTIES COMPILE_OPTIONS "-Wno-error=null-dereference"
    )
endif()

# Headless rendering tests. The interface is driven by a scripted FakeHidTransport
# rather than a deck, so the whole path from wire bytes to pixels runs with no
# hardware attached -- which is the only way the rendering gets checked when the
# machine with the deck on it is not available.
if(AX310_BUILD_TESTS)
    add_executable(ax310_gui_test
        app/GuiRender_test.cpp
        app/GuiTestMain.cpp
    )

    target_link_libraries(ax310_gui_test PRIVATE ax310_gui Catch2::Catch2)
    target_include_directories(ax310_gui_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    set_target_properties(ax310_gui_test PROPERTIES AUTOMOC ON)

    # Built under every preset, but registered with CTest under all but TSan.
    #
    # ThreadSanitizer reports a data race between the main thread and Qt's own
    # QQmlThread on every one of these -- a Qt-internal allocation freed on
    # another Qt thread, with no frame of ours anywhere in the trace. TSan cannot
    # see the synchronisation inside a Qt that was not itself built instrumented,
    # so these are artefacts of the tooling rather than findings, and scoring them
    # as failures would teach everyone to ignore a red TSan run.
    #
    # Our own threading is still covered there: the driver's tests exercise the
    # write mutex and the poll loop and they do run under TSan. And the GUI tests
    # still run under ASan and UBSan, which do not have this blind spot.
    if(ENABLE_SANITIZER_THREAD)
        message(STATUS "ax310_gui_test builds but is not run under TSan; Qt is not instrumented")
    else()
        include(Catch)
        # DL_PATHS is not decoration on Windows. catch_discover_tests runs the
        # binary with --list-tests at BUILD time to enumerate the cases, and this
        # one links Qt -- so without the directory holding Qt6Core.dll on the
        # search path the build fails at 0xc0000135, STATUS_DLL_NOT_FOUND, with
        # no output and nothing naming Qt. It is ignored where it is not needed.
        ax310_place_runtime_dlls(ax310_gui_test)
        catch_discover_tests(ax310_gui_test
            PROPERTIES SKIP_RETURN_CODE 4
            DL_PATHS "$<TARGET_FILE_DIR:Qt6::Core>")

        # A second run of the same binary, against the real scene graph.
        #
        # Everything above pins the software renderer, which is what lets it run
        # on a machine with no screen and no driver -- and it also means those
        # tests cannot see how the deck's frames are actually made, which is an
        # unmapped window grabbed through the RHI. A component can draw perfectly
        # under QPainter and not at all there; one did, and the panel went to the
        # hardware with every ring missing while the whole suite was green.
        #
        # It skips itself where there is no display, which includes CI. Leak
        # detection is off for it alone: it brings up the GL driver, Wayland and
        # DBus, and LeakSanitizer reports 25 kB across those with no frame of ours
        # in any trace. The other sanitiser checks still apply.
        add_test(NAME "the deck's frames keep their arcs (native backend)"
                 COMMAND ax310_gui_test "[native]")
        set_tests_properties("the deck's frames keep their arcs (native backend)" PROPERTIES
            ENVIRONMENT "AX310_TEST_NATIVE_BACKEND=1;ASAN_OPTIONS=detect_leaks=0"
            SKIP_RETURN_CODE 4)
    endif()

endif()
