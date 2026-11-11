#include "ReloadFromDD.h"
#include "ReloadFromDDHooks.h"
#include "ReloadFromDDLog.h"

DEFINE_LOG_CATEGORY(LogReloadFromDD);

void FReloadFromDDModule::StartupModule()
{
	// The hooks are process-wide, so they are installed once here. Which categories act is decided per world
	// by the settings subsystem (the host's Mod Options, replicated to clients).
	FReloadFromDDHooks::Install();
}

void FReloadFromDDModule::ShutdownModule()
{
	FReloadFromDDHooks::Uninstall();
}

IMPLEMENT_MODULE(FReloadFromDDModule, ReloadFromDD)
