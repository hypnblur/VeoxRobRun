#include "pch.h"
#include "kmbox.h"
#include "MakcuTransport.h"
#include "MediusTransport.h"
#include "CtlState.h"
#include "InputDevice.h"
#include <setupapi.h>
#include <devguid.h>
#include <stdio.h>
#include <string>
#include <cctype>
#include <mutex>

namespace kmbox
{
    HANDLE serial_handle = nullptr;
    bool connected = false;
    static bool g_adapterIsMacku = false;
    static bool g_normalMakcu = false;
    static bool g_mediusMakcu = false;
    static std::string g_connect_error;

    // Single mutex protecting every read/write on serial_handle. See header.
    static std::mutex g_serial_mutex;
    static int g_write_fail_streak = 0;
    static bool g_reconnect_requested = false;

    static constexpr int kMaxWriteFailStreak = 4;

    static bool SendCommand(const char* cmd, size_t len);

    // Desired vs delivered synthetic button state. SendCommand drops a
    // command when the OS write fails or times out (TX buffer full, USB
    // stall); for a movement delta that's a lost frame, but a lost
    // km.left(0) leaves the button latched DOWN on the device until the
    // next unrelated click — the "triggerbot keeps firing until you trigger
    // someone again" failure. The click functions record intent;
    // TickSerialLinkCheck re-sends until delivered matches desired.
    static bool g_leftDesiredDown    = false;
    static bool g_leftDeliveredDown  = false;
    static bool g_rightDesiredDown   = false;
    static bool g_rightDeliveredDown = false;

    static void LogWire(const char* what, const char* cmd, size_t len, bool ok)
    {
        (void)what; (void)cmd; (void)len; (void)ok;
    }

    static void set_connect_err(const char* msg)
    {
        g_connect_error = msg ? msg : "";
    }

    static void mark_serial_unhealthy_locked(const char* reason)
    {
        if (connected)
            printf("[Kmbox] link DOWN: %s\n", reason ? reason : "(no reason)");
        connected = false;
        g_adapterIsMacku = false;
        g_reconnect_requested = true;
        if (reason && reason[0])
            set_connect_err(reason);
        CtlState::InvalidateCtlCache();
        if (serial_handle) {
            CloseHandle(serial_handle);
            serial_handle = nullptr;
        }
    }

    static void mark_serial_unhealthy(const char* reason)
    {
        std::lock_guard<std::mutex> lock(g_serial_mutex);
        mark_serial_unhealthy_locked(reason);
    }

    HANDLE GetSerialHandle() { return serial_handle; }
    std::mutex& GetSerialMutex() { return g_serial_mutex; }

    static void set_connect_err_win32(const char* ctx, const char* portForUser)
    {
        DWORD e = GetLastError();
        char buf[384];
        if (e == 0)
            snprintf(buf, sizeof(buf), "%s: %s", ctx, portForUser ? portForUser : "COM");
        else
            snprintf(buf, sizeof(buf), "%s (%s) - Windows error %lu. Port may be in use, "
                "unplugged, or blocked (try another app closed / run as Administrator).",
                ctx, portForUser ? portForUser : "COM", (unsigned long)e);
        g_connect_error = buf;
    }

    const std::string& LastConnectError() { return g_connect_error; }
    void ClearConnectError() { g_connect_error.clear(); }

    void SetConnectDiagnostics(const char* message)
    {
        g_connect_error = message ? message : "";
    }

    static int clamp(int i)
    {
        if (i > 127) i = 127;
        if (i < -128) i = -128;
        return i;
    }

    static int clamp_stick_axis(int v)
    {
        if (v > 32767) return 32767;
        if (v < -32767) return -32767;
        return v;
    }

    std::string find_port(const std::string& targetDescription)
    {
        HDEVINFO hDevInfo = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, 0, 0, DIGCF_PRESENT);
        if (hDevInfo == INVALID_HANDLE_VALUE) return "";

        SP_DEVINFO_DATA deviceInfoData = {};
        deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

        for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &deviceInfoData); ++i)
        {
            char buf[512];
            DWORD nSize = 0;

            if (SetupDiGetDeviceRegistryPropertyA(hDevInfo, &deviceInfoData, SPDRP_FRIENDLYNAME, NULL, (PBYTE)buf, sizeof(buf), &nSize) && nSize > 0)
            {
                if (nSize >= sizeof(buf)) nSize = sizeof(buf) - 1;
                buf[nSize] = '\0';
                std::string deviceDescription = buf;

                size_t comPos = deviceDescription.find("COM");
                size_t endPos = deviceDescription.find(")", comPos);

                if (comPos != std::string::npos && endPos != std::string::npos && deviceDescription.find(targetDescription) != std::string::npos)
                {
                    SetupDiDestroyDeviceInfoList(hDevInfo);
                    std::string raw = deviceDescription.substr(comPos, endPos - comPos);
                    size_t numStart = 3;
                    while (numStart < raw.size() && raw[numStart] == ' ') numStart++;
                    size_t numEnd = numStart;
                    while (numEnd < raw.size() && std::isdigit((unsigned char)raw[numEnd])) numEnd++;
                    if (numEnd > numStart)
                        return "COM" + raw.substr(numStart, numEnd - numStart);
                    return raw;
                }
            }
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return "";
    }

    static std::string friendly_name_for_com(const std::string& com)
    {
        if (com.empty())
            return {};
        HDEVINFO hDevInfo = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, 0, 0, DIGCF_PRESENT);
        if (hDevInfo == INVALID_HANDLE_VALUE)
            return {};
        SP_DEVINFO_DATA deviceInfoData = {};
        deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
        std::string found;
        for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &deviceInfoData); ++i) {
            char buf[512];
            DWORD nSize = 0;
            if (!SetupDiGetDeviceRegistryPropertyA(hDevInfo, &deviceInfoData, SPDRP_FRIENDLYNAME,
                    NULL, (PBYTE)buf, sizeof(buf), &nSize) || nSize == 0)
                continue;
            if (nSize >= sizeof(buf))
                nSize = sizeof(buf) - 1;
            buf[nSize] = '\0';
            const std::string desc = buf;
            const size_t comPos = desc.find(com);
            if (comPos == std::string::npos)
                continue;
            const size_t after = comPos + com.size();
            if (after < desc.size() && std::isdigit(static_cast<unsigned char>(desc[after])))
                continue;
            found = desc;
            break;
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return found;
    }

    static bool description_looks_like_macku(const std::string& desc)
    {
        std::string d = desc;
        for (char& c : d)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return d.find("ch343") != std::string::npos;
    }

    bool SerialAdapterIsMacku()
    {
        return g_adapterIsMacku;
    }

    std::string suggest_serial_hardware_port(bool preferMacku)
    {
        if (preferMacku) {
            std::string p = find_port("USB-Enhanced-SERIAL CH343");
            if (!p.empty())
                return p;
            p = find_port("CH343");
            if (!p.empty())
                return p;
        }
        std::string p = find_port("USB-SERIAL CH340");
        if (!p.empty())
            return p;
        p = find_port("CH343");
        if (!p.empty())
            return p;
        p = find_port("CP210");
        if (!p.empty())
            return p;
        p = find_port("USB Serial Device");
        if (!p.empty())
            return p;
        return "";
    }

    std::vector<std::string> list_ports()
    {
        std::vector<std::string> result;
        HDEVINFO hDevInfo = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, 0, 0, DIGCF_PRESENT);
        if (hDevInfo == INVALID_HANDLE_VALUE) return result;

        SP_DEVINFO_DATA deviceInfoData = {};
        deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

        for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &deviceInfoData); ++i)
        {
            char buf[512];
            DWORD nSize = 0;

            if (SetupDiGetDeviceRegistryPropertyA(hDevInfo, &deviceInfoData, SPDRP_FRIENDLYNAME, NULL, (PBYTE)buf, sizeof(buf), &nSize) && nSize > 0)
            {
                if (nSize >= sizeof(buf)) nSize = sizeof(buf) - 1;
                buf[nSize] = '\0';
                std::string deviceDescription = buf;

                size_t comPos = deviceDescription.find("COM");
                if (comPos != std::string::npos)
                {
                    size_t numStart = comPos + 3;
                    while (numStart < deviceDescription.size() && deviceDescription[numStart] == ' ')
                        numStart++;
                    size_t numEnd = numStart;
                    while (numEnd < deviceDescription.size() && std::isdigit((unsigned char)deviceDescription[numEnd]))
                        numEnd++;
                    if (numEnd > numStart) {
                        std::string portName = "COM" + deviceDescription.substr(numStart, numEnd - numStart);
                        bool found = false;
                        for (const auto& p : result) { if (p == portName) { found = true; break; } }
                        if (!found)
                            result.push_back(portName);
                    }
                }
            }
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return result;
    }

    static std::string trim_ws(std::string s)
    {
        while (!s.empty() && std::isspace((unsigned char)s.front()))
            s.erase(s.begin());
        while (!s.empty() && std::isspace((unsigned char)s.back()))
            s.pop_back();
        return s;
    }

    static std::string normalize_com_port_name(std::string s)
    {
        s = trim_ws(std::move(s));
        if (s.size() < 4)
            return s;
        if (std::toupper((unsigned char)s[0]) != 'C' ||
            std::toupper((unsigned char)s[1]) != 'O' ||
            std::toupper((unsigned char)s[2]) != 'M')
            return s;
        size_t i = 3;
        while (i < s.size() && std::isspace((unsigned char)s[i]))
            i++;
        const std::string num = s.substr(i);
        if (num.empty())
            return s;
        for (char c : num) {
            if (!std::isdigit((unsigned char)c))
                return s;
        }
        return std::string("COM") + num;
    }

    static bool is_normalized_com_port(const std::string& p)
    {
        if (p.size() < 4 || p.compare(0, 3, "COM") != 0)
            return false;
        size_t i = 3;
        while (i < p.size() && std::isdigit((unsigned char)p[i]))
            i++;
        return i == p.size();
    }

    void KmboxInitialize(const std::string& port, int baudRate, bool preferMacku)
    {
        if (static_cast<int>(InputDevice::GetType()) == 5) {
            g_normalMakcu = true;
            g_mediusMakcu = false;
            connected = false;
            ClearConnectError();
            Makcu::Connect(port.c_str(), baudRate);
            return;
        }
        if (static_cast<int>(InputDevice::GetType()) == 8) {
            g_mediusMakcu = true;
            g_normalMakcu = false;
            connected = false;
            ClearConnectError();
            Medius::Connect(port.c_str());
            return;
        }
        std::lock_guard<std::mutex> lock(g_serial_mutex);

        ClearConnectError();
        connected = false;
        g_adapterIsMacku = false;
        CtlState::InvalidateCtlCache();
        if (serial_handle)
        {
            CloseHandle(serial_handle);
            serial_handle = nullptr;
        }

        const std::string raw = trim_ws(port);
        std::string portPath;
        const bool needsAuto =
            raw.empty() ||
            raw == "COM0" ||
            raw == "com0";

        if (needsAuto) {
            portPath = suggest_serial_hardware_port(preferMacku);
        } else {
            portPath = normalize_com_port_name(raw);
            if (!is_normalized_com_port(portPath)) {
                set_connect_err("Invalid COM port. Use COM followed by digits (e.g. COM5 or com5).");
                return;
            }
        }
        if (portPath.empty()) {
            set_connect_err("No COM port resolved. Plug in the Macku (USB-Enhanced-SERIAL CH343) or choose a port from the list.");
            return;
        }

        const std::string portLabel = portPath;
        portPath = "\\\\.\\" + portPath;

        serial_handle = CreateFileA(portPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
        if (serial_handle == INVALID_HANDLE_VALUE) {
            set_connect_err_win32("Could not open serial port", portLabel.c_str());
            return;
        }

        // 2 KB TX bound ≈ 180 ms of queued commands at 115200 baud. The old
        // 8192-byte buffer let many seconds of stale movement queue up when
        // producers briefly outran the wire — the crosshair then kept
        // replaying old deltas long after the operator released the key.
        // For a real-time control link, dropping late is better than late.
        if (!SetupComm(serial_handle, 2048, 2048))
        {
            set_connect_err_win32("SetupComm failed", portLabel.c_str());
            CloseHandle(serial_handle);
            serial_handle = nullptr;
            return;
        }

        COMMTIMEOUTS timeouts = {};
        if (!GetCommTimeouts(serial_handle, &timeouts))
        {
            set_connect_err_win32("GetCommTimeouts failed", portLabel.c_str());
            CloseHandle(serial_handle);
            serial_handle = nullptr;
            return;
        }
        // Bounded timeouts so a stalled USB-CDC stack can never block the
        // render thread long enough to trigger the Windows "Not Responding"
        // popup (>5 s). 200 ms is generous even at 9600 baud.
        timeouts.ReadIntervalTimeout = 0xFFFFFFFF;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 0;
        timeouts.WriteTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = 200;
        if (!SetCommTimeouts(serial_handle, &timeouts))
        {
            set_connect_err_win32("SetCommTimeouts failed", portLabel.c_str());
            CloseHandle(serial_handle);
            serial_handle = nullptr;
            return;
        }

        PurgeComm(serial_handle, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR);

        DCB dcb = {};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(serial_handle, &dcb))
        {
            set_connect_err_win32("GetCommState failed", portLabel.c_str());
            CloseHandle(serial_handle);
            serial_handle = nullptr;
            return;
        }
        dcb.BaudRate = baudRate;
        dcb.ByteSize = 8;
        dcb.StopBits = ONESTOPBIT;
        dcb.Parity = NOPARITY;
        if (!SetCommState(serial_handle, &dcb))
        {
            set_connect_err_win32("SetCommState failed (check baud rate)", portLabel.c_str());
            CloseHandle(serial_handle);
            serial_handle = nullptr;
            return;
        }

        connected = true;
        g_write_fail_streak = 0;
        g_reconnect_requested = false;
        {
            const std::string name = friendly_name_for_com(portLabel);
            g_adapterIsMacku = description_looks_like_macku(name);
        }

        // Normalize synthetic button state on every (re)connect: if the
        // previous session dropped a release (or the app exited mid-click),
        // the device still has the button latched down. One explicit release
        // per button clears it; the physical mouse passthrough is unaffected.
        g_leftDesiredDown  = g_leftDeliveredDown  = false;
        g_rightDesiredDown = g_rightDeliveredDown = false;
        static const char kNormalize[] = "km.left(0)\r\nkm.right(0)\r\n";
        DWORD normWritten = 0;
        WriteFile(serial_handle, kNormalize, (DWORD)(sizeof(kNormalize) - 1), &normWritten, NULL);

        printf("[Kmbox] link UP on %s @%d baud normalizeWrote=%u\n",
            portLabel.c_str(), baudRate, (unsigned)normWritten);
    }

    bool IsConnecting()
    {
        if (g_mediusMakcu) return Medius::GetState() == Medius::State::Connecting;
        return g_normalMakcu && Makcu::GetState() == Makcu::State::Connecting;
    }
    void ShutdownSerialWorker()
    {
        Makcu::Shutdown();
        Medius::Shutdown();
    }

    bool IsConnected()
    {
        return connected;
    }

    bool ConsumeReconnectRequested()
    {
        if (!g_reconnect_requested)
            return false;
        g_reconnect_requested = false;
        return true;
    }

    void TickSerialLinkCheck()
    {
        if (g_normalMakcu) {
            const auto state = Makcu::GetState();
            if (connected && state == Makcu::State::Failed) g_reconnect_requested = true;
            connected = state == Makcu::State::Connected;
            g_adapterIsMacku = connected;
            if (state == Makcu::State::Failed) {
                char error[240]{}; Makcu::GetError(error,sizeof(error));
                set_connect_err(error);
            }
            return;
        }
        if (g_mediusMakcu) {
            const auto state = Medius::GetState();
            if (connected && state == Medius::State::Failed) g_reconnect_requested = true;
            connected = state == Medius::State::Connected;
            g_adapterIsMacku = connected;
            if (state == Medius::State::Failed) {
                char error[240]{}; Medius::GetError(error, sizeof(error));
                set_connect_err(error);
            }
            return;
        }
        if (!connected)
            return;

        {
            std::lock_guard<std::mutex> lock(g_serial_mutex);
            if (!serial_handle || serial_handle == INVALID_HANDLE_VALUE) {
                mark_serial_unhealthy_locked("Serial handle lost.");
                return;
            }

            DWORD errors = 0;
            COMSTAT cs{};
            if (!ClearCommError(serial_handle, &errors, &cs)) {
                mark_serial_unhealthy_locked("Serial link error (device unplugged or port closed).");
                return;
            }

            // The firmware echoes a reply to every command, but nothing reads
            // it unless a CTL bind is polling ctl.state(). On a keyboard/mouse
            // aim type the 2 KB RX queue therefore fills within seconds of the
            // first aim burst and the driver raises CE_OVERRUN — which used to
            // be treated as "device unplugged" and tore down a perfectly
            // healthy link. RX conditions say nothing about the link: drain and
            // carry on. CE_BREAK is the only one that means the line dropped.
            if (cs.cbInQue > 0 || (errors & (CE_OVERRUN | CE_RXPARITY | CE_FRAME)))
                PurgeComm(serial_handle, PURGE_RXCLEAR);

            if (errors & CE_BREAK) {
                mark_serial_unhealthy_locked("Serial line break on COM port.");
                return;
            }
        }

        // Reconcile synthetic button state: re-send any press/release whose
        // last transmit was dropped by a full TX buffer or write timeout.
        // Runs once per render frame; a no-op when state already matches.
        if (g_leftDeliveredDown != g_leftDesiredDown) {
            static const char kDown[] = "km.left(1)\r\n";
            static const char kUp[]   = "km.left(0)\r\n";
            const bool want = g_leftDesiredDown;
            if (SendCommand(want ? kDown : kUp,
                            (want ? sizeof(kDown) : sizeof(kUp)) - 1))
                g_leftDeliveredDown = want;
        }
        if (g_rightDeliveredDown != g_rightDesiredDown) {
            static const char kDown[] = "km.right(1)\r\n";
            static const char kUp[]   = "km.right(0)\r\n";
            const bool want = g_rightDesiredDown;
            if (SendCommand(want ? kDown : kUp,
                            (want ? sizeof(kDown) : sizeof(kUp)) - 1))
                g_rightDeliveredDown = want;
        }
    }

    void KmboxDisconnect()
    {
        Makcu::Disconnect();
        Medius::Disconnect();
        g_normalMakcu = false;
        g_mediusMakcu = false;
        std::lock_guard<std::mutex> lock(g_serial_mutex);
        connected = false;
        g_adapterIsMacku = false;
        g_write_fail_streak = 0;
        g_reconnect_requested = false;
        g_leftDesiredDown  = g_leftDeliveredDown  = false;
        g_rightDesiredDown = g_rightDeliveredDown = false;
        ClearConnectError();
        CtlState::InvalidateCtlCache();
        if (serial_handle)
        {
            CloseHandle(serial_handle);
            serial_handle = nullptr;
        }
    }

    // Hot path: called up to several hundred times per second from aimbot
    // and triggerbot. Must (a) take the shared serial lock to
    // serialize with CtlState polls, (b) avoid heap allocation, and
    // (c) never call FlushFileBuffers — that primitive blocks until every
    // byte has clocked out of the TX shift register, which at 9600 baud
    // means tens of milliseconds per command and is the primary cause of
    // the "Not Responding" hang reported by users. The OS keeps writes
    // strictly FIFO regardless, so the device sees commands in order.
    static bool SendCommand(const char* cmd, size_t len)
    {
        if (!cmd || len == 0) return false;
        if (g_mediusMakcu) return false;
        if (g_normalMakcu) return Makcu::Send(cmd,len);
        std::lock_guard<std::mutex> lock(g_serial_mutex);
        if (!serial_handle || !connected) return false;

        DWORD bytesWritten = 0;
        if (!WriteFile(serial_handle, cmd, (DWORD)len, &bytesWritten, NULL)
            || bytesWritten != (DWORD)len) {
            // A WriteTotalTimeoutConstant expiry returns TRUE with a SHORT
            // count: the leading bytes are already on the wire, so the device
            // is sitting mid-command. Left alone the fragment gets glued to the
            // next command ("km.mo" + "km.move(3,1)") and the firmware's parser
            // desyncs — the state that only a USB replug clears. Drop the rest
            // of the queue and terminate the line so the device discards the
            // fragment as one bad command instead of corrupting the next.
            if (bytesWritten > 0 && bytesWritten < (DWORD)len) {
                PurgeComm(serial_handle, PURGE_TXCLEAR);
                DWORD eol = 0;
                WriteFile(serial_handle, "\r\n", 2, &eol, NULL);
            }
            if (++g_write_fail_streak >= kMaxWriteFailStreak)
                mark_serial_unhealthy_locked("Serial write failed repeatedly. Reconnect the Macku device.");
            (void)0;
            return false;
        }

        g_write_fail_streak = 0;
        return true;
    }

    bool move(int x, int y, bool recoil)
    {
        if (!connected) return false;
        x = clamp(x);
        y = clamp(y);
        char buf[48];
        const int n = snprintf(buf, sizeof(buf), "km.move(%d,%d)\r\n", x, y);
        if (g_mediusMakcu)
            return Medius::Move(x, y, recoil);
        return n > 0 && (g_normalMakcu
            ? Makcu::Send(buf, static_cast<size_t>(n), true, false, recoil)
            : SendCommand(buf, static_cast<size_t>(n)));
    }

    void macku_stick(int rx, int ry)
    {
        if (!connected) return;
        rx = clamp_stick_axis(rx);
        ry = clamp_stick_axis(ry);
        char buf[64];
        const int n = snprintf(buf, sizeof(buf), "ctl.stick(%d,%d)\r\n", rx, ry);
        if (n > 0)
            SendCommand(buf, (size_t)n);
    }

    void macku_clear()
    {
        if (!connected || g_normalMakcu || g_mediusMakcu) return;
        static const char kCmd[] = "ctl.clear()\r\n";
        SendCommand(kCmd, sizeof(kCmd) - 1);
    }

    void move_to_center()
    {
        if (!connected) return;
        // km.aim(x,y): absolute position, 0-65535 maps to full screen, center = 32768
        static const char kCmd[] = "km.aim(32768,32768)\r\n";
        SendCommand(kCmd, sizeof(kCmd) - 1);
    }

    void left_click()
    {
        if (!connected) {
            LogWire("left(1) skip-disconnected", "", 0, false);
            return;
        }
        if (g_mediusMakcu) { Medius::InjectButton(0, true); return; }
        g_leftDesiredDown = true;
        static const char kCmd[] = "km.left(1)\r\n";
        const bool ok = SendCommand(kCmd, sizeof(kCmd) - 1);
        if (ok)
            g_leftDeliveredDown = true;
        LogWire("left(1)", kCmd, sizeof(kCmd) - 1, ok);
    }

    void left_click_release()
    {
        if (!connected) {
            LogWire("left(0) skip-disconnected", "", 0, false);
            return;
        }
        if (g_mediusMakcu) { Medius::InjectButton(0, false); return; }
        g_leftDesiredDown = false;
        static const char kCmd[] = "km.left(0)\r\n";
        const bool ok = SendCommand(kCmd, sizeof(kCmd) - 1);
        if (ok)
            g_leftDeliveredDown = false;
        LogWire("left(0)", kCmd, sizeof(kCmd) - 1, ok);
    }

    void lock_ml(bool locked)
    {
        if (!connected) {
            LogWire(locked ? "lock_ml(1) skip-disconnected" : "lock_ml(0) skip-disconnected",
                "", 0, false);
            return;
        }
        if (static_cast<int>(InputDevice::GetType()) != 5 && static_cast<int>(InputDevice::GetType()) != 6) {
            LogWire(locked ? "lock_ml(1) skip-not-macku" : "lock_ml(0) skip-not-macku",
                "", 0, false);
            return;
        }
        if (!g_adapterIsMacku) {
            LogWire(locked ? "lock_ml(1) skip-kmbox-adapter" : "lock_ml(0) skip-kmbox-adapter",
                "", 0, false);
            return;
        }
        static const char kOn[]  = "km.lock_ml(1)\r\n";
        static const char kOff[] = "km.lock_ml(0)\r\n";
        const char* cmd = locked ? kOn : kOff;
        const size_t len = locked ? sizeof(kOn) - 1 : sizeof(kOff) - 1;
        const bool ok = SendCommand(cmd, len);
        LogWire(locked ? "lock_ml(1)" : "lock_ml(0)", cmd, len, ok);
    }

    void left_click_tap()
    {
        if (!connected) {
            LogWire("left-tap skip-disconnected", "", 0, false);
            return;
        }
        if (g_mediusMakcu) {
            Medius::InjectButton(0, true);
            Medius::InjectButton(0, false);
            return;
        }
        g_leftDesiredDown = false;
        static const char kCmd[] = "km.left(1)\r\nkm.left(0)\r\n";
        const bool ok = SendCommand(kCmd, sizeof(kCmd) - 1);
        if (ok)
            g_leftDeliveredDown = false;
        LogWire("left-tap", kCmd, sizeof(kCmd) - 1, ok);
    }

    void right_click()
    {
        if (!connected) return;
        if (g_mediusMakcu) { Medius::InjectButton(1, true); return; }
        g_rightDesiredDown = true;
        static const char kCmd[] = "km.right(1)\r\n";
        if (SendCommand(kCmd, sizeof(kCmd) - 1))
            g_rightDeliveredDown = true;
    }

    void right_click_release()
    {
        if (!connected) return;
        if (g_mediusMakcu) { Medius::InjectButton(1, false); return; }
        g_rightDesiredDown = false;
        static const char kCmd[] = "km.right(0)\r\n";
        if (SendCommand(kCmd, sizeof(kCmd) - 1))
            g_rightDeliveredDown = false;
    }

    static void send_button_command(const char* button, bool down)
    {
        if (!connected || !button) return;
        char buf[40];
        const int n = snprintf(buf, sizeof(buf), "km.%s(%d)\r\n", button, down ? 1 : 0);
        if (n > 0)
            SendCommand(buf, static_cast<size_t>(n));
    }

    void middle_click(bool down)
    {
        if (g_mediusMakcu) { if (connected) Medius::InjectButton(2, down); return; }
        send_button_command("middle", down);
    }
    void side1_click(bool down)
    {
        if (g_mediusMakcu) { if (connected) Medius::InjectButton(3, down); return; }
        send_button_command(g_normalMakcu ? "ms1" : "side1", down);
    }
    void side2_click(bool down)
    {
        if (g_mediusMakcu) { if (connected) Medius::InjectButton(4, down); return; }
        send_button_command(g_normalMakcu ? "ms2" : "side2", down);
    }

    static void send_key_command(const char* action, unsigned int hid)
    {
        if (!connected || !action || hid == 0 || hid > 0xE7) return;
        char buf[40];
        const int n = snprintf(buf, sizeof(buf), "km.%s(%u)\r\n", action, hid);
        if (n > 0)
            SendCommand(buf, static_cast<size_t>(n));
    }

    void key_down_hid(unsigned int hid)
    {
        if (g_mediusMakcu) { if (connected) Medius::InjectKey(hid, true); return; }
        send_key_command("down", hid);
    }
    void key_up_hid(unsigned int hid)
    {
        if (g_mediusMakcu) { if (connected) Medius::InjectKey(hid, false); return; }
        send_key_command("up", hid);
    }
    void key_press_hid(unsigned int hid)
    {
        if (!connected || hid == 0 || hid > 0xE7) return;
        if (g_mediusMakcu) {
            Medius::InjectKey(hid, true);
            Medius::InjectKey(hid, false);
            return;
        }
        char buf[40];
        const int n = snprintf(buf, sizeof(buf), "km.press(%u)\r\n", hid);
        if (n > 0)
            SendCommand(buf, static_cast<size_t>(n));
    }
}
