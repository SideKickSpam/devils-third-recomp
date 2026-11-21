// Host audio output (SDL3): the AX mixer's stereo output, played on the default device and optionally saved
// as a WAV file (--audio-dump). SDL converts the mixer's rate to the device's.
#pragma once

#include <cstddef>
#include <string>

namespace host::audio
{
constexpr int Channels = 2;

// Options, before the window opens: --mute (also implied by --hidden) plays at zero volume, so the device
// still paces the mixer; --no-window opens no device (the wall clock paces it); --audio-dump FILE.
void SetMuted(bool muted);
void SetDumpPath(const std::string& path);

// The rate the mixer produces (Hz). Called by the AX mixer once, before it pushes audio.
void SetSourceRate(int rate);

// Opens the playback device. Called on the window thread after SDL_Init (SDL's subsystems are initialised from
// one thread).
void OpenDevice();

// Apply audio.mute_unfocused and --mute (after a settings change or a focus change).
void UpdateVolume();

// Whether a device is playing: the mixer then paces itself on the device's consumption.
bool DeviceOpen();
// Interleaved stereo frames waiting to be played.
size_t QueuedFrames();
// Queues interleaved stereo float frames for playback (and the dump).
void Push(const float* samples, size_t frames);
// Finishes the WAV dump (its header holds the length). Safe to call more than once.
void Close();
} // namespace host::audio
