#pragma once

enum class AimDeviceType : int {
    None = 0,
    KmboxSerial = 1,    // CH340 USB serial
    KmboxNet = 2,       // KmBox Net UDP
    Macku = 5,          // Makcu/Macku serial (CH343)
    Ferrum = 6,         // Ferrum serial (controller emulation)
    Medius = 8,         // Medius (medius_capi.dll)
};

namespace InputDevice {
    void SetType(AimDeviceType type);
    AimDeviceType GetType();
}
