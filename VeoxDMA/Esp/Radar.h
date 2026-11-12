#pragma once
#include <vector>
#include <string>

namespace Radar {

void Init();
void Draw();
void Shutdown();

const std::vector<std::string>& GetMapFiles();
int GetCurrentMapIndex();
void SetMap(int index);

} // namespace Radar
