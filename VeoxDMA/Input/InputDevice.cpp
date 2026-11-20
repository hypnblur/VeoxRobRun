#include "pch.h"
#include "InputDevice.h"

namespace InputDevice {

static AimDeviceType g_type = AimDeviceType::None;

void SetType(AimDeviceType type) { g_type = type; }
AimDeviceType GetType() { return g_type; }

}
