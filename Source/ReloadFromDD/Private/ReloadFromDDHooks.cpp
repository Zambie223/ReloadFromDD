#include "ReloadFromDDHooks.h"

#include "ReloadFromDDLog.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Equipment/FGAmmoType.h"
#include "Equipment/FGChainsaw.h"
#include "Equipment/FGConsumableEquipment.h"
#include "Equipment/FGEquipment.h"
#include "Equipment/FGGasMask.h"
#include "Equipment/FGJetPack.h"
#include "FGJetPackFuelParameters.h"
#include "Equipment/FGSuitBase.h"
#include "Equipment/FGWeapon.h"
#include "FGCentralStorageSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGInventoryComponent.h"
#include "FGInventoryComponentEquipment.h"
#include "FGPlayerState.h"
#include "Interfaces/IPluginManager.h"
#include "Patching/NativeHookManager.h"
#include "Resources/FGConsumableDescriptor.h"
#include "UObject/UObjectIterator.h"

#if RELOADFROMDD_DEV_COMMANDS
#include "HAL/PlatformStackWalk.h"
#endif

#include <atomic>

namespace RFDD
{
	namespace
	{
		std::atomic<uint8> GEnabledMask{0};
		EHookState GHookState = EHookState::NotStarted;

		enum class EMatch : uint8
		{
			/** Only the items listed in FScope::Items. */
			ItemList,
			/** Any ammo item (weapons ask for one ammo class at a time). */
			AnyAmmo,
		};

		/** One active redirect. Lives on the stack of the wrapped equipment function. */
		struct FScope
		{
			FScope* Prev = nullptr;
			ECategory Category = ECategory::Count;
			/** The inventory whose counts and removals also see the depot. */
			const UFGInventoryComponent* Inventory = nullptr;
			AFGCentralStorageSubsystem* Depot = nullptr;
			/** The player's "take from inventory before Dimensional Depot" setting. */
			bool bInventoryFirst = true;
			/** Only the authority removes from the depot; a client's removal is only its local prediction. */
			bool bAuthority = false;
			EMatch Match = EMatch::ItemList;
			TArray<const UClass*, TInlineAllocator<6>> Items;
			/** GSuspend of the enclosing code, restored when this scope ends. */
			int32 SavedSuspend = 0;

			bool Allows(const UClass* ItemClass) const
			{
				if (!ItemClass)
				{
					return false; // a null class means "count everything"; never redirect that
				}
				if (Match == EMatch::AnyAmmo)
				{
					return ItemClass->IsChildOf(UFGAmmoType::StaticClass());
				}
				return Items.Contains(ItemClass);
			}
		};

		/** Innermost active scope on this thread. Null almost always, which keeps the inventory hooks cheap. */
		thread_local FScope* GTop = nullptr;

		/**
		 * Greater than zero while this mod calls inventory functions itself (to read the plain inventory count, or to
		 * run the original removal), so those calls are not redirected again. A scope pushed meanwhile (for example by
		 * an inventory delegate that refreshes a jetpack) clears it for its own lifetime.
		 */
		thread_local int32 GSuspend = 0;

		struct FPushScope
		{
			FScope& Scope;

			explicit FPushScope(FScope& InScope)
				: Scope(InScope)
			{
				Scope.Prev = GTop;
				Scope.SavedSuspend = GSuspend;
				GSuspend = 0;
				GTop = &Scope;
			}

			~FPushScope()
			{
				GTop = Scope.Prev;
				GSuspend = Scope.SavedSuspend;
			}

			FPushScope(const FPushScope&) = delete;
			FPushScope& operator=(const FPushScope&) = delete;
		};

		struct FSuspend
		{
			FSuspend() { ++GSuspend; }
			~FSuspend() { --GSuspend; }
			FSuspend(const FSuspend&) = delete;
			FSuspend& operator=(const FSuspend&) = delete;
		};

		/** The active scope if it redirects this inventory. Only the innermost scope is consulted. */
		const FScope* ScopeFor(const UFGInventoryComponent* Inventory)
		{
			const FScope* Top = GTop;
			if (!Top || GSuspend > 0 || Top->Inventory != Inventory)
			{
				return nullptr;
			}
			return Top;
		}

		int32 DepotCount(const FScope& Scope, const UClass* ItemClass)
		{
			if (!Scope.Depot || !ItemClass)
			{
				return 0;
			}
			return FMath::Max(0, Scope.Depot->GetNumItemsFromCentralStorage(const_cast<UClass*>(ItemClass)));
		}

		/** Inventory count without any redirect. */
		int32 PlainInventoryCount(const UFGInventoryComponent* Inventory, const UClass* ItemClass)
		{
			FSuspend Suspend;
			return Inventory->GetNumItems(const_cast<UClass*>(ItemClass));
		}

		/** Fills the parts of a scope common to every category. False if this equipment should not be redirected now. */
		bool BeginScope(FScope& Out, const AFGEquipment* Equipment, const ECategory Category)
		{
			if (!Equipment || !IsEnabled(Category))
			{
				return false;
			}
			AFGCharacterPlayer* Character = Equipment->GetInstigatorCharacter();
			if (!Character)
			{
				return false; // not carried by a player (e.g. another mod's turret)
			}
			UWorld* World = Equipment->GetWorld();
			AFGCentralStorageSubsystem* Depot = World ? AFGCentralStorageSubsystem::Get(World) : nullptr;
			UFGInventoryComponent* Inventory = Character->GetInventory();
			if (!Depot || !Inventory)
			{
				return false;
			}
			Out.Category = Category;
			Out.Inventory = Inventory;
			Out.Depot = Depot;
			const AFGPlayerState* PlayerState = Character->GetPlayerState<AFGPlayerState>();
			// Without a player state (only briefly, while a pawn is being set up) behave like the inventory-first setting.
			Out.bInventoryFirst = PlayerState ? PlayerState->GetTakeFromInventoryBeforeCentralStorage() : true;
			Out.bAuthority = Equipment->HasAuthority();
			return true;
		}

		/** Category of equipment that pays through the generic cost-to-use path, or Count if this mod leaves it alone. */
		ECategory CostCategory(const AFGEquipment* Equipment)
		{
			if (Equipment->IsA<AFGJetPack>())
			{
				return ECategory::JetpackFuel;
			}
			// The gas mask is native; the hazmat suit is a Blueprint based on AFGSuitBase that uses iodine filters.
			if (Equipment->IsA<AFGGasMask>() || Equipment->IsA<AFGSuitBase>())
			{
				return ECategory::Filters;
			}
			return ECategory::Count;
		}

		void AddCostItems(FScope& Scope, const AFGEquipment* Equipment)
		{
			for (const FItemAmount& Cost : Equipment->GetCostToUse())
			{
				if (Cost.ItemClass)
				{
					Scope.Items.AddUnique(Cost.ItemClass.Get());
				}
			}
		}

		/** How a removal of Num items splits between the inventory and the depot. */
		void SplitRemoval(const FScope& Scope, const int32 Num, const int32 InInventory, const int32 InDepot, int32& OutFromInventory, int32& OutFromDepot)
		{
			if (Scope.bInventoryFirst)
			{
				OutFromInventory = FMath::Min(Num, InInventory);
				OutFromDepot = FMath::Min(Num - OutFromInventory, InDepot);
			}
			else
			{
				OutFromDepot = FMath::Min(Num, InDepot);
				OutFromInventory = FMath::Min(Num - OutFromDepot, InInventory);
			}
		}

#if RELOADFROMDD_DEV_COMMANDS
		struct FCounters
		{
			std::atomic<uint64> GetNumItemsCalls{0};
			std::atomic<uint64> GetNumItemsRedirected{0};
			std::atomic<uint64> HasItemsCalls{0};
			std::atomic<uint64> HasItemsRedirected{0};
			std::atomic<uint64> RemoveCalls{0};
			std::atomic<uint64> RemoveRedirected{0};
			std::atomic<uint64> RemoveFromIndexCalls{0};
			std::atomic<uint64> RemoveFromIndexRedirected{0};
			std::atomic<uint64> ItemsTakenFromDepot{0};
			std::atomic<uint64> ItemsTakenFromInventory{0};
			double SinceSeconds = FPlatformTime::Seconds();
		};
		FCounters GCounters;
#define RFDD_COUNT(Field) (GCounters.Field.fetch_add(1, std::memory_order_relaxed))
#define RFDD_COUNT_N(Field, N) (GCounters.Field.fetch_add(static_cast<uint64>(N), std::memory_order_relaxed))

		/**
		 * Development aid: logs (once per item class and call site) consumable removals from a player's inventory that
		 * happen outside any scope, with a native stack, to find consumption paths this mod does not cover yet
		 * (the hazmat suit's filter logic is a Blueprint). Game thread only.
		 */
		void NoteUnscopedRemoval(const UFGInventoryComponent* Inventory, const UClass* ItemClass, const TCHAR* Function)
		{
			static int32 Logged = 0;
			static TSet<FString> Seen;
			// Factory buildings remove items on worker threads; only the game thread touches the statics below.
			if (!IsInGameThread() || Logged >= 40 || !ItemClass)
			{
				return;
			}
			const AActor* Owner = Inventory ? Inventory->GetOwner() : nullptr;
			if (!Owner || !Owner->IsA<AFGCharacterPlayer>())
			{
				return;
			}
			const FString ClassName = ItemClass->GetName();
			const bool bInteresting = ItemClass->IsChildOf(UFGAmmoType::StaticClass()) || ItemClass->IsChildOf(UFGConsumableDescriptor::StaticClass())
				|| ClassName.Contains(TEXT("Filter")) || ClassName.Contains(TEXT("Fuel")) || ClassName.Contains(TEXT("Biomass"));
			if (!bInteresting)
			{
				return;
			}
			ANSICHAR Stack[8192];
			Stack[0] = 0;
			FPlatformStackWalk::StackWalkAndDump(Stack, UE_ARRAY_COUNT(Stack), 2);
			FString StackText = ANSI_TO_TCHAR(Stack);
			// Keep the first frames only; the rest is the engine's tick loop.
			TArray<FString> Lines;
			StackText.ParseIntoArrayLines(Lines);
			FString Short;
			for (int32 Index = 0; Index < Lines.Num() && Index < 14; ++Index)
			{
				Short += TEXT("\n    ") + Lines[Index].TrimStartAndEnd();
			}
			const FString Key = ClassName + Short;
			if (Seen.Contains(Key))
			{
				return;
			}
			Seen.Add(Key);
			++Logged;
			UE_LOG(LogReloadFromDD, Warning, TEXT("[dev] Unscoped %s of %s from %s's inventory (not redirected). Stack:%s"),
				Function, *ClassName, *Owner->GetName(), *Short);
		}

		/** Development aid: logs once per equipment class what its cost-to-use path is asked to pay. */
		void NoteCostPath(const AFGEquipment* Equipment, const ECategory Category, const TCHAR* Function)
		{
			static TSet<const UClass*> Seen;
			if (!Equipment || !IsInGameThread() || Seen.Contains(Equipment->GetClass()))
			{
				return;
			}
			Seen.Add(Equipment->GetClass());
			FString Costs;
			for (const FItemAmount& Cost : Equipment->GetCostToUse())
			{
				Costs += FString::Printf(TEXT(" %s x%d"), Cost.ItemClass ? *Cost.ItemClass->GetName() : TEXT("null"), Cost.Amount);
			}
			UE_LOG(LogReloadFromDD, Log, TEXT("[dev] %s on %s (category %s), cost:%s"), Function, *Equipment->GetClass()->GetName(),
				Category == ECategory::Count ? TEXT("none - not redirected") : CategoryKey(Category), Costs.IsEmpty() ? TEXT(" (empty)") : *Costs);
		}
#define RFDD_NOTE_UNSCOPED(Inventory, ItemClass, Function) NoteUnscopedRemoval(Inventory, ItemClass, Function)
#define RFDD_NOTE_COST(Equipment, Category, Function) NoteCostPath(Equipment, Category, Function)
#else
// Directives can't appear inside SUBSCRIBE_METHOD's macro argument, so the dev aids are macros that vanish here.
#define RFDD_COUNT(Field) ((void)0)
#define RFDD_COUNT_N(Field, N) ((void)0)
#define RFDD_NOTE_UNSCOPED(Inventory, ItemClass, Function) ((void)0)
#define RFDD_NOTE_COST(Equipment, Category, Function) ((void)0)
#endif
	}

	void SetEnabledMask(const uint8 Mask)
	{
		GEnabledMask.store(Mask & AllCategoriesMask, std::memory_order_relaxed);
	}

	uint8 GetEnabledMask()
	{
		return GEnabledMask.load(std::memory_order_relaxed);
	}

	bool IsEnabled(const ECategory Category)
	{
		return GHookState == EHookState::Installed && (GetEnabledMask() & CategoryBit(Category)) != 0;
	}

	EHookState GetHookState()
	{
		return GHookState;
	}

	const TCHAR* DescribeHookState()
	{
		switch (GHookState)
		{
		case EHookState::NotStarted: return TEXT("not started");
		case EHookState::Editor: return TEXT("editor (no hooks)");
		case EHookState::DeferredToVanillaAddons: return TEXT("inactive: Vanilla Addons is installed and already does this");
		case EHookState::Installed: return TEXT("active");
		default: return TEXT("unknown");
		}
	}
}

using namespace RFDD;

void FReloadFromDDHooks::Install()
{
	if (GHookState != EHookState::NotStarted)
	{
		return;
	}
	if (WITH_EDITOR)
	{
		GHookState = EHookState::Editor;
		return;
	}
	if (IPluginManager::Get().FindEnabledPlugin(VanillaAddonsPlugin).IsValid())
	{
		GHookState = EHookState::DeferredToVanillaAddons;
		UE_LOG(LogReloadFromDD, Warning, TEXT("Vanilla Addons is installed and also lets equipment use the Dimensional Depot. Reload From DD stays inactive so items are not counted or taken twice."));
		return;
	}
	InstallInventoryHooks();
	InstallEquipmentHooks();
	GHookState = EHookState::Installed;
	UE_LOG(LogReloadFromDD, Log, TEXT("Hooks installed"));
}

void FReloadFromDDHooks::Uninstall()
{
	SetEnabledMask(0);
	if (GHookState == EHookState::Installed)
	{
		GHookState = EHookState::NotStarted;
	}
}

void FReloadFromDDHooks::InstallInventoryHooks()
{
	// Counts: inventory + depot for the scope's items.
	SUBSCRIBE_METHOD(UFGInventoryComponent::GetNumItems, [](auto& Call, const UFGInventoryComponent* Self, TSubclassOf<UFGItemDescriptor> ItemClass)
	{
		RFDD_COUNT(GetNumItemsCalls);
		if (!GTop)
		{
			return;
		}
		const FScope* Scope = ScopeFor(Self);
		if (!Scope || !Scope->Allows(ItemClass.Get()))
		{
			return;
		}
		RFDD_COUNT(GetNumItemsRedirected);
		int32 InInventory;
		{
			FSuspend Suspend;
			InInventory = Call(Self, ItemClass);
		}
		Call.Override(InInventory + DepotCount(*Scope, ItemClass.Get()));
	});

	SUBSCRIBE_METHOD(UFGInventoryComponent::HasItems, [](auto& Call, const UFGInventoryComponent* Self, TSubclassOf<UFGItemDescriptor> ItemClass, int32 Num)
	{
		RFDD_COUNT(HasItemsCalls);
		if (!GTop)
		{
			return;
		}
		const FScope* Scope = ScopeFor(Self);
		if (!Scope || !Scope->Allows(ItemClass.Get()))
		{
			return;
		}
		RFDD_COUNT(HasItemsRedirected);
		const int32 Total = PlainInventoryCount(Self, ItemClass.Get()) + DepotCount(*Scope, ItemClass.Get());
		Call.Override(Total >= Num);
	});

	// Removal by class (weapons, the cost-to-use path, the chainsaw): split between inventory and depot.
	SUBSCRIBE_METHOD(UFGInventoryComponent::Remove, [](auto& Call, UFGInventoryComponent* Self, TSubclassOf<UFGItemDescriptor> ItemClass, int32 Num)
	{
		RFDD_COUNT(RemoveCalls);
		if (!GTop)
		{
			RFDD_NOTE_UNSCOPED(Self, ItemClass.Get(), TEXT("Remove"));
			return;
		}
		const FScope* Scope = ScopeFor(Self);
		if (!Scope || Num <= 0 || !Scope->Allows(ItemClass.Get()))
		{
			return;
		}
		RFDD_COUNT(RemoveRedirected);
		const int32 InInventory = PlainInventoryCount(Self, ItemClass.Get());
		const int32 InDepot = DepotCount(*Scope, ItemClass.Get());
		int32 FromInventory = 0;
		int32 FromDepot = 0;
		SplitRemoval(*Scope, Num, InInventory, InDepot, FromInventory, FromDepot);

		// A client only predicts the inventory part; the server takes the depot part and replicates the result.
		if (FromDepot > 0 && Scope->bAuthority)
		{
			int32 Taken;
			{
				FSuspend Suspend;
				Taken = Scope->Depot->TryRemoveItemsFromCentralStorage(ItemClass, FromDepot);
			}
			RFDD_COUNT_N(ItemsTakenFromDepot, FMath::Max(0, Taken));
			if (Taken < FromDepot)
			{
				// Someone else emptied the depot in between; take what is left from the inventory instead.
				FromInventory = FMath::Min(InInventory, FromInventory + (FromDepot - Taken));
			}
		}
		if (FromInventory > 0)
		{
			RFDD_COUNT_N(ItemsTakenFromInventory, FromInventory);
			FSuspend Suspend;
			Call(Self, ItemClass, FromInventory);
		}
		else
		{
			Call.Cancel();
		}
	});

	// Removal by slot (healing items in the hand slot).
	SUBSCRIBE_METHOD(UFGInventoryComponent::RemoveFromIndex, [](auto& Call, UFGInventoryComponent* Self, int32 Index, int32 Num, UFGInventoryComponent* TargetInventory)
	{
		RFDD_COUNT(RemoveFromIndexCalls);
		if (!GTop)
		{
			return;
		}
		const FScope* Scope = ScopeFor(Self);
		if (!Scope || Scope->Category != ECategory::Healing || Num <= 0 || !Scope->bAuthority)
		{
			return;
		}
		FInventoryStack Stack;
		{
			FSuspend Suspend;
			if (!Self->GetStackFromIndex(Index, Stack))
			{
				return;
			}
		}
		const UClass* ItemClass = Stack.Item.GetItemClass().Get();
		if (!Scope->Allows(ItemClass))
		{
			return;
		}
		// Inventory first: use the held stack, but keep its last item in hand while the depot has more,
		// so the item stays equipped. Depot first: use the depot while it has enough.
		const bool bWouldEmptyHand = Stack.NumItems <= Num;
		if (Scope->bInventoryFirst && !bWouldEmptyHand)
		{
			return;
		}
		if (DepotCount(*Scope, ItemClass) < Num)
		{
			return;
		}
		RFDD_COUNT(RemoveFromIndexRedirected);
		int32 Taken;
		{
			FSuspend Suspend;
			Taken = Scope->Depot->TryRemoveItemsFromCentralStorage(const_cast<UClass*>(ItemClass), Num);
		}
		RFDD_COUNT_N(ItemsTakenFromDepot, FMath::Max(0, Taken));
		if (Taken >= Num)
		{
			Call.Cancel();
			return;
		}
		if (Taken > 0)
		{
			FSuspend Suspend;
			Call(Self, Index, Num - Taken, TargetInventory);
		}
	});
}

void FReloadFromDDHooks::InstallEquipmentHooks()
{
	// Weapons (rifle, rebar gun, nobelisk detonator). GetSpareAmmunition is the only ammo count: CanReload,
	// TryAutoReload and ActualReload go through it (the HUD spare-ammo counter does not: it still shows only the inventory). ActualReload then removes the loaded amount,
	// and only on the authority.
	SUBSCRIBE_METHOD(AFGWeapon::GetSpareAmmunition, [](auto& Call, const AFGWeapon* Self, TSubclassOf<UFGAmmoType> AmmoClass)
	{
		FScope Scope;
		if (!BeginScope(Scope, Self, ECategory::Ammo))
		{
			return;
		}
		Scope.Match = EMatch::AnyAmmo;
		FPushScope Push(Scope);
		Call(Self, AmmoClass);
	});

	SUBSCRIBE_METHOD(AFGWeapon::ActualReload, [](auto& Call, AFGWeapon* Self)
	{
		FScope Scope;
		if (!BeginScope(Scope, Self, ECategory::Ammo))
		{
			return;
		}
		Scope.Match = EMatch::AnyAmmo;
		FPushScope Push(Scope);
		Call(Self);
	});

	// The generic cost-to-use path: the jetpack pays for fuel and the gas mask for filters through it
	// (RegenerateFuel and AFGGasMask::Tick call CanAffordUse, then ChargeForUse_Internal on server and client).
	SUBSCRIBE_METHOD(AFGEquipment::CanAffordUse, [](auto& Call, const AFGEquipment* Self)
	{
		const ECategory Category = CostCategory(Self);
		RFDD_NOTE_COST(Self, Category, TEXT("CanAffordUse"));
		FScope Scope;
		if (Category == ECategory::Count || !BeginScope(Scope, Self, Category))
		{
			return;
		}
		AddCostItems(Scope, Self);
		FPushScope Push(Scope);
		Call(Self);
	});

	SUBSCRIBE_METHOD(AFGEquipment::ChargeForUse_Internal, [](auto& Call, AFGEquipment* Self)
	{
		const ECategory Category = CostCategory(Self);
		RFDD_NOTE_COST(Self, Category, TEXT("ChargeForUse_Internal"));
		FScope Scope;
		if (Category == ECategory::Count || !BeginScope(Scope, Self, Category))
		{
			return;
		}
		AddCostItems(Scope, Self);
		FPushScope Push(Scope);
		Call(Self);
	});

	// Jetpack fuel availability: which fuel types can be picked, and the fuel count shown on the HUD.
	SUBSCRIBE_METHOD(AFGJetPack::HasEnoughFuelInInventory, [](auto& Call, const AFGJetPack* Self, TSubclassOf<UFGItemDescriptor> ItemClass)
	{
		FScope Scope;
		if (!BeginScope(Scope, Self, ECategory::JetpackFuel))
		{
			return;
		}
		GetJetpackFuelTypes(Self, Scope.Items);
		FPushScope Push(Scope);
		Call(Self, ItemClass);
	});

	SUBSCRIBE_METHOD(AFGJetPack::UpdateAvailableFuelTypes, [](auto& Call, AFGJetPack* Self)
	{
		FScope Scope;
		if (!BeginScope(Scope, Self, ECategory::JetpackFuel))
		{
			return;
		}
		GetJetpackFuelTypes(Self, Scope.Items);
		FPushScope Push(Scope);
		Call(Self);
	});

	SUBSCRIBE_METHOD(AFGJetPack::UpdateCurrentFuelAmount, [](auto& Call, AFGJetPack* Self)
	{
		FScope Scope;
		if (!BeginScope(Scope, Self, ECategory::JetpackFuel))
		{
			return;
		}
		GetJetpackFuelTypes(Self, Scope.Items);
		FPushScope Push(Scope);
		Call(Self);
	});

	// Chainsaw: ConsumeFuel checks for and removes one fuel item when its stored energy runs out.
	SUBSCRIBE_METHOD(AFGChainsaw::ConsumeFuel, [](auto& Call, AFGChainsaw* Self, float DeltaSeconds)
	{
		FScope Scope;
		if (!BeginScope(Scope, Self, ECategory::ChainsawFuel) || !Self->mFuelClass)
		{
			return;
		}
		Scope.Items.Add(Self->mFuelClass.Get());
		FPushScope Push(Scope);
		Call(Self, DeltaSeconds);
	});

	SUBSCRIBE_METHOD(AFGChainsaw::HasAnyFuel, [](auto& Call, const AFGChainsaw* Self)
	{
		FScope Scope;
		if (!BeginScope(Scope, Self, ECategory::ChainsawFuel) || !Self->mFuelClass)
		{
			return;
		}
		Scope.Items.Add(Self->mFuelClass.Get());
		FPushScope Push(Scope);
		Call(Self);
	});

	// Healing items: on the server, Consume applies the item and removes one from the hand slot with RemoveFromIndex.
	// On a client it only sends Server_Consume, so nothing is redirected there.
	SUBSCRIBE_METHOD(AFGConsumableEquipment::Consume, [](auto& Call, AFGConsumableEquipment* Self)
	{
		if (!Self->HasAuthority())
		{
			return;
		}
		FScope Scope;
		if (!BeginScope(Scope, Self, ECategory::Healing))
		{
			return;
		}
		const AFGCharacterPlayer* Character = Self->GetInstigatorCharacter();
		UFGInventoryComponentEquipment* HandSlot = Character ? Character->GetEquipmentSlot(EEquipmentSlot::ES_ARMS) : nullptr;
		TSubclassOf<UFGConsumableDescriptor> Consumable;
		int32 NumInHand = 0;
		Self->GetConsumeable(Consumable, NumInHand);
		if (!HandSlot || !Consumable)
		{
			return;
		}
		Scope.Inventory = HandSlot;
		Scope.Items.Add(Consumable.Get());
		FPushScope Push(Scope);
		Call(Self);
	});
}

void FReloadFromDDHooks::GetJetpackFuelTypes(const AFGJetPack* Jetpack, TArray<const UClass*, TInlineAllocator<6>>& OutTypes)
{
	if (!Jetpack)
	{
		return;
	}
	for (const TObjectPtr<UFGJetPackFuelParameters>& Parameters : Jetpack->mAllowedFuelTypes)
	{
		if (Parameters && Parameters->mFuelTypeDesc)
		{
			OutTypes.AddUnique(Parameters->mFuelTypeDesc.Get());
		}
	}
}

void FReloadFromDDHooks::RefreshJetpacks(UWorld* World, const UClass* OnlyItem)
{
	if (!World || GHookState != EHookState::Installed)
	{
		return;
	}
	for (TActorIterator<AFGJetPack> It(World); It; ++It)
	{
		AFGJetPack* Jetpack = *It;
		if (!IsValid(Jetpack) || !Jetpack->IsEquipped() || !Jetpack->GetInstigatorCharacter())
		{
			continue;
		}
		TArray<const UClass*, TInlineAllocator<6>> FuelTypes;
		GetJetpackFuelTypes(Jetpack, FuelTypes);
		if (FuelTypes.IsEmpty() || (OnlyItem && !FuelTypes.Contains(OnlyItem)))
		{
			continue;
		}
		// The same refresh the game runs when the player's inventory changes: available fuel types, the preferred
		// type, and the HUD count. It runs through the hooked functions, so it sees the depot if fuel is enabled.
		Jetpack->OnInventoryChanged(const_cast<UClass*>(OnlyItem ? OnlyItem : FuelTypes[0]), 0, nullptr);
	}
}

void FReloadFromDDHooks::DescribeStatus(TArray<FString>& OutLines)
{
	OutLines.Add(FString::Printf(TEXT("  Hooks: %s"), DescribeHookState()));
	FString Categories;
	for (uint8 Index = 0; Index < static_cast<uint8>(ECategory::Count); ++Index)
	{
		const ECategory Category = static_cast<ECategory>(Index);
		Categories += FString::Printf(TEXT(" %s=%s"), CategoryKey(Category), (GetEnabledMask() & CategoryBit(Category)) ? TEXT("on") : TEXT("off"));
	}
	OutLines.Add(FString::Printf(TEXT("  In effect:%s"), *Categories));
}

#if RELOADFROMDD_DEV_COMMANDS
void FReloadFromDDHooks::DescribeCallCounts(TArray<FString>& OutLines, const bool bReset)
{
	const double Now = FPlatformTime::Seconds();
	const double Seconds = FMath::Max(0.001, Now - GCounters.SinceSeconds);
	auto Line = [&](const TCHAR* Name, const std::atomic<uint64>& Calls, const std::atomic<uint64>& Redirected)
	{
		const uint64 CallCount = Calls.load(std::memory_order_relaxed);
		OutLines.Add(FString::Printf(TEXT("  %-16s %12llu calls (%10.1f/s), %llu redirected"), Name, CallCount, CallCount / Seconds, Redirected.load(std::memory_order_relaxed)));
	};
	OutLines.Add(FString::Printf(TEXT("Hook calls over %.1f s:"), Seconds));
	Line(TEXT("GetNumItems"), GCounters.GetNumItemsCalls, GCounters.GetNumItemsRedirected);
	Line(TEXT("HasItems"), GCounters.HasItemsCalls, GCounters.HasItemsRedirected);
	Line(TEXT("Remove"), GCounters.RemoveCalls, GCounters.RemoveRedirected);
	Line(TEXT("RemoveFromIndex"), GCounters.RemoveFromIndexCalls, GCounters.RemoveFromIndexRedirected);
	OutLines.Add(FString::Printf(TEXT("  Items taken: %llu from the depot, %llu from inventories (redirected removals only)"),
		GCounters.ItemsTakenFromDepot.load(), GCounters.ItemsTakenFromInventory.load()));
	if (bReset)
	{
		for (std::atomic<uint64>* Counter : {&GCounters.GetNumItemsCalls, &GCounters.GetNumItemsRedirected, &GCounters.HasItemsCalls, &GCounters.HasItemsRedirected,
			&GCounters.RemoveCalls, &GCounters.RemoveRedirected, &GCounters.RemoveFromIndexCalls, &GCounters.RemoveFromIndexRedirected,
			&GCounters.ItemsTakenFromDepot, &GCounters.ItemsTakenFromInventory})
		{
			Counter->store(0, std::memory_order_relaxed);
		}
		GCounters.SinceSeconds = Now;
		OutLines.Add(TEXT("  (counters reset)"));
	}
}

void FReloadFromDDHooks::DumpEquipment(TArray<FString>& OutLines)
{
	OutLines.Add(TEXT("Loaded equipment classes (cost to use / ammo / fuel):"));
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class->IsChildOf(AFGEquipment::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists)
			|| Class->GetName().StartsWith(TEXT("SKEL_")) || Class->GetName().StartsWith(TEXT("REINST_")))
		{
			continue;
		}
		const AFGEquipment* Defaults = Cast<AFGEquipment>(Class->GetDefaultObject());
		if (!Defaults)
		{
			continue;
		}
		FString Details;
		for (const FItemAmount& Cost : Defaults->GetCostToUse())
		{
			Details += FString::Printf(TEXT(" cost:%s x%d"), Cost.ItemClass ? *Cost.ItemClass->GetName() : TEXT("null"), Cost.Amount);
		}
		if (const AFGWeapon* Weapon = Cast<AFGWeapon>(Defaults))
		{
			for (const TSubclassOf<UFGAmmoType>& Ammo : Weapon->mAllowedAmmoClasses)
			{
				Details += FString::Printf(TEXT(" ammo:%s"), Ammo ? *Ammo->GetName() : TEXT("null"));
			}
		}
		if (const AFGJetPack* Jetpack = Cast<AFGJetPack>(Defaults))
		{
			TArray<const UClass*, TInlineAllocator<6>> FuelTypes;
			GetJetpackFuelTypes(Jetpack, FuelTypes);
			for (const UClass* Fuel : FuelTypes)
			{
				Details += FString::Printf(TEXT(" fuel:%s"), *Fuel->GetName());
			}
		}
		if (const AFGChainsaw* Chainsaw = Cast<AFGChainsaw>(Defaults))
		{
			Details += FString::Printf(TEXT(" fuel:%s"), Chainsaw->mFuelClass ? *Chainsaw->mFuelClass->GetName() : TEXT("null"));
		}
		OutLines.Add(FString::Printf(TEXT("  %s (parent %s):%s"), *Class->GetName(), Class->GetSuperClass() ? *Class->GetSuperClass()->GetName() : TEXT("-"),
			Details.IsEmpty() ? TEXT(" -") : *Details));
	}
}

namespace RFDD
{
	namespace
	{
		bool ParseSlot(const FString& Text, EEquipmentSlot& OutSlot)
		{
			static const TPair<const TCHAR*, EEquipmentSlot> Slots[] = {
				{TEXT("arms"), EEquipmentSlot::ES_ARMS}, {TEXT("back"), EEquipmentSlot::ES_BACK}, {TEXT("head"), EEquipmentSlot::ES_HEAD},
				{TEXT("body"), EEquipmentSlot::ES_BODY}, {TEXT("legs"), EEquipmentSlot::ES_LEGS}};
			for (const TPair<const TCHAR*, EEquipmentSlot>& Slot : Slots)
			{
				if (Text.Equals(Slot.Key, ESearchCase::IgnoreCase))
				{
					OutSlot = Slot.Value;
					return true;
				}
			}
			return false;
		}

		FString CountsText(const AFGCharacterPlayer* Character, const UClass* ItemClass)
		{
			if (!ItemClass)
			{
				return TEXT("(no item)");
			}
			const UFGInventoryComponent* Inventory = Character->GetInventory();
			const AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(Character->GetWorld());
			return FString::Printf(TEXT("%s: inventory %d, depot %d"), *ItemClass->GetName(),
				Inventory ? PlainInventoryCount(Inventory, ItemClass) : -1,
				Depot ? Depot->GetNumItemsFromCentralStorage(const_cast<UClass*>(ItemClass)) : -1);
		}
	}
}

void FReloadFromDDHooks::DevUseEquipment(AFGCharacterPlayer* Character, const FString& Action, const FString& Argument, TArray<FString>& OutLines)
{
	if (!Character)
	{
		OutLines.Add(TEXT("No player character on this machine."));
		return;
	}
	AFGEquipment* InArms = Character->GetEquipmentInSlot(EEquipmentSlot::ES_ARMS);
	AFGEquipment* OnBack = Character->GetEquipmentInSlot(EEquipmentSlot::ES_BACK);

	if (Action == TEXT("weapon") || Action == TEXT("reload") || Action == TEXT("empty"))
	{
		AFGWeapon* Weapon = Cast<AFGWeapon>(InArms);
		if (!Weapon)
		{
			OutLines.Add(TEXT("No weapon in hand."));
			return;
		}
		if (Action == TEXT("empty"))
		{
			if (!Weapon->HasAuthority())
			{
				OutLines.Add(TEXT("Refused: empty works on the server or host only."));
				return;
			}
			Weapon->mCurrentAmmoCount = 0;
			Weapon->FlushItemState();
			OutLines.Add(TEXT("Magazine emptied."));
		}
		else if (Action == TEXT("reload"))
		{
			if (Weapon->HasAuthority())
			{
				Weapon->Server_Reload(); // runs locally on the authority: CanReload, then Multicast_Reload and the reload timer
			}
			else
			{
				Weapon->Reload();
			}
			OutLines.Add(FString::Printf(TEXT("Reload requested (takes %.2f s); run RFDD.Use weapon afterwards."), Weapon->mReloadTime));
		}
		const UClass* Ammo = Weapon->mCurrentAmmunitionClass.Get();
		OutLines.Add(FString::Printf(TEXT("%s: loaded %d / %d, spare (as the weapon sees it) %d, can reload %s; %s"), *Weapon->GetClass()->GetName(),
			Weapon->GetCurrentAmmo(), Weapon->GetMagSize(), Weapon->GetSpareAmmunition(Weapon->mCurrentAmmunitionClass),
			Weapon->CanReload() ? TEXT("yes") : TEXT("no"), *CountsText(Character, Ammo)));
		return;
	}

	if (Action == TEXT("eat"))
	{
		AFGConsumableEquipment* Consumable = Cast<AFGConsumableEquipment>(InArms);
		if (!Consumable)
		{
			OutLines.Add(TEXT("No healing item in hand."));
			return;
		}
		TSubclassOf<UFGConsumableDescriptor> Item;
		int32 InHand = 0;
		Consumable->GetConsumeable(Item, InHand);
		OutLines.Add(FString::Printf(TEXT("Before: %d in hand; %s"), InHand, *CountsText(Character, Item.Get())));
		Consumable->Consume();
		Consumable->GetConsumeable(Item, InHand);
		OutLines.Add(FString::Printf(TEXT("After:  %d in hand; %s (a client only sends the request)"), InHand, *CountsText(Character, Item.Get())));
		return;
	}

	if (Action == TEXT("charge"))
	{
		EEquipmentSlot Slot = EEquipmentSlot::ES_HEAD;
		if (!Argument.IsEmpty() && !ParseSlot(Argument, Slot))
		{
			OutLines.Add(TEXT("Slot must be arms, back, head, body or legs."));
			return;
		}
		AFGEquipment* Equipment = Character->GetEquipmentInSlot(Slot);
		if (!Equipment)
		{
			OutLines.Add(TEXT("Nothing equipped in that slot."));
			return;
		}
		const TArray<FItemAmount>& Costs = Equipment->GetCostToUse();
		const UClass* First = Costs.Num() > 0 ? Costs[0].ItemClass.Get() : nullptr;
		OutLines.Add(FString::Printf(TEXT("%s: can afford %s; before: %s"), *Equipment->GetClass()->GetName(),
			Equipment->CanAffordUse() ? TEXT("yes") : TEXT("no"), *CountsText(Character, First)));
		if (Equipment->CanAffordUse())
		{
			Equipment->ChargeForUse();
		}
		OutLines.Add(FString::Printf(TEXT("After: %s"), *CountsText(Character, First)));
		return;
	}

	if (Action == TEXT("jetpack") || Action == TEXT("drain") || Action == TEXT("thrust"))
	{
		AFGJetPack* Jetpack = Cast<AFGJetPack>(OnBack);
		if (!Jetpack)
		{
			OutLines.Add(TEXT("No jetpack on the back."));
			return;
		}
		if (Action == TEXT("drain"))
		{
			Jetpack->SetCurrentFuel(0.0f);
			OutLines.Add(TEXT("Tank set to 0; it refuels from items when the jetpack regenerates (on the ground)."));
		}
		else if (Action == TEXT("thrust"))
		{
			Jetpack->SetIsThrusting(Argument.Equals(TEXT("on"), ESearchCase::IgnoreCase));
		}
		FString Available;
		for (const TSubclassOf<UFGItemDescriptor>& Type : Jetpack->mAvailableFuelTypes)
		{
			Available += FString::Printf(TEXT(" %s"), Type ? *Type->GetName() : TEXT("null"));
		}
		TArray<const UClass*, TInlineAllocator<6>> FuelTypes;
		GetJetpackFuelTypes(Jetpack, FuelTypes);
		const int32 CurrentType = Jetpack->mCurrentFuelType;
		const UClass* CurrentFuel = FuelTypes.IsValidIndex(CurrentType) ? FuelTypes[CurrentType] : nullptr;
		OutLines.Add(FString::Printf(TEXT("Jetpack: tank %.3f, paid %.3f, fuel type %d (selected %d), HUD count %d, available:%s; %s"),
			Jetpack->mCurrentFuel, Jetpack->mPaidForFuel, CurrentType, Jetpack->mSelectedFuelType, Jetpack->mCurrentFuelAmount,
			Available.IsEmpty() ? TEXT(" none") : *Available, *CountsText(Character, CurrentFuel)));
		return;
	}

	if (Action == TEXT("chainsaw") || Action == TEXT("saw"))
	{
		AFGChainsaw* Chainsaw = Cast<AFGChainsaw>(InArms);
		if (!Chainsaw)
		{
			OutLines.Add(TEXT("No chainsaw in hand."));
			return;
		}
		if (Action == TEXT("saw"))
		{
			// Burns stored energy for a long step, so the next call has to take a fuel item.
			const bool bHadFuel = Chainsaw->ConsumeFuel(FMath::Max(1.0f, FCString::Atof(*Argument)));
			OutLines.Add(FString::Printf(TEXT("ConsumeFuel returned %s"), bHadFuel ? TEXT("true") : TEXT("false (out of fuel)")));
		}
		OutLines.Add(FString::Printf(TEXT("Chainsaw: stored energy %.1f MJ, has any fuel %s; %s"), Chainsaw->mEnergyStored,
			Chainsaw->HasAnyFuel() ? TEXT("yes") : TEXT("no"), *CountsText(Character, Chainsaw->mFuelClass.Get())));
		return;
	}

	OutLines.Add(TEXT("Actions: weapon | reload | empty | eat | charge [head|body|...] | jetpack | drain | thrust <on|off> | chainsaw | saw [seconds]"));
}
#endif
