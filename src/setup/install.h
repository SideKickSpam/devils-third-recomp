// The installation around the game module: where dt-port keeps its files, install.ini, save import from Cemu or a
// folder, shortcuts, and starting the game.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace setup
{
// %APPDATA%\dt-port (settings.ini, saves, session logs): the runtime's settings directory.
std::filesystem::path SettingsDir();
// Where the runtime keeps saves in an installed release: <settings dir>/save (account folder 80000001, common).
std::filesystem::path SaveDir();

// install.ini in the data directory (%LOCALAPPDATA%\dt-port): the game directory the runtime reads.
bool WriteInstallIni(const std::filesystem::path& dataDir, const std::filesystem::path& gameDir, std::string& error);
std::filesystem::path InstalledGameDir(const std::filesystem::path& dataDir);

struct SaveCandidate
{
    std::filesystem::path user;    // the folder holding d3sys.sav (an account's folder)
    std::filesystem::path common;  // the title's shared save folder, if any
    std::string label;             // where it was found, for the player
    std::filesystem::file_time_type time{};
};

// Whether dt-port already has a save.
bool HasSave();
// Devil's Third saves in Cemu's usual places.
std::vector<SaveCandidate> FindCemuSaves();
// A save in a folder the player chose: an account folder (with d3sys.sav), a folder holding account folders, or a Cemu
// mlc01 folder.
bool FindSaveIn(const std::filesystem::path& dir, SaveCandidate& out);
// Copies the save into SaveDir() (account 80000001); an existing save is kept as save-backup-<date>.
bool ImportSave(const SaveCandidate& save, std::string& error);

// The game's executable beside the setup tool.
std::filesystem::path GameExe();
// Desktop and Start menu shortcuts to the game (Windows).
bool CreateShortcuts(bool desktop, bool startMenu, std::string& error);
bool LaunchGame();
} // namespace setup
