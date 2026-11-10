#pragma once
#include <cstddef>
#include <cstdint>
namespace Makcu {
enum class State { Disconnected, Connecting, Connected, Failed };
enum class AimState { Disabled, Menu, KeyUp, StaleData, LocalInvalid, CameraInvalid,
    TargetFiltered, SwitchDelay, NoTarget, Deadzone, Subpixel, Delay, Queued, QueueRejected };
// These functions never wait on serial I/O. Connect copies the request; a
// single worker owns the handle, handshake, incoming reports, and log writes.
void Connect(const char* port, int baud);
void Disconnect();
void Shutdown(); // join only after the render loop exits, before DLL teardown
State GetState();
// A short renewable lease; source 0 is player aim, source 1 is mortar.
void SetAxisLock(bool locked, unsigned source = 0);
void GetError(char* out, size_t size);
bool Send(const char* command, size_t size, bool movement = false, bool test = false, bool recoil = false);
bool MouseDown(int vk, bool& down);
struct ButtonSample {
    unsigned down = 0;
    unsigned pressed = 0;
    bool valid = false;
};
// Consume press events once per frame, then share the sample between binds.
// Held state remains available independently through MouseDown.
ButtonSample TakeButtonSample();
bool ButtonsReady();
void ReportAim(AimState state);
void ReportBind(bool dmaReady, bool hasMouseBind, bool hasKeyboardBind, int mode);
int TakeTestResult(); // 0 pending/none, 1 full serial write, -1 failed/cancelled
}
