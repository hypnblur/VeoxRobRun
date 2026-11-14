#include "pch.h"
#include "ConfigFile.h"
#include "ConfigInstance.h"
#include <Shlobj.h>
#include <wchar.h>

namespace {

constexpr const wchar_t* kAppDataFolderName = L"VeoxDMA";
constexpr const wchar_t* kRelativeDir = L"RobRun";
constexpr size_t kMaxPath = 1024;

wchar_t g_configDir[kMaxPath] = {};
wchar_t g_configPath[kMaxPath] = {};
bool g_configResolved = false;

bool ResolveConfigPaths()
{
    if (g_configResolved) return true;
    PWSTR knownPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &knownPath)) && knownPath)
    {
        const std::wstring dir = std::wstring(knownPath) + L"\\" + kAppDataFolderName + L"\\" + kRelativeDir;
        wcscpy_s(g_configDir, kMaxPath, dir.c_str());
        const std::wstring path = dir + L"\\config.json";
        wcscpy_s(g_configPath, kMaxPath, path.c_str());
        CoTaskMemFree(knownPath);
    }
    g_configResolved = true;
    return true;
}

} // namespace

const wchar_t* ConfigFile::Directory()
{
    ResolveConfigPaths();
    return g_configDir;
}

const wchar_t* ConfigFile::Path()
{
    ResolveConfigPaths();
    return g_configPath;
}

bool ConfigFile::Save()
{
    std::error_code ec;
    std::filesystem::create_directories(g_configDir, ec);
    if (ec) {
        LOG("[Config] cannot create %ls: %s\n", g_configDir, ec.message().c_str());
        return false;
    }

    std::ofstream out(g_configPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        LOG("[Config] cannot open %ls for writing\n", g_configPath);
        return false;
    }

    try {
        out << ConfigInstance.ToJson().dump(2);
    }
    catch (const std::exception& e) {
        LOG("[Config] serialize failed: %s\n", e.what());
        return false;
    }

    if (!out.good()) {
        LOG("[Config] write failed for %ls\n", g_configPath);
        return false;
    }

    LOG("[Config] saved %ls\n", g_configPath);
    return true;
}

bool ConfigFile::Load()
{
    std::ifstream in(g_configPath, std::ios::binary);
    if (!in) {
        LOG("[Config] %ls not found, using defaults\n", g_configPath);
        return false;
    }

    try {
        json parsed;
        in >> parsed;
        ConfigInstance.FromJson(parsed);
    }
    catch (const std::exception& e) {
        LOG("[Config] parse failed for %ls: %s\n", g_configPath, e.what());
        return false;
    }

    LOG("[Config] loaded %ls\n", g_configPath);
    return true;
}
