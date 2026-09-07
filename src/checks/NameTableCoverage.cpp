// SPDX-License-Identifier: Apache-2.0
/// Checks that every enumerator has a row in its name table.
///
/// This is the one guarantee the enumerated tables cannot give themselves. Making
/// `WireName::value` an enumerator means a row can never name an address the
/// enumeration does not define -- but nothing stops the reverse, an enumerator
/// added with no row, which then decodes as an unnamed number for ever.
///
/// Catching that needs the list of enumerators, and there is no way to obtain one
/// in C++23. Writing it out by hand -- an `AllProperties` beside every enum --
/// would put the omission one step further away rather than removing it: the
/// check would pass vacuously for exactly the enumerator somebody forgot to add
/// to both places.
///
/// P2996 reflection answers it properly, and is C++26. As of writing, GCC 16
/// implements it behind `-freflection`; Clang 22 and MSVC do not implement it at
/// all. So this is **not** part of the library. It is a standalone translation
/// unit, built and run only where the compiler can manage it, that includes the
/// real headers and asserts against the real tables. One compiler is enough: a
/// missing row fails that build, and the answer does not vary by compiler.
///
/// The check was confirmed to fail by deleting a row, rather than trusted because
/// it compiled.

#include <ax310/Protocol.hpp>

#include <array>
#include <cstddef>
#include <meta>
#include <utility>

using namespace ax310;
using namespace ax310::protocol;

namespace
{

/// @param table A name table.
/// @return Whether every enumerator of its enumeration has a row.
template <typename Enum, std::size_t N>
consteval bool everyEnumeratorIsNamed(std::array<WireName<Enum>, N> const& table)
{
    for (auto const enumerator: std::meta::enumerators_of(^^Enum))
    {
        // extract<> rather than a [:splice:], because the loop variable is not
        // itself constexpr and a splice requires one.
        auto const value = std::to_underlying(std::meta::extract<Enum>(enumerator));
        if (nameIn(table, value).empty())
            return false;
    }

    return true;
}

static_assert(everyEnumeratorIsNamed(PropertyNames),
              "a Property enumerator has no row in PropertyNames");
static_assert(everyEnumeratorIsNamed(FramedCommandNames),
              "a FramedCommand enumerator has no row in FramedCommandNames");

} // namespace

int main()
{
    return 0;
}
