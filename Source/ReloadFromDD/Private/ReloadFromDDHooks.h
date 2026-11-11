#pragma once

#include "CoreMinimal.h"
#include "ReloadFromDDConstants.h"

class AFGEquipment;
class AFGJetPack;
class UWorld;

namespace RFDD
{
	enum class EHookState : uint8
	{
		NotStarted,
		/** Editor builds load the module for asset work only; nothing is hooked. */
		Editor,
		/** Vanilla Addons is enabled and already redirects the same calls. */
		DeferredToVanillaAddons,
		Installed,
	};

	/** One bit per ECategory. Written by the settings subsystem: the host's Mod Options, or on clients the host's replicated values. */
	void SetEnabledMask(uint8 Mask);
	uint8 GetEnabledMask();
	bool IsEnabled(ECategory Category);

	EHookState GetHookState();
	const TCHAR* DescribeHookState();
}

/**
 * Makes equipment draw its consumables from the Dimensional Depot.
 *
 * Each equipment function that checks for or spends a consumable is wrapped so that, while it runs, a thread-local
 * scope names the player's inventory, the items that may be redirected and the player's priority setting. The inventory
 * hooks (GetNumItems, HasItems, Remove, RemoveFromIndex) act only inside such a scope, only on that inventory and only
 * for those items: counts then include the depot, and removals are split between inventory and depot. Everything else
 * passes straight through, so the game's own logic still decides how much is used.
 *
 * Named as a friend of the hooked classes in Config/AccessTransformers.ini, so it may subscribe to their protected functions.
 */
class FReloadFromDDHooks
{
public:
	static void Install();
	/** Disables every redirect. The handlers stay registered and pass through (the module only unloads at exit). */
	static void Uninstall();

	/**
	 * Re-evaluates equipped jetpacks' cached fuel availability (the HUD count and which fuel types can be used).
	 * The game refreshes it only on inventory changes, so depot changes and settings changes call this.
	 * @param OnlyItem If set, only jetpacks that burn this item are refreshed.
	 */
	static void RefreshJetpacks(UWorld* World, const UClass* OnlyItem = nullptr);

	/** Fuel items the jetpack can burn, from its fuel parameters. */
	static void GetJetpackFuelTypes(const AFGJetPack* Jetpack, TArray<const UClass*, TInlineAllocator<6>>& OutTypes);

	static void DescribeStatus(TArray<FString>& OutLines);

#if RELOADFROMDD_DEV_COMMANDS
	/** Hook call counts since the last reset, for measuring overhead. */
	static void DescribeCallCounts(TArray<FString>& OutLines, bool bReset);
	/** Consumable data of every loaded equipment class (cost to use, ammo, fuel). */
	static void DumpEquipment(TArray<FString>& OutLines);
	/**
	 * Drives the player's equipment without input, for automated tests: weapon/reload/empty, eat, charge <slot>,
	 * jetpack/drain/thrust <on|off>, chainsaw/saw. Runs on the machine the command is typed on.
	 */
	static void DevUseEquipment(class AFGCharacterPlayer* Character, const FString& Action, const FString& Argument, TArray<FString>& OutLines);
#endif

private:
	static void InstallInventoryHooks();
	static void InstallEquipmentHooks();
};
