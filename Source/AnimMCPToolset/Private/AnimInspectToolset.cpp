// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#include "AnimInspectToolset.h"

#include "AnimMCPHelpers.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_CustomTransitionResult.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "UObject/UObjectHash.h"

namespace
{
	FString StatusToString(EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_Unknown:             return TEXT("unknown");
		case BS_Dirty:               return TEXT("dirty");
		case BS_Error:               return TEXT("error");
		case BS_UpToDate:            return TEXT("up_to_date");
		case BS_BeingCreated:        return TEXT("being_created");
		case BS_UpToDateWithWarnings:return TEXT("up_to_date_with_warnings");
		default:                     return TEXT("unknown");
		}
	}

	bool IsInternalAnimNodeClass(const UClass* Class)
	{
		return Class->IsChildOf(UAnimGraphNode_Root::StaticClass())
			|| Class->IsChildOf(UAnimGraphNode_StateResult::StaticClass())
			|| Class->IsChildOf(UAnimGraphNode_TransitionResult::StaticClass())
			|| Class->IsChildOf(UAnimGraphNode_CustomTransitionResult::StaticClass());
	}
}

FAnimMCPResult UAnimInspectToolset::anim_list_anim_blueprints(const FString& folder, const FString& skeleton_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	USkeleton* FilterSkeleton = nullptr;
	if (!AnimMCP::IsUnset(skeleton_path))
	{
		FilterSkeleton = AnimMCP::ResolveSkeleton(skeleton_path, Error);
		if (!FilterSkeleton)
		{
			return AnimMCP::Fail(Error);
		}
	}

	AnimMCP::EnsureFolderScanned(folder.IsEmpty() ? FString(TEXT("/Game")) : folder);

	FARFilter Filter;
	Filter.ClassPaths.Add(UAnimBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(FName(*(folder.IsEmpty() ? FString(TEXT("/Game")) : folder)));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	FAssetRegistryModule::GetRegistry().GetAssets(Filter, Assets);

	TArray<TSharedRef<FJsonObject>> Items;
	for (const FAssetData& Asset : Assets)
	{
		if (FilterSkeleton && !FilterSkeleton->IsCompatibleForEditor(Asset, TEXT("TargetSkeleton")))
		{
			continue;
		}

		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());

		FString Value;
		if (Asset.GetTagValue(TEXT("TargetSkeleton"), Value))
		{
			Item->SetStringField(TEXT("skeleton"), FPackageName::ExportTextPathToObjectPath(Value));
		}
		if (Asset.GetTagValue(TEXT("ParentClass"), Value))
		{
			Item->SetStringField(TEXT("parent_class"), FPackageName::ExportTextPathToObjectPath(Value));
		}
		Items.Add(Item);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("anim_blueprints"), AnimMCP::ToJsonArray(Items));
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimInspectToolset::anim_get_blueprint_info(const FString& blueprint_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ false, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}

	TArray<UEdGraph*> Graphs;
	AnimBP->GetAllGraphs(Graphs);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("path"), AnimBP->GetPathName());
	Payload->SetStringField(TEXT("skeleton"), AnimBP->TargetSkeleton ? AnimBP->TargetSkeleton->GetPathName() : FString());
	Payload->SetStringField(TEXT("parent_class"), AnimBP->ParentClass ? AnimBP->ParentClass->GetPathName() : FString());
	if (USkeletalMesh* PreviewMesh = AnimBP->GetPreviewMesh())
	{
		Payload->SetStringField(TEXT("preview_mesh"), PreviewMesh->GetPathName());
	}
	Payload->SetStringField(TEXT("status"), StatusToString(AnimBP->Status));
	Payload->SetBoolField(TEXT("is_template"), AnimBP->bIsTemplate);
	Payload->SetBoolField(TEXT("is_dirty"), AnimBP->GetOutermost()->IsDirty());
	Payload->SetNumberField(TEXT("graph_count"), Graphs.Num());
	Payload->SetNumberField(TEXT("variable_count"), AnimBP->NewVariables.Num());
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimInspectToolset::anim_list_graphs(const FString& blueprint_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ false, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}

	// Walk top-level graphs and every node-owned sub-graph so nested state machines are included.
	TArray<UEdGraph*> Pending;
	AnimBP->GetAllGraphs(Pending);
	TSet<UEdGraph*> Visited;
	TArray<TSharedRef<FJsonObject>> Items;
	while (!Pending.IsEmpty())
	{
		UEdGraph* Graph = Pending.Pop(EAllowShrinking::No);
		if (!Graph || Visited.Contains(Graph))
		{
			continue;
		}
		Visited.Add(Graph);
		Items.Add(AnimMCP::GraphToJson(Graph));
		Pending.Append(Graph->SubGraphs);
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				Pending.Append(Node->GetSubGraphs());
			}
		}
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("graphs"), AnimMCP::ToJsonArray(Items));
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimInspectToolset::anim_list_nodes(const FString& blueprint_path, const FString& graph, bool include_pins)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ false, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}

	UEdGraph* Graph = AnimMCP::FindGraph(AnimBP, graph, Error);
	if (!Graph)
	{
		return AnimMCP::Fail(Error);
	}

	TArray<TSharedRef<FJsonObject>> Items;
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node)
		{
			Items.Add(AnimMCP::NodeToJson(Node, include_pins));
		}
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetObjectField(TEXT("graph"), AnimMCP::GraphToJson(Graph));
	Payload->SetArrayField(TEXT("nodes"), AnimMCP::ToJsonArray(Items));
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimInspectToolset::anim_get_node(const FString& blueprint_path, const FString& node_guid)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ false, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}

	UEdGraphNode* Node = AnimMCP::FindNode(AnimBP, node_guid, Error);
	if (!Node)
	{
		return AnimMCP::Fail(Error);
	}
	return AnimMCP::Ok(AnimMCP::NodeToJson(Node, /*bIncludePins*/ true));
}

FAnimMCPResult UAnimInspectToolset::anim_list_skeleton_bones(const FString& asset_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	USkeleton* Skeleton = AnimMCP::ResolveSkeleton(asset_path, Error);
	if (!Skeleton)
	{
		return AnimMCP::Fail(Error);
	}

	const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
	TArray<TSharedRef<FJsonObject>> Bones;
	for (int32 BoneIndex = 0; BoneIndex < RefSkeleton.GetNum(); ++BoneIndex)
	{
		TSharedRef<FJsonObject> Bone = MakeShared<FJsonObject>();
		Bone->SetNumberField(TEXT("index"), BoneIndex);
		Bone->SetStringField(TEXT("name"), RefSkeleton.GetBoneName(BoneIndex).ToString());
		const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
		Bone->SetStringField(TEXT("parent"), ParentIndex != INDEX_NONE ? RefSkeleton.GetBoneName(ParentIndex).ToString() : FString());
		Bones.Add(Bone);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Payload->SetArrayField(TEXT("bones"), AnimMCP::ToJsonArray(Bones));
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimInspectToolset::anim_list_animation_assets(const FString& skeleton_path, const FString& folder, const FString& asset_class)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	USkeleton* Skeleton = AnimMCP::ResolveSkeleton(skeleton_path, Error);
	if (!Skeleton)
	{
		return AnimMCP::Fail(Error);
	}

	UClass* FilterClass = UAnimationAsset::StaticClass();
	if (!AnimMCP::IsUnset(asset_class))
	{
		UClass* Found = FindFirstObject<UClass>(*asset_class, EFindFirstObjectOptions::NativeFirst);
		if (!Found && asset_class.StartsWith(TEXT("U")))
		{
			Found = FindFirstObject<UClass>(*asset_class.RightChop(1), EFindFirstObjectOptions::NativeFirst);
		}
		if (!Found || !Found->IsChildOf(UAnimationAsset::StaticClass()))
		{
			return AnimMCP::Fail(FString::Printf(TEXT("'%s' is not an animation asset class. Try AnimSequence, AnimMontage, BlendSpace, BlendSpace1D, AimOffsetBlendSpace or PoseAsset."), *asset_class));
		}
		FilterClass = Found;
	}

	AnimMCP::EnsureFolderScanned(folder.IsEmpty() ? FString(TEXT("/Game")) : folder);

	FARFilter Filter;
	Filter.ClassPaths.Add(FilterClass->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(FName(*(folder.IsEmpty() ? FString(TEXT("/Game")) : folder)));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	FAssetRegistryModule::GetRegistry().GetAssets(Filter, Assets);

	TArray<TSharedRef<FJsonObject>> Items;
	for (const FAssetData& Asset : Assets)
	{
		if (!Skeleton->IsCompatibleForEditor(Asset))
		{
			continue;
		}
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Item->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
		Items.Add(Item);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Payload->SetArrayField(TEXT("assets"), AnimMCP::ToJsonArray(Items));
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimInspectToolset::anim_list_variables(const FString& blueprint_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ false, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}

	UObject* CDO = AnimBP->GeneratedClass ? AnimBP->GeneratedClass->GetDefaultObject(false) : nullptr;

	TArray<TSharedRef<FJsonObject>> Items;
	for (const FBPVariableDescription& Var : AnimBP->NewVariables)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Var.VarName.ToString());
		Item->SetStringField(TEXT("type"), AnimMCP::PinTypeToString(Var.VarType));
		Item->SetStringField(TEXT("category"), Var.Category.ToString());

		// The class default object holds the authoritative default once compiled.
		FString DefaultValue = Var.DefaultValue;
		if (CDO)
		{
			if (const FProperty* Property = AnimBP->GeneratedClass->FindPropertyByName(Var.VarName))
			{
				DefaultValue.Reset();
				Property->ExportText_InContainer(0, DefaultValue, CDO, CDO, CDO, PPF_None);
			}
		}
		Item->SetStringField(TEXT("default_value"), DefaultValue);
		Items.Add(Item);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("variables"), AnimMCP::ToJsonArray(Items));
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimInspectToolset::anim_list_node_types(const FString& filter)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	TArray<UClass*> Classes;
	GetDerivedClasses(UAnimGraphNode_Base::StaticClass(), Classes, /*bRecursive*/ true);

	TArray<TSharedRef<FJsonObject>> Items;
	for (UClass* Class : Classes)
	{
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists) || IsInternalAnimNodeClass(Class))
		{
			continue;
		}
		if (!AnimMCP::IsUnset(filter) && !Class->GetName().Contains(filter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("class"), Class->GetName());
		Item->SetStringField(TEXT("path"), Class->GetPathName());
		Item->SetStringField(TEXT("description"), Class->GetToolTipText(/*bShortTooltip*/ true).ToString());
		Items.Add(Item);
	}

	Items.Sort([](const TSharedRef<FJsonObject>& A, const TSharedRef<FJsonObject>& B)
	{
		return A->GetStringField(TEXT("class")) < B->GetStringField(TEXT("class"));
	});

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("node_types"), AnimMCP::ToJsonArray(Items));
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimInspectToolset::anim_get_skeleton_info(const FString& asset_path, bool include_bones)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	USkeleton* Skeleton = AnimMCP::ResolveSkeleton(asset_path, Error);
	if (!Skeleton)
	{
		return AnimMCP::Fail(Error);
	}

	auto VectorToJson = [](const FVector& V)
	{
		return TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) };
	};

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());

	const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
	Payload->SetNumberField(TEXT("bone_count"), RefSkeleton.GetNum());
	if (include_bones)
	{
		TArray<TSharedRef<FJsonObject>> Bones;
		for (int32 BoneIndex = 0; BoneIndex < RefSkeleton.GetNum(); ++BoneIndex)
		{
			TSharedRef<FJsonObject> Bone = MakeShared<FJsonObject>();
			Bone->SetNumberField(TEXT("index"), BoneIndex);
			Bone->SetStringField(TEXT("name"), RefSkeleton.GetBoneName(BoneIndex).ToString());
			const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
			Bone->SetStringField(TEXT("parent"), ParentIndex != INDEX_NONE ? RefSkeleton.GetBoneName(ParentIndex).ToString() : FString());
			Bones.Add(Bone);
		}
		Payload->SetArrayField(TEXT("bones"), AnimMCP::ToJsonArray(Bones));
	}

	TArray<TSharedRef<FJsonObject>> VirtualBones;
	for (const FVirtualBone& VirtualBone : Skeleton->GetVirtualBones())
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), VirtualBone.VirtualBoneName.ToString());
		Item->SetStringField(TEXT("source"), VirtualBone.SourceBoneName.ToString());
		Item->SetStringField(TEXT("target"), VirtualBone.TargetBoneName.ToString());
		VirtualBones.Add(Item);
	}
	Payload->SetArrayField(TEXT("virtual_bones"), AnimMCP::ToJsonArray(VirtualBones));

	// Skeleton sockets only; sockets added on a skeletal mesh live on the mesh.
	TArray<TSharedRef<FJsonObject>> Sockets;
	for (const USkeletalMeshSocket* Socket : Skeleton->Sockets)
	{
		if (!Socket)
		{
			continue;
		}
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Socket->SocketName.ToString());
		Item->SetStringField(TEXT("bone"), Socket->BoneName.ToString());
		Item->SetArrayField(TEXT("location"), VectorToJson(Socket->RelativeLocation));
		Item->SetArrayField(TEXT("rotation"), VectorToJson(FVector(Socket->RelativeRotation.Pitch, Socket->RelativeRotation.Yaw, Socket->RelativeRotation.Roll)));
		Item->SetArrayField(TEXT("scale"), VectorToJson(Socket->RelativeScale));
		Sockets.Add(Item);
	}
	Payload->SetArrayField(TEXT("sockets"), AnimMCP::ToJsonArray(Sockets));

	TArray<FString> Compatible;
	for (const TSoftObjectPtr<USkeleton>& Other : Skeleton->GetCompatibleSkeletons())
	{
		Compatible.Add(Other.ToSoftObjectPath().ToString());
	}
	Payload->SetArrayField(TEXT("compatible_skeletons"), AnimMCP::ToJsonArray(Compatible));
	Payload->SetBoolField(TEXT("use_retarget_modes_from_compatible"), Skeleton->GetUseRetargetModesFromCompatibleSkeleton());

	TArray<TSharedRef<FJsonObject>> SlotGroups;
	for (const FAnimSlotGroup& Group : Skeleton->GetSlotGroups())
	{
		TArray<FString> Slots;
		for (const FName& Slot : Group.SlotNames)
		{
			Slots.Add(Slot.ToString());
		}
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("group"), Group.GroupName.ToString());
		Item->SetArrayField(TEXT("slots"), AnimMCP::ToJsonArray(Slots));
		SlotGroups.Add(Item);
	}
	Payload->SetArrayField(TEXT("slot_groups"), AnimMCP::ToJsonArray(SlotGroups));
	return AnimMCP::Ok(Payload);
}
