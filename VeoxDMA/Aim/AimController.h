#pragma once

// Render-thread aim loop. Tick() reads the published snapshot, picks the
// closest valid target inside the configured FOV and emits one motion step per
// frame. Draw() renders the FOV circle as on-screen feedback.
namespace Aim {

void Tick();
void Draw();

// Activation-key labels shared with the menu so the combo and the runtime
// lookup cannot drift apart.
const char* const* KeyNames();
int KeyCount();

} // namespace Aim
