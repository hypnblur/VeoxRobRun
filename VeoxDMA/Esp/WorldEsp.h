#pragma once

namespace RobRunReader { struct Snapshot; }

// World-entity ESP: containers, loot, vehicles and grenades. Reads only the
// snapshot produced by RobRunReader::Update; no DMA work happens here.
void DrawWorldEntities(const RobRunReader::Snapshot* snapshot);
void DrawGrenadePrediction(const RobRunReader::Snapshot* snapshot);
void DrawHouseInfo(const RobRunReader::Snapshot* snapshot);
void DrawUnrevealedLootLabels(const RobRunReader::Snapshot* snapshot);
