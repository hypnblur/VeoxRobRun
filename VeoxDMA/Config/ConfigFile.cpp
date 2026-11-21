#include "pch.h"
#include "ConfigFile.h"
#include "ConfigInstance.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <thread>

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

// Config names come straight from the UI, so they are treated as untrusted:
// "..\..\Startup\x" must not escape the config directory, and ':' or '*' must
// not reach CreateFile. A name that fails these checks is rejected (returns
// empty) instead of being rewritten into a different config, so an existing
// name like "my config (1)" keeps working.
std::string SanitizeName(std::string name)
{
    if (name.size() >= 5 && name.substr(name.size() - 5) == ".json")
        name = name.substr(0, name.size() - 5);

    if (name.find("..") != std::string::npos) return {};

    for (char c : name)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) return {};
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
            c == '\\' || c == '|' || c == '?' || c == '*')
            return {};
    }

    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.size() > 64) name.resize(64);
    return name;
}

void UpdateActivePath()
{
    EnsureDirectoryExists();
    std::string safeName = SanitizeName(g_activeConfig);
    if (safeName.empty()) safeName = "default";

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
    if (name.empty()) return;
    const std::string safe = SanitizeName(name);
    if (safe.empty()) {
        LOG("[Config] rejected config name '%s'\n", name.c_str());
        return;
    }
    g_activeConfig = safe;
    UpdateActivePath();
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
    std::string target = SanitizeName(name.empty() ? g_activeConfig : name);
    if (target.empty()) {
        if (!name.empty()) {
            LOG("[Config] rejected config name '%s'\n", name.c_str());
            return false;
        }
        target = "default";
    }

    g_activeConfig = target;
    UpdateActivePath();

    const std::filesystem::path fullPath =
        std::filesystem::path(kConfigDirW) / (target + ".json");
    const std::filesystem::path tmpPath = fullPath.string() + ".tmp";

    // Serialize first: a throw here must leave the live file untouched.
    std::string text;
    try {
        text = ConfigInstance.ToJson().dump(2);
    }
    catch (const std::exception& e) {
        LOG("[Config] serialize failed: %s\n", e.what());
        return false;
    }

    // Write a temp file, then rename over the target. A crash or power loss
    // mid-save then leaves the previous config intact instead of a truncated
    // file that fails to parse (and silently loads a different config).
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            LOG("[Config] cannot open %ls for writing\n", tmpPath.c_str());
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out.good()) {
            LOG("[Config] write failed for %ls\n", tmpPath.c_str());
            out.close();
            std::error_code ignore;
            std::filesystem::remove(tmpPath, ignore);
            return false;
        }
    }

    // std::filesystem::rename replaces an existing destination. Retry a couple
    // of times because an AV scanner can hold the file open briefly. Never
    // delete the live config: if the swap fails, the old file must survive.
    std::error_code ec;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::filesystem::rename(tmpPath, fullPath, ec);
        if (!ec) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (ec) {
        LOG("[Config] rename %ls -> %ls failed: %s (previous config kept)\n",
            tmpPath.c_str(), fullPath.c_str(), ec.message().c_str());
        return false;
    }

    LOG("[Config] saved %ls\n", fullPath.c_str());
    return true;
}

bool Load(const std::string& name)
{
    EnsureDirectoryExists();
    std::string target = SanitizeName(name.empty() ? g_activeConfig : name);
    if (target.empty()) {
        if (!name.empty()) {
            LOG("[Config] rejected config name '%s'\n", name.c_str());
            return false;
        }
        target = "default";
    }

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
            ConfigInstance.LootFilter.InitDefaults();
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

    if (ConfigInstance.LootFilter.Items.empty())
        ConfigInstance.LootFilter.InitDefaults();
    else
        ConfigInstance.LootFilter.MigrateIconFlags();

    LOG("[Config] loaded %ls\n", fullPath.c_str());
    return true;
}

bool CreateConfig(const std::string& name)
{
    const std::string safe = SanitizeName(name);
    if (safe.empty()) {
        LOG("[Config] rejected config name '%s'\n", name.c_str());
        return false;
    }
    return Save(safe);
}

bool RenameConfig(const std::string& oldName, const std::string& newName)
{
    if (oldName.empty() || newName.empty() || oldName == newName) return false;
    EnsureDirectoryExists();

    const std::string safeOld = SanitizeName(oldName);
    const std::string safeNew = SanitizeName(newName);
    if (safeOld.empty() || safeNew.empty() || safeOld == safeNew) {
        LOG("[Config] rejected rename '%s' -> '%s'\n", oldName.c_str(), newName.c_str());
        return false;
    }

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
    const std::string safe = SanitizeName(name);
    if (safe.empty()) {
        LOG("[Config] rejected config name '%s'\n", name.c_str());
        return false;
    }
    EnsureDirectoryExists();

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
