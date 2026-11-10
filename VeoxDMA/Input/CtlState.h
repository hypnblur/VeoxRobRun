#pragma once

#include <cstdint>

// Read-only view of controller input sampled by the device worker.

// Application button mask; firmware-specific canonical IDs are mapped at input.
#define CTL_BTN_A       0x0001
#define CTL_BTN_B       0x0002
#define CTL_BTN_X       0x0004
#define CTL_BTN_Y       0x0008
#define CTL_BTN_DPAD_UP    0x0010
#define CTL_BTN_DPAD_DOWN  0x0020
#define CTL_BTN_BACK    0x0040
#define CTL_BTN_START   0x0080
#define CTL_BTN_L3      0x0100
#define CTL_BTN_R3      0x0200
#define CTL_BTN_GUIDE   0x0400
#define CTL_BTN_LB      0x0800
#define CTL_BTN_RB      0x1000
#define CTL_BTN_DPAD_LEFT  0x2000
#define CTL_BTN_DPAD_RIGHT 0x4000

// Virtual-key codes for controller binds (stored in the same int fields as Windows VK codes).
// Range 0x1001..0x10FF is reserved; normal VK codes are 0x01..0xFE.
constexpr int CTL_VK_BASE       = 0x1000;
constexpr int CTL_VK_A          = 0x1001;
constexpr int CTL_VK_B          = 0x1002;
constexpr int CTL_VK_X          = 0x1003;
constexpr int CTL_VK_Y          = 0x1004;
constexpr int CTL_VK_DPAD_UP    = 0x1005;
constexpr int CTL_VK_DPAD_DOWN  = 0x1006;
constexpr int CTL_VK_BACK       = 0x1007;
constexpr int CTL_VK_START      = 0x1008;
constexpr int CTL_VK_L3         = 0x1009;
constexpr int CTL_VK_R3         = 0x100A;
constexpr int CTL_VK_GUIDE      = 0x100B;
constexpr int CTL_VK_LB         = 0x100C;
constexpr int CTL_VK_RB         = 0x100D;
constexpr int CTL_VK_DPAD_LEFT  = 0x100E;
constexpr int CTL_VK_DPAD_RIGHT = 0x100F;
constexpr int CTL_VK_LT         = 0x1010;
constexpr int CTL_VK_RT         = 0x1011;
constexpr int CTL_VK_MAX        = 0x1020;

inline bool IsCtlVk(int vk) { return vk > CTL_VK_BASE && vk <= CTL_VK_MAX; }
inline std::uint32_t CtlInputBits(std::uint16_t buttons, std::uint8_t lt, std::uint8_t rt) {
    return buttons | (lt > 128 ? 1u << 16 : 0) | (rt > 128 ? 1u << 17 : 0);
}
inline std::uint32_t CtlVkBit(int vk) {
    constexpr std::uint32_t bits[] = {CTL_BTN_A, CTL_BTN_B, CTL_BTN_X, CTL_BTN_Y,
        CTL_BTN_DPAD_UP, CTL_BTN_DPAD_DOWN, CTL_BTN_BACK, CTL_BTN_START, CTL_BTN_L3,
        CTL_BTN_R3, CTL_BTN_GUIDE, CTL_BTN_LB, CTL_BTN_RB, CTL_BTN_DPAD_LEFT,
        CTL_BTN_DPAD_RIGHT, 1u << 16, 1u << 17};
    return vk >= CTL_VK_A && vk <= CTL_VK_RT ? bits[vk - CTL_VK_A] : 0;
}

namespace CtlState {

// Validate the session snapshot; never performs serial I/O.
bool PollControllerState();

// Check whether a controller virtual key (CTL_VK_*) is currently held.
bool IsCtlKeyDown(int ctlVk);

// Reset the bind capture history (held queries always use the live snapshot).
void InvalidateCtlCache();

// Return the CTL_VK_* for any button/trigger that is newly pressed since last call, or 0 if none.
// Useful for key-bind listening.
int PollNextCtlPress();
// Start listening from the current publication; exclude earlier button taps.
void BeginCtlBindCapture();

} // namespace CtlState
