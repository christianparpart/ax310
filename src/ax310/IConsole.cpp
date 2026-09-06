// SPDX-License-Identifier: Apache-2.0
#include "IConsole.hpp"

#include <cstdio>

namespace ax310
{

// fwrite rather than std::print, and deliberately.
//
// std::print would format a string that has already been formatted, which means
// paying for it twice and, worse, treating the caller's text as a format string
// -- a device name containing a brace would then throw. These write bytes.

void SystemConsole::write(std::string_view text)
{
    std::fwrite(text.data(), 1, text.size(), stdout);
}

void SystemConsole::writeError(std::string_view text)
{
    // Unbuffered by convention, so a diagnostic survives a crash that happens
    // before anything flushes.
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

} // namespace ax310
