// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#include "AnimAssetToolset.h"
#include "AnimDataToolset.h"
#include "AnimGraphEditToolset.h"
#include "AnimInspectToolset.h"
#include "AnimStateMachineToolset.h"

/**
 * Toolsets are not discovered automatically: each UToolsetDefinition subclass must be
 * registered explicitly. The Unreal MCP plugin then exposes every registered toolset.
 */
class FAnimMCPToolsetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolsetRegistry::RegisterToolsetClass(UAnimInspectToolset::StaticClass());
		UToolsetRegistry::RegisterToolsetClass(UAnimGraphEditToolset::StaticClass());
		UToolsetRegistry::RegisterToolsetClass(UAnimStateMachineToolset::StaticClass());
		UToolsetRegistry::RegisterToolsetClass(UAnimAssetToolset::StaticClass());
		UToolsetRegistry::RegisterToolsetClass(UAnimDataToolset::StaticClass());
	}

	virtual void ShutdownModule() override
	{
		if (!UObjectInitialized())
		{
			return;
		}
		UToolsetRegistry::UnregisterToolsetClass(UAnimDataToolset::StaticClass());
		UToolsetRegistry::UnregisterToolsetClass(UAnimAssetToolset::StaticClass());
		UToolsetRegistry::UnregisterToolsetClass(UAnimStateMachineToolset::StaticClass());
		UToolsetRegistry::UnregisterToolsetClass(UAnimGraphEditToolset::StaticClass());
		UToolsetRegistry::UnregisterToolsetClass(UAnimInspectToolset::StaticClass());
	}
};

IMPLEMENT_MODULE(FAnimMCPToolsetModule, AnimMCPToolset);
