// proc_ui: foreground/background management. The game is always in the foreground; registered callbacks
// (HOME button menu, release foreground, ...) never fire.
#include "../imports.h"

namespace cafe::proc_ui
{
namespace
{
constexpr uint32_t PROCUI_STATUS_IN_FOREGROUND = 0;

void ProcUIInit(uint32_t saveCallback) { (void)saveCallback; }
uint32_t ProcUIProcessMessages(bool block) { (void)block; return PROCUI_STATUS_IN_FOREGROUND; }
void ProcUIRegisterCallback(uint32_t message, uint32_t callback, uint32_t param, uint32_t priority)
{
    (void)message, (void)callback, (void)param, (void)priority;
}
void ProcUIDrawDoneRelease() {}
void ProcUIShutdown() {}
} // namespace

CAFE_IMPORT(proc_ui, "ProcUIInit", ProcUIInit);
CAFE_IMPORT(proc_ui, "ProcUIProcessMessages", ProcUIProcessMessages);
CAFE_IMPORT(proc_ui, "ProcUIRegisterCallback", ProcUIRegisterCallback);
CAFE_IMPORT(proc_ui, "ProcUIDrawDoneRelease", ProcUIDrawDoneRelease);
CAFE_IMPORT(proc_ui, "ProcUIShutdown", ProcUIShutdown);
} // namespace cafe::proc_ui
