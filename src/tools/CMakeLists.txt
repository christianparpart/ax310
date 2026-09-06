# SPDX-License-Identifier: Apache-2.0
# Reverse-engineering tools, built against the driver library.
#
# They live here rather than in scripts/ because they encode protocol knowledge
# the driver also depends on -- addresses, command names, framing, checksums --
# and a second copy of that in a script drifts silently. A tool printing a name we
# have since corrected teaches its reader something false.
#
# Being built against the library also means they exercise it, and being under
# src/ means ax310_collect_executables() sees them.
add_library(ax310_tools_capture STATIC
    UsbmonCapture.cpp
    UsbmonCapture.hpp
)
target_link_libraries(ax310_tools_capture PUBLIC ax310)

add_executable(ax310_decode DecodeCapture.cpp)
target_link_libraries(ax310_decode PRIVATE ax310_tools_capture)

add_executable(ax310_probe Probe.cpp)
target_link_libraries(ax310_probe PRIVATE ax310)

# Writes docs/wire-protocol.md out of the tables that implement the protocol.
#
# The document is a projection of Protocol.hpp and Types.hpp, never a description
# of them kept alongside -- that is the whole anti-drift mechanism, and the test
# below is its enforcement. A change to a register, an offset, a parameter range
# or a name makes the committed document stale, and `ctest` says so with the first
# line that differs and the command that fixes it.
add_executable(ax310_spec SpecGen.cpp)
target_link_libraries(ax310_spec PRIVATE ax310)

if(AX310_BUILD_TESTS)
    add_test(NAME "wire-specification is what the headers say"
             COMMAND ax310_spec --check "${PROJECT_SOURCE_DIR}/docs/wire-protocol.md")
endif()
