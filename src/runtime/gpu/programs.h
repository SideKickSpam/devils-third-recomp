// The game's shader programs as the renderer first saw them, with everything their translation depends on, kept across
// builds in <shader cache>/programs.bin (local only, like the rest of the shader cache: never in git). When a
// translator or renderer change invalidates the translated shaders, Precompile translates them again from here at
// startup, so areas already played do not compile mid-game again.
#pragma once

#include "shader/translator.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gpu::programs
{
// Loads the library (any thread, once, before the others are used).
void Open(const std::string& path);
// Any thread: saves the request, its program bytes included, if no program with this hash is saved yet.
void Remember(uint64_t hash, const shader::Request& req);
// Any thread: the saved request for `hash`; its program points into `storage`.
bool Find(uint64_t hash, shader::Request& req, std::vector<uint8_t>& storage);
size_t Count();
std::vector<uint64_t> Hashes();
} // namespace gpu::programs
