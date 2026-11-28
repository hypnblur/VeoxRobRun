#pragma once
#include "CtlState.h"
#include <Windows.h>
#include <cstddef>
#include <iterator>
#include <string>

namespace Hotkey {

constexpr int Off = 0;
constexpr int Inherit = -1;
constexpr int Always = -2;
constexpr int Stick = 0x1021;

enum Options { AllowOff = 1, AllowAlways = 2, AllowInherit = 4, AllowMouse = 8, AllowPad = 16 };
constexpr int AimInputs = AllowMouse | AllowPad;

constexpr int LegacyAim[] = {
    VK_RBUTTON, VK_LBUTTON, VK_LMENU, VK_LSHIFT, VK_XBUTTON1, VK_XBUTTON2,
    VK_MBUTTON, VK_LCONTROL, VK_CAPITAL, VK_SPACE, VK_TAB, 'C', 'V', 'E',
    'F', 'Q', 'X', 'Z', Always, CTL_VK_LT, CTL_VK_RT, CTL_VK_LB, CTL_VK_RB,
    CTL_VK_L3, CTL_VK_R3, CTL_VK_A, CTL_VK_B, CTL_VK_X, CTL_VK_Y,
    CTL_VK_DPAD_UP, CTL_VK_DPAD_DOWN, CTL_VK_DPAD_LEFT, CTL_VK_DPAD_RIGHT,
    CTL_VK_BACK, CTL_VK_START, Stick
};
constexpr int LegacyMenu[] = { VK_INSERT, VK_HOME, VK_DELETE, VK_END, VK_F11, VK_F12, VK_RSHIFT };
constexpr int LegacyPanic[] = { Off, VK_END, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12 };
constexpr int LegacyUtility[] = {
    Off, VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9,
    VK_F10, VK_F11, VK_F12, VK_INSERT, VK_HOME, VK_END, VK_DELETE, VK_PRIOR, VK_NEXT
};

inline bool IsMouse(int key)
{
    return key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON ||
        key == VK_XBUTTON1 || key == VK_XBUTTON2;
}

inline bool IsPad(int key) { return (key >= CTL_VK_A && key <= CTL_VK_RT) || key == Stick; }

inline bool IsKeyboard(int key)
{
    return key >= VK_BACK && key <= 0xFE && key != VK_PROCESSKEY && key != VK_PACKET &&
        !(key >= VK_GAMEPAD_A && key <= VK_GAMEPAD_RIGHT_THUMBSTICK_LEFT);
}

inline bool Allowed(int key, int options)
{
    if (key == Off) return (options & AllowOff) != 0;
    if (key == Always) return (options & AllowAlways) != 0;
    if (key == Inherit) return (options & AllowInherit) != 0;
    return IsKeyboard(key) || ((options & AllowMouse) && IsMouse(key)) ||
        ((options & AllowPad) && IsPad(key));
}

template<class Json, std::size_t N>
int Read(const Json& data, const char* field, const char* oldField,
    const int (&legacy)[N], int fallback, int options)
{
    if (const auto it = data.find(field); it != data.end()) {
        if (it->is_number_integer() && *it >= Always && *it <= Stick) {
            const int key = it->template get<int>();
            if (Allowed(key, options)) return key;
        }
        return fallback;
    }
    if (const auto it = data.find(oldField); it != data.end()) {
        if (it->is_number_integer() && *it >= 0 && *it < static_cast<int>(N)) {
            const int key = legacy[it->template get<int>()];
            if (Allowed(key, options)) return key;
        }
        // Older optional aim bindings used -1 for Off or Inherit.
        if ((options & AllowOff) && !(options & AllowInherit)) return Off;
    }
    return fallback;
}

inline std::string Label(int key)
{
    switch (key) {
    case Off: return "Off";
    case Inherit: return "Inherit";
    case Always: return "Always";
    case VK_LBUTTON: return "Left Mouse";
    case VK_RBUTTON: return "Right Mouse";
    case VK_MBUTTON: return "Middle Mouse";
    case VK_XBUTTON1: return "Mouse 4";
    case VK_XBUTTON2: return "Mouse 5";
    case Stick: return "Gamepad Stick";
    }
    if (key >= CTL_VK_A && key <= CTL_VK_RT) {
        constexpr const char* names[] = { "A", "B", "X", "Y", "D-Up", "D-Down", "Back", "Start",
            "L3", "R3", "Guide", "LB", "RB", "D-Left", "D-Right", "LT", "RT" };
        return std::string("Gamepad ") + names[key - CTL_VK_A];
    }
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC_EX);
    const LONG keyData = static_cast<LONG>(((scan & 0xff) << 16) | ((scan & 0xff00) ? 1u << 24 : 0));
    wchar_t name[128]{};
    if (const int length = GetKeyNameTextW(keyData, name, static_cast<int>(std::size(name))); length > 0) {
        char utf8[512]{};
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, name, length, utf8, sizeof(utf8), nullptr, nullptr);
        if (bytes > 0) return std::string(utf8, bytes);
    }
    return "Key " + std::to_string(key);
}

} // namespace Hotkey
