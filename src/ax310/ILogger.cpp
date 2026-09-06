// SPDX-License-Identifier: Apache-2.0
#include "ILogger.hpp"

#include "IConsole.hpp"

namespace ax310
{

void ConsoleLogger::log(LogLevel level, std::string_view message)
{
    if (!isLoggable(level, _threshold))
        return;

    // Diagnostics, not output: a log line must not land in the middle of what a
    // tool was asked to print.
    writeErrorLine(_console, "[{}] {}", nameOf(level), message);
}

} // namespace ax310
