#include "pch.h"
#include "DeviceSession.h"
#include "MakcuProtocol.h"
#include "MediusProtocol.h"
#include "FerrumProtocol.h"
#include "HidTable.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <thread>
#include <cctype>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "setupapi.lib")

namespace Base::Hardware {
namespace {
class Backend final : public DeviceBackend {
    DeviceConfig config;
    HANDLE serial = INVALID_HANDLE_VALUE;
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in address{};
    bool winsock = false, verified = false, initialized = false;
    std::string endpoint, error, lastIdentity;
    DeviceKind lastKind = DeviceKind::Off;
    int lastBaud = 115200;
    std::function<bool()> cancelled;
    Makcu::Parser makcu;
    Medius::Decoder medius;
    PhysicalInput input;
    Makcu::ControllerState controllerOutput;
    bool controllerResubscribe = false, controllerBaseline = false;
    bool controllerTextSnapshots = false;
    uint32_t controllerFirmware = 0;
    uint8_t controllerKinds = 0;
    std::string line;
    std::array<bool, 232> injectedKeys{};
    std::uint8_t sequence = 0, proto = 0;
    std::uint32_t netSequence = 0, netMac = 0;
    struct Pending { std::uint32_t sequence = 0, command = 0; bool valid = false; };
    std::array<Pending, 256> pending{};
    soft_mouse_t mouse{};
    soft_keyboard_t keyboard{};
    Clock::time_point lastReply{}, heartbeat{}, controllerPoll{}, controllerReply{};
    bool netHandshake = false, testAcknowledged = false;
    bool Fail(const char* reason, unsigned code = 0) {
        error = reason;
        if (code) error += " (OS error " + std::to_string(code) + ")";
        return false;
    }
    bool Pause(unsigned ms) {
        const auto until = Clock::now() + std::chrono::milliseconds(ms);
        while (!cancelled() && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return !cancelled();
    }
    bool Write(const void* data, size_t len) {
        DWORD count = 0;
        if (!WriteFile(serial, data, DWORD(len), &count, nullptr)) return Fail("Serial write failed", GetLastError());
        if (count != len) return Fail("Short serial write; command delivery is uncertain", ERROR_WRITE_FAULT);
        return true;
    }
    bool Text(const char* s) { return Write(s, std::strlen(s)); }
    bool MakFrame(uint8_t opcode, const uint8_t* payload = nullptr, size_t n = 0) {
        std::array<uint8_t, 37> frame{0xDE, 0xAD, uint8_t(n), uint8_t(n >> 8), opcode};
        if (n > frame.size() - 5) return Fail("MAKCU frame too large");
        if (n) std::memcpy(frame.data() + 5, payload, n);
        return Write(frame.data(), n + 5);
    }
    bool ControllerOutput() {
        const auto& s = controllerOutput;
        uint8_t payload[20]{};
        const auto put = [&](unsigned offset, uint32_t value, unsigned count) {
            for (unsigned i = 0; i < count; ++i) payload[offset + i] = uint8_t(value >> (8 * i));
        };
        put(0, s.low, 4); put(4, s.high, 4); put(8, s.lt, 2); put(10, s.rt, 2);
        put(12, uint16_t(s.lx), 2); put(14, uint16_t(s.ly), 2);
        put(16, uint16_t(s.rx), 2); put(18, uint16_t(s.ry), 2);
        return MakFrame(0x40, payload, sizeof(payload));
    }
    bool Frame(std::uint8_t type, const std::uint8_t* payload, size_t n) {
        std::uint8_t frame[7 + Medius::kMaxPayload];
        const auto len = Medius::EncodeFrame(type, sequence++, payload, n, frame, sizeof(frame));
        return len && Write(frame, len);
    }
    bool Configure(int baud) {
        DCB dcb{}; dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(serial, &dcb)) return Fail("Cannot read serial settings", GetLastError());
        dcb.BaudRate = baud; dcb.ByteSize = 8; dcb.StopBits = ONESTOPBIT; dcb.Parity = NOPARITY;
        dcb.fBinary = TRUE; dcb.fParity = FALSE; dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE; dcb.fDsrSensitivity = FALSE; dcb.fTXContinueOnXoff = FALSE;
        dcb.fOutX = FALSE; dcb.fInX = FALSE; dcb.fErrorChar = FALSE; dcb.fNull = FALSE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE; dcb.fAbortOnError = FALSE;
        return SetCommState(serial, &dcb) ? true : Fail("Cannot configure serial baud", GetLastError());
    }
    void Buttons(unsigned mask) {
        input.pressed |= mask & ~input.down; input.down = mask; input.valid = true;
    }
    void ControllerSample(uint16_t buttons, uint8_t lt, uint8_t rt, bool detectPress = true) {
        const auto previous = input.controllerValid ? CtlInputBits(input.controllerButtons, input.lt, input.rt) : 0;
        const auto pressed = detectPress ? CtlInputBits(buttons, lt, rt) & ~previous : 0;
        input.controllerPressed |= pressed;
        for (unsigned i = 0; i < input.controllerPressSequence.size(); ++i)
            if (pressed & (1u << i)) ++input.controllerPressSequence[i];
        input.controllerButtons = buttons; input.lt = lt; input.rt = rt;
        input.controllerValid = true;
    }
    void ControllerPhysical(const Makcu::ControllerState& state) {
        ControllerSample(Makcu::ControllerButtons(state.low), Makcu::Trigger(state.lt), Makcu::Trigger(state.rt),
            !controllerBaseline);
        controllerBaseline = false;
        input.lx = state.lx; input.ly = state.ly; input.rx = state.rx; input.ry = state.ry;
        controllerReply = lastReply = Clock::now();
    }
    void InvalidateController() {
        input.controllerValid = false; input.controllerButtons = 0; input.lt = input.rt = 0;
        input.lx = input.ly = input.rx = input.ry = 0; input.controllerPressed = 0;
        controllerBaseline = true;
    }
    bool MakEvent() {
        const auto n = makcu.frameLength;
        const auto* p = makcu.payload.data();
        if (config.kind == DeviceKind::Controller && makcu.opcode == 0x04 && n == 4)
            controllerFirmware = Makcu::Read32(p);
        if (config.kind == DeviceKind::Controller && makcu.opcode == 0x02 && n == 1 && p[0] != 0xFF)
            controllerKinds = p[0];
        if (makcu.opcode == 0x54 && config.kind == DeviceKind::Controller) {
            Makcu::ControllerState state;
            if (Makcu::DecodePhysical(p, n, state)) ControllerPhysical(state);
            else {
                InvalidateController();
                if (!(n == 1 && p[0] == 0xFF)) {
                    controllerTextSnapshots = true; controllerPoll = {};
                }
            }
        } else if (n == 1 && p[0] == 0xFF && config.kind == DeviceKind::Controller
            && (makcu.opcode == 0x40 || makcu.opcode == 0x51 || makcu.opcode == 0x41)) {
            return Fail("MAKCU rejected controller output; check the routed controller and V4 firmware");
        } else if (makcu.opcode == 0x53 && n >= 3) {
            if (p[1] == 0xFF && p[2] == 0xFF && n == 3) {
                if (p[0] == 3 && config.kind == DeviceKind::Controller) {
                    InvalidateController(); controllerResubscribe = true;
                }
                return true;
            }
            if (p[0] == 1 && n == 3 && p[1] < 5 && p[2] <= 1) {
                const auto bit = 1u << p[1];
                Buttons(p[2] ? input.down | bit : input.down & ~bit);
            } else if (p[0] == 3 && config.kind == DeviceKind::Controller && input.controllerValid) {
                auto buttons = input.controllerButtons; auto lt = input.lt, rt = input.rt;
                const auto bit = Makcu::ControllerButton(p[1]);
                if (n == 3 && bit && p[2] <= 1) buttons = p[2] ? buttons | bit : buttons & ~bit;
                else if (n == 4 && (p[1] == 10 || p[1] == 11) && Makcu::Read16(p + 2) <= 1023) {
                    (p[1] == 10 ? lt : rt) = Makcu::Trigger(Makcu::Read16(p + 2));
                } else return true;
                ControllerSample(buttons, lt, rt);
            }
        }
        return true;
    }
    bool ReadSerial() {
        DWORD errors = 0; COMSTAT status{};
        if (!ClearCommError(serial, &errors, &status)) return Fail("Serial link unavailable", GetLastError());
        if (errors & (CE_BREAK | CE_FRAME | CE_RXPARITY | CE_OVERRUN | CE_RXOVER)) {
            input = {}; return Fail("Serial receive corruption; resetting the session", errors);
        }
        for (int batch = 0; batch < 8 && status.cbInQue; ++batch) {
            unsigned char bytes[512]; DWORD count = 0;
            if (!ReadFile(serial, bytes, std::min<DWORD>(status.cbInQue, sizeof(bytes)), &count, nullptr))
                return Fail("Serial read failed", GetLastError());
            if (!count) break;
            status.cbInQue -= (std::min)(status.cbInQue, count);
            for (DWORD i = 0; i < count; ++i) {
                if (IsMakcu(config.kind)) {
                    const auto event = makcu.Feed(bytes[i]);
                    if (event == Makcu::Parser::Event::Version) lastReply = Clock::now();
                    if (event == Makcu::Parser::Event::Buttons) Buttons(makcu.mask);
                    if (event == Makcu::Parser::Event::Physical && config.kind == DeviceKind::Controller)
                        ControllerPhysical(makcu.controller);
                    if (event == Makcu::Parser::Event::Frame && !MakEvent()) return false;
                } else if (config.kind == DeviceKind::Medius) {
                    uint8_t type, seq; const uint8_t* payload; size_t n;
                    if (!medius.Feed(bytes[i], type, seq, payload, n)) continue;
                    if (type == Medius::kTypeResp && n >= 1) {
                        if (payload[0] == Medius::kQueryVersion && n >= 11) { proto = payload[1]; lastReply = Clock::now(); }
                        else if (payload[0] == Medius::kQueryHealth) lastReply = Clock::now();
                    }
                    if (type == Medius::kTypeUsageEvent && n >= 8 && payload[5] == Medius::kClassButton
                        && n >= 8 + 3u * payload[7]) {
                        unsigned mask = 0;
                        for (unsigned j = 0; j < payload[7]; ++j) {
                            const auto id = unsigned(payload[9 + 3*j]) | unsigned(payload[10 + 3*j]) << 8;
                            if (payload[8 + 3*j] == Medius::kClassButton && id < 5) mask |= 1u << id;
                        }
                        Buttons(mask);
                    }
                } else if (config.kind == DeviceKind::Ferrum && config.ferrumController) {
                    if (bytes[i] == '\r' || bytes[i] == '\n') {
                        Ferrum::State state;
                        if (Ferrum::ParseState(line, state)) {
                            ControllerSample(state.buttons, state.lt, state.rt);
                            controllerReply = lastReply = Clock::now();
                        }
                        line.clear();
                    } else if (line.size() < 256) line.push_back(char(bytes[i]));
                    else line.clear();
                }
            }
        }
        return true;
    }
    bool ProbeMakcu(int baud) {
        makcu = {}; lastReply = {}; PurgeComm(serial, PURGE_RXCLEAR);
        if (!Configure(baud) || !Text("km.version()\r\n")) return false;
        const auto until = Clock::now() + std::chrono::milliseconds(250);
        while (!cancelled() && Clock::now() < until) {
            if (!ReadSerial()) return false;
            if (lastReply != Clock::time_point{}) return true;
            Pause(2);
        }
        return false;
    }
    bool OpenSerial() {
        auto ports = Ports();
        std::string selected = config.port;
        if (!config.manualPort && config.kind != DeviceKind::Ferrum) {
            std::vector<PortInfo> candidates;
            for (auto p : ports) {
                std::string name = p.name;
                for (char& ch : name) ch = char(std::toupper(static_cast<unsigned char>(ch)));
                const bool usb = config.kind == DeviceKind::KmboxUsb;
                if (name.find("CH340") != std::string::npos || (!usb && (name.find("CH343") != std::string::npos
                    || name.find("CP210") != std::string::npos || name.find("USB SERIAL") != std::string::npos))) candidates.push_back(p);
            }
            selected.clear();
            if (lastKind == config.kind && !lastIdentity.empty())
                for (const auto& p : candidates) if (p.identity == lastIdentity) selected = p.port;
            if (selected.empty() && candidates.size() == 1) selected = candidates[0].port;
            if (selected.empty()) return Fail(candidates.empty() ? "No matching serial device found" : "Several serial devices match; select a COM port explicitly");
        }
        endpoint = selected;
        serial = CreateFileA(("\\\\.\\" + selected).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (serial == INVALID_HANDLE_VALUE) return Fail("Cannot open COM port; check cable and other applications", GetLastError());
        COMMTIMEOUTS timeouts{}; timeouts.ReadIntervalTimeout = MAXDWORD; timeouts.WriteTotalTimeoutConstant = 20;
        if (!SetupComm(serial, 4096, 256) || !SetCommTimeouts(serial, &timeouts)) return Fail("Cannot configure COM buffers/timeouts", GetLastError());
        PurgeComm(serial, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR);
        if (IsMakcu(config.kind)) {
            const int rates[] = {config.baud, lastBaud, 115200, 4000000, 921600, 1500000, 2000000, 3000000};
            int found = 0;
            for (size_t i = 0; i < std::size(rates) && !cancelled(); ++i) {
                bool duplicate = false; for (size_t j = 0; j < i; ++j) duplicate |= rates[j] == rates[i];
                if (!duplicate && ProbeMakcu(rates[i])) { found = rates[i]; break; }
            }
            if (!found) return Fail("No Makcu version reply; check selected port, firmware and baud");
            if (found != config.baud) {
                const auto command = Makcu::BaudCommand(config.baud);
                if (!Write(command.data(), command.size())) return false;
                const auto until = Clock::now() + std::chrono::milliseconds(100);
                COMSTAT status{}; DWORD errors = 0;
                do {
                    if (!ClearCommError(serial, &errors, &status)) return Fail("Baud change drain failed", GetLastError());
                    if (!status.cbOutQue) break;
                    if (!Pause(2)) return false;
                } while (Clock::now() < until);
                if (status.cbOutQue || !Pause(50) || !ProbeMakcu(config.baud)) return Fail("Makcu did not confirm requested baud");
            }
            lastBaud = config.baud; verified = true;
        } else if (config.kind == DeviceKind::Medius) {
            if (!Configure(Medius::kBaud)) return false;
            proto = 0; const uint8_t query[] = {Medius::kQueryVersion};
            const auto until = Clock::now() + std::chrono::seconds(5);
            auto next = Clock::time_point{};
            while (!cancelled() && Clock::now() < until && !proto) {
                if (Clock::now() >= next) { if (!Frame(Medius::kTypeQuery, query, 1)) return false; next = Clock::now() + std::chrono::milliseconds(250); }
                if (!ReadSerial() || !Pause(2)) return false;
            }
            if (!proto) return Fail("No Medius version reply at 6 Mbaud; check COM port and firmware");
            if (!Medius::IsSupportedVersion(proto)) {
                const auto reason = "Medius reported protocol " + std::to_string(proto)
                    + "; supported protocols are 8 and 9";
                return Fail(reason.c_str());
            }
            verified = true;
        } else if (!Configure(config.baud)) return false;
        if (config.kind == DeviceKind::Controller) {
            if (!Text("km.controller_physical()\r\n")) return false;
            const auto until = Clock::now() + std::chrono::milliseconds(750);
            while (!cancelled() && Clock::now() < until && !input.controllerValid)
                if (!ReadSerial() || !Pause(2)) return false;
            if (!input.controllerValid) return Fail("No MAKCU physical controller reply; attach and move the controller, check its route and use V4 firmware with controller_physical support");
            if (!MakFrame(0x04) || !MakFrame(0x02)) return false;
            if (!Text("km.controller(1)\r\n")) return false;
            controllerPoll = Clock::now();
        }
        if (config.kind == DeviceKind::Ferrum && config.ferrumController) {
            if (!Text("km.controller()\r\n")) return false;
            const auto until = Clock::now() + std::chrono::milliseconds(500);
            while (!cancelled() && Clock::now() < until && !input.controllerValid)
                if (!ReadSerial() || !Pause(2)) return false;
            if (!input.controllerValid) return Fail("Ferrum controller API did not reply. Use Mouse mode for older versions; select a Controller in the Ferrum app for 2.0.4 or newer.");
            verified = true;
        }
        if (cancelled()) return false;
        initialized = true;
        if (!Neutralize()) return false;
        if (config.kind == DeviceKind::Makcu && !Text("km.buttons(1)\r\n")) return false;
        if (config.kind == DeviceKind::Medius) {
            const uint8_t subscription[] = {Medius::kClassButton, 0xFF, 0xFF, 0, 1, 0};
            if (!Frame(Medius::kTypeCatch, subscription, sizeof(subscription))) return false;
        }
        for (const auto& p : ports) if (p.port == selected) { lastIdentity = p.identity; lastKind = config.kind; }
        error.clear(); heartbeat = Clock::now(); return true;
    }
    bool NetSend(uint32_t command, const void* payload, size_t n) {
        client_data packet{};
        packet.head.mac = netMac; packet.head.cmd = command;
        packet.head.indexpts = command == cmd_connect ? netSequence : ++netSequence;
        packet.head.rand = netSequence ^ 0x65A97B13u;
        if (n) std::memcpy(&packet.u8buff, payload, n);
        const int size = int(sizeof(cmd_head_t) + n);
        if (sendto(socket, reinterpret_cast<const char*>(&packet), size, 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != size)
            return Fail("UDP send failed", WSAGetLastError());
        pending[netSequence % pending.size()] = {netSequence, command, true};
        return true;
    }
    bool ReadNet() {
        for (unsigned i = 0; i < 64; ++i) {
            client_data packet{}; sockaddr_in from{}; int fromLength = sizeof(from);
            const int n = recvfrom(socket, reinterpret_cast<char*>(&packet), sizeof(packet), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (n == SOCKET_ERROR) {
                const auto code = WSAGetLastError();
                if (code == WSAEWOULDBLOCK) return true;
                if (code == WSAECONNRESET) continue;
                return Fail("UDP receive failed", code);
            }
            if (n < int(sizeof(cmd_head_t)) || from.sin_addr.s_addr != address.sin_addr.s_addr || from.sin_port != address.sin_port) continue;
            auto& expected = pending[packet.head.indexpts % pending.size()];
            if (!expected.valid || expected.sequence != packet.head.indexpts || expected.command != packet.head.cmd) continue;
            expected.valid = false; lastReply = Clock::now();
            if (packet.head.cmd == cmd_connect) netHandshake = true;
        }
        return true;
    }
    bool WaitNetReply(std::uint32_t sequence, const char* failure, bool* timedOut = nullptr) {
        const auto until = Clock::now() + std::chrono::milliseconds(500);
        while (!cancelled() && Clock::now() < until) {
            if (!ReadNet()) return false;
            const auto& reply = pending[sequence % pending.size()];
            if (reply.sequence == sequence && !reply.valid) return true;
            if (!Pause(2)) return false;
        }
        if (cancelled()) return false;
        if (timedOut) { *timedOut = true; return false; }
        return Fail(failure);
    }
    bool OpenNet() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) return Fail("Winsock startup failed");
        winsock = true;
        address.sin_family = AF_INET; address.sin_port = htons(static_cast<unsigned short>(std::stoi(config.networkPort)));
        if (InetPtonA(AF_INET, config.ip.c_str(), &address.sin_addr) != 1) return Fail("Invalid IPv4 address");
        netMac = uint32_t(std::stoul(config.mac, nullptr, 16));
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_SOCKET) return Fail("Cannot create UDP socket", WSAGetLastError());
        u_long nonblocking = 1;
        if (ioctlsocket(socket, FIONBIO, &nonblocking)) return Fail("Cannot configure UDP socket", WSAGetLastError());
        endpoint = config.ip + ":" + config.networkPort;
        if (!NetSend(cmd_connect, nullptr, 0)) return false;
        const auto until = Clock::now() + std::chrono::seconds(2);
        while (!cancelled() && Clock::now() < until) {
            if (!ReadNet()) return false;
            if (netHandshake) {
                initialized = true; heartbeat = Clock::now();
                if (!Neutralize()) return false;
                if (!WaitNetReply(netSequence - 1,
                    "KMBox connected but did not acknowledge mouse commands; check firmware and device settings")) return false;
                verified = true;
                return true;
            }
            Pause(2);
        }
        return Fail("Kmbox Net handshake timed out; check IP, port and device ID");
    }
public:
    ~Backend() override { Close(); }
    bool Open(const DeviceConfig& c, const std::function<bool()>& cancel) override {
        Close(); config = c; cancelled = cancel; error.clear(); endpoint.clear();
        return IsNetworkDevice(config.kind) ? OpenNet() : OpenSerial();
    }
    void Close() override {
        if (initialized) Neutralize();
        if (initialized && config.kind == DeviceKind::Controller) Text("km.controller(0)\r\n");
        if (serial != INVALID_HANDLE_VALUE) { CloseHandle(serial); serial = INVALID_HANDLE_VALUE; }
        if (socket != INVALID_SOCKET) { closesocket(socket); socket = INVALID_SOCKET; }
        if (winsock) { WSACleanup(); winsock = false; }
        initialized = verified = netHandshake = testAcknowledged = false; input = {}; makcu = {}; medius = {}; line.clear();
        pending = {}; mouse = {}; keyboard = {}; sequence = 0; netSequence = 0;
        lastReply = heartbeat = controllerPoll = controllerReply = {};
        controllerOutput = {}; controllerResubscribe = controllerBaseline = false;
        controllerTextSnapshots = false; controllerFirmware = 0; controllerKinds = 0;
    }
    bool Poll() override {
        const auto now = Clock::now();
        if (IsNetworkDevice(config.kind)) {
            if (!ReadNet()) return false;
            if (now - lastReply > std::chrono::seconds(5)) return Fail("Kmbox Net stopped replying");
            if (now - heartbeat >= std::chrono::seconds(1)) {
                heartbeat = now;
                return NetSend(cmd_mouse_move, &mouse, sizeof(mouse));
            }
            return true;
        }
        if (!ReadSerial()) return false;
        if ((IsMakcu(config.kind) || config.kind == DeviceKind::Medius)
            && now - lastReply > std::chrono::seconds(15)) return Fail("Device health reply timed out");
        if (UsesController(config)) {
            if (controllerResubscribe) {
                if (!Text("km.controller(1)\r\n")) return false;
                controllerResubscribe = false; controllerPoll = {};
            }
            if (now - controllerPoll >= std::chrono::milliseconds(16)) {
                controllerPoll = now;
                if (config.kind == DeviceKind::Controller) {
                    if (controllerTextSnapshots ? !Text("km.controller_physical()\r\n") : !MakFrame(0x54)) return false;
                } else if (!Text("km.controller()\r\n")) return false;
            }
            if (controllerReply == Clock::time_point{} || now - controllerReply > std::chrono::milliseconds(100)) {
                InvalidateController();
                if (config.kind == DeviceKind::Controller && !controllerTextSnapshots) {
                    controllerTextSnapshots = true; controllerPoll = {};
                }
            }
            if (config.kind == DeviceKind::Controller && !input.controllerValid
                && (controllerOutput.low || controllerOutput.high || controllerOutput.lt || controllerOutput.rt
                    || controllerOutput.lx || controllerOutput.ly || controllerOutput.rx || controllerOutput.ry)) {
                controllerOutput = {}; if (!ControllerOutput()) return false;
            }
        }
        if (now - heartbeat >= std::chrono::seconds(1)) {
            heartbeat = now;
            if (IsMakcu(config.kind)) return Text("km.version()\r\n");
            if (config.kind == DeviceKind::Medius) {
                const uint8_t query[] = {Medius::kQueryHealth};
                return Frame(Medius::kTypeQuery, query, 1);
            }
        }
        return true;
    }
    bool Move(int x, int y, bool stick) override {
        if (IsNetworkDevice(config.kind)) {
            auto data = mouse; data.x = x; data.y = y;
            return NetSend(cmd_mouse_move, &data, sizeof(data));
        }
        if (config.kind == DeviceKind::Medius) {
            const uint8_t payload[] = {0, uint8_t(x), uint8_t(x >> 8), uint8_t(y), uint8_t(y >> 8), 0};
            return Frame(Medius::kTypeMove, payload, sizeof(payload));
        }
        char text[80];
        if (stick && config.kind == DeviceKind::Ferrum && config.ferrumController)
            return Text(Ferrum::Stick(x, y).c_str());
        if (stick && config.kind == DeviceKind::Controller) {
            if ((x || y) && !input.controllerValid) return true;
            controllerOutput.rx = int16_t(std::clamp(x, -32768, 32767));
            controllerOutput.ry = int16_t(std::clamp(y, -32768, 32767));
            return ControllerOutput();
        } else snprintf(text, sizeof(text), "km.move(%d,%d)\r\n", x, y);
        return Text(text);
    }
    bool TestMove(int x, int y, bool stick) override {
        testAcknowledged = false;
        if (config.kind == DeviceKind::Controller && !input.controllerValid)
            return Fail("MAKCU physical controller input is unavailable; reconnect the controller before testing movement");
        if (!Move(x, y, stick)) return false;
        if (config.kind == DeviceKind::Controller) {
            if (!Pause(40) || !ReadSerial()) return false;
        }
        if (!IsNetworkDevice(config.kind)) { testAcknowledged = true; return true; }
        bool timedOut = false;
        testAcknowledged = WaitNetReply(netSequence,
            "KMBox did not acknowledge test movement", &timedOut);
        return testAcknowledged || timedOut;
    }
    bool TestMoveAcknowledged() const override { return testAcknowledged; }
    bool SetControl(Control c) override {
        if (c.keyboard && c.down && c.id < injectedKeys.size()) injectedKeys[c.id] = true;
        const auto complete = [&](bool ok) {
            if (ok && c.keyboard && !c.down && c.id < injectedKeys.size()) injectedKeys[c.id] = false;
            return ok;
        };
        if (config.kind == DeviceKind::Medius) {
            const uint8_t payload[] = {uint8_t(c.keyboard ? Medius::kClassKey : Medius::kClassButton),
                uint8_t(c.id), uint8_t(c.id >> 8), uint8_t(c.down ? Medius::kActPress : Medius::kActSoftRel)};
            return complete(Frame(Medius::kTypeInject, payload, sizeof(payload)));
        }
        if (IsNetworkDevice(config.kind)) {
            if (!c.keyboard) {
                mouse.button = c.down ? mouse.button | (1 << c.id) : mouse.button & ~(1 << c.id);
                const unsigned commands[] = {cmd_mouse_left, cmd_mouse_right, cmd_mouse_middle, cmd_mouse_move, cmd_mouse_move};
                return NetSend(commands[c.id], &mouse, sizeof(mouse));
            }
            if (c.id >= 0xE0) {
                unsigned mask = 1u << (c.id - 0xE0);
                keyboard.ctrl = char(c.down ? unsigned(uint8_t(keyboard.ctrl)) | mask : unsigned(uint8_t(keyboard.ctrl)) & ~mask);
            } else {
                for (char& key : keyboard.button) if (uint8_t(key) == c.id) { if (!c.down) key = 0; else return true; }
                if (c.down) { auto it = std::find(std::begin(keyboard.button), std::end(keyboard.button), char(0));
                    if (it == std::end(keyboard.button)) return Fail("Kmbox Net keyboard slots full"); *it = char(c.id); }
            }
            return complete(NetSend(cmd_keyboard_all, &keyboard, sizeof(keyboard)));
        }
        char text[80];
        if (config.kind == DeviceKind::Controller && !c.keyboard) {
            if (c.id >= 5) return Fail("Unsupported controller output control");
            if (c.down && !input.controllerValid) return true;
            if (c.id < 2) (c.id == 0 ? controllerOutput.rt : controllerOutput.lt) = c.down ? 1023 : 0;
            else {
                constexpr unsigned ids[] = {11, 10, 17, 8, 9};
                const auto bit = 1u << ids[c.id];
                controllerOutput.low = c.down ? controllerOutput.low | bit : controllerOutput.low & ~bit;
            }
            return ControllerOutput();
        }
        if (config.kind == DeviceKind::Ferrum && config.ferrumController && !c.keyboard)
            return Text(Ferrum::Control(c.id, c.down).c_str());
        if (c.keyboard) snprintf(text, sizeof(text), "km.%s(%u)\r\n", c.down ? "down" : "up", c.id);
        else {
            const char* names[] = {"left", "right", "middle", config.kind == DeviceKind::Makcu ? "ms1" : "side1", config.kind == DeviceKind::Makcu ? "ms2" : "side2"};
            snprintf(text, sizeof(text), "km.%s(%d)\r\n", names[c.id], c.down ? 1 : 0);
        }
        return complete(Text(text));
    }
    bool AxisLock(bool on) override {
        if (config.kind == DeviceKind::Controller) {
            for (uint8_t id : {uint8_t(14), uint8_t(15)}) {
                const uint8_t mask[] = {id, uint8_t(on ? 4 : 0)};
                if (!MakFrame(0x51, mask, sizeof(mask))) return false;
            }
            return true;
        }
        if (config.kind == DeviceKind::Makcu || (config.kind == DeviceKind::Ferrum && !config.ferrumController))
            return Text(on ? "km.lock_mx(1)\r\nkm.lock_my(1)\r\n" : "km.lock_mx(0)\r\nkm.lock_my(0)\r\n");
        if (config.kind == DeviceKind::Medius) {
            for (uint8_t axis = 0; axis < 2; ++axis) {
                const uint8_t payload[] = {3, axis, 0, 0, uint8_t(on ? 0 : 100), 0};
                if (!Frame(0x0A, payload, sizeof(payload))) return false;
            }
        }
        return true;
    }
    bool Neutralize() override {
        if (!initialized) return true;
        if (config.kind == DeviceKind::Controller) {
            controllerOutput = {};
            if (!ControllerOutput() || !AxisLock(false)) return false;
            for (unsigned i = 1; i < injectedKeys.size(); ++i)
                if (injectedKeys[i] && !SetControl({i, true, false})) return false;
            return true;
        }
        if (config.kind == DeviceKind::Ferrum && config.ferrumController) {
            if (!Text(Ferrum::Clear)) return false;
            for (unsigned i = 1; i < injectedKeys.size(); ++i)
                if (injectedKeys[i] && !SetControl({i, true, false})) return false;
            return true;
        }
        if (config.kind == DeviceKind::Medius) {
            injectedKeys = {};
            const uint8_t subscription[] = {Medius::kClassButton, 0xFF, 0xFF, 0, 1, 0};
            const uint8_t resetFlags[] = {0};
            return Frame(Medius::kTypeReset, proto == Medius::kProtoVer ? resetFlags : nullptr,
                proto == Medius::kProtoVer ? sizeof(resetFlags) : 0) && AxisLock(false)
                && Frame(Medius::kTypeCatch, subscription, sizeof(subscription));
        }
        if (IsNetworkDevice(config.kind)) {
            mouse = {}; keyboard = {}; injectedKeys = {};
            return NetSend(cmd_mouse_move, &mouse, sizeof(mouse)) && NetSend(cmd_keyboard_all, &keyboard, sizeof(keyboard));
        }
        if (!Text("\r\n")) return false;
        for (unsigned i = 0; i < 5; ++i) if (!SetControl({i, false, false})) return false;
        for (unsigned i = 1; i < injectedKeys.size(); ++i) if (injectedKeys[i] && !SetControl({i, true, false})) return false;
        return AxisLock(false);
    }
    PhysicalInput Input() override { auto result = input; input.pressed = 0; input.controllerPressed = 0; return result; }
    std::string Error() const override { return error; }
    std::string Endpoint() const override { return endpoint; }
    bool Verified() const override { return verified; }
    std::vector<PortInfo> Ports() override {
        std::vector<PortInfo> result;
        HDEVINFO devices = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
        if (devices == INVALID_HANDLE_VALUE) return result;
        SP_DEVINFO_DATA device{}; device.cbSize = sizeof(device);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devices, i, &device); ++i) {
            char name[512]{}, identity[512]{}, port[64]{}; DWORD size = sizeof(port), type = 0;
            SetupDiGetDeviceRegistryPropertyA(devices, &device, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<PBYTE>(name), sizeof(name)-1, nullptr);
            SetupDiGetDeviceInstanceIdA(devices, &device, identity, sizeof(identity), nullptr);
            HKEY key = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (key == INVALID_HANDLE_VALUE) continue;
            const auto code = RegQueryValueExA(key, "PortName", nullptr, &type, reinterpret_cast<LPBYTE>(port), &size);
            RegCloseKey(key); port[sizeof(port)-1] = 0;
            if (code == ERROR_SUCCESS && type == REG_SZ && strncmp(port, "COM", 3) == 0) result.push_back({port, name, identity});
        }
        SetupDiDestroyDeviceInfoList(devices);
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.port < b.port; });
        return result;
    }
};
}
std::unique_ptr<DeviceBackend> MakeDeviceBackend() { return std::make_unique<Backend>(); }
}
