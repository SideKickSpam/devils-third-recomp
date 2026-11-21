#pragma once

#include <cstdarg>

namespace dtlog
{
// Messages go to the log file if one was opened with --log, otherwise to stderr.
void Open(const char* path);
void Write(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void WriteV(const char* fmt, va_list args);
// Also write every Important message to `path` (the session log; flushed per message, so it survives a crash).
void Mirror(const char* path);
// Always goes to stderr as well as the log file (and the mirror).
void Important(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Flush();
} // namespace dtlog
