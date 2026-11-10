#include "pch.h"
#pragma comment(lib, "medius_capi.dll.lib")
#include "MediusTransport.h"
#include "MakcuTransport.h"
#include "kmbox.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>

// windows.h defines 'interface' as a macro (__declspec(novtable)); the C header uses it as a field name.
#pragma push_macro("interface")
#undef interface
#include "medius/medius.h"
#pragma pop_macro("interface")

namespace Medius {
namespace {

struct Transport {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> generation{0};
    std::atomic<State> state{State::Disconnected};
    std::atomic<unsigned> buttons{0};
    std::atomic<int> aim{0};
    std::atomic<int> testResult{0};
    std::atomic<bool> testPending{false};
    std::atomic<uint64_t> axisLease[2]{};
    bool axesLocked = false;
    char port[64]{}, error[240]{};
    bool wantConnection = false;

    MediusDevice* dev = nullptr;
    MediusEventStream* eventStream = nullptr;

    ~Transport() { Stop(); }
    void Stop() {
        stop = true; wake.notify_all();
        if (worker.joinable()) worker.join();
    }
    bool Current(uint64_t gen) { return !stop && generation.load() == gen; }

    void Log(const char* fmt, ...) {
#if !defined(BASE_DLL_RELEASE)
        char body[512]{};
        va_list args; va_start(args, fmt);
        vsnprintf_s(body, _TRUNCATE, fmt, args); va_end(args);
        CreateDirectoryA("C:\\PureDMA", nullptr);
        HANDLE file = CreateFileA("C:\\PureDMA\\puredma.log", FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        SYSTEMTIME time{}; GetLocalTime(&time);
        char line[640]{};
        const int n = snprintf(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u] [Medius] %s\r\n",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, body);
        DWORD written = 0;
        if (n > 0) WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
        CloseHandle(file);
#else
        (void)fmt;
#endif
    }

    void Close() {
        axisLease[0] = axisLease[1] = 0;
        axesLocked = false;
        if (eventStream) {
            medius_event_stream_close(eventStream);
            medius_event_stream_free(eventStream);
            eventStream = nullptr;
        }
        if (dev) {
            medius_device_reset(dev);
            medius_device_free(dev);
            dev = nullptr;
        }
        buttons = 0;
    }

    void Fail(uint64_t gen, const char* reason) {
        { std::lock_guard<std::mutex> lock(mutex);
          if (!Current(gen)) return;
          char buf[256];
          medius_last_error_message(buf, sizeof(buf));
          if (buf[0])
              snprintf(error, sizeof(error), "%s (%s)", reason, buf);
          else
              snprintf(error, sizeof(error), "%s", reason);
          state = State::Failed; buttons = 0;
          if (testPending.exchange(false)) testResult = -1;
        }
        Log("link failed generation=%llu reason=%s", gen, reason);
    }

    bool Open(uint64_t gen, std::string selected) {
        if (medius_abi_version() != MEDIUS_ABI_VERSION) {
            Fail(gen, "Medius ABI version mismatch — update medius_capi.dll or medius.h");
            return false;
        }

        MediusStatus st;
        if (selected.empty() || selected == "COM0" || selected == "com0") {
            st = medius_device_find(&dev);
        } else {
            for (char& c : selected) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            std::string path = "\\\\.\\" + selected;
            st = medius_device_open(path.c_str(), &dev);
        }

        if (st != MEDIUS_STATUS_OK) {
            Fail(gen, "Could not open Medius device");
            return false;
        }

        MediusVersion ver;
        if (medius_device_query_version(dev, &ver) == MEDIUS_STATUS_OK) {
            Log("connected generation=%llu fw=%u.%u.%u proto=%u name=%s",
                gen, ver.fw_major, ver.fw_minor, ver.fw_patch, ver.proto_ver, ver.name);
        }

        // Subscribe to button events for readback
        MediusCatchFilter filters[1];
        filters[0] = medius_catch_filter_watch_class(MEDIUS_CLASS_BUTTON);
        if (medius_device_catch_events(dev, filters, 1, &eventStream) != MEDIUS_STATUS_OK) {
            Log("catch subscription failed, button readback unavailable");
            eventStream = nullptr;
        }

        // Initial axis unlock
        medius_device_unlock_all(dev, MEDIUS_BLANKET_AIM, MEDIUS_DIRECTION_BOTH);

        { std::lock_guard<std::mutex> lock(mutex);
          if (!Current(gen)) return false;
          error[0] = 0; state = State::Connected;
        }
        return true;
    }

    void PollEvents() {
        if (!eventStream) return;
        MediusCatchEvent ev;
        while (medius_event_stream_try_recv(eventStream, &ev)) {
            if (ev.kind == MEDIUS_CATCH_EVENT_KIND_USAGES) {
                unsigned mask = 0x100u;
                for (uint16_t i = 0; i < ev.data.usages.n; ++i) {
                    const MediusUsage& u = ev.data.usages.usages[i];
                    if (u.kind == MEDIUS_CLASS_BUTTON && u.id < 5)
                        mask |= 1u << u.id;
                }
                buttons.store(mask);
            }
        }
    }

    void Run() {
        uint64_t active = 0;
        while (!stop) {
            uint64_t gen; char selected[64]{}; bool wanted;
            { std::unique_lock<std::mutex> lock(mutex);
              if (state != State::Connected && generation.load() == active)
                  wake.wait(lock, [&] { return stop || generation.load() != active; });
              else wake.wait_for(lock, std::chrono::milliseconds(2), [&] {
                  return stop || generation.load() != active;
              });
              if (stop) break;
              gen = generation; wanted = wantConnection;
              memcpy(selected, port, sizeof(selected));
            }
            if (gen != active) {
                Close(); active = gen;
                if (!wanted) continue;
                try { if (!Open(gen, selected)) { Close(); continue; } }
                catch (...) { Fail(gen, "Medius connection exception"); Close(); continue; }
            }
            if (!Current(gen) || state != State::Connected) continue;

            // Axis lease management
            const auto leaseNow = GetTickCount64();
            const bool wantAxes = axisLease[0].load() > leaseNow || axisLease[1].load() > leaseNow;
            if (wantAxes != axesLocked) {
                axesLocked = wantAxes;
                if (wantAxes)
                    medius_device_lock_all(dev, MEDIUS_BLANKET_AIM, MEDIUS_DIRECTION_BOTH);
                else
                    medius_device_unlock_all(dev, MEDIUS_BLANKET_AIM, MEDIUS_DIRECTION_BOTH);
            }

            PollEvents();
        }
        Close();
    }
};
Transport transport;
}

void Connect(const char* port) {
    std::lock_guard<std::mutex> lock(transport.mutex);
    transport.buttons = 0;
    if (transport.testPending.exchange(false)) transport.testResult = -1;
    snprintf(transport.port, sizeof(transport.port), "%s", port ? port : "");
    transport.wantConnection = true;
    transport.error[0] = 0; transport.state = State::Connecting;
    ++transport.generation;
    if (!transport.worker.joinable()) {
        transport.stop = false;
        try { transport.worker = std::thread([] { transport.Run(); }); }
        catch (...) { transport.state = State::Failed; snprintf(transport.error, sizeof(transport.error), "Could not start Medius worker"); }
    }
    transport.wake.notify_all();
}
void Disconnect() {
    std::lock_guard<std::mutex> lock(transport.mutex);
    transport.wantConnection = false; ++transport.generation;
    transport.state = State::Disconnected; transport.buttons = 0;
    if (transport.testPending.exchange(false)) transport.testResult = -1;
    transport.wake.notify_all();
}
void Shutdown() { Disconnect(); transport.Stop(); }
State GetState() { return transport.state; }
void SetAxisLock(bool locked, unsigned source) {
    if (source > 1) return;
    transport.axisLease[source] = locked && GetState() == State::Connected
        ? GetTickCount64() + 150 : 0;
    transport.wake.notify_all();
}
void GetError(char* out, size_t size) {
    if (!out || !size) return;
    std::lock_guard<std::mutex> lock(transport.mutex);
    snprintf(out, size, "%s", transport.error);
}
bool Move(int x, int y, bool recoil, bool test) {
    (void)recoil;
    if (GetState() != State::Connected || !transport.dev) return false;
    const int16_t dx = static_cast<int16_t>(std::clamp(x, -32768, 32767));
    const int16_t dy = static_cast<int16_t>(std::clamp(y, -32768, 32767));
    const bool ok = medius_device_move_rel(transport.dev, dx, dy) == MEDIUS_STATUS_OK;
    if (test) {
        transport.testResult = ok ? 1 : -1;
        transport.testPending = false;
    }
    return ok;
}
bool InjectButton(uint8_t id, bool down) {
    if (id > 4 || GetState() != State::Connected || !transport.dev) return false;
    MediusUsage usage = medius_usage_button(static_cast<MediusButton>(id));
    if (down)
        return medius_device_press(transport.dev, usage) == MEDIUS_STATUS_OK;
    else
        return medius_device_soft_release(transport.dev, usage) == MEDIUS_STATUS_OK;
}
bool InjectKey(unsigned hid, bool down) {
    if (!hid || hid > 0xE7 || GetState() != State::Connected || !transport.dev) return false;
    MediusUsage usage = medius_usage_key(static_cast<MediusKey>(hid));
    if (down)
        return medius_device_press(transport.dev, usage) == MEDIUS_STATUS_OK;
    else
        return medius_device_soft_release(transport.dev, usage) == MEDIUS_STATUS_OK;
}
bool MouseDown(int vk, bool& down) {
    if (GetState() != State::Connected) return false;
    const unsigned value = transport.buttons;
    if (!(value & 0x100)) return false;
    const int bit = vk == VK_LBUTTON ? 0 : vk == VK_RBUTTON ? 1 : vk == VK_MBUTTON ? 2
        : vk == VK_XBUTTON1 ? 3 : vk == VK_XBUTTON2 ? 4 : -1;
    if (bit < 0) return false;
    down = (value & (1u << bit)) != 0; return true;
}
void ReportAim(int state) {
    transport.aim = state;
}
int TakeTestResult() { return transport.testResult.exchange(0); }
} // namespace Medius
