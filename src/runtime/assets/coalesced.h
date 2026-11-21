// The game's Coalesced_<language>.bin: every ini and localisation file in one, encrypted on disk. The game decrypts it
// at boot (appDecryptData); a hook hands us the plain bytes, which stay in memory only. We read the controller layouts
// and their action names from it, in the player's own language.
#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace assets::coalesced
{
// The decrypted bytes of something the game decrypted; kept if they are the coalesced file. Any thread.
void Offer(const uint8_t* data, size_t size);
bool Loaded();
// Every value of `key` in `[section]` of the file whose name ends with `file` (case-insensitive: "DT_UI_Pause.INT"),
// in file order, as UTF-8.
std::vector<std::string> Values(const std::string& file, const std::string& section, const std::string& key);
// The first value, or `fallback`.
// Every entry whose section contains the text (any file), as section, key, value (section and key in lower case).
std::vector<std::array<std::string, 3>> Entries(const std::string& sectionContains);
std::string Value(const std::string& file, const std::string& section, const std::string& key, const std::string& fallback = "");
} // namespace assets::coalesced
