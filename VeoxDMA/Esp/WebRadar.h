#pragma once
#include <string>

namespace WebRadar {

void Init();
bool Start(int port = 8888);
void Stop();
bool IsRunning();
int GetPort();
std::string GetUrl();
void OpenInBrowser();

} // namespace WebRadar
