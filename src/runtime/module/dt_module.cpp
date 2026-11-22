// The game module's own part (Windows releases): built with the generated code into dt_game.dll on the player's
// machine, by the setup tool's bundled clang, with no C or C++ library (see ppc_context.h, DT_MODULE). The runtime
// loads the DLL and calls dt_module_init with its table of functions and data; the module answers with the generated
// tables. The routines below are the few the compiler may call on its own in code without a C library.
#include "ppc_context.h"
#include "module_abi.h"

extern "C"
{
DtModuleHost dt_host;

// (MSVC-ABI objects that use floating point reference this symbol; the C library normally defines it)
int _fltused = 0;

// Block copies and fills the compiler emits for structure copies and large zeroing.
void* memcpy(void* dst, const void* src, size_t n)
{
    void* d = dst;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dst;
}

void* memmove(void* dst, const void* src, size_t n)
{
    unsigned char* d = static_cast<unsigned char*>(dst);
    const unsigned char* s = static_cast<const unsigned char*>(src);
    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);
    d += n - 1, s += n - 1;  // overlapping, destination above: copy backwards
    __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

void* memset(void* dst, int value, size_t n)
{
    void* d = dst;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(value) : "memory");
    return dst;
}

int memcmp(const void* a, const void* b, size_t n)
{
    const unsigned char* x = static_cast<const unsigned char*>(a);
    const unsigned char* y = static_cast<const unsigned char*>(b);
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

__declspec(dllexport) const DtModuleExports* dt_module_init(const DtModuleHost* host)
{
    static const DtModuleExports exports = { DT_MODULE_VERSION, PPCFuncMappings, PPCHookSlots, PPCImportMappings,
        PPCDataImports, DT_MODULE_ABI };
    if (!host || host->version != DT_MODULE_VERSION)
        return nullptr;
    dt_host = *host;
    return &exports;
}
}
