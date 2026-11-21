// nn_act (accounts), nn_fp (friends) and nn_save (save data) for an offline console with one local user in
// slot 1 who has no Nintendo Network ID.
#include "../imports.h"
#include "../log.h"
#include "../vfs.h"
#include "coreinit.h"

#include <filesystem>
#include <format>

namespace cafe::account
{
namespace
{
constexpr uint32_t ResultSuccess = 0;
// Failure results (bit 31 set). Exact description codes are not observed by the game logic seen so far.
constexpr uint32_t ResultActNotNetworkAccount = 0xA0335000;
constexpr uint32_t ResultFpOffline = 0xC0C00700;
constexpr uint8_t LocalSlot = 1;
constexpr uint8_t CommonSlot = 0xFF;

// ---- nn::act ----
uint32_t ActInitialize() { return ResultSuccess; }
uint32_t ActFinalize() { return ResultSuccess; }
uint8_t GetSlotNo() { return LocalSlot; }
uint32_t GetPrincipalId() { return 0; }  // no Nintendo Network ID
bool IsNetworkAccount() { return false; }
bool HasNfsAccount() { return false; }
// GetAccountId(char out[17]): the NNID. A local account has none.
uint32_t GetAccountId(uint32_t out)
{
    if (out)
        guest::Write8(out, 0);
    return ResultActNotNetworkAccount;
}

// ---- nn::fp ----
bool gFpInitialized = false;
uint32_t FpInitialize()
{
    gFpInitialized = true;
    return ResultSuccess;
}
uint32_t FpFinalize()
{
    gFpInitialized = false;
    return ResultSuccess;
}
bool FpIsInitialized() { return gFpInitialized; }
bool FpIsOnline() { return false; }
// GetFriendList/GetBlackList(u32* pids, u32* count, u32 offset, u32 max): nobody.
uint32_t GetPrincipalList(uint32_t pids, uint32_t outCount, uint32_t offset, uint32_t max)
{
    (void)pids, (void)offset, (void)max;
    if (outCount)
        guest::Write32(outCount, 0);
    return ResultSuccess;
}
uint32_t UpdateGameModeDescription(uint32_t text) { (void)text; return ResultFpOffline; }
uint32_t AddRecentPlayRecord(uint32_t records, uint32_t count) { (void)records, (void)count; return ResultSuccess; }
uint32_t RegisterAccountAsync(uint32_t callback, uint32_t context) { (void)callback, (void)context; return ResultFpOffline; }
uint32_t ResultToErrorCode(uint32_t result) { return result ? 1010000 + (result & 0x3FF) : 0; }

// ---- nn_save ----
// Account saves live in /vol/save/<persistent id>, shared ones in /vol/save/common.
std::string SaveDir(uint8_t slot)
{
    return slot == CommonSlot ? "/vol/save/common" : std::format("/vol/save/{:08x}", 0x80000000u | slot);
}

int32_t SAVEInit() { return 0; }

int32_t SAVEInitSaveDir(uint8_t slot)
{
    if (auto host = vfs::ResolveForWrite(SaveDir(slot)))
    {
        std::error_code ec;
        std::filesystem::create_directories(*host, ec);
    }
    if (auto common = vfs::ResolveForWrite(SaveDir(CommonSlot)))
    {
        std::error_code ec;
        std::filesystem::create_directories(*common, ec);
    }
    return 0;
}

int32_t SAVEOpenFile(uint32_t client, uint32_t block, uint8_t slot, GuestString path, GuestString mode, uint32_t outHandle,
    uint32_t errMask)
{
    (void)client, (void)block, (void)errMask;
    return coreinit::FsOpen(vfs::Normalize(SaveDir(slot) + "/" + path.str()), mode.str(), outHandle);
}

int32_t SAVEOpenFileAsync(uint32_t client, uint32_t block, uint8_t slot, GuestString path, GuestString mode,
    uint32_t outHandle, uint32_t errMask, uint32_t async)
{
    (void)errMask;
    // Strings are read now: the guest may reuse its buffers once the call returns.
    std::string guestPath = vfs::Normalize(SaveDir(slot) + "/" + path.str()), m = mode.str();
    return coreinit::FsAsync(client, block, async, [guestPath, m, outHandle] { return coreinit::FsOpen(guestPath, m, outHandle); });
}

int32_t SAVEFlushQuotaAsync(uint32_t client, uint32_t block, uint8_t slot, uint32_t errMask, uint32_t async)
{
    (void)slot, (void)errMask;
    return coreinit::FsComplete(client, block, 0, async);
}
} // namespace

CAFE_IMPORT(nn_act, "Initialize__Q2_2nn3actFv", ActInitialize);
CAFE_IMPORT(nn_act, "Finalize__Q2_2nn3actFv", ActFinalize);
CAFE_IMPORT(nn_act, "GetSlotNo__Q2_2nn3actFv", GetSlotNo);
CAFE_IMPORT(nn_act, "GetPrincipalId__Q2_2nn3actFv", GetPrincipalId);
CAFE_IMPORT(nn_act, "IsNetworkAccount__Q2_2nn3actFv", IsNetworkAccount);
CAFE_IMPORT(nn_act, "HasNfsAccount__Q2_2nn3actFv", HasNfsAccount);
CAFE_IMPORT(nn_act, "GetAccountId__Q2_2nn3actFPc", GetAccountId);
CAFE_IMPORT(nn_fp, "Initialize__Q2_2nn2fpFv", FpInitialize);
CAFE_IMPORT(nn_fp, "Finalize__Q2_2nn2fpFv", FpFinalize);
CAFE_IMPORT(nn_fp, "IsInitialized__Q2_2nn2fpFv", FpIsInitialized);
CAFE_IMPORT(nn_fp, "IsOnline__Q2_2nn2fpFv", FpIsOnline);
CAFE_IMPORT(nn_fp, "GetFriendList__Q2_2nn2fpFPUiT1UiT3", GetPrincipalList);
CAFE_IMPORT(nn_fp, "GetBlackList__Q2_2nn2fpFPUiT1UiT3", GetPrincipalList);
CAFE_IMPORT(nn_fp, "UpdateGameModeDescription__Q2_2nn2fpFPCw", UpdateGameModeDescription);
CAFE_IMPORT(nn_fp, "AddRecentPlayRecord__Q2_2nn2fpFPCQ3_2nn2fp16RecentPlayRecordUi", AddRecentPlayRecord);
CAFE_IMPORT(nn_fp, "RegisterAccountAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv", RegisterAccountAsync);
CAFE_IMPORT(nn_fp, "ResultToErrorCode__Q2_2nn2fpFQ2_2nn6Result", ResultToErrorCode);
CAFE_IMPORT(nn_save, "SAVEInit", SAVEInit);
CAFE_IMPORT(nn_save, "SAVEInitSaveDir", SAVEInitSaveDir);
CAFE_IMPORT(nn_save, "SAVEOpenFile", SAVEOpenFile);
CAFE_IMPORT(nn_save, "SAVEOpenFileAsync", SAVEOpenFileAsync);
CAFE_IMPORT(nn_save, "SAVEFlushQuotaAsync", SAVEFlushQuotaAsync);
} // namespace cafe::account
