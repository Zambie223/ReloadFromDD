using UnrealBuildTool;

public class ReloadFromDD : ModuleRules
{
	public ReloadFromDD(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		// FactoryGame transitive dependencies (the subset this mod's headers pull in).
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject",
			"Engine",
			"DeveloperSettings",
			"PhysicsCore",
			"InputCore",
			"GeometryCollectionEngine",
			"AnimGraphRuntime",
			"AssetRegistry",
			"NavigationSystem",
			"AIModule",
			"GameplayTasks",
			"SlateCore", "Slate", "UMG",
			"RenderCore",
			"CinematicCamera",
			"Foliage",
			"EnhancedInput",
			"NetCore",
			"GameplayTags",
			"Json", "JsonUtilities"
		});

		// Header stubs
		PublicDependencyModuleNames.AddRange(new string[] {
			"DummyHeaders",
		});

		if (Target.Type == TargetRules.TargetType.Editor) {
			PublicDependencyModuleNames.AddRange(new string[] {"AnimGraph"});
		}
		PublicDependencyModuleNames.AddRange(new string[] {"FactoryGame", "SML"});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"Projects", // IPluginManager, to detect Vanilla Addons
		});

		// Development aids: dev console commands, hook call counters and unscoped-removal diagnostics.
		// Set to 0 for every public release.
		PublicDefinitions.Add("RELOADFROMDD_DEV_COMMANDS=0");
	}
}
