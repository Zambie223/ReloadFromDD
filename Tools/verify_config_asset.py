"""Loads /ReloadFromDD/ReloadFromDDConfiguration in a fresh editor process and prints what was saved.

Run headless from the project root (the editor must not be open):
  UnrealEditor-Cmd.exe FactoryGame.uproject -run=pythonscript -script="<this file>" -unattended -nullrhi -nosplash
Output goes to Saved/Logs/FactoryGame.log, not stdout.
"""
import unreal

MOD = "ReloadFromDD"
EXPECTED_KEYS = ["Ammo", "JetpackFuel", "ChainsawFuel", "Filters", "Healing"]

cls = unreal.load_class(None, f"/{MOD}/ReloadFromDDConfiguration.ReloadFromDDConfiguration_C")
if cls is None:
    unreal.log_error(f"[{MOD}] VERIFY: class not found")
    raise RuntimeError("class not found")
cdo = unreal.get_default_object(cls)
config_id = cdo.get_editor_property("config_id")
unreal.log(f"[{MOD}] VERIFY config_id = {config_id.get_editor_property('mod_reference')!r} / {config_id.get_editor_property('config_category')!r}")
unreal.log(f"[{MOD}] VERIFY display_name = {cdo.get_editor_property('display_name')}")
root = cdo.get_editor_property("root_section")
if root is None:
    unreal.log_error(f"[{MOD}] VERIFY: root_section is None")
    raise RuntimeError("root_section missing")
unreal.log(f"[{MOD}] VERIFY root_section class = {root.get_class().get_path_name()} widget_type = {root.get_editor_property('widget_type')}")
properties = root.get_editor_property("section_properties")
for key, prop in properties.items():
    unreal.log(
        f"[{MOD}] VERIFY {key} -> {prop.get_class().get_name()} default={prop.get_editor_property('default_value')} "
        f"world_reload={prop.get_editor_property('requires_world_reload')} name='{prop.get_editor_property('display_name')}'"
    )
missing = [key for key in EXPECTED_KEYS if key not in properties]
if missing:
    unreal.log_error(f"[{MOD}] VERIFY: missing keys {missing}")
    raise RuntimeError("missing keys")
unreal.log(f"[{MOD}] VERIFY OK ({len(properties)} switches)")
