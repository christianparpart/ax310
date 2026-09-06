# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

# The presets set PEDANTIC_COMPILER, PEDANTIC_COMPILER_WERROR, ENABLE_TIDY,
# ENABLE_SANITIZER_* and ENABLE_COVERAGE; the modules that declare and honour
# those options live in cmake/ and cmake/portable/ and are include()d below.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/portable")

project(ax310 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if((CMAKE_CXX_COMPILER_ID STREQUAL "Clang") OR (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
    set(CMAKE_CXX_FLAGS_DEBUG "-Og -g")
endif()

if(MSVC)
    # /utf-8 is correctness here, not tidiness. Without it MSVC reads a source
    # file with no BOM as the machine's active code page, so the em dashes, the
    # multiplication sign and the ellipsis in SpecGen.cpp's string literals come
    # out as different bytes -- and those bytes are the wire specification. The
    # generated document would then differ from the committed one and the drift
    # check would fail on Windows, reporting a document that is "out of date"
    # when what is wrong is the compiler's idea of the alphabet.
    add_compile_options(/utf-8)

    # windows.h defines min and max as macros, which breaks any std::max in a
    # header that sees it -- Protocol.hpp has one, in decodeReport. Qt usually
    # defines this itself; relying on that is relying on an include order.
    add_compile_definitions(NOMINMAX)
endif()

option(AX310_BUILD_TESTS "Build unit tests" ON)

include(CTest)

# --- Dependencies ---------------------------------------------------------
#
# Fetched BEFORE the pedantic/sanitizer/tidy modules on purpose. Those modules
# publish their settings with add_compile_options() and CMAKE_CXX_CLANG_TIDY at
# this scope, both of which are captured by a target when it is created -- so
# anything added here is out of reach of -Werror and clang-tidy, which is what
# we want for third-party code we do not maintain.

# Workaround for older dependencies like hidapi, whose cmake_minimum_required()
# predates what CMake 4.x still accepts.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(CPM)

find_package(Qt6 COMPONENTS Core Gui Qml Quick Concurrent REQUIRED)

# Dependencies are pinned to a release tag, never to a moving branch: a build
# that changes because upstream moved is not a build anyone can reason about.
#
# SYSTEM YES puts their headers behind -isystem. Without it a dependency's own
# header is compiled with OUR warning flags, and a warning we cannot fix in code
# we do not maintain becomes a build failure.
CPMAddPackage(
    NAME hidapi
    GITHUB_REPOSITORY libusb/hidapi
    GIT_TAG hidapi-0.15.0
    SYSTEM YES
)

# Catch2 is pinned to a tag rather than requested by VERSION, so every machine
# compiles the same Catch2 even where a system package exists. Fetched here, in
# the dependency block, for the reason above: under -Werror and a clang-tidy that
# treats every check as an error, Catch2's own sources would not survive.
if(AX310_BUILD_TESTS)
    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        GIT_TAG v3.16.0
        SYSTEM YES
    )

    # Where Catch.cmake (which provides catch_discover_tests) lives depends on how
    # Catch2 arrived: a CPM source build keeps it in extras/, a system package next
    # to Catch2Config.cmake. Only the first branch can fire while the fetch above
    # names a tag; the second is what silently breaks if that ever changes.
    if(Catch2_SOURCE_DIR)
        list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
    elseif(Catch2_DIR)
        list(APPEND CMAKE_MODULE_PATH "${Catch2_DIR}")
    endif()
endif()

unset(CMAKE_POLICY_VERSION_MINIMUM)

# --- Build hygiene --------------------------------------------------------
#
# Everything below applies to first-party code only, for the reason above.
include(PedanticCompiler)
include(Sanitizers)
include(Coverage)
include(ClangTidy)
include(RuntimeDlls)

add_subdirectory(src)

# After add_subdirectory(src), because it installs targets defined there.
include(Packaging)

# Must come after every add_subdirectory(): it derives the binary list from the
# build system as it stands rather than from a hand-maintained list.
if(ENABLE_COVERAGE)
    ax310_add_coverage_targets()
endif()
