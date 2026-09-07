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

/// @return How many values @p Enum declares consecutively from zero.
///
/// The cheap counterpart to enumeratorCount(), for the enumerations that are
/// dense by construction -- the ones whose enumerators index a table, where a gap
/// would already break rowsInEnumeratorOrder() and every array beside it.
///
/// Cheap matters. enumeratorCount() instantiates a template for all 256 candidate
/// values, which costs about 1.7 seconds per translation unit once the whole of
/// Types.hpp is done that way, and Types.hpp is included everywhere. This stops at
/// the first gap, so it instantiates one template per enumerator and one more.
///
/// Deriving a count this way rather than from a trailing `Last` enumerator means
/// an enumerator added anywhere is counted; a `Last` has to be moved by hand, and
/// leaves every count one short when it is not.
template <typename Enum>
[[nodiscard]] consteval std::size_t denseEnumeratorCount() noexcept
{
    static_assert(std::is_enum_v<Enum>);

    std::size_t count = 0;
    [&count]<std::size_t... I>(std::index_sequence<I...>) {
        // Short-circuits at the first value the enumeration does not declare, so
        // the instantiations after it are never needed.
        (void) ((isEnumerator<static_cast<Enum>(I)>() ? (++count, true) : false) && ...);
    }(std::make_index_sequence<CandidateCount> {});

    return count;
}

/// @return Every value @p Enum declares, for an enumeration dense from zero.
template <typename Enum, std::size_t N = denseEnumeratorCount<Enum>()>
[[nodiscard]] consteval std::array<Enum, N> denseEnumeratorsOf() noexcept
{
    return []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array<Enum, N> { static_cast<Enum>(I)... };
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

    /// Dense from zero, with a trailing alias, so the cheap walk is exercised on
    /// the shape the counts actually run against.
    enum class DenseSelfTest : std::uint8_t
    {
        A = 0,
        B = 1,
        C = 2,
        Alias = C,
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
    constexpr auto Dense = denseEnumeratorsOf<detail::DenseSelfTest>();

    return Found.size() == 3                       // Alias repeats Middle, so three values.
           && Found[0] == detail::SelfTest::Zero   // Zero is a value like any other.
           && Found[1] == detail::SelfTest::Middle // The gap is skipped.
           && Found[2] == detail::SelfTest::Top    // The last candidate is still searched.
           // And the cheap count stops at the gap rather than running past it,
           // which is the whole difference between the two.
           && denseEnumeratorCount<detail::SelfTest>() == 1
           && Dense.size() == 3
           && Dense[2] == detail::DenseSelfTest::C;
}

static_assert(selfTestPasses(), "the enumerator-listing trick does not work on this compiler");

} // namespace ax310::enumerators
