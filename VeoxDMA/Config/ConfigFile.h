#pragma once
#include <string>
#include <vector>

namespace ConfigFile {

// Config directory is fixed at C:\VeoxDMA\RobRun (auto-created if missing)
const wchar_t* Directory();
const wchar_t* Path();

std::string GetConfigDirectory();
std::string GetActiveConfigName();
void SetActiveConfigName(const std::string& name);

// Lists all config file names in C:\VeoxDMA\RobRun without the .json extension
std::vector<std::string> ListConfigs();

// Serializes ConfigInstance to C:\VeoxDMA\RobRun\<name>.json
bool Save(const std::string& name = "");

// Reads C:\VeoxDMA\RobRun\<name>.json and applies to ConfigInstance
bool Load(const std::string& name = "");

// Creates a new config file initialized with current settings
bool CreateConfig(const std::string& name);

// Renames an existing config file
bool RenameConfig(const std::string& oldName, const std::string& newName);

// Deletes a config file
bool DeleteConfig(const std::string& name);

} // namespace ConfigFile
