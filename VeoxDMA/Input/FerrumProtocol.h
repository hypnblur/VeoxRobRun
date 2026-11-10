#pragma once
#include "CtlState.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <cstdint>
namespace Ferrum {
// Ferrum's bits differ from the application's Macku-compatible controller mask.
inline std::uint16_t ControllerButtons(unsigned bits) {
    constexpr unsigned masks[] = {CTL_BTN_DPAD_UP, CTL_BTN_DPAD_RIGHT, CTL_BTN_DPAD_DOWN,
        CTL_BTN_DPAD_LEFT, CTL_BTN_Y, CTL_BTN_B, CTL_BTN_A, CTL_BTN_X,
        CTL_BTN_L3, CTL_BTN_R3, CTL_BTN_LB, CTL_BTN_RB, CTL_BTN_BACK, CTL_BTN_START, 0, CTL_BTN_GUIDE, 0};
    std::uint16_t result = 0;
    for (unsigned i = 0; i < std::size(masks); ++i) if (bits & (1u << i)) result |= masks[i];
    return result;
}
struct State { std::uint16_t buttons = 0; std::uint8_t lt = 0, rt = 0; };
inline bool ParseState(const std::string& line, State& state) {
    const auto start = line.find('(');
    if (start == std::string::npos) return false;
    unsigned buttons, lt, rt;
    int lx, ly, rx, ry, consumed = 0;
    if (std::sscanf(line.c_str() + start, "(%u, %u, %u, %d, %d, %d, %d)%n",
        &buttons, &lt, &rt, &lx, &ly, &rx, &ry, &consumed) != 7 || !consumed
        || buttons > 0x1ffff || lt > 65535 || rt > 65535
        || lx < -32768 || lx > 32767 || ly < -32768 || ly > 32767
        || rx < -32768 || rx > 32767 || ry < -32768 || ry > 32767) return false;
    state = {ControllerButtons(buttons), std::uint8_t((lt * 255u + 32767u) / 65535u),
        std::uint8_t((rt * 255u + 32767u) / 65535u)};
    return true;
}
inline std::string Stick(int x, int y) {
    if (!x && !y) return "km.controller(rx=None,ry=None)\r\n";
    // Pixel Y increases downward; Ferrum stick Y increases upward.
    return "km.controller(rx=" + std::to_string(std::clamp(x, -32768, 32767))
        + ",ry=" + std::to_string(std::clamp(-static_cast<long long>(y), -32768LL, 32767LL))
        + ",hold=100)\r\n";
}
inline std::string Control(unsigned id, bool down) {
    constexpr const char* names[] = {"rt", "lt", "rs", "lb", "rb"};
    if (id >= std::size(names)) return {};
    return std::string("km.controller(") + names[id] + "="
        + (down ? id < 2 ? "65535" : "1" : "None") + ",hold=100)\r\n";
}
inline constexpr const char* Clear = "km.controller(rx=None,ry=None,rt=None,lt=None,rs=None,lb=None,rb=None)\r\n";
}
