#include "pch.h"
#include "DeviceSession.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cctype>
#include <utility>

namespace Base::Hardware {
namespace {
bool ValidIpv4(const std::string& value) {
    unsigned parts = 0, digits = 0, number = 0;
    for (char ch : value + '.') {
        if (ch == '.') {
            if (!digits || number > 255 || ++parts > 4) return false;
            digits = number = 0;
        } else if (ch >= '0' && ch <= '9' && ++digits <= 3) number = number * 10 + unsigned(ch - '0');
        else return false;
    }
    return parts == 4;
}
std::string Validate(const DeviceConfig& c) {
    const int kind = int(c.kind);
    if (kind < 3 || kind > 9) return "Choose a hardware device.";
    if (IsNetworkDevice(c.kind)) {
        if (!ValidIpv4(c.ip) || c.mac.empty() || c.mac.size() > 8
            || c.mac.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            return "Enter an IPv4 address and up to eight hexadecimal device-ID digits.";
        try { size_t n = 0; int p = std::stoi(c.networkPort, &n);
            if (n != c.networkPort.size() || p < 1 || p > 65535) return "Network port must be 1-65535.";
        } catch (...) { return "Network port must be 1-65535."; }
    } else {
        if (c.kind == DeviceKind::Controller && c.baud != 115200 && c.baud != 4000000)
            return "MAKCU V4 controller mode supports 115200 or 4000000 baud.";
        if (c.kind != DeviceKind::Medius && (c.baud < (IsMakcu(c.kind) ? 115200 : 9600)
            || c.baud > (IsMakcu(c.kind) ? 4000000 : 6000000)))
            return "Unsupported serial baud for the selected device.";
        if (c.manualPort || c.kind == DeviceKind::Ferrum) {
            std::string p = c.port;
            for (char& ch : p) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            if (p.size() < 4 || p.substr(0, 3) != "COM" || p[3] == '0'
                || p.find_first_not_of("0123456789", 3) != std::string::npos)
                return "Select the device COM port (Ferrum uses its virtual endpoint).";
        }
    }
    return {};
}
constexpr size_t sourceCount = size_t(Source::Count);
struct Slot { MotionIntent intent; bool active = false; std::uint64_t revision = 0; };
struct QueuedControl { Control value; Clock::time_point expires; };
struct ControlLease { bool held = false; Clock::time_point expires, renewed; };
}
struct DeviceSession::Impl {
    std::unique_ptr<DeviceBackend> backend;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> generation{0}, outputEpoch{0};
    DeviceConfig config;
    Status status;
    bool wanted = false, neutralize = false, refresh = true;
    std::array<Slot, sourceCount> slots{};
    std::deque<QueuedControl> controls;
    std::array<ControlLease, 237> controlLeases{};
    std::vector<PortInfo> ports;
    explicit Impl(std::unique_ptr<DeviceBackend> b) : backend(std::move(b)) {}
    void Start() { if (!worker.joinable()) { stop = false; worker = std::thread([this] { Run(); }); } }
    void ClearOutput() {
        for (auto& s : slots) { s.active = false; ++s.revision; }
        controls.clear(); controlLeases = {}; ++outputEpoch;
        if (status.test == OutputResult::Accepted) status.test = OutputResult::Cancelled;
    }
    void Run() {
        std::uint64_t active = 0, epoch = 0;
        bool opened = false, locked = false;
        unsigned attempts = 0;
        auto retry = Clock::time_point{};
        std::array<AimMotion::Controller, sourceCount> motion{};
        std::array<Clock::time_point, sourceCount> last{};
        std::array<std::uint64_t, sourceCount> revisions{};
        std::array<std::uint64_t, sourceCount> oneShots{};
        std::array<AimMotion::Vec2, sourceCount> stickCommands{};
        unsigned stickSources = 0;
        AimMotion::Quantizer combinedQuantizer;
        while (!stop) {
            DeviceConfig c; bool want, clear, enumerate;
            std::uint64_t gen;
            {
                std::unique_lock lock(mutex);
                if (!wanted && !refresh && active == generation)
                    wake.wait(lock, [&] { return stop || wanted || refresh || active != generation; });
                else wake.wait_for(lock, std::chrono::milliseconds(2));
                if (stop) break;
                c = config; want = wanted; gen = generation;
                clear = std::exchange(neutralize, false);
                enumerate = std::exchange(refresh, false);
            }
            if (gen != active) {
                backend->Close(); opened = locked = false;
                active = gen; attempts = 0; retry = {}; motion = {}; last = {};
                combinedQuantizer.Reset();
                stickCommands = {}; stickSources = 0;
            }
            if (enumerate) {
                auto found = backend->Ports();
                std::lock_guard lock(mutex); ports = std::move(found);
            }
            const auto current = [&] { return !stop && generation == gen; };
            if (!want || !current()) continue;
            auto now = Clock::now();
            if (!opened && now >= retry) {
                { std::lock_guard lock(mutex);
                  if (!current()) continue;
                  status.state = LinkState::Connecting; status.retryInMs = 0;
                }
                bool ok = false;
                try { ok = backend->Open(c, [&] { return !current(); }); }
                catch (...) { ok = false; }
                if (!current()) { backend->Close(); continue; }
                opened = ok;
                {
                    std::lock_guard lock(mutex);
                    if (!current()) continue;
                    status.endpoint = backend->Endpoint();
                    status.protocolVerified = ok && backend->Verified();
                    status.input = {};
                    if (ok) {
                        status.state = LinkState::Ready; status.error.clear();
                        status.detail = status.protocolVerified ? "Protocol verified" : "Port open; device reply verification unavailable";
                        attempts = 0; motion = {}; last = {};
                    } else {
                        status.state = LinkState::Recovering;
                        status.error = backend->Error();
                        if (status.error.empty()) status.error = "Device open or handshake failed.";
                        ++status.failures;
                        const unsigned delay = (std::min)(3000u, 500u << (std::min)(attempts++, 3u));
                        retry = Clock::now() + std::chrono::milliseconds(delay);
                        status.retryInMs = delay;
                    }
                }
                if (!ok) backend->Close();
                continue;
            }
            if (!opened) {
                std::lock_guard lock(mutex);
                status.retryInMs = unsigned(std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(retry - now).count()));
                continue;
            }
            bool ok = true;
            if (clear || epoch != outputEpoch.load()) {
                epoch = outputEpoch; motion = {}; last = {}; combinedQuantizer.Reset();
                ok = backend->Neutralize(); locked = false;
                stickCommands = {}; stickSources = 0;
            }
            std::array<Slot, sourceCount> local;
            Control control{}; bool haveControl = false, test = false;
            std::vector<Control> releases;
            {
                std::lock_guard lock(mutex);
                local = slots;
                for (size_t i = 0; i < controlLeases.size(); ++i) {
                    auto& lease = controlLeases[i];
                    if (lease.held && Clock::now() >= lease.expires) {
                        lease.held = false;
                        releases.push_back({unsigned(i < 5 ? i : i - 5), i >= 5, false});
                    }
                }
                while (!controls.empty()) {
                    const auto item = controls.front(); controls.pop_front();
                    if (item.value.down && Clock::now() >= item.expires) continue;
                    control = item.value; haveControl = true; break;
                }
                test = status.test == OutputResult::Accepted;
            }
            const auto outputCurrent = [&] { return current() && epoch == outputEpoch.load(); };
            for (const auto release : releases)
                if (ok && outputCurrent()) ok = backend->SetControl(release);
            if (ok && haveControl && outputCurrent()) ok = backend->SetControl(control);
            if (ok && test && outputCurrent()) {
                ok = backend->Neutralize() && backend->TestMove(UsesController(c) ? 5000 : 25, 0, UsesController(c));
                if (ok && UsesController(c)) ok = backend->Move(0, 0, true);
                std::lock_guard lock(mutex);
                if (outputCurrent() && status.test == OutputResult::Accepted)
                    status.test = !ok ? OutputResult::Failed
                        : backend->TestMoveAcknowledged() ? OutputResult::Written : OutputResult::Unconfirmed;
            }
            now = Clock::now();
            for (size_t i = 0; i < sourceCount; ++i) {
                if (local[i].revision != revisions[i]) { motion[i].Reset(); last[i] = {}; oneShots[i] = 0; stickCommands[i] = {}; combinedQuantizer.Reset(); revisions[i] = local[i].revision; }
                if (local[i].active && now >= local[i].intent.expires) {
                    local[i].active = false;
                    std::lock_guard lock(mutex);
                    if (slots[i].active && slots[i].intent.expires <= now) { slots[i].active = false; ++status.expired; }
                }
                if (!local[i].active) { motion[i].Reset(); last[i] = {}; oneShots[i] = 0; stickCommands[i] = {}; }
            }
            const int exclusive = local[size_t(Source::Preview)].active ? int(Source::Preview)
                : local[size_t(Source::Mortar)].active ? int(Source::Mortar) : -1;
            AimMotion::Vec2 sum{};
            const bool stick = UsesController(c);
            unsigned activeSources = 0;
            bool axes = false, any = false;
            std::array<AimMotion::Vec2, sourceCount> contributions{};
            std::array<bool, sourceCount> used{};
            for (size_t i = 0; i < sourceCount && !test; ++i) {
                if (!local[i].active || (exclusive >= 0 && int(i) != exclusive)) continue;
                const auto& in = local[i].intent;
                any = true; axes |= in.axisLock;
                activeSources |= 1u << i;
                if (!in.error && !in.rate && oneShots[i] == in.observation) continue;
                const int interval = std::clamp(in.intervalMs, 1, 100);
                if (last[i] != Clock::time_point{} && now - last[i] < std::chrono::milliseconds(interval)) {
                    if (stick) { sum.x += stickCommands[i].x; sum.y += stickCommands[i].y; }
                    continue;
                }
                float dt = last[i] == Clock::time_point{} ? float(interval) / 1000
                    : (std::min)(0.05f, std::chrono::duration<float>(now - last[i]).count());
                last[i] = now;
                AimMotion::Vec2 v = in.value;
                if (in.error) {
                    motion[i].Observe(in.target, in.observation, in.value);
                    v = AimMotion::ApplyVariation(motion[i].variation, motion[i].target,
                        motion[i].remaining, AimMotion::Step(motion[i].remaining, in.settings, dt),
                        in.settings);
                } else if (in.rate) { v.x *= dt; v.y *= dt; }
                if (stick) { v.x *= in.stickScale; v.y *= in.stickScale; stickCommands[i] = v; }
                contributions[i] = v; used[i] = true;
                sum.x += v.x; sum.y += v.y;
            }
            if (!any) combinedQuantizer.Reset();
            const float limit = stick ? 32767.f : 127.f;
            const AimMotion::Vec2 scale{std::fabs(sum.x) > limit ? limit / std::fabs(sum.x) : 1.f,
                std::fabs(sum.y) > limit ? limit / std::fabs(sum.y) : 1.f};
            const bool updated = std::any_of(used.begin(), used.end(), [](bool value) { return value; });
            bool valid = outputCurrent();
            { std::lock_guard lock(mutex);
              for (size_t i = 0; i < sourceCount; ++i)
                  if (local[i].active && slots[i].revision != local[i].revision) valid = false;
            }
            if (ok && valid) {
                const auto counts = stick ? AimMotion::Vec2{std::round(sum.x * scale.x), std::round(sum.y * scale.y)}
                    : combinedQuantizer.Round({sum.x * scale.x, sum.y * scale.y}, int(limit));
                if (axes != locked) { ok = backend->AxisLock(axes); locked = axes; }
                const bool write = stick ? updated || activeSources != stickSources : counts.x || counts.y;
                if (ok && outputCurrent() && write) {
                    ok = backend->Move(int(counts.x), int(counts.y), stick);
                    if (ok && stick) stickSources = activeSources;
                    if (ok) {
                        std::lock_guard lock(mutex); ++status.writes;
                    }
                }
                if (ok && outputCurrent() && !stick)
                    for (size_t i = 0; i < sourceCount; ++i)
                        if (used[i] && local[i].intent.error)
                            motion[i].Written({contributions[i].x * scale.x,
                                contributions[i].y * scale.y}, local[i].intent.settings);
                if (ok && outputCurrent())
                    for (size_t i = 0; i < sourceCount; ++i)
                        if (used[i] && !local[i].intent.error && !local[i].intent.rate)
                            oneShots[i] = local[i].intent.observation;
            }
            if (ok && current()) ok = backend->Poll();
            if (!current()) continue;
            if (!ok) {
                const auto error = backend->Error();
                backend->Close(); opened = locked = false;
                std::lock_guard lock(mutex);
                if (!current()) continue;
                status.state = LinkState::Recovering; status.error = error;
                if (status.error.empty()) status.error = "Device I/O failed; reconnecting.";
                ++status.failures; status.input = {}; status.protocolVerified = false;
                ClearOutput(); retry = Clock::now() + std::chrono::milliseconds(500); attempts = 1;
            } else {
                const auto input = backend->Input();
                std::lock_guard lock(mutex);
                if (current()) {
                    const auto pressed = status.input.pressed | input.pressed;
                    const auto controllerPressed = input.controllerValid
                        ? status.input.controllerPressed | input.controllerPressed : 0;
                    status.input = input; status.input.pressed = pressed;
                    status.input.controllerPressed = controllerPressed;
                }
            }
        }
        backend->Close();
    }
};
DeviceSession::DeviceSession(std::unique_ptr<DeviceBackend> b) : impl(std::make_unique<Impl>(std::move(b))) {}
DeviceSession::~DeviceSession() { Shutdown(); }
bool DeviceSession::Connect(const DeviceConfig& c) {
    const auto error = Validate(c);
    std::lock_guard lock(impl->mutex);
    if (error.empty() && impl->wanted && impl->config == c) return true;
    impl->ClearOutput(); impl->config = c; impl->wanted = error.empty();
    impl->status = {}; impl->status.kind = c.kind; impl->status.generation = ++impl->generation;
    impl->status.state = error.empty() ? LinkState::Connecting : LinkState::NeedsConfiguration;
    impl->status.error = error; impl->Start(); impl->wake.notify_all(); return error.empty();
}
void DeviceSession::Disconnect() {
    std::lock_guard lock(impl->mutex);
    impl->wanted = false; impl->ClearOutput(); ++impl->generation;
    impl->status = {}; impl->status.generation = impl->generation; impl->wake.notify_all();
}
void DeviceSession::Shutdown() {
    Disconnect(); impl->stop = true; impl->wake.notify_all();
    if (impl->worker.joinable()) impl->worker.join();
}
void DeviceSession::Neutralize() {
    std::lock_guard lock(impl->mutex); impl->ClearOutput(); impl->neutralize = true; impl->wake.notify_all();
}
bool DeviceSession::Submit(Source source, const MotionIntent& in) {
    if (size_t(source) >= sourceCount || !AimMotion::Finite(in.value) || in.expires <= Clock::now()
        || !std::isfinite(in.stickScale) || in.stickScale <= 0) return false;
    std::lock_guard lock(impl->mutex);
    if (impl->status.state != LinkState::Ready || impl->status.test == OutputResult::Accepted) return false;
    auto& slot = impl->slots[size_t(source)];
    if (!slot.active || slot.intent.target != in.target || slot.intent.error != in.error || slot.intent.rate != in.rate) ++slot.revision;
    slot.intent = in;
    slot.intent.expires = (std::min)(in.expires, Clock::now() + std::chrono::milliseconds(50));
    slot.active = true; impl->wake.notify_all(); return true;
}
void DeviceSession::Cancel(Source source) {
    if (size_t(source) >= sourceCount) return;
    std::lock_guard lock(impl->mutex); auto& slot = impl->slots[size_t(source)];
    if (slot.active) { slot.active = false; ++slot.revision; impl->wake.notify_all(); }
}
bool DeviceSession::Button(Control c, DeviceKind owner) {
    if ((c.keyboard && (!c.id || c.id > 0xE7)) || (!c.keyboard && c.id > 4)) return false;
    std::lock_guard lock(impl->mutex);
    if (impl->status.state != LinkState::Ready || impl->config.kind != owner
        || impl->status.test == OutputResult::Accepted) return false;
    auto& lease = impl->controlLeases[c.keyboard ? c.id + 5 : c.id];
    const auto expires = Clock::now() + std::chrono::milliseconds(50);
    if (c.down && lease.held) {
        lease.expires = expires;
        if (!(impl->config.kind == DeviceKind::Ferrum && impl->config.ferrumController)
            || Clock::now() - lease.renewed < std::chrono::milliseconds(25)) return true;
    }
    if (impl->controls.size() >= 64) { impl->ClearOutput(); impl->neutralize = true; return false; }
    lease = {c.down, expires, Clock::now()};
    impl->controls.push_back({c, expires}); impl->wake.notify_all(); return true;
}
bool DeviceSession::TestMovement() {
    std::lock_guard lock(impl->mutex);
    if (impl->status.state != LinkState::Ready || impl->status.test == OutputResult::Accepted) return false;
    impl->ClearOutput(); impl->status.test = OutputResult::Accepted; impl->wake.notify_all(); return true;
}
Status DeviceSession::GetStatus() const { std::lock_guard lock(impl->mutex); return impl->status; }
PhysicalInput DeviceSession::TakeInput() {
    std::lock_guard lock(impl->mutex); const auto input = impl->status.input;
    impl->status.input.pressed = 0; impl->status.input.controllerPressed = 0; return input;
}
void DeviceSession::RefreshPorts() { std::lock_guard lock(impl->mutex); impl->refresh = true; impl->Start(); impl->wake.notify_all(); }
std::vector<PortInfo> DeviceSession::GetPorts() const { std::lock_guard lock(impl->mutex); return impl->ports; }
DeviceSession& Session() { static DeviceSession session(MakeDeviceBackend()); return session; }
bool Ready(int device) { const auto s = Session().GetStatus(); return s.state == LinkState::Ready && (device < 0 || int(s.kind) == device); }
bool MouseDown(int vk, bool& down) {
    const auto s = Session().GetStatus();
    const int bit = vk == 1 ? 0 : vk == 2 ? 1 : vk == 4 ? 2 : vk == 5 ? 3 : vk == 6 ? 4 : -1;
    if (s.state != LinkState::Ready || !s.input.valid || bit < 0) return false;
    down = (s.input.down & (1u << bit)) != 0; return true;
}
const char* StateLabel(LinkState s) {
    switch (s) { case LinkState::Connecting: return "Connecting"; case LinkState::Ready: return "Connected";
    case LinkState::Recovering: return "Reconnecting"; case LinkState::NeedsConfiguration: return "Check settings"; default: return "Disconnected"; }
}
const char* DeviceLabel(DeviceKind d) {
    switch (d) { case DeviceKind::KmboxUsb: return "Kmbox USB"; case DeviceKind::KmboxNet: return "Kmbox Net";
    case DeviceKind::KmboxNbPro: return "Kmbox NB Pro";
    case DeviceKind::Makcu: return "Macku"; case DeviceKind::Controller: return "Macku Controller";
    case DeviceKind::Ferrum: return "Ferrum"; case DeviceKind::Medius: return "Macku Medius"; default: return "Off"; }
}
}
