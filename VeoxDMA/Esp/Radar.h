#pragma once
#include <vector>
#include <string>
#include <filesystem>

namespace Radar {

void Init();
void Draw();
void DrawFullMap();
void DrawInGameMapOverlay();
void Shutdown();

const std::vector<std::string>& GetMapFiles();
int GetCurrentMapIndex();
void SetMap(int index);
// Thread-safe map switch request (e.g. from WebRadar HTTP thread). Applied on
// the render thread by ProcessPendingMap() so D3D11/WIC stay single-threaded.
void RequestSetMap(int index);
void ProcessPendingMap();
std::string GetCurrentMapName();
const char* MapFileForLocation(const std::string& locationId);
void AutoSelectMap(const std::string& locationId);
std::filesystem::path GetMapsDirectory();
void CalibrateBoundsForMap(const std::string& mapName);

} // namespace Radar
