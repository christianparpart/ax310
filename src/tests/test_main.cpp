// SPDX-License-Identifier: Apache-2.0
// Catch2 entry point for the ax310_test binary.
//
// A custom main rather than linking Catch2WithMain, so there is somewhere to
// register CLI flags when the suite eventually grows options of its own.

#include <catch2/catch_session.hpp>

int main(int argc, char* argv[])
{
    Catch::Session session;

    auto const cliResult = session.applyCommandLine(argc, argv);
    if (cliResult != 0)
        return cliResult;

    return session.run();
}
