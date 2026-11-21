#pragma once
#include <vector>
#include <string>
#include <filesystem>

namespace Radar {

void Init();
void Draw();
void DrawFullMap();
void Shutdown();

const std::vector<std::string>& GetMapFiles();
int GetCurrentMapIndex();
void SetMap(int index);
const std::string& GetCurrentMapName();
const char* MapFileForLocation(const std::string& locationId);
void AutoSelectMap(const std::string& locationId);
std::filesystem::path GetMapsDirectory();
void CalibrateBoundsForMap(const std::string& mapName);

} // namespace Radar
