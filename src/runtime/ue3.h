// Calling the game's UnrealScript from native code: an object's script function found by name and run with
// UObject::ProcessEvent, as the engine runs events, and script properties found by name and read or written in place.
// Game Thread only (the script VM is not thread-safe), from outside the game's own script calls (gfx_hooks.cpp runs
// these from its UGameEngine::Tick hook).
#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>

namespace ue3
{
// One argument of a script call. Each is written as its parameter's type needs: ints, floats, objects and bytes by
// value, bools as their bit, names as an FName, strings as an FString.
struct Arg
{
    enum Kind { Int, Float, Bool, Name, String, Vector } kind = Int;
    uint32_t u = 0;
    float f = 0;
    float v[3] = {};
    std::string s;
    Arg(int32_t v) : kind(Int), u(uint32_t(v)) {}
    Arg(uint32_t v) : kind(Int), u(v) {}
    Arg(float v) : kind(Float), f(v) {}
    Arg(bool v) : kind(Bool), u(v) {}
    static Arg MakeName(std::string n) { Arg a(0); a.kind = Name, a.s = std::move(n); return a; }
    static Arg MakeString(std::string n) { Arg a(0); a.kind = String, a.s = std::move(n); return a; }
    static Arg MakeVector(float x, float y, float z) { Arg a(0); a.kind = Vector, a.v[0] = x, a.v[1] = y, a.v[2] = z; return a; }
};

// Run `object`'s script function `function`. Returns its return value (bools as 0 or 1, floats as their bits, 0 for
// none), or nothing if the object or function does not exist. Missing trailing arguments stay zero (optional ones).
std::optional<uint32_t> Call(uint32_t object, const char* function, std::initializer_list<Arg> args = {});
std::optional<float> CallFloat(uint32_t object, const char* function, std::initializer_list<Arg> args = {});

// A script property of an object (its own class or a parent's).
struct Property
{
    uint32_t address = 0, size = 0, mask = 0;  // mask: a bool's bit in its 32-bit word, 0 otherwise
};
std::optional<Property> FindProperty(uint32_t object, const char* name);
// The UProperty object for `name` in the object's class (0 if none): its offset at +0x5C, element size at +0x4C;
// an ArrayProperty's Inner, a StructProperty's Struct and a ByteProperty's Enum at +0x70.
uint32_t FindPropertyField(uint32_t object, const char* name);
// appRealloc with the game's allocator (memory the game may free or grow later). Game Thread.
uint32_t Realloc(uint32_t pointer, uint32_t size);
std::optional<uint32_t> GetObject(uint32_t object, const char* name);  // object (or int) property
std::optional<bool> GetBool(uint32_t object, const char* name);
bool SetBool(uint32_t object, const char* name, bool value);

// The game's UI manager (UUIManager::GetUIManager, a D3UIManager).
uint32_t UIManager();
std::string PathName(uint32_t object);  // "Transient.D3Engine_0"
std::string ClassName(uint32_t object);  // "D3Engine"
// The object's class or one of its parents is called `className`.
bool IsA(uint32_t object, const char* className);
// The name of a function object (UObject::Name), and its class's.
uint32_t NameIndex(uint32_t object);
uint32_t FindNameIndex(const char* name);  // 0 if the name does not exist
uint32_t OuterOf(uint32_t object);
} // namespace ue3
