// SPDX-License-Identifier: Apache-2.0
#include "ILogger.hpp"

#include <cstdio>
#include <print>

namespace ax310
{

void StderrLogger::log(LogLevel level, std::string_view message)
{
    if (!isLoggable(level, _threshold))
        return;

    std::println(stderr, "[{}] {}", nameOf(level), message);
}

} // namespace ax310
