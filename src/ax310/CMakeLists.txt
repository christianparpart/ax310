# SPDX-License-Identifier: Apache-2.0
# The AX310 driver library.
#
# Qt-free, and the build is what enforces that rather than a rule in a document:
# this target links hidapi and nothing else, so a Qt include added to any file
# here fails to compile. AUTOMOC is deliberately absent for the same reason.
#
# hidapi is PRIVATE: only HidApiTransport.cpp includes <hidapi.h>, and its header
# forward-declares the handle rather than exposing it, so nothing downstream
# inherits a dependency on the USB stack.
add_library(ax310
    Commands.hpp
    Device.cpp
    Device.hpp
    Event.hpp
    HidApiTransport.cpp
    HidApiTransport.hpp
    IClock.hpp
    IHidTransport.hpp
    ILogger.cpp
    ILogger.hpp
    Protocol.hpp
    Types.hpp
)

target_include_directories(ax310 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(ax310 PRIVATE hidapi::hidapi)
target_compile_features(ax310 PUBLIC cxx_std_23)

# The unit tests. Declared here rather than in a directory of their own so the
# library and what proves it stay described in one place, and under src/ because
# ax310_collect_executables() -- which the coverage target derives its binary
# list from -- only walks that tree.
if(AX310_BUILD_TESTS)
    add_executable(ax310_test
        Device_test.cpp
        Fakes_test.cpp
        ILogger_test.cpp
        Protocol_test.cpp
        Types_test.cpp
        ../tests/test_main.cpp
    )

    target_link_libraries(ax310_test PRIVATE ax310 Catch2::Catch2)
    target_include_directories(ax310_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tests")

    # SKIP_RETURN_CODE 4 is not optional: Catch2's SKIP() exits 4, and without
    # this CTest scores that as a failure. (77 is the shell-script convention and
    # is the wrong number here.)
    include(Catch)
    ax310_place_runtime_dlls(ax310_test)
    catch_discover_tests(ax310_test PROPERTIES SKIP_RETURN_CODE 4)
endif()
