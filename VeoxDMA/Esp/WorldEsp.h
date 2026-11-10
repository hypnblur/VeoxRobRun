#pragma once

// World-entity ESP: containers, loot, vehicles and grenades. Reads only the
// snapshot produced by RobRunReader::Update; no DMA work happens here.
void DrawWorldEntities();
