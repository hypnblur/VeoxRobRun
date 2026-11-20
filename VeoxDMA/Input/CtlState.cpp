#include "pch.h"
#include "CtlState.h"
#include "MakcuTransport.h"

namespace CtlState {

static uint32_t g_lastBits = 0;
static uint32_t g_captureBaseline = 0;
static bool g_capturing = false;

bool PollControllerState()
{
    auto sample = Makcu::TakeButtonSample();
    if (!sample.valid)
        return false;
    g_lastBits = CtlInputBits(0, 0, 0);
    return true;
}

bool IsCtlKeyDown(int ctlVk)
{
    if (!IsCtlVk(ctlVk))
        return false;
    bool down = false;
    Makcu::MouseDown(ctlVk, down);
    return down;
}

void InvalidateCtlCache()
{
    g_lastBits = 0;
}

void BeginCtlBindCapture()
{
    g_captureBaseline = g_lastBits;
    g_capturing = true;
}

int PollNextCtlPress()
{
    if (!g_capturing)
        return 0;
    auto sample = Makcu::TakeButtonSample();
    if (!sample.valid)
        return 0;
    uint32_t pressed = sample.pressed;
    for (int vk = CTL_VK_A; vk <= CTL_VK_RT; ++vk)
    {
        uint32_t bit = CtlVkBit(vk);
        if (bit && (pressed & bit))
            return vk;
    }
    return 0;
}

} // namespace CtlState
