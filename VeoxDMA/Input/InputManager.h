#pragma once
#include <cstdint>
#include <string>

namespace Keyboard {

bool InitKeyboard();
void UpdateKeys();
bool IsKeyDown(uint32_t virtual_key_code);
bool IsReady();

enum class CursorCalStep : int { Idle = 0, Learning, Done, Failed };

void TickCursor(int32_t screenW, int32_t screenH);
bool IsCursorLocked();
bool GetCursor(int32_t& x, int32_t& y);
bool GetCursorGafDelta(int64_t& delta);
bool RestoreCursorFromGafDelta(int64_t delta, int32_t screenW, int32_t screenH);
void BeginCursorCalibration();
void CancelCursorCalibration();
CursorCalStep GetCursorCalStep();
const std::string& GetCursorCalMessage();

}
