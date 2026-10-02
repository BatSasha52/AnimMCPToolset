// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

using UnrealBuildTool;

public class AnimMCPToolset : ModuleRules
{
	public AnimMCPToolset(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Json",
				"JsonUtilities",
				"ToolsetRegistry",
			}
		);

		// The Unreal MCP plugin (ModelContextProtocol) discovers toolsets through the
		// ToolsetRegistry at runtime, so this module does not link against it.
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AnimGraph",
				"AnimGraphRuntime",
				"AssetRegistry",
				"AssetTools",
				"BlueprintGraph",
				"Kismet",
				"UnrealEd",
			}
		);
	}
}
