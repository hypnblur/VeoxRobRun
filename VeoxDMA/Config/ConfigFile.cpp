#include "pch.h"
#include "ConfigFile.h"
#include "ConfigInstance.h"

namespace {

constexpr const wchar_t* kDirectory = L"C:\\VeoxDMA\\RobRun";
constexpr const wchar_t* kConfigPath = L"C:\\VeoxDMA\\RobRun\\config.json";

} // namespace

const wchar_t* ConfigFile::Directory()
{
    return kDirectory;
}

const wchar_t* ConfigFile::Path()
{
    return kConfigPath;
}

bool ConfigFile::Save()
{
    std::error_code ec;
    std::filesystem::create_directories(kDirectory, ec);
    if (ec) {
        LOG("[Config] cannot create %ls: %s\n", kDirectory, ec.message().c_str());
        return false;
    }

    std::ofstream out(kConfigPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        LOG("[Config] cannot open %ls for writing\n", kConfigPath);
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
        LOG("[Config] write failed for %ls\n", kConfigPath);
        return false;
    }

    LOG("[Config] saved %ls\n", kConfigPath);
    return true;
}

bool ConfigFile::Load()
{
    std::ifstream in(kConfigPath, std::ios::binary);
    if (!in) {
        LOG("[Config] %ls not found, using defaults\n", kConfigPath);
        return false;
    }

    try {
        json parsed;
        in >> parsed;
        ConfigInstance.FromJson(parsed);
    }
    catch (const std::exception& e) {
        LOG("[Config] parse failed for %ls: %s\n", kConfigPath, e.what());
        return false;
    }

    LOG("[Config] loaded %ls\n", kConfigPath);
    return true;
}
