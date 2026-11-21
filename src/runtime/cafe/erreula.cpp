// erreula (system error viewer), reached through OSDynLoad. The viewer is never drawn. If the game shows an
// error, it is reported on the console, the viewer reports itself as displayed, and the next "button decided"
// query answers yes, so the game is not left waiting for input nobody can give.
#include "../imports.h"
#include "../log.h"

#include <atomic>

namespace cafe::erreula
{
namespace
{
enum State : uint32_t { Hidden = 0, FadeIn = 1, Display = 2, FadeOut = 3 };
std::atomic<uint32_t> gState{ Hidden };
std::atomic<bool> gDecided{ false };
std::atomic<int32_t> gErrorCode{ 0 };

void Create(uint32_t workMemory, uint32_t region, uint32_t lang, uint32_t fsClient) { (void)workMemory, (void)region, (void)lang, (void)fsClient; }
void Destroy() { gState = Hidden; }
void Calc(uint32_t controllerInfo) { (void)controllerInfo; }
void DrawTV() {}
void DrawDRC() {}

// nn::erreula::AppearArg begins with ErrorArg: errorType (u32), renderTarget (u32), controllerType (u32),
// errorCode (s32), ...
void AppearError(uint32_t appearArg)
{
    gErrorCode = int32_t(guest::Read32(appearArg + 0x0C));
    dtlog::Important("[erreula] the game shows an error: type %u, code %d (auto-confirmed)\n", guest::Read32(appearArg),
        gErrorCode.load());
    gState = Display;
    gDecided = true;
}
void DisappearError() { gState = Hidden; }
uint32_t GetStateErrorViewer() { return gState; }
bool IsDecideSelectButtonError() { return gDecided.exchange(false); }
bool IsDecideSelectLeftButtonError() { return IsDecideSelectButtonError(); }
bool IsDecideSelectRightButtonError() { return false; }
int32_t GetResultCode() { return gErrorCode; }
uint32_t GetResultType() { return 0; }
int32_t GetSelectButtonNumError() { return 0; }
bool IsSelectCursorActive() { return false; }
void SetControllerRemo(uint32_t type) { (void)type; }
void ChangeLang(uint32_t lang) { (void)lang; }
void AppearHomeNixSign(uint32_t arg) { (void)arg; }
void DisappearHomeNixSign() {}
bool IsAppearHomeNixSign() { return false; }
} // namespace

CAFE_IMPORT(erreula, "ErrEulaCreate__3RplFPUcQ3_2nn7erreula10RegionTypeQ3_2nn7erreula8LangTypeP8FSClient", Create);
CAFE_IMPORT(erreula, "ErrEulaDestroy__3RplFv", Destroy);
CAFE_IMPORT(erreula, "ErrEulaCalc__3RplFRCQ3_2nn7erreula14ControllerInfo", Calc);
CAFE_IMPORT(erreula, "ErrEulaDrawTV__3RplFv", DrawTV);
CAFE_IMPORT(erreula, "ErrEulaDrawDRC__3RplFv", DrawDRC);
CAFE_IMPORT(erreula, "ErrEulaAppearError__3RplFRCQ3_2nn7erreula9AppearArg", AppearError);
CAFE_IMPORT(erreula, "ErrEulaDisappearError__3RplFv", DisappearError);
CAFE_IMPORT(erreula, "ErrEulaGetStateErrorViewer__3RplFv", GetStateErrorViewer);
CAFE_IMPORT(erreula, "ErrEulaIsDecideSelectButtonError__3RplFv", IsDecideSelectButtonError);
CAFE_IMPORT(erreula, "ErrEulaIsDecideSelectLeftButtonError__3RplFv", IsDecideSelectLeftButtonError);
CAFE_IMPORT(erreula, "ErrEulaIsDecideSelectRightButtonError__3RplFv", IsDecideSelectRightButtonError);
CAFE_IMPORT(erreula, "ErrEulaGetResultCode__3RplFv", GetResultCode);
CAFE_IMPORT(erreula, "ErrEulaGetResultType__3RplFv", GetResultType);
CAFE_IMPORT(erreula, "ErrEulaGetSelectButtonNumError__3RplFv", GetSelectButtonNumError);
CAFE_IMPORT(erreula, "ErrEulaIsSelectCursorActive__3RplFv", IsSelectCursorActive);
CAFE_IMPORT(erreula, "ErrEulaSetControllerRemo__3RplFQ3_2nn7erreula14ControllerType", SetControllerRemo);
CAFE_IMPORT(erreula, "ErrEulaChangeLang__3RplFQ3_2nn7erreula8LangType", ChangeLang);
CAFE_IMPORT(erreula, "ErrEulaAppearHomeNixSign__3RplFRCQ3_2nn7erreula14HomeNixSignArg", AppearHomeNixSign);
CAFE_IMPORT(erreula, "ErrEulaDisappearHomeNixSign__3RplFv", DisappearHomeNixSign);
CAFE_IMPORT(erreula, "ErrEulaIsAppearHomeNixSign__3RplFv", IsAppearHomeNixSign);
} // namespace cafe::erreula
