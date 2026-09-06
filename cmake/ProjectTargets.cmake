# SPDX-License-Identifier: Apache-2.0
# ProjectTargets.cmake - what this project actually built, derived from the build
# system rather than listed by hand.
#
# One caller today: the `coverage` target hands llvm-cov every binary the suite
# runs. It fails SILENTLY when the list is wrong -- a report of one fewer file
# still renders, and nothing says which file is missing -- so the list may not be
# maintained by hand. The walk lives here rather than inside Coverage.cmake
# because a second caller wanting the same list must share this one rather than
# keep a private copy that drifts.

# Collect the executable targets defined under src/.
#
# A new app under src/apps/ is a new row in that directory's app table and nothing
# else, so nothing here needs editing when one appears.
#
# The subdirectory walk is filtered to src/ because CPM adds each dependency's
# source tree as a subdirectory too, and those carry executables (Catch2's own
# self-tests, for one) that neither caller wants. Compared with string(FIND)
# rather than a regex: a checkout path is arbitrary text, and this repository
# routinely has worktrees with a `+` in the name.
#
# Reads the build system as it stands, so every caller must run AFTER the
# add_subdirectory() calls that define the targets.
function(ax310_collect_executables DIR OUT_VAR)
    set(found "")

    get_property(targets DIRECTORY "${DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type STREQUAL "EXECUTABLE")
            list(APPEND found ${target})
        endif()
    endforeach()

    get_property(subdirectories DIRECTORY "${DIR}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        string(FIND "${subdirectory}" "${CMAKE_SOURCE_DIR}/src" position)
        if(position EQUAL 0)
            ax310_collect_executables("${subdirectory}" nested)
            list(APPEND found ${nested})
        endif()
    endforeach()

    set(${OUT_VAR} "${found}" PARENT_SCOPE)
endfunction()
