// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <ax310/ILogger.hpp>
#include <ax310/Types.hpp>

#include <catch2/catch_tostring.hpp>

#include <string>

/// Catch2 needs to be told how to render this project's enums.
///
/// Without these, a failing `REQUIRE(state == ConnectionState::Connected)` prints
/// `{?} == {?}` -- which says a comparison failed and nothing whatsoever about
/// what the value was. Every one of these reuses a name the library already
/// carries, so there is no second table to drift out of step.
namespace Catch
{

template <>
struct StringMaker<ax310::DeviceError>
{
    static std::string convert(ax310::DeviceError value) { return std::string { ax310::describe(value) }; }
};

template <>
struct StringMaker<ax310::LogLevel>
{
    static std::string convert(ax310::LogLevel value) { return std::string { ax310::nameOf(value) }; }
};

template <>
struct StringMaker<ax310::ConnectionState>
{
    static std::string convert(ax310::ConnectionState value)
    {
        switch (value)
        {
            case ax310::ConnectionState::Disconnected: return "Disconnected";
            case ax310::ConnectionState::Connecting: return "Connecting";
            case ax310::ConnectionState::Connected: return "Connected";
        }
        return "ConnectionState(?)";
    }
};

template <>
struct StringMaker<ax310::Button>
{
    static std::string convert(ax310::Button value)
    {
        switch (value)
        {
            case ax310::Button::TopLeft: return "TopLeft";
            case ax310::Button::TopRight: return "TopRight";
            case ax310::Button::BottomLeft: return "BottomLeft";
            case ax310::Button::BottomRight: return "BottomRight";
        }
        return "Button(?)";
    }
};

template <>
struct StringMaker<ax310::KnobId>
{
    static std::string convert(ax310::KnobId value)
    {
        return "Knob" + std::to_string(ax310::indexOf(value) + 1);
    }
};

template <>
struct StringMaker<ax310::Touch>
{
    static std::string convert(ax310::Touch value)
    {
        return value == ax310::Touch::Touched ? "Touched" : "Released";
    }
};

template <>
struct StringMaker<ax310::TouchPhase>
{
    static std::string convert(ax310::TouchPhase value)
    {
        switch (value)
        {
            case ax310::TouchPhase::Released: return "Released";
            case ax310::TouchPhase::Pressed: return "Pressed";
            case ax310::TouchPhase::Moved: return "Moved";
        }
        return "TouchPhase(?)";
    }
};

} // namespace Catch
