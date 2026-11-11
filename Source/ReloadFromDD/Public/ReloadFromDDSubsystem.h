#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "ReloadFromDDSubsystem.generated.h"

class AFGCentralStorageSubsystem;
class UConfigPropertySection;
class UFGItemDescriptor;

/**
 * Decides which consumable categories draw from the Dimensional Depot in this world.
 *
 * The server (or single player / listen host) reads its own Mod Options and replicates the result, so every client
 * redirects exactly what the server does; a client's own Mod Options are ignored. It also refreshes equipped jetpacks
 * when depot stock or the settings change, because the game only re-checks fuel availability on inventory changes.
 */
UCLASS()
class RELOADFROMDD_API AReloadFromDDSubsystem : public AModSubsystem
{
	GENERATED_BODY()

public:
	AReloadFromDDSubsystem();

	/** Returns the subsystem in this world, or null (main menu, or on a client before replication). */
	static AReloadFromDDSubsystem* Get(UWorld* World);

	/** Returns this mod's live configuration root section, or null if the configuration is not registered. */
	static UConfigPropertySection* GetConfigRootSection(const UWorld* World);

	/** Sets one switch on this machine (host or server only), saves it, and lets the change handler apply it. */
	static bool SetConfigValue(const UWorld* World, const FString& Key, const FString& ValueText, FString& OutMessage);

	/** Writes the configured and effective settings to the output device and the log. */
	void DescribeStatus(FOutputDevice& Ar) const;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	virtual void Init() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** Reads this machine's switches; a missing key counts as on. */
	uint8 ReadConfigMask() const;
	void ApplyServer(const TCHAR* Reason);
	void ApplyLocally(uint8 Mask, const TCHAR* Reason);
	void BindConfig();
	void UnbindConfig();
	void TryBindDepot();
	void UnbindDepot();
	void OnDebounceElapsed();

	UFUNCTION()
	void OnConfigValueChanged();

	UFUNCTION()
	void OnRep_EnabledMask();

	UFUNCTION()
	void OnDepotItemAmountUpdated(TSubclassOf<UFGItemDescriptor> ItemClass, int32 NewAmount);

	/** One bit per RFDD::ECategory, as decided by the server. 0 if the server's hooks are inactive. */
	UPROPERTY(ReplicatedUsing = OnRep_EnabledMask)
	uint8 EnabledMask = 0;

	/** True once the server has applied its settings, so clients can tell "all off" from "not replicated yet". */
	UPROPERTY(ReplicatedUsing = OnRep_EnabledMask)
	bool bServerApplied = false;

	TWeakObjectPtr<UConfigPropertySection> BoundConfigSection;
	TWeakObjectPtr<AFGCentralStorageSubsystem> BoundDepot;

	FTimerHandle DebounceTimer;
	FTimerHandle DepotBindTimer;

	/** Mask this machine last applied, to skip refreshes when nothing changed. */
	int32 LastAppliedMask = -1;
};
