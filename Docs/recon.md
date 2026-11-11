# Consumable call paths (game 1.2.5, CL 517554)

Found by disassembling the shipped game DLL with its PDB (`dumpbin /disasm`). These are the paths the hooks rely on; recheck them after a game update.

| Equipment | Check | Spend | Runs on |
|---|---|---|---|
| Weapons (rifle, rebar gun, nobelisk detonator) | `AFGWeapon::GetSpareAmmunition` → owner inventory `GetNumItems`. `CanReload`, `TryAutoReload`, the HUD and `ActualReload` all use it. | `AFGWeapon::ActualReload` (reload timer): loads `min(mag - loaded, spare)` everywhere, then `UFGInventoryComponent::Remove(ammo, n)` only if `Role == Authority`. | Reload → `Server_Reload` → `CanReload` → `Multicast_Reload` → timer → `ActualReload` on every machine |
| Jetpack | `HasEnoughFuelInInventory` and `UpdateAvailableFuelTypes` → `HasItems`; HUD count `UpdateCurrentFuelAmount` → `GetNumItems`. Refreshed from `OnInventoryChanged` (inventory delegates) and `Equip`. | `RegenerateFuel` → `AFGEquipment::CanAffordUse` → `ChargeForUse_Internal` (also `Server_ChargeForUse`). `ConsumeFuel` only burns the tank. | Client and server |
| Gas mask | `AFGGasMask::Tick` → `CanAffordUse` | `ChargeForUse_Internal` → `Remove` for each `mCostToUse` entry | Client and server; no authority check in `ChargeForUse_Internal` |
| Hazmat suit | Blueprint `Equip_HazmatSuit` (parent `FGSuitBase`, references `Desc_HazmatFilter`) | Not visible natively; expected to use `CanAffordUse`/`ChargeForUse` (the exec thunks call the native functions). To confirm in game with the dev unscoped-removal log. | ? |
| Chainsaw | `HasAnyFuel` → `HasItems`; `ConsumeFuel` → `HasItems` | `ConsumeFuel` → `Remove(mFuelClass, 1)`, then adds its energy | Client and server |
| Healing items | — | `AFGConsumableEquipment::Consume`: on a client only `Server_Consume`; on the server reads the arms equipment-slot stack, `ConsumedBy`, then `RemoveFromIndex(activeIndex, 1)` on the slot inventory | Server |
| Parachute | Deploy paths touch no inventory | — | Out of scope |

Other facts:
- `AFGEquipment::CanAffordUse` returns true without checking when a game-state flag at +0x730 is set (no-cost cheat, presumably).
- `UFGInventoryLibrary::GrabItemsFromInventoryAndCentralStorage` (build costs) has no `IsCentralStorageBuilt()` check.
- `AFGCentralStorageSubsystem::TryRemoveItemsFromCentralStorage` multicasts `Client_ItemAmountUpdatedInCentralStorage`.
- Every `exec*` thunk checked (`ActualReload`, `CanReload`, `GetSpareAmmunition`, `CanAffordUse`, `ChargeForUse`) calls the native function, so Blueprint callers reach the native hooks.
