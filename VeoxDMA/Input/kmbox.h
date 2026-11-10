#pragma once

#include <windows.h>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#pragma comment(lib, "setupapi.lib")
#endif

namespace kmbox
{
    extern bool connected;
    bool SerialAdapterIsMacku();

    // Read-only access to the shared serial handle (for ctl.state polling)
    HANDLE GetSerialHandle();

    // Serializes every read/write against the shared serial handle. Aimbot,
    // triggerbot, and CtlState all funnel through this lock so a
    // controller-state poll can never interleave bytes with an in-flight
    // km.move/ctl.stick command.
    std::mutex& GetSerialMutex();

    // Find COM port by device description (e.g. "USB-Enhanced-SERIAL CH343")
    std::string find_port(const std::string& targetDescription);

    // Auto-pick a serial port; preferMacku tries CH343 / Macku adapters first.
    std::string suggest_serial_hardware_port(bool preferMacku = false);

    // List all available COM ports (e.g. "COM1", "COM3")
    std::vector<std::string> list_ports();

    // Initialize kmbox: port can be "COM3", empty, or "COM0" to auto-detect
    void KmboxInitialize(const std::string& port, int baudRate, bool preferMacku = false);

    // Human-readable reason when the last KmboxInitialize failed (empty if none / success).
    const std::string& LastConnectError();
    void ClearConnectError();
    // For UI-only failures (e.g. user clicked Connect with no ports listed).
    void SetConnectDiagnostics(const char* message);

    // Disconnect and close the serial port
    void KmboxDisconnect();

    // True when the last open succeeded and the link has not been marked bad.
    bool IsConnected();

    // Set by the serial layer after repeated I/O failures; cleared on successful open.
    bool ConsumeReconnectRequested();

    // Lightweight USB/COM health probe (call once per frame from render loop).
    void TickSerialLinkCheck();

    // Normal Makcu: accepted into bounded worker queue; others: full serial write.
    bool move(int x, int y, bool recoil = false);
    bool IsConnecting();
    void ShutdownSerialWorker();
    // Macku controller: ctl.stick(rx,ry) signed ints [-32767,32767]; ctl.clear() releases override
    void macku_stick(int rx, int ry);
    void macku_clear();
    // Move mouse to absolute position (0-65535 range, center = 32768,32768)
    void move_to_center();
    void left_click();
    void left_click_release();
    void left_click_tap();
    // Macku only. Do not send on kmbox USB/Net.
    void lock_ml(bool locked);
    void right_click();
    void right_click_release();
    void middle_click(bool down);
    void side1_click(bool down);
    void side2_click(bool down);
    void key_down_hid(unsigned int hid);
    void key_up_hid(unsigned int hid);
    void key_press_hid(unsigned int hid);
}
