// zlib125: the game links against the system's zlib 1.2.5; we forward to the host zlib.
// Guest z_stream (0x38 bytes, big-endian, 32-bit pointers): next_in, avail_in, total_in, next_out, avail_out,
// total_out, msg, state, zalloc, zfree, opaque, data_type, adler, reserved. Each guest stream is paired with
// a host z_stream; fields are copied across around every call.
#include "../gpu/dump.h"
#include "../imports.h"

#include <memory>
#include <mutex>
#include <unordered_map>

#include <zlib.h>

namespace cafe::zlib
{
namespace
{
constexpr uint32_t GuestStreamSize = 0x38;

std::mutex gMutex;
std::unordered_map<uint32_t, std::unique_ptr<z_stream>> gStreams;

z_stream* Host(uint32_t strm)
{
    std::lock_guard lock(gMutex);
    auto it = gStreams.find(strm);
    return it == gStreams.end() ? nullptr : it->second.get();
}

// Guest pointers become host pointers into guest memory and back.
void In(uint32_t g, z_stream& h)
{
    const uint32_t nextIn = guest::Read32(g + 0x00), nextOut = guest::Read32(g + 0x0C);
    h.next_in = nextIn ? guest::Ptr(nextIn) : nullptr;
    h.avail_in = guest::Read32(g + 0x04);
    h.next_out = nextOut ? guest::Ptr(nextOut) : nullptr;
    h.avail_out = guest::Read32(g + 0x10);
}

void Out(uint32_t g, const z_stream& h)
{
    auto guestAddr = [](const Bytef* p) { return p ? uint32_t(p - guest::base) : 0u; };
    guest::Write32(g + 0x00, guestAddr(h.next_in));
    guest::Write32(g + 0x04, h.avail_in);
    guest::Write32(g + 0x08, uint32_t(h.total_in));
    guest::Write32(g + 0x0C, guestAddr(h.next_out));
    guest::Write32(g + 0x10, h.avail_out);
    guest::Write32(g + 0x14, uint32_t(h.total_out));
    guest::Write32(g + 0x18, 0);  // msg: host strings are not visible to the guest
    guest::Write32(g + 0x2C, uint32_t(h.data_type));
    guest::Write32(g + 0x30, uint32_t(h.adler));
}

int32_t inflateInit_(uint32_t strm, GuestString version, int32_t streamSize)
{
    (void)version;
    if (streamSize != int32_t(GuestStreamSize))
        return Z_VERSION_ERROR;
    auto h = std::make_unique<z_stream>();
    In(strm, *h);
    const int r = ::inflateInit(h.get());
    Out(strm, *h);
    std::lock_guard lock(gMutex);
    gStreams[strm] = std::move(h);
    return r;
}

int32_t inflate(uint32_t strm, int32_t flush)
{
    z_stream* h = Host(strm);
    if (!h)
        return Z_STREAM_ERROR;
    In(strm, *h);
    const uint32_t out = guest::Read32(strm + 0x0C), avail = h->avail_out;
    const int r = ::inflate(h, flush);
    Out(strm, *h);
    gpu::dump::Copy(out, avail - h->avail_out);
    return r;
}

int32_t inflateReset(uint32_t strm)
{
    z_stream* h = Host(strm);
    if (!h)
        return Z_STREAM_ERROR;
    const int r = ::inflateReset(h);
    Out(strm, *h);
    return r;
}

int32_t inflateEnd(uint32_t strm)
{
    std::unique_ptr<z_stream> h;
    {
        std::lock_guard lock(gMutex);
        auto it = gStreams.find(strm);
        if (it == gStreams.end())
            return Z_STREAM_ERROR;
        h = std::move(it->second);
        gStreams.erase(it);
    }
    return ::inflateEnd(h.get());
}

// uncompress/compress/compress2(dest, uLongf* destLen, source, sourceLen[, level])
int32_t uncompress(uint32_t dest, uint32_t destLen, uint32_t source, uint32_t sourceLen)
{
    uLongf len = guest::Read32(destLen);
    const int r = ::uncompress(guest::Ptr(dest), &len, guest::Ptr(source), sourceLen);
    guest::Write32(destLen, uint32_t(len));
    gpu::dump::Copy(dest, uint32_t(len));
    return r;
}

int32_t compress2(uint32_t dest, uint32_t destLen, uint32_t source, uint32_t sourceLen, int32_t level)
{
    uLongf len = guest::Read32(destLen);
    const int r = ::compress2(guest::Ptr(dest), &len, guest::Ptr(source), sourceLen, level);
    guest::Write32(destLen, uint32_t(len));
    return r;
}

int32_t compress(uint32_t dest, uint32_t destLen, uint32_t source, uint32_t sourceLen)
{
    return compress2(dest, destLen, source, sourceLen, Z_DEFAULT_COMPRESSION);
}
} // namespace

CAFE_IMPORT(zlib125, "inflateInit_", inflateInit_);
CAFE_IMPORT(zlib125, "inflate", inflate);
CAFE_IMPORT(zlib125, "inflateReset", inflateReset);
CAFE_IMPORT(zlib125, "inflateEnd", inflateEnd);
CAFE_IMPORT(zlib125, "uncompress", uncompress);
CAFE_IMPORT(zlib125, "compress", compress);
CAFE_IMPORT(zlib125, "compress2", compress2);
} // namespace cafe::zlib
