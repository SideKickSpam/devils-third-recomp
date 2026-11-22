// The game module (Windows releases, built with DT_GAME_MODULE): the generated code is not linked into dt_port.exe but
// built on the player's machine into dt_game.dll by the setup tool, from their own copy of the game. Load() finds it,
// gives it the runtime's functions and data (DtModuleHost, ppc_context.h) and takes the generated tables from it.
#pragma once

#include <string>

namespace game_module
{
// Whether this runtime loads the generated code from a module (false: it is linked in, nothing to load).
bool Used();
// Loads the module: `path` if given, else dt_game.dll beside the executable, else %LOCALAPPDATA%\dt-port\dt_game.dll.
// False with `error` set if there is none or it does not fit this runtime (then the setup tool must build it again).
bool Load(const std::string& path, std::string& error);

// An installed release (set up by the setup tool): %LOCALAPPDATA%\dt-port, which holds install.ini (the game
// directory), dt_game.dll, the shader cache and the setup tool's build files. Empty in builds without a module.
std::string DataDir();
// The game directory install.ini names (empty if there is none).
std::string InstalledGameDir();
// Starts the setup tool beside the executable (DevilsThirdSetup.exe) with `args`: when the game is not set up yet or
// the module is from another dt-port version. False if there is none.
bool LaunchSetup(const std::string& args);
} // namespace game_module
