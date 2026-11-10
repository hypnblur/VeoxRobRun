#pragma once
#include "AimMotion.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <array>

namespace Base::Hardware {
using Clock = std::chrono::steady_clock;
enum class DeviceKind : int { Off = 0, KmboxUsb = 3, KmboxNet = 4, Makcu = 5, Controller = 6, Ferrum = 7, Medius = 8, KmboxNbPro = 9 };
inline constexpr bool IsNetworkDevice(DeviceKind kind) {
    return kind == DeviceKind::KmboxNet || kind == DeviceKind::KmboxNbPro;
}
inline constexpr bool IsMakcu(DeviceKind kind) {
    return kind == DeviceKind::Makcu || kind == DeviceKind::Controller;
}
enum class LinkState { Disconnected, Connecting, Ready, Recovering, NeedsConfiguration };
enum class OutputResult { None, Accepted, Written, Expired, Cancelled, Failed, Unconfirmed };
enum class Source : unsigned { Aim, Recoil, Mortar, Preview, Count };
struct DeviceConfig {
    DeviceKind kind = DeviceKind::Off;
    std::string port, ip, networkPort = "8899", mac;
    int baud = 115200;
    bool manualPort = false;
    bool ferrumController = false;
    bool operator==(const DeviceConfig&) const = default;
};
inline bool UsesController(const DeviceConfig& c) {
    return c.kind == DeviceKind::Controller || (c.kind == DeviceKind::Ferrum && c.ferrumController);
}
struct PortInfo { std::string port, name, identity; };
struct PhysicalInput {
    unsigned down = 0, pressed = 0;
    bool valid = false;
    std::uint16_t controllerButtons = 0;
    std::uint8_t lt = 0, rt = 0;
    bool controllerValid = false;
    std::int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    std::uint32_t controllerPressed = 0;
    std::array<std::uint64_t, 18> controllerPressSequence{};
};
struct Status {
    LinkState state = LinkState::Disconnected;
    DeviceKind kind = DeviceKind::Off;
    std::uint64_t generation = 0, writes = 0, failures = 0, expired = 0;
    std::string endpoint, detail, error;
    bool protocolVerified = false;
    PhysicalInput input;
    unsigned retryInMs = 0;
    OutputResult test = OutputResult::None;
};
struct MotionIntent {
    Base::AimMotion::Vec2 value{};
    Base::AimMotion::Settings settings;
    std::uint64_t target = 0, observation = 0;
    Clock::time_point expires{};
    int intervalMs = 8;
    float stickScale = 1;
    bool error = false, rate = false, axisLock = false;
};
struct Control { unsigned id = 0; bool keyboard = false, down = false; };
class DeviceBackend {
public:
    virtual ~DeviceBackend() = default;
    virtual bool Open(const DeviceConfig&, const std::function<bool()>& cancelled) = 0;
    virtual void Close() = 0;
    virtual bool Poll() = 0;
    virtual bool Move(int x, int y, bool stick) = 0;
    virtual bool TestMove(int x, int y, bool stick) { return Move(x, y, stick); }
    virtual bool TestMoveAcknowledged() const { return true; }
    virtual bool SetControl(Control) = 0;
    virtual bool AxisLock(bool) = 0;
    virtual bool Neutralize() = 0;
    virtual PhysicalInput Input() = 0;
    virtual std::string Error() const = 0;
    virtual std::string Endpoint() const = 0;
    virtual bool Verified() const = 0;
    virtual std::vector<PortInfo> Ports() = 0;
};
std::unique_ptr<DeviceBackend> MakeDeviceBackend();
class DeviceSession {
public:
    explicit DeviceSession(std::unique_ptr<DeviceBackend>);
    ~DeviceSession();
    DeviceSession(const DeviceSession&) = delete;
    DeviceSession& operator=(const DeviceSession&) = delete;
    bool Connect(const DeviceConfig&);
    void Disconnect();
    void Shutdown();
    void Neutralize();
    bool Submit(Source, const MotionIntent&);
    void Cancel(Source);
    bool Button(Control, DeviceKind owner);
    bool TestMovement();
    Status GetStatus() const;
    PhysicalInput TakeInput();
    void RefreshPorts();
    std::vector<PortInfo> GetPorts() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
DeviceSession& Session();
bool Ready(int device = -1);
bool MouseDown(int vk, bool& down);
const char* StateLabel(LinkState);
const char* DeviceLabel(DeviceKind);
}
