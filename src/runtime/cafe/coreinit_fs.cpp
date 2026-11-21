// coreinit FS: file access through the /vol path mapping (vfs).
// Async calls return at once; the FS worker thread runs them in order and the result is delivered afterwards,
// as on the Wii U: FSAsyncData callbacks run on the AppIO thread, otherwise the result message goes to the caller's
// queue.
#include "../imports.h"
#include "../log.h"
#include "../platform.h"
#include "../trace.h"
#include "gx2.h"
#include "../vfs.h"
#include "coreinit.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <unordered_map>


namespace cafe::coreinit
{
namespace
{
enum FSStatus : int32_t
{
    FS_STATUS_OK = 0,
    FS_STATUS_END = -2,
    FS_STATUS_EXISTS = -5,
    FS_STATUS_NOT_FOUND = -6,
    FS_STATUS_NOT_FILE = -7,
    FS_STATUS_ACCESS_ERROR = -9,
    FS_STATUS_PERMISSION_ERROR = -10,
    FS_STATUS_FATAL_ERROR = -0x400,
};

struct OpenFile
{
    FILE* f = nullptr;
    std::string guestPath;
    std::mutex m;  // seek + read/write pairs are atomic: async commands run on the FS worker, sync ones on callers
    ~OpenFile()
    {
        if (f)
            fclose(f);
    }
};

std::mutex gFilesMutex;
std::unordered_map<uint32_t, std::unique_ptr<OpenFile>> gFiles;
uint32_t gNextHandle = 1;
std::unordered_map<uint32_t, std::string> gCwd;  // per client

OpenFile* GetFile(uint32_t handle)
{
    std::lock_guard lock(gFilesMutex);
    auto it = gFiles.find(handle);
    return it == gFiles.end() ? nullptr : it->second.get();
}

std::string GuestPath(uint32_t client, GuestString path)
{
    std::lock_guard lock(gFilesMutex);
    auto it = gCwd.find(client);
    return vfs::Normalize(path.str(), it == gCwd.end() ? "/" : it->second);
}

// ---- operations shared by sync and async entry points ----
int32_t DoOpen(uint32_t client, GuestString path, GuestString mode, uint32_t outHandle)
{
    return FsOpen(GuestPath(client, path), mode.str(), outHandle);
}
} // namespace

int32_t FsOpenTimed(const std::string& gp, const std::string& m, uint32_t outHandle);

// Opening is slow where the game directory is a network share (\\wsl.localhost): report opens over 5 ms like reads.
int32_t FsOpen(const std::string& gp, const std::string& m, uint32_t outHandle)
{
    const auto start = std::chrono::steady_clock::now();
    const int32_t result = FsOpenTimed(gp, m, outHandle);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (ms > 5)
        dtlog::Important("[fs] slow open: %.0f ms, %s on %s (frame %llu)\n", ms, gp.c_str(),
            trace::CurrentThreadName().c_str(), (unsigned long long)gx2::FrameCount());
    return result;
}

int32_t FsOpenTimed(const std::string& gp, const std::string& m, uint32_t outHandle)
{
    const bool write = m.find_first_of("wa+") != std::string::npos;
    std::optional<std::string> host = vfs::Resolve(gp);
    if (write)
        host = vfs::ResolveForWrite(gp);
    if (!host)
    {
        dtlog::Write("FS: %s not found\n", gp.c_str());
        return write ? FS_STATUS_PERMISSION_ERROR : FS_STATUS_NOT_FOUND;
    }
    if (std::filesystem::is_directory(*host))
        return FS_STATUS_NOT_FILE;
    std::string hostMode = m;
    if (hostMode.find('b') == std::string::npos)
        hostMode += 'b';
    FILE* f = fopen(host->c_str(), hostMode.c_str());
    if (!f)
        return write ? FS_STATUS_ACCESS_ERROR : FS_STATUS_NOT_FOUND;
    std::lock_guard lock(gFilesMutex);
    const uint32_t handle = gNextHandle++;
    auto of = std::make_unique<OpenFile>();
    of->f = f;
    of->guestPath = gp;
    gFiles[handle] = std::move(of);
    guest::Write32(outHandle, handle);
    return FS_STATUS_OK;
}

namespace
{

int32_t DoRead(uint32_t dst, uint32_t size, uint32_t count, uint32_t handle, int64_t pos)
{
    OpenFile* of = GetFile(handle);
    if (!of)
        return FS_STATUS_FATAL_ERROR;
    const auto lockStart = std::chrono::steady_clock::now();
    std::lock_guard fileLock(of->m);
    const auto start = std::chrono::steady_clock::now();
    const double cpuStart = platform::ThreadCpuSeconds();
    if (pos >= 0)
        platform::FileSeek(of->f, pos);
    const auto seekEnd = std::chrono::steady_clock::now();
    const size_t bytes = size_t(size) * count;
    const size_t n = fread(guest::Ptr(dst), 1, bytes, of->f);
    // A slow read stalls the calling guest thread (async reads complete on it too): report those over 5 ms, with the
    // time waiting for the file's lock, seeking, and the thread's own CPU time (low CPU time: it was waiting).
    const auto end = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - start).count();
    if (ms > 5)
        dtlog::Important("[fs] slow read: %.0f ms (seek %.1f, cpu %.1f, lock wait %.1f), %zu KB of %s on %s (frame %llu)\n",
            ms, std::chrono::duration<double, std::milli>(seekEnd - start).count(),
            (platform::ThreadCpuSeconds() - cpuStart) * 1000, std::chrono::duration<double, std::milli>(start - lockStart).count(),
            bytes / 1024, of->guestPath.c_str(), trace::CurrentThreadName().c_str(), (unsigned long long)gx2::FrameCount());
    return size ? int32_t(n / size) : 0;  // elements read
}

int32_t DoWrite(uint32_t src, uint32_t size, uint32_t count, uint32_t handle, int64_t pos)
{
    OpenFile* of = GetFile(handle);
    if (!of)
        return FS_STATUS_FATAL_ERROR;
    std::lock_guard fileLock(of->m);
    if (pos >= 0)
        platform::FileSeek(of->f, pos);
    const size_t n = fwrite(guest::Ptr(src), 1, size_t(size) * count, of->f);
    return size ? int32_t(n / size) : 0;
}

int32_t DoClose(uint32_t handle)
{
    std::lock_guard lock(gFilesMutex);
    return gFiles.erase(handle) ? FS_STATUS_OK : FS_STATUS_FATAL_ERROR;
}

// FSStat (0x64 bytes): flags, mode, owner, group, size, allocSize, quotaSize (u64), entryId, created (u64),
// modified (u64), attributes.
void WriteStat(uint32_t out, uint64_t size, bool dir)
{
    memset(guest::Ptr(out), 0, 0x64);
    guest::Write32(out + 0x00, dir ? 0x80000000u : 0x01000000u);
    guest::Write32(out + 0x04, 0x666);
    guest::Write32(out + 0x10, uint32_t(size));
    guest::Write32(out + 0x14, uint32_t(size));
}

int32_t DoStatFile(uint32_t handle, uint32_t out)
{
    OpenFile* of = GetFile(handle);
    if (!of)
        return FS_STATUS_FATAL_ERROR;
    std::lock_guard fileLock(of->m);
    const int64_t size = platform::FileSize(of->f);
    if (size < 0)
        return FS_STATUS_FATAL_ERROR;
    WriteStat(out, uint64_t(size), false);
    return FS_STATUS_OK;
}

// ---- async delivery ----
// FSAsyncData: callback, param, ioMsgQueue. FSAsyncResult (0x28): asyncData (0xC), ioMsg (an OSMessage pointing back
// at the result, type 8 = FS_CMD_ASYNC), client @0x1C, block @0x20, status @0x24. The game reads the status at
// 0x24 (FSaveGameDataAsyncTask::ProcessAsyncResults).
std::mutex gResultsMutex;
std::unordered_map<uint32_t, uint32_t> gResults;  // cmd block -> FSAsyncResult in system memory
} // namespace

namespace
{
// ---- the FS worker and the AppIO thread ----
// FSAsyncData as it was when the command was issued: the Wii U copies it into the command block, and the
// caller's struct may be gone (or reused) by the time the command completes.
struct AsyncData
{
    uint32_t callback, param, queue;
};

struct AsyncCommand
{
    uint32_t client, block;
    AsyncData data;
    std::function<int32_t()> op;
};
std::mutex gAsyncMutex;
std::condition_variable gAsyncCv;
std::deque<AsyncCommand> gAsyncQueue;

struct AppIoCallback
{
    uint32_t callback, client, block, status, param;
};
std::mutex gAppIoMutex;
std::condition_variable gAppIoCv;
std::deque<AppIoCallback> gAppIoQueue;

// The AppIO thread's body (a guest thread running this host function): FSAsyncData callbacks, one at a time.
void AppIoThreadMain(PPCContext&, uint8_t*)
{
    for (;;)
    {
        AppIoCallback c;
        {
            std::unique_lock lock(gAppIoMutex);
            gAppIoCv.wait(lock, [] { return !gAppIoQueue.empty(); });
            c = gAppIoQueue.front();
            gAppIoQueue.pop_front();
        }
        guest::Call(c.callback, { c.client, c.block, c.status, c.param });
    }
}

// Write the command's FSAsyncResult and hand it on: the callback to the AppIO thread, else the message to the queue.
void Deliver(uint32_t client, uint32_t block, int32_t status, const AsyncData& data)
{
    uint32_t result;
    {
        std::lock_guard lock(gResultsMutex);
        uint32_t& r = gResults[block];
        if (!r)
            r = guest::SystemAlloc(0x28);
        result = r;
    }
    const uint32_t callback = data.callback, param = data.param, queue = data.queue;
    // OSMessage {message = FSAsyncResult*, args = {0, 0, OS_FUNCTION_TYPE_FS_CMD_ASYNC (8)}}
    const uint32_t msg[4] = { result, 0, 0, 8 };
    guest::Write32(result + 0x0, callback);
    guest::Write32(result + 0x4, param);
    guest::Write32(result + 0x8, queue);
    for (int i = 0; i < 4; i++)
        guest::Write32(result + 0x0C + i * 4, msg[i]);
    guest::Write32(result + 0x1C, client);
    guest::Write32(result + 0x20, block);
    guest::Write32(result + 0x24, uint32_t(status));
    if (callback)
    {
        std::lock_guard lock(gAppIoMutex);
        gAppIoQueue.push_back({ callback, client, block, uint32_t(status), param });
        gAppIoCv.notify_one();
    }
    else if (queue)
        SendMessage(queue, msg, true);
}

void FsWorkerMain()
{
    for (;;)
    {
        AsyncCommand c;
        {
            std::unique_lock lock(gAsyncMutex);
            gAsyncCv.wait(lock, [] { return !gAsyncQueue.empty(); });
            c = std::move(gAsyncQueue.front());
            gAsyncQueue.pop_front();
        }
        Deliver(c.client, c.block, c.op(), c.data);
    }
}

void StartAsyncThreads()
{
    static std::once_flag once;
    std::call_once(once, [] {
        platform::StartThread(FsWorkerMain);
        const uint32_t entry = RegisterHostFunction("coreinit", "__AppIoThread", &AppIoThreadMain, nullptr, nullptr);
        StartSystemThread("AppIO", entry, 1);
    });
}
} // namespace

int32_t FsAsync(uint32_t client, uint32_t block, uint32_t asyncData, std::function<int32_t()> op)
{
    if (!asyncData)
        return op();
    StartAsyncThreads();
    {
        std::lock_guard lock(gAsyncMutex);
        const AsyncData data{ guest::Read32(asyncData), guest::Read32(asyncData + 4), guest::Read32(asyncData + 8) };
        gAsyncQueue.push_back({ client, block, data, std::move(op) });
    }
    gAsyncCv.notify_one();
    return FS_STATUS_OK;
}

int32_t FsComplete(uint32_t client, uint32_t block, int32_t status, uint32_t asyncData)
{
    return FsAsync(client, block, asyncData, [status] { return status; });
}

namespace
{

// ---- imports ----
void FSInit() {}
int32_t FSAddClient(uint32_t client, uint32_t errHandling) { (void)client, (void)errHandling; return FS_STATUS_OK; }
int32_t FSDelClient(uint32_t client, uint32_t errHandling) { (void)client, (void)errHandling; return FS_STATUS_OK; }
void FSInitCmdBlock(uint32_t block) { memset(guest::Ptr(block), 0, 0xA80); }
void FSSetStateChangeNotification(uint32_t client, uint32_t info) { (void)client, (void)info; }
int32_t FSGetLastErrorCodeForViewer(uint32_t client) { (void)client; return 0; }

int32_t FSOpenFile(uint32_t client, uint32_t block, GuestString path, GuestString mode, uint32_t outHandle, uint32_t err)
{
    (void)block, (void)err;
    return DoOpen(client, path, mode, outHandle);
}

int32_t FSReadFile(uint32_t client, uint32_t block, uint32_t dst, uint32_t size, uint32_t count, uint32_t handle,
    uint32_t flag, uint32_t err)
{
    (void)client, (void)block, (void)flag, (void)err;
    return DoRead(dst, size, count, handle, -1);
}

int32_t FSReadFileAsync(uint32_t client, uint32_t block, uint32_t dst, uint32_t size, uint32_t count, uint32_t handle,
    uint32_t flag, uint32_t err, uint32_t async)
{
    (void)flag, (void)err;
    return FsAsync(client, block, async, [=] { return DoRead(dst, size, count, handle, -1); });
}

int32_t FSReadFileWithPosAsync(uint32_t client, uint32_t block, uint32_t dst, uint32_t size, uint32_t count, uint32_t pos,
    uint32_t handle, uint32_t flag, uint32_t err, uint32_t async)
{
    (void)flag, (void)err;
    return FsAsync(client, block, async, [=] { return DoRead(dst, size, count, handle, pos); });
}

int32_t FSWriteFile(uint32_t client, uint32_t block, uint32_t src, uint32_t size, uint32_t count, uint32_t handle,
    uint32_t flag, uint32_t err)
{
    (void)client, (void)block, (void)flag, (void)err;
    return DoWrite(src, size, count, handle, -1);
}

int32_t FSWriteFileWithPos(uint32_t client, uint32_t block, uint32_t src, uint32_t size, uint32_t count, uint32_t pos,
    uint32_t handle, uint32_t flag, uint32_t err)
{
    (void)client, (void)block, (void)flag, (void)err;
    return DoWrite(src, size, count, handle, pos);
}

int32_t FSWriteFileAsync(uint32_t client, uint32_t block, uint32_t src, uint32_t size, uint32_t count, uint32_t handle,
    uint32_t flag, uint32_t err, uint32_t async)
{
    (void)flag, (void)err;
    return FsAsync(client, block, async, [=] { return DoWrite(src, size, count, handle, -1); });
}

int32_t FSCloseFile(uint32_t client, uint32_t block, uint32_t handle, uint32_t err)
{
    (void)client, (void)block, (void)err;
    return DoClose(handle);
}

int32_t FSCloseFileAsync(uint32_t client, uint32_t block, uint32_t handle, uint32_t err, uint32_t async)
{
    (void)err;
    return FsAsync(client, block, async, [=] { return DoClose(handle); });
}

int32_t FSGetStatFile(uint32_t client, uint32_t block, uint32_t handle, uint32_t out, uint32_t err)
{
    (void)client, (void)block, (void)err;
    return DoStatFile(handle, out);
}

int32_t FSGetStatFileAsync(uint32_t client, uint32_t block, uint32_t handle, uint32_t out, uint32_t err, uint32_t async)
{
    (void)err;
    return FsAsync(client, block, async, [=] { return DoStatFile(handle, out); });
}

int32_t FSGetPosFile(uint32_t client, uint32_t block, uint32_t handle, uint32_t outPos, uint32_t err)
{
    (void)client, (void)block, (void)err;
    OpenFile* of = GetFile(handle);
    if (!of)
        return FS_STATUS_FATAL_ERROR;
    std::lock_guard fileLock(of->m);
    guest::Write32(outPos, uint32_t(platform::FileTell(of->f)));
    return FS_STATUS_OK;
}

int32_t FSSetPosFile(uint32_t client, uint32_t block, uint32_t handle, uint32_t pos, uint32_t err)
{
    (void)client, (void)block, (void)err;
    OpenFile* of = GetFile(handle);
    if (!of)
        return FS_STATUS_FATAL_ERROR;
    std::lock_guard fileLock(of->m);
    return platform::FileSeek(of->f, pos) == 0 ? FS_STATUS_OK : FS_STATUS_FATAL_ERROR;
}

int32_t FSMakeDir(uint32_t client, uint32_t block, GuestString path, uint32_t err)
{
    (void)block, (void)err;
    auto host = vfs::ResolveForWrite(GuestPath(client, path));
    if (!host)
        return FS_STATUS_PERMISSION_ERROR;
    std::error_code ec;
    if (std::filesystem::exists(*host))
        return FS_STATUS_EXISTS;
    return std::filesystem::create_directories(*host, ec) ? FS_STATUS_OK : FS_STATUS_ACCESS_ERROR;
}

int32_t FSRemove(uint32_t client, uint32_t block, GuestString path, uint32_t err)
{
    (void)block, (void)err;
    auto host = vfs::ResolveForWrite(GuestPath(client, path));
    std::error_code ec;
    if (!host || !std::filesystem::exists(*host))
        return FS_STATUS_NOT_FOUND;
    return std::filesystem::remove(*host, ec) ? FS_STATUS_OK : FS_STATUS_ACCESS_ERROR;
}

int32_t FSRename(uint32_t client, uint32_t block, GuestString from, GuestString to, uint32_t err)
{
    (void)block, (void)err;
    auto a = vfs::ResolveForWrite(GuestPath(client, from)), b = vfs::ResolveForWrite(GuestPath(client, to));
    if (!a || !b)
        return FS_STATUS_PERMISSION_ERROR;
    std::error_code ec;
    std::filesystem::rename(*a, *b, ec);
    return ec ? FS_STATUS_NOT_FOUND : FS_STATUS_OK;
}

uint32_t FSGetAsyncResult(uint32_t msg) { return guest::Read32(msg); }
} // namespace

CAFE_IMPORT(coreinit, "FSInit", FSInit);
CAFE_IMPORT(coreinit, "FSAddClient", FSAddClient);
CAFE_IMPORT(coreinit, "FSDelClient", FSDelClient);
CAFE_IMPORT(coreinit, "FSInitCmdBlock", FSInitCmdBlock);
CAFE_IMPORT(coreinit, "FSSetStateChangeNotification", FSSetStateChangeNotification);
CAFE_IMPORT(coreinit, "FSGetLastErrorCodeForViewer", FSGetLastErrorCodeForViewer);
CAFE_IMPORT(coreinit, "FSOpenFile", FSOpenFile);
CAFE_IMPORT(coreinit, "FSReadFile", FSReadFile);
CAFE_IMPORT(coreinit, "FSReadFileAsync", FSReadFileAsync);
CAFE_IMPORT(coreinit, "FSReadFileWithPosAsync", FSReadFileWithPosAsync);
CAFE_IMPORT(coreinit, "FSWriteFile", FSWriteFile);
CAFE_IMPORT(coreinit, "FSWriteFileWithPos", FSWriteFileWithPos);
CAFE_IMPORT(coreinit, "FSWriteFileAsync", FSWriteFileAsync);
CAFE_IMPORT(coreinit, "FSCloseFile", FSCloseFile);
CAFE_IMPORT(coreinit, "FSCloseFileAsync", FSCloseFileAsync);
CAFE_IMPORT(coreinit, "FSGetStatFile", FSGetStatFile);
CAFE_IMPORT(coreinit, "FSGetStatFileAsync", FSGetStatFileAsync);
CAFE_IMPORT(coreinit, "FSGetPosFile", FSGetPosFile);
CAFE_IMPORT(coreinit, "FSSetPosFile", FSSetPosFile);
CAFE_IMPORT(coreinit, "FSMakeDir", FSMakeDir);
CAFE_IMPORT(coreinit, "FSRemove", FSRemove);
CAFE_IMPORT(coreinit, "FSRename", FSRename);
CAFE_IMPORT(coreinit, "FSGetAsyncResult", FSGetAsyncResult);
} // namespace cafe::coreinit
