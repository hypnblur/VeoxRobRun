#pragma once
#include <cstddef>
#include <cstdint>

namespace Medius {
enum class State { Disconnected, Connecting, Connected, Failed };

void Connect(const char* port);
void Disconnect();
void Shutdown();
State GetState();
// A short renewable lease; source 0 is player aim, source 1 is mortar.
void SetAxisLock(bool locked, unsigned source = 0);
void GetError(char* out, size_t size);
bool Move(int x, int y, bool recoil = false, bool test = false);
bool InjectButton(uint8_t id, bool down);
bool InjectKey(unsigned hid, bool down);
bool MouseDown(int vk, bool& down);
void ReportAim(int state);
int TakeTestResult();
} // namespace Medius
