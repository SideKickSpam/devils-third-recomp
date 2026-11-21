// Text that names Wii U buttons (move lists, tips, operation guides) rewritten for the device in use and the player's
// bindings, and text about the Wii U GamePad's own features explained for PC. Applied to every string the game looks up
// with Localize (gfx_hooks.cpp), so a menu opened after switching devices shows the new names. English text only: the
// patterns are the English wording ("Y Button", "L Stick", "select button").
#pragma once

#include <optional>
#include <string>

namespace host::button_text
{
// Any thread. The rewritten text, or nothing if it names no button and is not about the GamePad. `section` and `key`
// are where the game found it (some stick mentions mean a click, others a tilt).
std::optional<std::u16string> Rewrite(const std::u16string& section, const std::u16string& key, const std::u16string& text);
} // namespace host::button_text
