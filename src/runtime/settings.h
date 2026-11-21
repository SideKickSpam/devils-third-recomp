// Persistent settings: settings.ini in the user's config directory (%APPDATA%\dt-port on Windows,
// ~/.local/share/dt-port on Linux) or the file given with --config. Lines are `key = value` under `[section]`
// headers; keys are addressed as "section.key". Command-line options override values for one run without
// changing the file. Thread-safe.
#pragma once

#include <cstdint>
#include <string>

namespace settings
{
// Load the file (the default path when empty). Missing files are fine: every setting has a default.
void Load(const std::string& path = "");
bool Loaded();
const std::string& Path();
// The directory holding the settings file, with a trailing separator ("" for the current directory).
std::string Directory();
// Write the current values (not the overrides) back to the file.
void Save();

std::string Get(const std::string& key, const std::string& fallback);
int GetInt(const std::string& key, int fallback);
float GetFloat(const std::string& key, float fallback);
bool GetBool(const std::string& key, bool fallback);

// Change a value (and drop any command-line override of it). Saved by the next Save().
void Set(const std::string& key, const std::string& value);
void SetInt(const std::string& key, int value);
void SetFloat(const std::string& key, float value);
void SetBool(const std::string& key, bool value);
// A value for this run only (command line).
void Override(const std::string& key, const std::string& value);

// Increments whenever a value changes, so users can notice changes cheaply.
uint64_t Generation();
} // namespace settings
