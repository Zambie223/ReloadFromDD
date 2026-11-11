// Console commands. On a dedicated server they can be run from the Server Manager console, which the game
// restricts to administrators. RFDD.Set is refused on clients.

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ReloadFromDDConstants.h"
#include "ReloadFromDDSubsystem.h"

#if RELOADFROMDD_DEV_COMMANDS
#include "Equipment/FGEquipment.h"
#include "FGCentralStorageSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGInventoryComponent.h"
#include "FGInventoryComponentEquipment.h"
#include "FGPlayerState.h"
#include "GameFramework/PlayerController.h"
#include "ReloadFromDDHooks.h"
#include "Resources/FGItemDescriptor.h"
#include "UObject/UObjectIterator.h"
#endif

namespace
{
	AReloadFromDDSubsystem* FindSubsystem(UWorld* World, FOutputDevice& Ar)
	{
		AReloadFromDDSubsystem* Subsystem = AReloadFromDDSubsystem::Get(World);
		if (!Subsystem)
		{
			Ar.Log(TEXT("Reload From DD is not active in this world (main menu, or not replicated to this client yet)."));
		}
		return Subsystem;
	}

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GStatusCommand(
		TEXT("RFDD.Status"),
		TEXT("Shows which consumables Reload From DD takes from the Dimensional Depot."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			if (const AReloadFromDDSubsystem* Subsystem = FindSubsystem(World, Ar))
			{
				Subsystem->DescribeStatus(Ar);
			}
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GSetCommand(
		TEXT("RFDD.Set"),
		TEXT("RFDD.Set <Ammo|JetpackFuel|ChainsawFuel|Filters|Healing> <true|false> - changes and saves a switch on the host or server."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			if (Args.Num() != 2)
			{
				Ar.Log(TEXT("Usage: RFDD.Set <Ammo|JetpackFuel|ChainsawFuel|Filters|Healing> <true|false>"));
				return;
			}
			FString Message;
			AReloadFromDDSubsystem::SetConfigValue(World, Args[0], Args[1], Message);
			Ar.Log(Message);
		}));

#if RELOADFROMDD_DEV_COMMANDS
	void LogLines(FOutputDevice& Ar, const TArray<FString>& Lines)
	{
		for (const FString& Line : Lines)
		{
			Ar.Log(Line);
			UE_LOG(LogTemp, Log, TEXT("[RFDD dev] %s"), *Line);
		}
	}

	/** Finds a loaded item descriptor class by name, with or without the _C suffix. */
	UClass* FindItemClass(const FString& Name)
	{
		const FString WithSuffix = Name.EndsWith(TEXT("_C")) ? Name : Name + TEXT("_C");
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (Class->IsChildOf(UFGItemDescriptor::StaticClass()) && !Class->HasAnyClassFlags(CLASS_NewerVersionExists)
				&& (Class->GetName().Equals(WithSuffix, ESearchCase::IgnoreCase) || Class->GetName().Equals(Name, ESearchCase::IgnoreCase)))
			{
				return Class;
			}
		}
		return nullptr;
	}

	/** Which player the dev commands act on, in player-controller order (0 = host or first player). Clients only have their own. */
	TAutoConsoleVariable<int32> CVarDevPlayer(
		TEXT("RFDD.DevPlayer"), 0,
		TEXT("[dev] Index of the player the RFDD dev commands act on (0 = host or first player)."));

	/** The selected player's character (see RFDD.DevPlayer), or on a client its own. */
	AFGCharacterPlayer* FindCharacter(UWorld* World)
	{
		if (!World)
		{
			return nullptr;
		}
		const int32 Wanted = World->GetNetMode() == NM_Client ? 0 : FMath::Max(0, CVarDevPlayer.GetValueOnGameThread());
		int32 Index = 0;
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			if (AFGCharacterPlayer* Character = Cast<AFGCharacterPlayer>(It->Get() ? It->Get()->GetPawn() : nullptr))
			{
				if (Index++ == Wanted)
				{
					return Character;
				}
			}
		}
		return nullptr;
	}

	FString Counts(AFGCharacterPlayer* Character, UClass* Item)
	{
		const AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(Character->GetWorld());
		return FString::Printf(TEXT("%s: inventory %d, depot %d"), *Item->GetName(),
			Character->GetInventory() ? Character->GetInventory()->GetNumItems(Item) : -1, Depot ? Depot->GetNumItemsFromCentralStorage(Item) : -1);
	}

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GCallsCommand(
		TEXT("RFDD.Calls"),
		TEXT("[dev] RFDD.Calls [reset] - hook call counts per second, to measure overhead."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			TArray<FString> Lines;
			FReloadFromDDHooks::DescribeCallCounts(Lines, Args.Num() > 0 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase));
			LogLines(Ar, Lines);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GDumpCommand(
		TEXT("RFDD.DumpEquipment"),
		TEXT("[dev] Lists every loaded equipment class with its cost to use, ammo and fuel."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			TArray<FString> Lines;
			FReloadFromDDHooks::DumpEquipment(Lines);
			LogLines(Ar, Lines);
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GCountCommand(
		TEXT("RFDD.Count"),
		TEXT("[dev] RFDD.Count <ItemDesc> - the player's inventory count (without the depot) and the depot count."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			AFGCharacterPlayer* Character = FindCharacter(World);
			UClass* Item = Args.Num() > 0 ? FindItemClass(Args[0]) : nullptr;
			if (!Character || !Item)
			{
				Ar.Log(TEXT("Usage: RFDD.Count <ItemDesc> (needs a player and a loaded item class)"));
				return;
			}
			LogLines(Ar, {Counts(Character, Item)});
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GDepotCommand(
		TEXT("RFDD.Depot"),
		TEXT("[dev] RFDD.Depot <ItemDesc> <+N|-N> - adds (via an empty inventory slot and the normal upload) or removes depot items. Host or server only."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			AFGCharacterPlayer* Character = FindCharacter(World);
			AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(World);
			UClass* Item = Args.Num() > 1 ? FindItemClass(Args[0]) : nullptr;
			if (!Character || !Depot || !Item || !Character->HasAuthority())
			{
				Ar.Log(TEXT("Usage (host or server): RFDD.Depot <ItemDesc> <+N|-N>"));
				return;
			}
			const int32 Delta = FCString::Atoi(*Args[1]);
			if (Delta < 0)
			{
				const int32 Taken = Depot->TryRemoveItemsFromCentralStorage(Item, -Delta);
				LogLines(Ar, {FString::Printf(TEXT("Removed %d from the depot. %s"), Taken, *Counts(Character, Item))});
				return;
			}
			UFGInventoryComponent* Inventory = Character->GetInventory();
			const int32 StackSize = FMath::Max(1, UFGItemDescriptor::GetStackSize(Item));
			int32 Remaining = Delta;
			int32 Uploaded = 0;
			while (Remaining > 0)
			{
				const int32 Slot = Inventory->FindEmptyIndex();
				if (Slot == INDEX_NONE)
				{
					Ar.Log(TEXT("Stopped: needs one empty inventory slot to stage the items."));
					break;
				}
				const int32 Batch = FMath::Min(Remaining, StackSize);
				Inventory->AddStackToIndex(Slot, FInventoryStack(Batch, Item), true);
				int32 Guard = 0;
				while (Inventory->IsSomethingOnIndex(Slot) && Guard++ < Batch + 1)
				{
					FInventoryStack Before;
					Inventory->GetStackFromIndex(Slot, Before);
					if (!Depot->UploadItemFromInventoryToCentralStorage(Inventory, Slot))
					{
						break;
					}
					FInventoryStack After;
					Inventory->GetStackFromIndex(Slot, After);
					Uploaded += Before.NumItems - (After.HasItems() ? After.NumItems : 0);
				}
				if (Inventory->IsSomethingOnIndex(Slot))
				{
					Inventory->RemoveAllFromIndex(Slot);
					Ar.Log(TEXT("Stopped: the depot is full for this item (leftover staged items were discarded)."));
					break;
				}
				Remaining -= Batch;
			}
			LogLines(Ar, {FString::Printf(TEXT("Uploaded %d. %s"), Uploaded, *Counts(Character, Item))});
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GInvCommand(
		TEXT("RFDD.Inv"),
		TEXT("[dev] RFDD.Inv <ItemDesc> <+N|-N|clear> - adds to or removes from the player's inventory. Host or server only."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			AFGCharacterPlayer* Character = FindCharacter(World);
			UClass* Item = Args.Num() > 1 ? FindItemClass(Args[0]) : nullptr;
			if (!Character || !Item || !Character->HasAuthority())
			{
				Ar.Log(TEXT("Usage (host or server): RFDD.Inv <ItemDesc> <+N|-N|clear>"));
				return;
			}
			UFGInventoryComponent* Inventory = Character->GetInventory();
			const int32 Have = Inventory->GetNumItems(Item);
			if (Args[1].Equals(TEXT("clear"), ESearchCase::IgnoreCase))
			{
				if (Have > 0)
				{
					Inventory->Remove(Item, Have);
				}
			}
			else
			{
				const int32 Delta = FCString::Atoi(*Args[1]);
				if (Delta > 0)
				{
					Inventory->AddStack(FInventoryStack(Delta, Item), true);
				}
				else if (Delta < 0)
				{
					Inventory->Remove(Item, FMath::Min(Have, -Delta));
				}
			}
			LogLines(Ar, {Counts(Character, Item)});
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GEquipCommand(
		TEXT("RFDD.Equip"),
		TEXT("[dev] RFDD.Equip <EquipmentDesc> <arms|back|head|body|legs> [count] - puts an item into an equipment slot and makes it active. Host or server only."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			AFGCharacterPlayer* Character = FindCharacter(World);
			UClass* Item = Args.Num() > 1 ? FindItemClass(Args[0]) : nullptr;
			static const TPair<const TCHAR*, EEquipmentSlot> Slots[] = {
				{TEXT("arms"), EEquipmentSlot::ES_ARMS}, {TEXT("back"), EEquipmentSlot::ES_BACK}, {TEXT("head"), EEquipmentSlot::ES_HEAD},
				{TEXT("body"), EEquipmentSlot::ES_BODY}, {TEXT("legs"), EEquipmentSlot::ES_LEGS}};
			EEquipmentSlot Slot = EEquipmentSlot::ES_NONE;
			for (const TPair<const TCHAR*, EEquipmentSlot>& Pair : Slots)
			{
				if (Args.Num() > 1 && Args[1].Equals(Pair.Key, ESearchCase::IgnoreCase))
				{
					Slot = Pair.Value;
				}
			}
			if (!Character || !Item || Slot == EEquipmentSlot::ES_NONE || !Character->HasAuthority())
			{
				Ar.Log(TEXT("Usage (host or server): RFDD.Equip <EquipmentDesc> <arms|back|head|body|legs> [count]"));
				return;
			}
			UFGInventoryComponentEquipment* SlotInventory = Character->GetEquipmentSlot(Slot);
			if (!SlotInventory)
			{
				Ar.Log(TEXT("No such equipment slot."));
				return;
			}
			int32 Index = SlotInventory->FindEmptyIndex();
			if (Index == INDEX_NONE)
			{
				// Test saves only: replace whatever is active in the full slot.
				Index = FMath::Max(0, SlotInventory->GetActiveIndex());
				FInventoryStack Replaced;
				SlotInventory->GetStackFromIndex(Index, Replaced);
				SlotInventory->RemoveAllFromIndex(Index);
				Ar.Logf(TEXT("Slot was full; discarded %s from index %d."), Replaced.HasItems() ? *Replaced.Item.GetItemClass()->GetName() : TEXT("nothing"), Index);
			}
			const int32 Count = Args.Num() > 2 ? FMath::Max(1, FCString::Atoi(*Args[2])) : 1;
			const int32 Added = SlotInventory->AddStackToIndex(Index, FInventoryStack(Count, Item), true);
			SlotInventory->SetActiveEquipmentIndex(Index);
			AFGEquipment* Equipped = Character->GetEquipmentInSlot(Slot);
			LogLines(Ar, {FString::Printf(TEXT("Added %d to slot index %d; now equipped: %s"), Added, Index, Equipped ? *Equipped->GetClass()->GetName() : TEXT("nothing"))});
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GPriorityCommand(
		TEXT("RFDD.InvFirst"),
		TEXT("[dev] RFDD.InvFirst <0|1> - sets the game's \"take from inventory before Dimensional Depot\" setting for the player."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			AFGCharacterPlayer* Character = FindCharacter(World);
			AFGPlayerState* PlayerState = Character ? Character->GetPlayerState<AFGPlayerState>() : nullptr;
			if (!PlayerState)
			{
				Ar.Log(TEXT("No player state."));
				return;
			}
			if (Args.Num() > 0)
			{
				PlayerState->SetTakeFromInventoryBeforeCentralStorage(FCString::Atoi(*Args[0]) != 0);
			}
			LogLines(Ar, {FString::Printf(TEXT("Take from inventory first: %s"), PlayerState->GetTakeFromInventoryBeforeCentralStorage() ? TEXT("yes") : TEXT("no (depot first)"))});
		}));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice GUseCommand(
		TEXT("RFDD.Use"),
		TEXT("[dev] RFDD.Use <weapon|reload|empty|eat|charge [slot]|jetpack|drain|thrust <on|off>|chainsaw|saw [seconds]> - drives equipment without input."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			TArray<FString> Lines;
			FReloadFromDDHooks::DevUseEquipment(FindCharacter(World), Args.Num() > 0 ? Args[0].ToLower() : FString(), Args.Num() > 1 ? Args[1] : FString(), Lines);
			LogLines(Ar, Lines);
		}));
#endif
}
