#include "log.h"

#include <cstdio>
#include <mutex>

namespace dtlog
{
namespace
{
std::mutex gMutex;
FILE* gFile = nullptr;
FILE* gMirror = nullptr;
} // namespace

void Mirror(const char* path)
{
    std::lock_guard lock(gMutex);
    gMirror = fopen(path, "w");
}

void Open(const char* path)
{
    gFile = fopen(path, "w");
    if (!gFile)
        perror(path);
}

void WriteV(const char* fmt, va_list args)
{
    std::lock_guard lock(gMutex);
    vfprintf(gFile ? gFile : stderr, fmt, args);
}

void Write(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    WriteV(fmt, args);
    va_end(args);
}

void Important(const char* fmt, ...)
{
    std::lock_guard lock(gMutex);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    if (gFile)
    {
        va_start(args, fmt);
        vfprintf(gFile, fmt, args);
        va_end(args);
    }
    if (gMirror)
    {
        va_start(args, fmt);
        vfprintf(gMirror, fmt, args);
        va_end(args);
        fflush(gMirror);
    }
}

void Flush()
{
    std::lock_guard lock(gMutex);
    fflush(stderr);
    if (gFile)
        fflush(gFile);
}
} // namespace dtlog
