# The name-table coverage check needs P2996 reflection, which is C++26 and which
# only some compilers implement. Probed rather than assumed, so this contributes
# a check where it can and nothing at all where it cannot -- the other presets
# must not start failing because a toolchain lacks a language feature.
include(CheckCXXSourceCompiles)

# Probing requires being able to *ask* for C++26 in the first place. CMake has
# no -std flag for C++26 on MSVC, so requesting the dialect there is not a failed
# probe but a hard "requires the language dialect CXX26" generation error out of
# try_compile -- which takes the whole configure with it, exactly the outcome the
# note above says must not happen. CMAKE_CXX_STANDARD_LATEST reports 23 for MSVC
# and 26 for GCC and Clang, so it separates "cannot be asked" from "asked and
# said no" before try_compile is ever reached.
if(CMAKE_CXX_STANDARD_LATEST LESS 26)
    message(STATUS "C++26 cannot be requested from ${CMAKE_CXX_COMPILER_ID}; "
                   "the name-table coverage check is skipped")
    return()
endif()

# CMAKE_CXX_STANDARD has to move for the probe, not just CMAKE_REQUIRED_FLAGS:
# CMake appends its own -std= after the required flags, so a -std=c++26 passed
# that way is overridden and the probe reports a false negative.
set(_ax310_saved_standard "${CMAKE_CXX_STANDARD}")
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_REQUIRED_FLAGS "-freflection")
check_cxx_source_compiles([[
#include <meta>
enum class E : unsigned char { A = 1 };
consteval bool ok() { return std::meta::enumerators_of(^^E).size() == 1; }
static_assert(ok());
int main() { return 0; }
]] AX310_HAS_REFLECTION)
unset(CMAKE_REQUIRED_FLAGS)
set(CMAKE_CXX_STANDARD "${_ax310_saved_standard}")

if(NOT AX310_HAS_REFLECTION)
    message(STATUS "P2996 reflection unavailable; the name-table coverage check is skipped")
    return()
endif()

add_executable(ax310_name_coverage NameTableCoverage.cpp)
target_include_directories(ax310_name_coverage PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(ax310_name_coverage PRIVATE -freflection)
set_target_properties(ax310_name_coverage PROPERTIES CXX_STANDARD 26)

# The assertions are static, so building the target *is* the check. Running it
# only proves the build happened, which is what makes it a useful CTest entry.
add_test(NAME "the enumerator trick agrees with P2996" COMMAND ax310_name_coverage)
