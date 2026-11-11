#include "ReloadFromDDRootModules.h"

#include "Configuration/ModConfiguration.h"
#include "ReloadFromDDConstants.h"
#include "ReloadFromDDLog.h"
#include "ReloadFromDDSubsystem.h"

UReloadFromDDGameInstanceModule::UReloadFromDDGameInstanceModule()
{
	bRootModule = true;
}

void UReloadFromDDGameInstanceModule::DispatchLifecycleEvent(const ELifecyclePhase Phase)
{
	// Configurations are registered during INITIALIZATION, so add ours during CONSTRUCTION.
	// Loaded here instead of in the constructor so the mod's content is guaranteed to be mounted.
	if (Phase == ELifecyclePhase::CONSTRUCTION)
	{
		const TSoftClassPtr<UModConfiguration> ConfigClass{FSoftObjectPath(RFDD::ConfigClassPath)};
		if (UClass* Loaded = ConfigClass.LoadSynchronous())
		{
			ModConfigurations.AddUnique(Loaded);
		}
		else
		{
			UE_LOG(LogReloadFromDD, Error, TEXT("Could not load the Mod Configuration %s; every category stays on (the default)"), RFDD::ConfigClassPath);
		}
	}
	Super::DispatchLifecycleEvent(Phase);
}

UReloadFromDDGameWorldModule::UReloadFromDDGameWorldModule()
{
	bRootModule = true;
	ModSubsystems.Add(AReloadFromDDSubsystem::StaticClass());
}
