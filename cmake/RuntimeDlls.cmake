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

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy -t "$<TARGET_FILE_DIR:${target}>"
                "$<TARGET_RUNTIME_DLLS:${target}>"
        COMMAND_EXPAND_LISTS
        VERBATIM)
endfunction()
