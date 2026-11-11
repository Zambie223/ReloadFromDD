#pragma once

#include "CoreMinimal.h"

namespace RFDD
{
	/** Must match the plugin name; SML uses it as the config file name (Configs/ReloadFromDD.cfg). */
	inline constexpr const TCHAR* ModReference = TEXT("ReloadFromDD");

	/** Mod Configuration asset, created by Tools/create_config_asset.py. */
	inline constexpr const TCHAR* ConfigClassPath = TEXT("/ReloadFromDD/ReloadFromDDConfiguration.ReloadFromDDConfiguration_C");

	/** Another mod with the same feature. Both redirecting the same calls would double-count and double-remove. */
	inline constexpr const TCHAR* VanillaAddonsPlugin = TEXT("VanillaAddons");

	/** What each switch covers. The order is the bit order of the enabled mask. */
	enum class ECategory : uint8
	{
		Ammo = 0,
		JetpackFuel,
		ChainsawFuel,
		Filters,
		Healing,
		Count
	};

	inline constexpr uint8 CategoryBit(const ECategory Category)
	{
		return static_cast<uint8>(1u << static_cast<uint8>(Category));
	}

	inline constexpr uint8 AllCategoriesMask = (1u << static_cast<uint8>(ECategory::Count)) - 1u;

	/** Keys in the configuration's root section, one per category, in ECategory order. Renaming them renames the values in users' config files. */
	inline constexpr const TCHAR* CategoryKeys[] = {
		TEXT("Ammo"),
		TEXT("JetpackFuel"),
		TEXT("ChainsawFuel"),
		TEXT("Filters"),
		TEXT("Healing"),
	};
	static_assert(UE_ARRAY_COUNT(CategoryKeys) == static_cast<int32>(ECategory::Count), "One config key per category");

	inline const TCHAR* CategoryKey(const ECategory Category)
	{
		return static_cast<uint8>(Category) < static_cast<uint8>(ECategory::Count) ? CategoryKeys[static_cast<uint8>(Category)] : TEXT("?");
	}
}
