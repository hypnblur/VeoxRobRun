#include "pch.h"
#include "kmboxNet.h"
#include "kmboxNetWrapper.h"
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#include <mstcpip.h> // SIO_UDP_CONNRESET
#endif
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

static int err_creat_socket = -9000;
static int err_net_version = -9001;
static int err_net_rx_timeout = -9003;
static int err_net_cmd = -9005;
static int err_net_pts = -9006;

namespace {
constexpr int kConnectTimeoutMs = 2000;
constexpr int kRuntimeRecvTimeoutMs = 350;
// UDP sends should not use SO_SNDTIMEO at runtime — a full kernel send
// buffer or transient Windows UDP reset can make sendto fail even when the
// device is reachable; only recv waits for an ack.
constexpr int kRuntimeSendTimeoutMs = 0;
// UDP loses packets in normal operation; a single missed ack must not tear
// down the session. Disconnect only after this many consecutive failures.
constexpr int kMaxConsecutiveFailures = 8;
constexpr int kMaxSendRetries = 3;
// Bound on how many stale/mismatched acks we drain per command before
// treating the exchange as failed (protects against a pathological flood).
constexpr int kMaxAckDrainAttempts = 12;
constexpr int kMaxStaleAckDrain = 32;
} // namespace

static std::string g_kmbox_net_init_err;

static const char* KmBoxNetExplainInitCode(int code);

KmBoxNetManager::~KmBoxNetManager()
{
    Disconnect();
}

void KmBoxNetManager::SetSocketTimeouts(int recvMs, int sendMs)
{
    if (s_Client == INVALID_SOCKET)
        return;
    setsockopt(s_Client, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&recvMs), sizeof(recvMs));
    setsockopt(s_Client, SOL_SOCKET, SO_SNDTIMEO,
        reinterpret_cast<const char*>(&sendMs), sizeof(sendMs));
}

void KmBoxNetManager::DisableUdpConnReset()
{
    if (s_Client == INVALID_SOCKET)
        return;
    // Disable Windows' UDP "connection reset" behavior. Without this, any
    // datagram that triggers an ICMP Port Unreachable (device momentarily
    // busy, rebooting, or a network blip) queues WSAECONNRESET on the
    // socket and fails a LATER sendto/recvfrom — which surfaced to users as
    // random "UDP send failed" disconnects even though the link was fine.
    BOOL newBehavior = FALSE;
    DWORD bytesReturned = 0;
    WSAIoctl(s_Client, SIO_UDP_CONNRESET, &newBehavior, sizeof(newBehavior),
        nullptr, 0, &bytesReturned, nullptr, nullptr);
}

void KmBoxNetManager::DisconnectNoLock()
{
    m_connected = false;
    m_consecFails = 0;
    m_keyboardData = {};
    if (s_Client != INVALID_SOCKET) {
        closesocket(s_Client);
        s_Client = INVALID_SOCKET;
    }
    WSACleanup();
}

void KmBoxNetManager::Disconnect()
{
    std::lock_guard<std::mutex> lock(m_ioMutex);
    DisconnectNoLock();
}

int KmBoxNetManager::InitDevice(const std::string& IP, uint16_t Port, const std::string& Mac)
{
    m_connected = false;
    m_keyboardData = {};

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(1, 1), &wsaData) != 0)
        return err_creat_socket;

    if (LOBYTE(wsaData.wVersion) != 1 || HIBYTE(wsaData.wVersion) != 1) {
        WSACleanup();
        return err_net_version;
    }

    srand(static_cast<unsigned int>(time(nullptr)));

    s_Client = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_Client == INVALID_SOCKET) {
        WSACleanup();
        return err_creat_socket;
    }

    DisableUdpConnReset();

    SetSocketTimeouts(kConnectTimeoutMs, kConnectTimeoutMs);

    const unsigned long addr = inet_addr(IP.c_str());
    if (addr == INADDR_NONE) {
        Disconnect();
        return err_creat_socket;
    }

    AddrServer.sin_addr.s_addr = addr;
    AddrServer.sin_family = AF_INET;
    AddrServer.sin_port = htons(Port);

    PostData.head.mac = static_cast<unsigned int>(strtoul(Mac.c_str(), nullptr, 16));
    PostData.head.rand = rand();
    PostData.head.indexpts = 0;
    PostData.head.cmd = cmd_connect;

    const int r = SendData(static_cast<int>(sizeof(cmd_head_t)));
    if (r != 0) {
        Disconnect();
        return r;
    }

    SetSocketTimeouts(kRuntimeRecvTimeoutMs, kRuntimeSendTimeoutMs);
    m_connected = true;
    m_consecFails = 0;
    m_reconnectRequested = false;
    return 0;
}

bool KmBoxNetManager::ConsumeReconnectRequested()
{
    if (!m_reconnectRequested)
        return false;
    m_reconnectRequested = false;
    return true;
}

bool KmBoxNetManager::IsFireAndForgetCmd(unsigned int cmd) const
{
    // High-frequency aimbot mouse deltas do not need a blocking ack round
    // trip; waiting on every move was the main source of recv timeouts and
    // spurious disconnects under combat load.
    return cmd == cmd_mouse_move;
}

void KmBoxNetManager::DrainStaleAcksNoLock()
{
    if (s_Client == INVALID_SOCKET)
        return;

    u_long nonBlocking = 1;
    ioctlsocket(s_Client, FIONBIO, &nonBlocking);

    for (int i = 0; i < kMaxStaleAckDrain; ++i) {
        SOCKADDR_IN from{};
        int fromLen = sizeof(from);
        client_data discard{};
        const int status = recvfrom(s_Client, reinterpret_cast<char*>(&discard),
            sizeof(discard), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (status >= 0)
            continue;
        if (WSAGetLastError() == WSAEWOULDBLOCK)
            break;
        break;
    }

    nonBlocking = 0;
    ioctlsocket(s_Client, FIONBIO, &nonBlocking);
}

int KmBoxNetManager::SendToWithRetry(int DataLength)
{
    for (int attempt = 0; attempt < kMaxSendRetries; ++attempt) {
        const int sent = sendto(s_Client, reinterpret_cast<const char*>(&PostData), DataLength, 0,
            reinterpret_cast<sockaddr*>(&AddrServer), sizeof(AddrServer));
        if (sent != SOCKET_ERROR)
            return sent;

        const int wsaErr = WSAGetLastError();
        if (wsaErr == WSAECONNRESET) {
            // Clear a queued ICMP reset then retry — even with
            // SIO_UDP_CONNRESET disabled, a reset already pending when the
            // socket was created can still poison the next sendto.
            DrainStaleAcksNoLock();
            DisableUdpConnReset();
            continue;
        }
        if (wsaErr == WSAEINTR || wsaErr == WSAEWOULDBLOCK)
            continue;
        break;
    }
    return SOCKET_ERROR;
}

// Called with m_ioMutex held. Counts a transient failure; only tears the
// connection down once several failures happen back-to-back.
int KmBoxNetManager::RegisterIoFailure(bool wasConnected, const char* message)
{
    if (wasConnected && ++m_consecFails >= kMaxConsecutiveFailures) {
        g_kmbox_net_init_err = message;
        m_reconnectRequested = true;
        DisconnectNoLock();
    }
    return err_net_rx_timeout;
}

int KmBoxNetManager::SendData(int DataLength)
{
    std::lock_guard<std::mutex> lock(m_ioMutex);
    return SendDataNoLock(DataLength);
}

int KmBoxNetManager::SendDataNoLock(int DataLength)
{
    if (s_Client == INVALID_SOCKET)
        return err_creat_socket;

    const bool wasConnected = m_connected;
    const bool fireAndForget = IsFireAndForgetCmd(PostData.head.cmd);

    if (!fireAndForget)
        DrainStaleAcksNoLock();

    if (SendToWithRetry(DataLength) == SOCKET_ERROR) {
        return RegisterIoFailure(wasConnected,
            "UDP send failed repeatedly. Check network connection to the Kmbox Net.");
    }

    if (fireAndForget) {
        m_consecFails = 0;
        return 0;
    }

    // Wait for the ack matching this command. Acks from earlier commands
    // can arrive late (after their timeout window); drain and ignore them
    // instead of treating a stale sequence number as a fatal protocol error.
    for (int attempt = 0; attempt < kMaxAckDrainAttempts; ++attempt) {
        SOCKADDR_IN s_NewClient = {};
        int FromLen = sizeof(s_NewClient);
        const int status = recvfrom(s_Client, reinterpret_cast<char*>(&ReceiveData),
            sizeof(ReceiveData), 0, reinterpret_cast<sockaddr*>(&s_NewClient), &FromLen);

        if (status < 0) {
            return RegisterIoFailure(wasConnected,
                "Connection lost: device stopped responding. Check IP, port, power, and network.");
        }

        if (NetHandler() == 0) {
            m_consecFails = 0;
            return 0;
        }
    }

    return RegisterIoFailure(wasConnected,
        "Connection lost: device kept sending mismatched replies. Check for another program using the Kmbox Net.");
}

int KmBoxNetManager::NetHandler()
{
    if (ReceiveData.head.cmd != PostData.head.cmd)
        return err_net_cmd;
    if (ReceiveData.head.indexpts != PostData.head.indexpts)
        return err_net_pts;
    return 0;
}

int KmBoxMouse::Move(int x, int y)
{
    if (!KmBoxMgr.IsConnected())
        return err_creat_socket;

    KmBoxMgr.PostData.head.indexpts++;
    KmBoxMgr.PostData.head.cmd = cmd_mouse_move;
    KmBoxMgr.PostData.head.rand = rand();

    MouseData.x = x;
    MouseData.y = y;
    memcpy(&KmBoxMgr.PostData.cmd_mouse, &MouseData, sizeof(soft_mouse_t));
    MouseData.x = 0;
    MouseData.y = 0;

    const int length = static_cast<int>(sizeof(cmd_head_t) + sizeof(soft_mouse_t));
    return KmBoxMgr.SendData(length);
}

int KmBoxMouse::Move_Auto(int x, int y, int Runtime)
{
    if (!KmBoxMgr.IsConnected())
        return err_creat_socket;

    KmBoxMgr.PostData.head.indexpts++;
    KmBoxMgr.PostData.head.cmd = cmd_mouse_automove;
    KmBoxMgr.PostData.head.rand = Runtime;

    MouseData.x = x;
    MouseData.y = y;
    memcpy(&KmBoxMgr.PostData.cmd_mouse, &MouseData, sizeof(soft_mouse_t));
    MouseData.x = 0;
    MouseData.y = 0;

    const int length = static_cast<int>(sizeof(cmd_head_t) + sizeof(soft_mouse_t));
    return KmBoxMgr.SendData(length);
}

int KmBoxMouse::Left(bool Down)
{
    if (!KmBoxMgr.IsConnected())
        return err_creat_socket;

    KmBoxMgr.PostData.head.indexpts++;
    KmBoxMgr.PostData.head.cmd = cmd_mouse_left;
    KmBoxMgr.PostData.head.rand = rand();

    MouseData.button = Down ? (MouseData.button | 0x01) : (MouseData.button & ~0x01);
    memcpy(&KmBoxMgr.PostData.cmd_mouse, &MouseData, sizeof(soft_mouse_t));

    const int length = static_cast<int>(sizeof(cmd_head_t) + sizeof(soft_mouse_t));
    return KmBoxMgr.SendData(length);
}

int KmBoxMouse::Right(bool Down)
{
    if (!KmBoxMgr.IsConnected())
        return err_creat_socket;

    KmBoxMgr.PostData.head.indexpts++;
    KmBoxMgr.PostData.head.cmd = cmd_mouse_right;
    KmBoxMgr.PostData.head.rand = rand();

    MouseData.button = Down ? (MouseData.button | 0x02) : (MouseData.button & ~0x02);
    memcpy(&KmBoxMgr.PostData.cmd_mouse, &MouseData, sizeof(soft_mouse_t));

    const int length = static_cast<int>(sizeof(cmd_head_t) + sizeof(soft_mouse_t));
    return KmBoxMgr.SendData(length);
}

int KmBoxMouse::Middle(bool Down)
{
    if (!KmBoxMgr.IsConnected())
        return err_creat_socket;

    KmBoxMgr.PostData.head.indexpts++;
    KmBoxMgr.PostData.head.cmd = cmd_mouse_middle;
    KmBoxMgr.PostData.head.rand = rand();

    MouseData.button = Down ? (MouseData.button | 0x04) : (MouseData.button & ~0x04);
    memcpy(&KmBoxMgr.PostData.cmd_mouse, &MouseData, sizeof(soft_mouse_t));

    const int length = static_cast<int>(sizeof(cmd_head_t) + sizeof(soft_mouse_t));
    return KmBoxMgr.SendData(length);
}

int KmBoxMouse::Side1(bool Down)
{
    if (!KmBoxMgr.IsConnected())
        return err_creat_socket;
    MouseData.button = Down ? (MouseData.button | 0x08) : (MouseData.button & ~0x08);
    return Move(0, 0);
}

int KmBoxMouse::Side2(bool Down)
{
    if (!KmBoxMgr.IsConnected())
        return err_creat_socket;
    MouseData.button = Down ? (MouseData.button | 0x10) : (MouseData.button & ~0x10);
    return Move(0, 0);
}

int KmBoxNetManager::KeyboardKey(unsigned char hid, bool down)
{
    if (!IsConnected() || hid == 0 || hid > 0xE7)
        return err_creat_socket;

    if (hid >= 0xE0) {
        const unsigned char mask = static_cast<unsigned char>(1u << (hid - 0xE0));
        m_keyboardData.ctrl = static_cast<char>(down
            ? (static_cast<unsigned char>(m_keyboardData.ctrl) | mask)
            : (static_cast<unsigned char>(m_keyboardData.ctrl) & ~mask));
    } else if (down) {
        bool alreadyDown = false;
        for (char key : m_keyboardData.button)
            alreadyDown = alreadyDown || static_cast<unsigned char>(key) == hid;
        if (!alreadyDown) {
            for (char& key : m_keyboardData.button) {
                if (key == 0) {
                    key = static_cast<char>(hid);
                    break;
                }
            }
        }
    } else {
        for (char& key : m_keyboardData.button) {
            if (static_cast<unsigned char>(key) == hid)
                key = 0;
        }
    }

    PostData.head.indexpts++;
    PostData.head.cmd = cmd_keyboard_all;
    PostData.head.rand = rand();
    memcpy(&PostData.cmd_keyboard, &m_keyboardData, sizeof(m_keyboardData));
    const int length = static_cast<int>(sizeof(cmd_head_t) + sizeof(soft_keyboard_t));
    return SendData(length);
}

KmBoxNetManager KmBoxMgr;

static const char* KmBoxNetExplainInitCode(int code)
{
    switch (code) {
    case 0: return "";
    case -9000: return "Winsock startup, UDP socket creation, or IP address failed.";
    case -9001: return "Winsock version not supported.";
    case -9003: return "Device did not respond (timeout). Check IP, port, firewall, and that the Kmbox Net is powered and on the same network.";
    case -9005: return "Handshake failed: unexpected response from device (wrong MAC or firmware?).";
    case -9006: return "Handshake failed: sequence mismatch.";
    default: return "Unknown network error during connect.";
    }
}

int KmBoxNetTryInit(const std::string& IP, uint16_t Port, const std::string& Mac)
{
    g_kmbox_net_init_err.clear();
    KmBoxNetDisconnect();

    if (IP.empty()) {
        g_kmbox_net_init_err = "IP address is empty.";
        return err_creat_socket;
    }
    if (inet_addr(IP.c_str()) == INADDR_NONE) {
        g_kmbox_net_init_err = "Invalid IP address.";
        return err_creat_socket;
    }

    const int r = KmBoxMgr.InitDevice(IP, Port, Mac);
    if (r != 0) {
        if (g_kmbox_net_init_err.empty())
            g_kmbox_net_init_err = KmBoxNetExplainInitCode(r);
        KmBoxNetDisconnect();
        return r;
    }
    return 0;
}

const std::string& KmBoxNetLastInitError()
{
    return g_kmbox_net_init_err;
}

void KmBoxNetClearInitError()
{
    g_kmbox_net_init_err.clear();
}

void KmBoxNetMouseMove(int x, int y)
{
    if (!KmBoxNetIsConnected())
        return;
    (void)KmBoxMgr.Mouse.Move(x, y);
}

void KmBoxNetMouseMoveToCenter()
{
    if (!KmBoxNetIsConnected())
        return;
    (void)KmBoxMgr.Mouse.Move_Auto(32768, 32768, 100);
}

void KmBoxNetMouseLeft(bool down)
{
    if (!KmBoxNetIsConnected())
        return;
    (void)KmBoxMgr.Mouse.Left(down);
}

void KmBoxNetMouseRight(bool down)
{
    if (!KmBoxNetIsConnected())
        return;
    (void)KmBoxMgr.Mouse.Right(down);
}

void KmBoxNetMouseMiddle(bool down)
{
    if (!KmBoxNetIsConnected())
        return;
    (void)KmBoxMgr.Mouse.Middle(down);
}

void KmBoxNetMouseSide1(bool down)
{
    if (!KmBoxNetIsConnected())
        return;
    (void)KmBoxMgr.Mouse.Side1(down);
}

void KmBoxNetMouseSide2(bool down)
{
    if (!KmBoxNetIsConnected())
        return;
    (void)KmBoxMgr.Mouse.Side2(down);
}

void KmBoxNetKeyboardKey(unsigned char hid, bool down)
{
    if (!KmBoxNetIsConnected())
        return;
    (void)KmBoxMgr.KeyboardKey(hid, down);
}

bool KmBoxNetIsConnected()
{
    return KmBoxMgr.IsConnected();
}

bool KmBoxNetConsumeReconnectRequested()
{
    return KmBoxMgr.ConsumeReconnectRequested();
}

void KmBoxNetDisconnect()
{
    KmBoxMgr.Disconnect();
}
