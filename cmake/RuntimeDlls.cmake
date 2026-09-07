# SPDX-License-Identifier: Apache-2.0
# Putting an executable's DLLs where Windows will find them.
#
# Windows has no rpath. A binary finds a DLL on PATH or beside itself, and
# nowhere else -- so on Windows an executable built against Qt and hidapi cannot
# be run from the build tree at all until its dependencies are next to it. That
# is not only a convenience: catch_discover_tests runs the test binary during the
# BUILD to enumerate its cases, and a binary that cannot start fails the build
# with 0xC0000135 and no output naming what was missing.
#
# $<TARGET_RUNTIME_DLLS:target> is CMake's answer, and it is exact: it expands to
# the DLLs of everything the target links that CMake knows about, imported Qt
# targets and locally built ones alike. It found hidapi.dll, which builds shared
# on Windows and static on Linux -- a difference nothing in this project chose or
# had reason to notice until it was built there.
#
# Call this immediately after add_executable() and BEFORE catch_discover_tests(),
# because both attach POST_BUILD commands and they run in the order they were
# added. Copy first, then discover.
function(ax310_place_runtime_dlls target)
    if(NOT WIN32)
        return()
    endif()

    # Each executable gets a directory of its own first, and that is not tidiness.
    # Three executables live in src/ and each copies the same Qt DLLs beside
    # itself; Ninja builds them in parallel and `cmake -E copy` takes no lock, so
    # two of them writing Qt6Core.dll to the same path at the same moment is a
    # sharing violation and the build fails. It is a race, so it passed for
    # several runs before it did not -- which is the worst way for one to behave.
    #
    # Separate destinations make it impossible rather than unlikely. Everything
    # that refers to these binaries goes through $<TARGET_FILE:...> or the target
    # name, so nothing else has to know where they moved.
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${target}")

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_RUNTIME_DLLS:${target}>" "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS
        VERBATIM)

    ax310_place_qt_conf(${target})
endfunction()

# Telling a cross-built binary where Qt's plugins and QML modules are.
#
# TARGET_RUNTIME_DLLS above copies Qt6Core.dll and friends beside the executable,
# but Qt's platform plugins and QML modules are loaded by path at runtime rather
# than linked, so CMake does not know about them and they stay behind. On Windows
# that is covered by QT_PLUGIN_PATH and QML2_IMPORT_PATH, which install-qt-action
# exports in CI.
#
# Cross-compiled, nothing exports them, and the failure is genuinely hard to
# read: Qt reports "no Qt platform plugin could be initialized" through a message
# box rather than stderr and then calls qFatal, which is abort(), which the MSVC
# runtime raises as __fastfail. So a headless run shows no output whatsoever and
# exit code 0xC0000409 -- STATUS_STACK_BUFFER_OVERRUN -- which reads like memory
# corruption and is nothing of the sort.
#
# A qt.conf beside the executable names the prefix outright and settles plugins
# and QML imports together. Wine maps the Unix root at Z:, which is where the
# drive letter comes from; this only applies when cross-compiling, since a native
# Windows build resolves Qt the way it always has.
function(ax310_place_qt_conf target)
    if(NOT CMAKE_CROSSCOMPILING OR NOT TARGET Qt6::Core)
        return()
    endif()

    file(GENERATE
        OUTPUT "$<TARGET_FILE_DIR:${target}>/qt.conf"
        CONTENT "[Paths]\nPrefix=Z:${QT6_INSTALL_PREFIX}\n")
endfunction()
