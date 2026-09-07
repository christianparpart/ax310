// SPDX-License-Identifier: Apache-2.0
/// Confirms the hand-rolled enumerator listing against the standard one.
///
/// Protocol.hpp already asserts that every enumerator has a name row, on every
/// compiler, using ax310::enumerators -- which works by reading the compiler's
/// own diagnostic strings. That trick is guarded by a self-test, but a self-test
/// written by the same hand that wrote the trick shares its blind spots, and the
/// way it fails is quiet: if a compiler ever renders a template argument
/// differently, every value reads as invalid, the list comes back empty, and the
/// coverage assertions pass having compared nothing at all.
///
/// So this asks a completely different implementation the same question. P2996
/// reflection is C++26 and, of the compilers this project builds with, GCC 16 is
/// the only one that has it. That is enough for a cross-check: if the two ever
/// disagree about what an enumeration contains, one build fails.
///
/// Delete this once P2996 is available everywhere -- at that point the trick in
/// Enumerators.hpp goes too, and this becomes the implementation rather than
/// the second opinion.

#include <ax310/Enumerators.hpp>
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
/// @return Whether every enumerator of its enumeration has a row, asked via P2996.
template <typename Enum, std::size_t N>
consteval bool everyEnumeratorIsNamedByReflection(std::array<WireName<Enum>, N> const& table)
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

/// @return How many enumerators P2996 says @p Enum has.
template <typename Enum>
consteval std::size_t reflectedCount()
{
    return std::meta::enumerators_of(^^Enum).size();
}

static_assert(everyEnumeratorIsNamedByReflection(PropertyNames),
              "a Property enumerator has no row in PropertyNames");
static_assert(everyEnumeratorIsNamedByReflection(FramedCommandNames),
              "a FramedCommand enumerator has no row in FramedCommandNames");

// And the part Protocol.hpp cannot check about itself: that the trick finds the
// same enumerators the language does. An alias -- a Last repeating an earlier
// enumerator -- is one value but two declarations, so the counts are compared
// on the enumerations that have no alias.
static_assert(enumerators::enumeratorsOf<Property>().size() == reflectedCount<Property>(),
              "the enumerator-listing trick disagrees with P2996 about Property");
static_assert(enumerators::enumeratorsOf<FramedCommand>().size()
                  == reflectedCount<FramedCommand>(),
              "the enumerator-listing trick disagrees with P2996 about FramedCommand");

} // namespace

int main()
{
    return 0;
}
