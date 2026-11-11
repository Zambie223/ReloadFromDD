"""Creates or updates /ReloadFromDD/ReloadFromDDConfiguration, the mod's SML Mod Configuration asset.

Run headless from the project root (the editor must not be open):
  UnrealEditor-Cmd.exe FactoryGame.uproject -run=pythonscript -script="<this file>" -unattended -nullrhi -nosplash

The asset is a Blueprint because SML's Mod Options widgets come from its Blueprint property classes
(the native UConfigProperty::CreateEditorWidget returns null). Keys must match ReloadFromDDConstants.h.
"""
import unreal

MOD = "ReloadFromDD"
ASSET_DIR = f"/{MOD}"
ASSET_NAME = "ReloadFromDDConfiguration"
ASSET_PATH = f"{ASSET_DIR}/{ASSET_NAME}"
SML_PROPS = "/SML/Interface/UI/Menu/Mods/ConfigProperties"

COMMON_NOTE = (
    "Your inventory or the depot is used first according to the game's own setting,\n"
    "\"Take from inventory before Dimensional Depot\". In multiplayer the host's or server's switches apply."
)

# key, display name, what it covers. All default on.
SWITCHES = [
    ("Ammo", "Weapon ammo",
     "Weapons reload from the Dimensional Depot: rifle ammo, rebar and nobelisks."),
    ("JetpackFuel", "Jetpack fuel",
     "The jetpack refuels from the Dimensional Depot, and its fuel count includes depot stock."),
    ("ChainsawFuel", "Chainsaw fuel",
     "The chainsaw refuels from the Dimensional Depot."),
    ("Filters", "Gas mask and hazmat filters",
     "The gas mask and the hazmat suit take new filters from the Dimensional Depot."),
    ("Healing", "Healing items",
     "Eating or using a healing item in your hand takes it from the Dimensional Depot.\n"
     "With inventory first, the item in your hand is used first and its last one stays in hand while the depot has more."),
]


def fail(message):
    unreal.log_error(f"[{MOD}] {message}")
    raise RuntimeError(message)


def load_sml_class(name):
    cls = unreal.load_class(None, f"{SML_PROPS}/{name}.{name}_C")
    if cls is None:
        fail(f"Could not load SML class {name}")
    return cls


def generated_class_of(blueprint):
    try:
        cls = unreal.BlueprintEditorLibrary.generated_class(blueprint)
        if cls is not None:
            return cls
    except Exception as error:  # library may be unavailable; fall back to loading by path
        unreal.log_warning(f"[{MOD}] BlueprintEditorLibrary.generated_class failed: {error}")
    return unreal.load_class(None, f"{ASSET_PATH}.{ASSET_NAME}_C")


def get_or_create_blueprint():
    """Updates the existing asset in place, so the asset never has to be deleted.

    Tries loading first: in the headless commandlet the asset registry has not scanned the plugin, so
    does_asset_exist() says False for an asset that exists, and create_asset() then collides with it.
    """
    blueprint = unreal.load_asset(ASSET_PATH)
    if blueprint is not None:
        return blueprint
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", unreal.ModConfiguration)
    blueprint = unreal.AssetToolsHelpers.get_asset_tools().create_asset(ASSET_NAME, ASSET_DIR, unreal.Blueprint, factory)
    if blueprint is None:
        fail("create_asset returned None")
    return blueprint


def get_or_create_subobject(existing, cls, outer, name):
    """Reuses an existing instanced subobject of the right class, so re-running never clashes on the object name."""
    if existing is not None and existing.get_class() == cls:
        return existing
    return unreal.new_object(cls, outer=outer, name=name)


def main():
    blueprint = get_or_create_blueprint()
    cls = generated_class_of(blueprint)
    if cls is None:
        fail("No generated class for the Blueprint")
    cdo = unreal.get_default_object(cls)

    section_cls = load_sml_class("BP_ConfigPropertySection")
    bool_cls = load_sml_class("BP_ConfigPropertyBool")

    cdo.modify()
    root = get_or_create_subobject(cdo.get_editor_property("root_section"), section_cls, cdo, "RootSection")
    root.modify()
    existing_properties = root.get_editor_property("section_properties") or {}

    section_properties = {}
    for key, display_name, covers in SWITCHES:
        prop = get_or_create_subobject(existing_properties.get(key), bool_cls, root, key)
        prop.modify()
        prop.set_editor_property("display_name", display_name)
        prop.set_editor_property("tooltip", f"{covers}\n{COMMON_NOTE}")
        prop.set_editor_property("default_value", True)
        prop.set_editor_property("value", True)
        prop.set_editor_property("requires_world_reload", False)
        section_properties[key] = prop
    root.set_editor_property("section_properties", section_properties)
    # SML defaults sections to a horizontal list (with a scroll bar); the Mods menu reads top to bottom, so stack them.
    root.set_editor_property("widget_type", unreal.CP_SectionWidgetType.CPS_VERTICAL)

    config_id = unreal.ConfigId()
    config_id.set_editor_property("mod_reference", MOD)
    config_id.set_editor_property("config_category", "")
    cdo.set_editor_property("config_id", config_id)
    cdo.set_editor_property("display_name", "Reload From DD")
    cdo.set_editor_property("description", "Which consumables your equipment takes from the Dimensional Depot.")
    cdo.set_editor_property("root_section", root)

    if not unreal.EditorAssetLibrary.save_loaded_asset(blueprint, False):
        fail("Saving the asset failed")
    unreal.log(f"[{MOD}] Saved {ASSET_PATH}")


main()
