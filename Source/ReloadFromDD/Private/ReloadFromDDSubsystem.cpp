#include "ReloadFromDDSubsystem.h"

#include "ReloadFromDDConstants.h"
#include "ReloadFromDDDevScript.h"
#include "ReloadFromDDHooks.h"
#include "ReloadFromDDLog.h"

#include "Configuration/ConfigManager.h"
#include "Configuration/Properties/ConfigPropertyBool.h"
#include "Configuration/Properties/ConfigPropertySection.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FGCentralStorageSubsystem.h"
#include "FGPlayerState.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "Resources/FGItemDescriptor.h"
#include "Subsystem/SubsystemActorManager.h"
#include "TimerManager.h"

namespace
{
	/** Debounce for Mod Options changes; checkboxes can fire more than once per click. */
	constexpr float ConfigChangeDebounceSeconds = 0.25f;

	/** How often a machine retries binding to the depot subsystem until it exists (clients get it by replication). */
	constexpr float DepotBindRetrySeconds = 1.0f;

	const TCHAR* NetModeName(const UWorld* World)
	{
		if (!World)
		{
			return TEXT("no world");
		}
		switch (World->GetNetMode())
		{
		case NM_Standalone: return TEXT("single player");
		case NM_DedicatedServer: return TEXT("dedicated server");
		case NM_ListenServer: return TEXT("listen server (host)");
		case NM_Client: return TEXT("client");
		default: return TEXT("unknown");
		}
	}

	UConfigPropertyBool* FindBoolProperty(UConfigPropertySection* Section, const FString& Key, FString* OutCanonicalKey = nullptr)
	{
		if (!Section)
		{
			return nullptr;
		}
		for (const TPair<FString, TObjectPtr<UConfigProperty>>& Pair : Section->SectionProperties)
		{
			if (Pair.Key.Equals(Key, ESearchCase::IgnoreCase))
			{
				if (OutCanonicalKey)
				{
					*OutCanonicalKey = Pair.Key;
				}
				return Cast<UConfigPropertyBool>(Pair.Value);
			}
		}
		return nullptr;
	}

	/** Accepts true/false, on/off, yes/no and 1/0. */
	bool ParseSwitch(const FString& Text, bool& OutValue)
	{
		for (const TCHAR* Word : {TEXT("true"), TEXT("on"), TEXT("yes"), TEXT("1")})
		{
			if (Text.Equals(Word, ESearchCase::IgnoreCase))
			{
				OutValue = true;
				return true;
			}
		}
		for (const TCHAR* Word : {TEXT("false"), TEXT("off"), TEXT("no"), TEXT("0")})
		{
			if (Text.Equals(Word, ESearchCase::IgnoreCase))
			{
				OutValue = false;
				return true;
			}
		}
		return false;
	}

	FString DescribeMask(const uint8 Mask)
	{
		FString Text;
		for (uint8 Index = 0; Index < static_cast<uint8>(RFDD::ECategory::Count); ++Index)
		{
			const RFDD::ECategory Category = static_cast<RFDD::ECategory>(Index);
			Text += FString::Printf(TEXT("%s%s=%s"), Text.IsEmpty() ? TEXT("") : TEXT(", "), RFDD::CategoryKey(Category),
				(Mask & RFDD::CategoryBit(Category)) ? TEXT("on") : TEXT("off"));
		}
		return Text;
	}
}

AReloadFromDDSubsystem::AReloadFromDDSubsystem()
{
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer_Replicate;
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(1.0f);
}

AReloadFromDDSubsystem* AReloadFromDDSubsystem::Get(UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	USubsystemActorManager* Manager = World->GetSubsystem<USubsystemActorManager>();
	return Manager ? Manager->GetSubsystemActor<AReloadFromDDSubsystem>() : nullptr;
}

UConfigPropertySection* AReloadFromDDSubsystem::GetConfigRootSection(const UWorld* World)
{
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UConfigManager* ConfigManager = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
	if (!ConfigManager)
	{
		return nullptr;
	}
	FConfigId ConfigId;
	ConfigId.ModReference = RFDD::ModReference;
	return ConfigManager->GetConfigurationRootSection(ConfigId);
}

void AReloadFromDDSubsystem::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// REPNOTIFY_Always so a client also applies a mask equal to the class default on the initial bunch.
	DOREPLIFETIME_CONDITION_NOTIFY(AReloadFromDDSubsystem, EnabledMask, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(AReloadFromDDSubsystem, bServerApplied, COND_None, REPNOTIFY_Always);
}

void AReloadFromDDSubsystem::Init()
{
	Super::Init();

	// On the server SML calls Init() at spawn, after the save is loaded and before any actor's BeginPlay.
	// On clients Init() runs from BeginPlay; they wait for the replicated mask instead.
	if (!HasAuthority())
	{
		return;
	}
	UE_LOG(LogReloadFromDD, Log, TEXT("Init on %s; hooks %s"), NetModeName(GetWorld()), RFDD::DescribeHookState());
	BindConfig();
	ApplyServer(TEXT("Init"));
}

void AReloadFromDDSubsystem::BeginPlay()
{
	Super::BeginPlay();

	TryBindDepot();
	if (HasAuthority())
	{
		// Re-bind in case the configuration was not registered yet at Init, and apply again now that the
		// equipment in the world has begun play (so equipped jetpacks are refreshed).
		BindConfig();
		ApplyServer(TEXT("BeginPlay"));
#if RELOADFROMDD_DEV_COMMANDS
		RFDD::DevScript::OnGameWorldBegan(GetWorld());
#endif
	}
	else
	{
		UE_LOG(LogReloadFromDD, Log, TEXT("BeginPlay on %s; using the host's settings"), NetModeName(GetWorld()));
		OnRep_EnabledMask();
#if RELOADFROMDD_DEV_COMMANDS
		RFDD::DevScript::OnGameWorldBegan(GetWorld());
#endif
	}
}

void AReloadFromDDSubsystem::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearAllTimersForObject(this);
	}
	UnbindConfig();
	UnbindDepot();
	RFDD::SetEnabledMask(0);
	LastAppliedMask = -1;
	Super::EndPlay(EndPlayReason);
}

uint8 AReloadFromDDSubsystem::ReadConfigMask() const
{
	UConfigPropertySection* Root = GetConfigRootSection(GetWorld());
	if (!Root)
	{
		UE_LOG(LogReloadFromDD, Warning, TEXT("Configuration '%s' is not registered; every category stays on (the default)"), RFDD::ModReference);
		return RFDD::AllCategoriesMask;
	}
	uint8 Mask = 0;
	for (uint8 Index = 0; Index < static_cast<uint8>(RFDD::ECategory::Count); ++Index)
	{
		const RFDD::ECategory Category = static_cast<RFDD::ECategory>(Index);
		const UConfigPropertyBool* Switch = FindBoolProperty(Root, RFDD::CategoryKey(Category));
		if (!Switch)
		{
			UE_LOG(LogReloadFromDD, Warning, TEXT("Config key '%s' missing or not a switch; treating it as on"), RFDD::CategoryKey(Category));
		}
		if (!Switch || Switch->Value)
		{
			Mask |= RFDD::CategoryBit(Category);
		}
	}
	return Mask;
}

void AReloadFromDDSubsystem::ApplyServer(const TCHAR* Reason)
{
	if (!HasAuthority())
	{
		return;
	}
	const bool bHooksActive = RFDD::GetHookState() == RFDD::EHookState::Installed;
	const uint8 Mask = bHooksActive ? ReadConfigMask() : 0;
	EnabledMask = Mask;
	bServerApplied = true;
	ForceNetUpdate();
	ApplyLocally(Mask, Reason);
}

void AReloadFromDDSubsystem::ApplyLocally(const uint8 Mask, const TCHAR* Reason)
{
	RFDD::SetEnabledMask(Mask);
	if (LastAppliedMask == Mask)
	{
		return;
	}
	LastAppliedMask = Mask;
	UE_LOG(LogReloadFromDD, Log, TEXT("Settings applied (%s) on %s: %s"), Reason, NetModeName(GetWorld()), *DescribeMask(Mask));
	// Jetpacks cache which fuel types are available; recompute them with the new settings.
	FReloadFromDDHooks::RefreshJetpacks(GetWorld());
}

void AReloadFromDDSubsystem::OnRep_EnabledMask()
{
	if (HasAuthority() || !bServerApplied)
	{
		return;
	}
	ApplyLocally(EnabledMask, TEXT("from host"));
}

void AReloadFromDDSubsystem::BindConfig()
{
	UConfigPropertySection* Root = GetConfigRootSection(GetWorld());
	if (!Root || BoundConfigSection.Get() == Root)
	{
		return;
	}
	if (UConfigPropertySection* Old = BoundConfigSection.Get())
	{
		Old->OnPropertyValueChanged.RemoveDynamic(this, &AReloadFromDDSubsystem::OnConfigValueChanged);
	}
	Root->OnPropertyValueChanged.AddUniqueDynamic(this, &AReloadFromDDSubsystem::OnConfigValueChanged);
	BoundConfigSection = Root;
}

void AReloadFromDDSubsystem::UnbindConfig()
{
	if (UConfigPropertySection* Section = BoundConfigSection.Get())
	{
		Section->OnPropertyValueChanged.RemoveDynamic(this, &AReloadFromDDSubsystem::OnConfigValueChanged);
	}
	BoundConfigSection.Reset();
}

void AReloadFromDDSubsystem::OnConfigValueChanged()
{
	if (!HasAuthority())
	{
		return;
	}
	GetWorldTimerManager().SetTimer(DebounceTimer, this, &AReloadFromDDSubsystem::OnDebounceElapsed, ConfigChangeDebounceSeconds, false);
}

void AReloadFromDDSubsystem::OnDebounceElapsed()
{
	ApplyServer(TEXT("Config changed"));
}

void AReloadFromDDSubsystem::TryBindDepot()
{
	AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(GetWorld());
	if (!Depot)
	{
		// Clients receive the depot subsystem by replication; retry until it is there.
		if (!DepotBindTimer.IsValid())
		{
			GetWorldTimerManager().SetTimer(DepotBindTimer, this, &AReloadFromDDSubsystem::TryBindDepot, DepotBindRetrySeconds, true);
		}
		return;
	}
	GetWorldTimerManager().ClearTimer(DepotBindTimer);
	if (BoundDepot.Get() == Depot)
	{
		return;
	}
	UnbindDepot();
	Depot->mOnCentralStorageItemAmountUpdatedDelegate.AddUniqueDynamic(this, &AReloadFromDDSubsystem::OnDepotItemAmountUpdated);
	BoundDepot = Depot;
	// Jetpacks equipped before the depot existed on this machine still have inventory-only fuel availability.
	FReloadFromDDHooks::RefreshJetpacks(GetWorld());
}

void AReloadFromDDSubsystem::UnbindDepot()
{
	if (AFGCentralStorageSubsystem* Depot = BoundDepot.Get())
	{
		Depot->mOnCentralStorageItemAmountUpdatedDelegate.RemoveDynamic(this, &AReloadFromDDSubsystem::OnDepotItemAmountUpdated);
	}
	BoundDepot.Reset();
}

void AReloadFromDDSubsystem::OnDepotItemAmountUpdated(TSubclassOf<UFGItemDescriptor> ItemClass, int32 NewAmount)
{
	if (ItemClass && RFDD::IsEnabled(RFDD::ECategory::JetpackFuel))
	{
		FReloadFromDDHooks::RefreshJetpacks(GetWorld(), ItemClass.Get());
	}
}

bool AReloadFromDDSubsystem::SetConfigValue(const UWorld* World, const FString& Key, const FString& ValueText, FString& OutMessage)
{
	if (World && World->GetNetMode() == NM_Client)
	{
		OutMessage = TEXT("Refused: this is a client. Settings come from the host or server.");
		return false;
	}
	bool bNewValue = false;
	if (!ParseSwitch(ValueText, bNewValue))
	{
		OutMessage = TEXT("Refused: the value must be true or false.");
		return false;
	}
	FString CanonicalKey;
	UConfigPropertyBool* Property = FindBoolProperty(GetConfigRootSection(World), Key, &CanonicalKey);
	if (!Property)
	{
		FString Keys;
		for (const TCHAR* Name : RFDD::CategoryKeys)
		{
			Keys += FString::Printf(TEXT("%s%s"), Keys.IsEmpty() ? TEXT("") : TEXT(", "), Name);
		}
		OutMessage = FString::Printf(TEXT("Unknown key '%s', or the configuration is not registered. Keys: %s"), *Key, *Keys);
		return false;
	}
	Property->Value = bNewValue;
	// MarkDirty saves the file, refreshes SML's caches and fires OnPropertyValueChanged, which applies the value.
	Property->MarkDirty();
	OutMessage = FString::Printf(TEXT("%s set to %s. Applies within a second."), *CanonicalKey, bNewValue ? TEXT("true") : TEXT("false"));
	return true;
}

void AReloadFromDDSubsystem::DescribeStatus(FOutputDevice& Ar) const
{
	UWorld* World = GetWorld();
	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("Reload From DD status (%s)"), NetModeName(World)));
	if (HasAuthority())
	{
		Lines.Add(FString::Printf(TEXT("  Config on this machine: %s"), *DescribeMask(ReadConfigMask())));
	}
	else
	{
		Lines.Add(FString::Printf(TEXT("  Config on this machine: ignored (client); host sent %s"),
			bServerApplied ? *DescribeMask(EnabledMask) : TEXT("nothing yet")));
	}
	FReloadFromDDHooks::DescribeStatus(Lines);
	Lines.Add(FString::Printf(TEXT("  Depot subsystem bound: %s"), BoundDepot.IsValid() ? TEXT("yes") : TEXT("no")));

	if (World)
	{
		if (const APlayerController* Controller = World->GetFirstPlayerController())
		{
			if (const AFGPlayerState* PlayerState = Controller->GetPlayerState<AFGPlayerState>())
			{
				Lines.Add(FString::Printf(TEXT("  This player's priority: %s"), PlayerState->GetTakeFromInventoryBeforeCentralStorage()
					? TEXT("inventory first, then the depot") : TEXT("depot first, then the inventory")));
			}
		}
	}

	for (const FString& Line : Lines)
	{
		Ar.Log(Line);
		UE_LOG(LogReloadFromDD, Log, TEXT("%s"), *Line);
	}
}
