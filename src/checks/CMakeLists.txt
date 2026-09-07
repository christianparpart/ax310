# The name-table coverage check needs P2996 reflection, which is C++26 and which
# only some compilers implement. Probed rather than assumed, so this contributes
# a check where it can and nothing at all where it cannot -- the other presets
# must not start failing because a toolchain lacks a language feature.
include(CheckCXXSourceCompiles)

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
add_test(NAME "every enumerator has a name" COMMAND ax310_name_coverage)
