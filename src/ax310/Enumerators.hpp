// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

/// Listing an enumeration's enumerators, without reflection.
///
/// C++ cannot ask an enumeration what it contains, and the tables in Protocol.hpp
/// need exactly that: a row per enumerator is only checkable against a list of
/// enumerators. Writing that list by hand moves the omission rather than removing
/// it -- the check then passes vacuously for whichever enumerator somebody forgot
/// to add to both places.
///
/// P2996 answers it properly and is C++26; of the compilers this project builds
/// with, one implements it. So the answer here is the older trick: instantiate a
/// function template for every candidate value and read what the compiler calls
/// it in its own diagnostic string. A real enumerator renders as its name, an
/// unused value as a cast:
///
///     [with auto V = Property::MicGain]     valid
///     [with auto V = (Property)42]          not an enumerator
///
/// Only the leading character is examined, never the name, which is the least
/// this can depend on and so the most likely to survive a compiler release. The
/// names are not wanted anyway -- the tables carry better ones than an identifier.
///
/// **This is a compiler-behaviour dependency, and it fails silently in the worst
/// direction**: a format change would make every value look invalid, the lists
/// would come back empty, and every check built on them would pass having tested
/// nothing. selfTestPasses() below exists for that and is asserted at the bottom
/// of this header, so a compiler that breaks the trick breaks the build instead.
namespace ax310::enumerators
{

/// @return The compiler's own rendering of this instantiation.
template <auto V>
[[nodiscard]] constexpr std::string_view enumeratorSignature() noexcept
{
#if defined(_MSC_VER) && !defined(__clang__)
    return __FUNCSIG__;
#else
    return __PRETTY_FUNCTION__;
#endif
}

/// Where the value begins in that rendering.
///
/// GCC and Clang both write `V = ` before it. MSVC writes the template argument
/// into the function name instead, so the marker there is the name itself.
#if defined(_MSC_VER) && !defined(__clang__)
inline constexpr std::string_view SignatureMarker = "enumeratorSignature<";
#else
inline constexpr std::string_view SignatureMarker = "V = ";
#endif

/// @return Whether @p V is a value the enumeration actually declares.
template <auto V>
[[nodiscard]] constexpr bool isEnumerator() noexcept
{
    constexpr std::string_view Signature = enumeratorSignature<V>();
    constexpr std::size_t Start = Signature.find(SignatureMarker);
    static_assert(Start != std::string_view::npos,
                  "this compiler does not render a template argument the way "
                  "SignatureMarker expects; see selfTestPasses()");

    return Signature[Start + SignatureMarker.size()] != '(';
}

/// How many candidate values are tried. One byte's worth, which is every
/// enumeration in this project; a wider one would want a different approach than
/// instantiating a template per value.
inline constexpr std::size_t CandidateCount = 256;

/// @return How many enumerators @p Enum declares, counting each value once.
///
/// An alias -- a `Last` that repeats an earlier enumerator -- is one value and is
/// counted once, because this walks values rather than declarations.
template <typename Enum>
[[nodiscard]] consteval std::size_t enumeratorCount() noexcept
{
    static_assert(std::is_enum_v<Enum>);
    static_assert(sizeof(std::underlying_type_t<Enum>) == 1,
                  "only one-byte enumerations are searched exhaustively");

    std::size_t count = 0;
    [&count]<std::size_t... I>(std::index_sequence<I...>) {
        ((count += isEnumerator<static_cast<Enum>(I)>() ? 1U : 0U), ...);
    }(std::make_index_sequence<CandidateCount> {});

    return count;
}

/// @return Every value @p Enum declares, ascending.
template <typename Enum, std::size_t N = enumeratorCount<Enum>()>
[[nodiscard]] consteval std::array<Enum, N> enumeratorsOf() noexcept
{
    // Collected as the underlying integer rather than as Enum. An array of Enum
    // would have to be value-initialised first, and for an enumeration with no
    // zero enumerator -- which is most of them here -- that briefly holds a value
    // the type does not have.
    std::array<std::underlying_type_t<Enum>, N> raw {};
    std::size_t next = 0;

    [&raw, &next]<std::size_t... I>(std::index_sequence<I...>) {
        (((void) (isEnumerator<static_cast<Enum>(I)>()
                      ? (raw[next++] = static_cast<std::underlying_type_t<Enum>>(I), 0)
                      : 0)),
         ...);
    }(std::make_index_sequence<CandidateCount> {});

    return [&raw]<std::size_t... I>(std::index_sequence<I...>) {
        return std::array<Enum, N> { static_cast<Enum>(raw[I])... };
    }(std::make_index_sequence<N> {});
}

namespace detail
{
    /// Deliberately awkward: a zero, a gap, an alias, and the top of the range.
    enum class SelfTest : std::uint8_t
    {
        Zero = 0,
        Middle = 0x2a,
        Top = 0xff,
        Alias = Middle,
    };
} // namespace detail

/// @return Whether the trick still works on the compiler in hand.
///
/// The failure this guards is silent and total: if a compiler changes how it
/// renders a template argument, nothing here would find a marker or every value
/// would read as a cast, and the lists would come back empty. Every check built
/// on them would then pass having compared nothing at all.
[[nodiscard]] consteval bool selfTestPasses() noexcept
{
    constexpr auto Found = enumeratorsOf<detail::SelfTest>();
    return Found.size() == 3                       // Alias repeats Middle, so three values.
           && Found[0] == detail::SelfTest::Zero   // Zero is a value like any other.
           && Found[1] == detail::SelfTest::Middle // The gap is skipped.
           && Found[2] == detail::SelfTest::Top;   // The last candidate is still searched.
}

static_assert(selfTestPasses(), "the enumerator-listing trick does not work on this compiler");

} // namespace ax310::enumerators
