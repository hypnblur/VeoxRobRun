#include "pch.h"
#include "ConfigFile.h"
#include "ConfigInstance.h"
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace {

const std::wstring kConfigDirW = L"C:\\VeoxDMA\\RobRun";
const std::string kConfigDirA = "C:\\VeoxDMA\\RobRun";

std::string g_activeConfig = "default";
wchar_t g_activeConfigPathW[1024] = {};

void EnsureDirectoryExists()
{
    std::error_code ec;
    std::filesystem::create_directories(kConfigDirW, ec);
}

void UpdateActivePath()
{
    EnsureDirectoryExists();
    std::string safeName = g_activeConfig;
    if (safeName.empty()) safeName = "default";
    if (safeName.size() >= 5 && safeName.substr(safeName.size() - 5) == ".json")
        safeName = safeName.substr(0, safeName.size() - 5);

    g_activeConfig = safeName;
    std::wstring fullPath = kConfigDirW + L"\\" + std::wstring(safeName.begin(), safeName.end()) + L".json";
    wcscpy_s(g_activeConfigPathW, fullPath.c_str());
}

} // namespace

namespace ConfigFile {

const wchar_t* Directory()
{
    EnsureDirectoryExists();
    return kConfigDirW.c_str();
}

const wchar_t* Path()
{
    UpdateActivePath();
    return g_activeConfigPathW;
}

std::string GetConfigDirectory()
{
    EnsureDirectoryExists();
    return kConfigDirA;
}

std::string GetActiveConfigName()
{
    return g_activeConfig;
}

void SetActiveConfigName(const std::string& name)
{
    if (!name.empty()) {
        g_activeConfig = name;
        if (g_activeConfig.size() >= 5 && g_activeConfig.substr(g_activeConfig.size() - 5) == ".json")
            g_activeConfig = g_activeConfig.substr(0, g_activeConfig.size() - 5);
        UpdateActivePath();
    }
}

std::vector<std::string> ListConfigs()
{
    EnsureDirectoryExists();
    std::vector<std::string> configs;
    std::error_code ec;

    for (const auto& entry : std::filesystem::directory_iterator(kConfigDirW, ec))
    {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".json")
        {
            configs.push_back(entry.path().stem().string());
        }
    }

    if (configs.empty())
    {
        configs.push_back("default");
    }

    std::sort(configs.begin(), configs.end());
    return configs;
}

bool Save(const std::string& name)
{
    EnsureDirectoryExists();
    std::string target = name.empty() ? g_activeConfig : name;
    if (target.empty()) target = "default";
    if (target.size() >= 5 && target.substr(target.size() - 5) == ".json")
        target = target.substr(0, target.size() - 5);

    g_activeConfig = target;
    UpdateActivePath();

    std::filesystem::path fullPath = std::filesystem::path(kConfigDirW) / (target + ".json");
    std::ofstream out(fullPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        LOG("[Config] cannot open %ls for writing\n", fullPath.c_str());
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
        LOG("[Config] write failed for %ls\n", fullPath.c_str());
        return false;
    }

    LOG("[Config] saved %ls\n", fullPath.c_str());
    return true;
}

bool Load(const std::string& name)
{
    EnsureDirectoryExists();
    std::string target = name.empty() ? g_activeConfig : name;
    if (target.empty()) target = "default";
    if (target.size() >= 5 && target.substr(target.size() - 5) == ".json")
        target = target.substr(0, target.size() - 5);

    std::filesystem::path fullPath = std::filesystem::path(kConfigDirW) / (target + ".json");
    std::ifstream in(fullPath, std::ios::binary);
    if (!in) {
        auto list = ListConfigs();
        if (!list.empty()) {
            fullPath = std::filesystem::path(kConfigDirW) / (list[0] + ".json");
            target = list[0];
            in.open(fullPath, std::ios::binary);
        }
        if (!in) {
            LOG("[Config] %ls not found, initializing default\n", fullPath.c_str());
            Save("default");
            return true;
        }
    }

    try {
        json parsed;
        in >> parsed;
        ConfigInstance.FromJson(parsed);
        g_activeConfig = target;
        UpdateActivePath();
    }
    catch (const std::exception& e) {
        LOG("[Config] parse failed for %ls: %s\n", fullPath.c_str(), e.what());
        return false;
    }

    LOG("[Config] loaded %ls\n", fullPath.c_str());
    return true;
}

bool CreateConfig(const std::string& name)
{
    if (name.empty()) return false;
    std::string safe = name;
    if (safe.size() >= 5 && safe.substr(safe.size() - 5) == ".json")
        safe = safe.substr(0, safe.size() - 5);

    return Save(safe);
}

bool RenameConfig(const std::string& oldName, const std::string& newName)
{
    if (oldName.empty() || newName.empty() || oldName == newName) return false;
    EnsureDirectoryExists();

    std::string safeOld = oldName;
    if (safeOld.size() >= 5 && safeOld.substr(safeOld.size() - 5) == ".json")
        safeOld = safeOld.substr(0, safeOld.size() - 5);

    std::string safeNew = newName;
    if (safeNew.size() >= 5 && safeNew.substr(safeNew.size() - 5) == ".json")
        safeNew = safeNew.substr(0, safeNew.size() - 5);

    std::filesystem::path oldPath = std::filesystem::path(kConfigDirW) / (safeOld + ".json");
    std::filesystem::path newPath = std::filesystem::path(kConfigDirW) / (safeNew + ".json");

    std::error_code ec;
    if (!std::filesystem::exists(oldPath, ec)) return false;

    std::filesystem::rename(oldPath, newPath, ec);
    if (ec) {
        LOG("[Config] rename %ls -> %ls failed: %s\n", oldPath.c_str(), newPath.c_str(), ec.message().c_str());
        return false;
    }

    if (g_activeConfig == safeOld) {
        g_activeConfig = safeNew;
        UpdateActivePath();
    }

    LOG("[Config] renamed %ls -> %ls\n", oldPath.c_str(), newPath.c_str());
    return true;
}

bool DeleteConfig(const std::string& name)
{
    if (name.empty()) return false;
    EnsureDirectoryExists();

    std::string safe = name;
    if (safe.size() >= 5 && safe.substr(safe.size() - 5) == ".json")
        safe = safe.substr(0, safe.size() - 5);

    std::filesystem::path path = std::filesystem::path(kConfigDirW) / (safe + ".json");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;

    std::filesystem::remove(path, ec);
    if (ec) return false;

    auto list = ListConfigs();
    if (!list.empty())
        Load(list[0]);
    else
        Save("default");

    return true;
}

} // namespace ConfigFile
