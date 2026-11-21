// nn_ac (network connection), nn_ndm (network daemons): an offline console.
// nn::Result is a 32-bit code; bit 31 set means failure.
#include "../imports.h"

namespace cafe::nn
{
namespace
{
constexpr uint32_t ResultSuccess = 0;
// nn::ac "not connected" style failure (level fatal-ish, module ac). The exact description code is not
// observed by the game logic we have seen; the failure bit is what matters.
constexpr uint32_t ResultAcNotConnected = 0xA0335400;

namespace ac
{
uint32_t Initialize() { return ResultSuccess; }
void Finalize() {}
uint32_t Connect() { return ResultAcNotConnected; }
uint32_t ConnectAsync() { return ResultAcNotConnected; }
uint32_t Close() { return ResultSuccess; }
// Status: 0 = not connected.
uint32_t GetConnectStatus(uint32_t outStatus)
{
    if (outStatus)
        guest::Write32(outStatus, 0);
    return ResultSuccess;
}
uint32_t IsApplicationConnected(uint32_t outBool)
{
    if (outBool)
        guest::Write8(outBool, 0);
    return ResultSuccess;
}
uint32_t GetAssignedAddress(uint32_t out)
{
    if (out)
        guest::Write32(out, 0);
    return ResultAcNotConnected;
}
uint32_t GetAssignedSubnet(uint32_t out)
{
    if (out)
        guest::Write32(out, 0);
    return ResultAcNotConnected;
}
uint32_t GetLastErrorCode(uint32_t out)
{
    if (out)
        guest::Write32(out, 0);
    return ResultSuccess;
}
} // namespace ac

namespace ndm
{
uint32_t Initialize() { return ResultSuccess; }
uint32_t Finalize() { return ResultSuccess; }
uint32_t SuspendDaemons(uint32_t mask) { (void)mask; return ResultSuccess; }
uint32_t ResumeDaemons(uint32_t mask) { (void)mask; return ResultSuccess; }
} // namespace ndm
} // namespace

CAFE_IMPORT(nn_ac, "Initialize__Q2_2nn2acFv", ac::Initialize);
CAFE_IMPORT(nn_ac, "Finalize__Q2_2nn2acFv", ac::Finalize);
CAFE_IMPORT(nn_ac, "Connect__Q2_2nn2acFv", ac::Connect);
CAFE_IMPORT(nn_ac, "ConnectAsync__Q2_2nn2acFv", ac::ConnectAsync);
CAFE_IMPORT(nn_ac, "Close__Q2_2nn2acFv", ac::Close);
CAFE_IMPORT(nn_ac, "GetConnectStatus__Q2_2nn2acFPQ3_2nn2ac6Status", ac::GetConnectStatus);
CAFE_IMPORT(nn_ac, "IsApplicationConnected__Q2_2nn2acFPb", ac::IsApplicationConnected);
CAFE_IMPORT(nn_ac, "GetAssignedAddress__Q2_2nn2acFPUl", ac::GetAssignedAddress);
CAFE_IMPORT(nn_ac, "GetAssignedSubnet__Q2_2nn2acFPUl", ac::GetAssignedSubnet);
CAFE_IMPORT(nn_ac, "GetLastErrorCode__Q2_2nn2acFPUi", ac::GetLastErrorCode);
CAFE_IMPORT(nn_ndm, "Initialize__Q2_2nn3ndmFv", ndm::Initialize);
CAFE_IMPORT(nn_ndm, "Finalize__Q2_2nn3ndmFv", ndm::Finalize);
CAFE_IMPORT(nn_ndm, "SuspendDaemons__Q2_2nn3ndmFUi", ndm::SuspendDaemons);
CAFE_IMPORT(nn_ndm, "ResumeDaemons__Q2_2nn3ndmFUi", ndm::ResumeDaemons);
} // namespace cafe::nn

// nsysnet: socket library on an offline console.
namespace cafe::nsysnet
{
namespace
{
int32_t gLastError = 0;
int32_t socket_lib_init() { return 0; }
int32_t socket_lib_finish() { return 0; }
int32_t socketlasterr() { return gLastError; }
int32_t NSSLInit() { return 0; }
int32_t NSSLFinish() { return 0; }
// SSL contexts can be created (they only hold configuration); connections cannot be made offline.
constexpr int32_t NSSL_ERROR_GENERIC = -2621441;
int32_t gNextSslContext = 1;
int32_t NSSLCreateContext(int32_t version) { (void)version; return gNextSslContext++; }
int32_t NSSLDestroyContext(int32_t context) { (void)context; return 0; }
int32_t NSSLAddServerPKIGroups(int32_t context, uint32_t groups, uint32_t outValid, uint32_t outInvalid)
{
    (void)context, (void)groups;
    if (outValid)
        guest::Write32(outValid, 0);
    if (outInvalid)
        guest::Write32(outInvalid, 0);
    return 0;
}
int32_t NSSLCreateConnection(int32_t context, int32_t socket, uint32_t options, int32_t a, int32_t b)
{
    (void)context, (void)socket, (void)options, (void)a, (void)b;
    return NSSL_ERROR_GENERIC;
}
// Espresso is big-endian, so network byte order conversions are identities.
uint32_t Identity32(uint32_t v) { return v; }
uint32_t Identity16(uint32_t v) { return v & 0xFFFF; }
} // namespace

CAFE_IMPORT(nsysnet, "socket_lib_init", socket_lib_init);
CAFE_IMPORT(nsysnet, "NSSLInit", NSSLInit);
CAFE_IMPORT(nsysnet, "NSSLFinish", NSSLFinish);
CAFE_IMPORT(nsysnet, "NSSLCreateContext", NSSLCreateContext);
CAFE_IMPORT(nsysnet, "NSSLDestroyContext", NSSLDestroyContext);
CAFE_IMPORT(nsysnet, "NSSLAddServerPKIGroups", NSSLAddServerPKIGroups);
CAFE_IMPORT(nsysnet, "NSSLCreateConnection", NSSLCreateConnection);
CAFE_IMPORT(nsysnet, "socket_lib_finish", socket_lib_finish);
CAFE_IMPORT(nsysnet, "socketlasterr", socketlasterr);
CAFE_IMPORT(nsysnet, "htonl", Identity32);
CAFE_IMPORT(nsysnet, "ntohl", Identity32);
CAFE_IMPORT(nsysnet, "htons", Identity16);
CAFE_IMPORT(nsysnet, "ntohs", Identity16);
} // namespace cafe::nsysnet

// nlibcurl: an offline network. Handles are real guest allocations so the game's bookkeeping works,
// the multi interface never has running transfers, and blocking transfers fail to connect.
namespace cafe::nlibcurl
{
namespace
{
constexpr int32_t CURLE_OK = 0, CURLE_COULDNT_CONNECT = 7;

int32_t curl_global_init(uint32_t flags) { (void)flags; return CURLE_OK; }
void curl_global_cleanup() {}

uint32_t curl_multi_init() { return guest::SystemAlloc(0x20); }
int32_t curl_multi_cleanup(uint32_t multi) { guest::SystemFree(multi); return 0; }
int32_t curl_multi_add_handle(uint32_t multi, uint32_t easy) { (void)multi, (void)easy; return 0; }
int32_t curl_multi_remove_handle(uint32_t multi, uint32_t easy) { (void)multi, (void)easy; return 0; }
int32_t curl_multi_perform(uint32_t multi, uint32_t outRunning)
{
    (void)multi;
    if (outRunning)
        guest::Write32(outRunning, 0);
    return 0;
}
uint32_t curl_multi_info_read(uint32_t multi, uint32_t outQueued)
{
    (void)multi;
    if (outQueued)
        guest::Write32(outQueued, 0);
    return 0;
}

uint32_t curl_easy_init() { return guest::SystemAlloc(0x20); }
void curl_easy_cleanup(uint32_t easy) { guest::SystemFree(easy); }
int32_t curl_easy_perform(uint32_t easy) { (void)easy; return CURLE_COULDNT_CONNECT; }
// curl_easy_setopt/getinfo are variadic; their third argument is in r5.
void curl_easy_setopt(PPCContext& ctx, uint8_t*) { ctx.r[3].s32 = CURLE_OK; }
void curl_easy_getinfo(PPCContext& ctx, uint8_t*)
{
    if (ctx.r[5].u32)
        guest::Write32(ctx.r[5].u32, 0);  // e.g. CURLINFO_RESPONSE_CODE: no response
    ctx.r[3].s32 = CURLE_OK;
}

// curl_slist: {char* data; curl_slist* next}. Strings are copied, as libcurl does.
uint32_t curl_slist_append(uint32_t list, GuestString str)
{
    const std::string s = str.str();
    const uint32_t node = guest::SystemAlloc(8), copy = guest::SystemAlloc(uint32_t(s.size() + 1));
    guest::WriteString(copy, s, s.size() + 1);
    guest::Write32(node, copy);
    guest::Write32(node + 4, 0);
    if (!list)
        return node;
    uint32_t last = list;
    while (guest::Read32(last + 4))
        last = guest::Read32(last + 4);
    guest::Write32(last + 4, node);
    return list;
}
void curl_slist_free_all(uint32_t list)
{
    while (list)
    {
        const uint32_t next = guest::Read32(list + 4);
        guest::SystemFree(guest::Read32(list));
        guest::SystemFree(list);
        list = next;
    }
}
} // namespace

CAFE_IMPORT(nlibcurl, "curl_global_init", curl_global_init);
CAFE_IMPORT(nlibcurl, "curl_global_cleanup", curl_global_cleanup);
CAFE_IMPORT(nlibcurl, "curl_multi_init", curl_multi_init);
CAFE_IMPORT(nlibcurl, "curl_multi_cleanup", curl_multi_cleanup);
CAFE_IMPORT(nlibcurl, "curl_multi_add_handle", curl_multi_add_handle);
CAFE_IMPORT(nlibcurl, "curl_multi_remove_handle", curl_multi_remove_handle);
CAFE_IMPORT(nlibcurl, "curl_multi_perform", curl_multi_perform);
CAFE_IMPORT(nlibcurl, "curl_multi_info_read", curl_multi_info_read);
CAFE_IMPORT(nlibcurl, "curl_easy_init", curl_easy_init);
CAFE_IMPORT(nlibcurl, "curl_easy_cleanup", curl_easy_cleanup);
CAFE_IMPORT(nlibcurl, "curl_easy_perform", curl_easy_perform);
CAFE_IMPORT_RAW(nlibcurl, "curl_easy_setopt", curl_easy_setopt);
CAFE_IMPORT_RAW(nlibcurl, "curl_easy_getinfo", curl_easy_getinfo);
CAFE_IMPORT(nlibcurl, "curl_slist_append", curl_slist_append);
CAFE_IMPORT(nlibcurl, "curl_slist_free_all", curl_slist_free_all);
} // namespace cafe::nlibcurl
