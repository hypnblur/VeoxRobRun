#pragma once

// Render-thread aim loop. Tick() reads the published snapshot, picks the
// closest valid target inside the configured FOV and emits one motion step per
// frame. Draw() renders the FOV circle as on-screen feedback.
namespace Aim {

// Live state of the last Tick(), shown by the menu and the optional on-screen
// HUD. Every early return records a reason so a dead aim loop is diagnosable
// without a debugger.
struct DebugInfo {
	bool enabled = false;
	bool menuOpen = false;
	bool held = false;
	const char* keySource = "-";
	const char* reason = "";
	int entities = 0;
	int candidates = 0;
	int target = -1;
	float dx = 0.f;
	float dy = 0.f;
	int stepX = 0;
	int stepY = 0;
	int aimType = 0;
	bool deviceReady = false;
	// DMA angle write (Memory Aim). writeState is a short verdict from the
	// post-write readback: "held" means the game kept our orientation,
	// "reverted" means the camera controller rebuilt it from its own input.
	bool writeMode = false;
	bool writeOk = false;
	const char* writeState = "-";
	float tgtYawDeg = 0.f;
	float tgtPitchDeg = 0.f;
	float distance = 0.f;
	int leadMs = 0;
};

void Tick();
void Draw();
void DrawDebug();
DebugInfo GetDebug();

// Activation-key labels shared with the menu so the combo and the runtime
// lookup cannot drift apart.
const char* const* KeyNames();
int KeyCount();

} // namespace Aim
