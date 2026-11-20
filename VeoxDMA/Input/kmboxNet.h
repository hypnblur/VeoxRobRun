#pragma once

#include <string>
#include <cstdint>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include "HidTable.h"

class KmBoxMouse
{
public:
    soft_mouse_t MouseData = {};

    int Move(int x, int y);
    int Move_Auto(int x, int y, int Runtime);
    int Left(bool Down);
    int Right(bool Down);
    int Middle(bool Down);
    int Side1(bool Down);
    int Side2(bool Down);
};

class KmBoxNetManager
{
private:
    SOCKADDR_IN AddrServer = {};
    SOCKET s_Client = INVALID_SOCKET;
    client_data ReceiveData = {};
    client_data PostData = {};
    bool m_connected = false;
    // Serializes SendData across threads (combat thread + UI test button
    // share the socket and the PostData buffer).
    std::mutex m_ioMutex;
    // Transient UDP loss is expected; only disconnect after this many
    // consecutive send/ack failures, not on the first one.
    int m_consecFails = 0;
    bool m_reconnectRequested = false;
    soft_keyboard_t m_keyboardData = {};

    void SetSocketTimeouts(int recvMs, int sendMs);
    void DisableUdpConnReset();
    void DrainStaleAcksNoLock();
    int SendToWithRetry(int DataLength);
    int NetHandler();
    int SendData(int DataLength);
    int SendDataNoLock(int DataLength);
    void DisconnectNoLock();
    int RegisterIoFailure(bool wasConnected, const char* message);
    bool IsFireAndForgetCmd(unsigned int cmd) const;

public:
    ~KmBoxNetManager();

    int InitDevice(const std::string& IP, uint16_t Port, const std::string& Mac);

    void Disconnect();

    bool IsConnected() const { return m_connected && s_Client != INVALID_SOCKET; }

    bool ConsumeReconnectRequested();
    int KeyboardKey(unsigned char hid, bool down);

    friend class KmBoxMouse;
    KmBoxMouse Mouse;
};

extern KmBoxNetManager KmBoxMgr;

// Disconnects any previous session, then runs the UDP handshake. Returns 0 on success.
// On failure the socket is closed and a human-readable message is stored for the UI.
int KmBoxNetTryInit(const std::string& IP, uint16_t Port, const std::string& Mac);
const std::string& KmBoxNetLastInitError();
void KmBoxNetClearInitError();
