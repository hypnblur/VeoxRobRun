#pragma once

namespace ConfigFile {

// Config lives at %APPDATA%\VeoxDMA\RobRun\config.json (Roaming AppData). The
// directory is created on first save, so a fresh machine does not need any setup.
const wchar_t* Directory();
const wchar_t* Path();

// Serializes ConfigInstance to disk. Returns false if the directory or file
// could not be written.
bool Save();

// Reads config.json and applies it to ConfigInstance. Returns false when the
// file does not exist or cannot be parsed; the current settings are kept.
bool Load();

} // namespace ConfigFile
