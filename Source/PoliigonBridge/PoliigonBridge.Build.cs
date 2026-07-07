// GPL v2+ — derived in part from the Poliigon Blender addon (GPL).

using UnrealBuildTool;

public class PoliigonBridge : ModuleRules
{
	public PoliigonBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"InputCore",
			"Projects",
			"HTTP",
			"Json",
			"ImageWrapper",
			"RenderCore",
			"DeveloperSettings",
			// Editor
			"UnrealEd",
			"AssetTools",
			"AssetRegistry",
			"MaterialEditor",
			"ContentBrowser",
			"ToolMenus",
			"WorkspaceMenuStructure",
		});
	}
}
