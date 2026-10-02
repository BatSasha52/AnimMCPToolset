// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#include "AnimMCPHelpers.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_AssetPlayerBase.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateMachineGraph.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY(LogAnimMCP);

namespace AnimMCP
{
	FAnimMCPResult Ok(const TSharedRef<FJsonObject>& Payload)
	{
		FAnimMCPResult Out;
		Out.Success = true;
		Out.Result.JsonObject = Payload;
		return Out;
	}

	FAnimMCPResult Fail(const FString& Message)
	{
		FAnimMCPResult Out;
		Out.Success = false;
		Out.Result.JsonObject = MakeShared<FJsonObject>();
		Out.Error = Message;
		UE_LOG(LogAnimMCP, Verbose, TEXT("Tool failed: %s"), *Message);
		return Out;
	}

	bool IsUnset(const FString& Value)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		return Trimmed.IsEmpty() || Trimmed == TEXT("*") || Trimmed.Equals(TEXT("none"), ESearchCase::IgnoreCase);
	}

	bool ValidateBoolText(const FEdGraphPinType& PinType, const FString& Value, FString& OutError)
	{
		if (PinType.PinCategory != UEdGraphSchema_K2::PC_Boolean || PinType.IsContainer())
		{
			return true;
		}
		const FString Trimmed = Value.TrimStartAndEnd();
		if (Trimmed.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			return true;
		}
		OutError = FString::Printf(TEXT("'%s' is not a bool. Use 'true' or 'false'."), *Value);
		return false;
	}

	void EnsureFolderScanned(const FString& Folder)
	{
		if (Folder.StartsWith(TEXT("/")))
		{
			// Cheap for folders that were already scanned; needed while the registry is still discovering assets.
			FAssetRegistryModule::GetRegistry().ScanPathsSynchronous({ Folder }, /*bForceRescan*/ false);
		}
	}

	// ---- Paths and assets ------------------------------------------------------------------

	bool NormalizeObjectPath(const FString& InPath, FString& OutObjectPath, FString& OutError)
	{
		FString Path = InPath.TrimStartAndEnd();
		if (Path.IsEmpty())
		{
			OutError = TEXT("Asset path is empty. Use a content path such as '/Game/Characters/ABP_Hero'.");
			return false;
		}

		// Accept export-text form: /Script/Engine.AnimBlueprint'/Game/A/B.B'
		if (Path.Contains(TEXT("'")))
		{
			Path = FPackageName::ExportTextPathToObjectPath(Path);
		}

		if (!Path.StartsWith(TEXT("/")))
		{
			OutError = FString::Printf(TEXT("'%s' is not a content path. Paths must start with '/', e.g. '/Game/Characters/ABP_Hero'."), *InPath);
			return false;
		}

		FString PackageName = Path;
		FString ObjectName;
		if (Path.Split(TEXT("."), &PackageName, &ObjectName))
		{
			OutObjectPath = Path;
		}
		else
		{
			ObjectName = FPackageName::GetLongPackageAssetName(PackageName);
			OutObjectPath = PackageName + TEXT(".") + ObjectName;
		}

		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid long package name."), *PackageName);
			return false;
		}
		return true;
	}

	bool IsUnderGameRoot(const FString& PackageOrObjectPath)
	{
		return PackageOrObjectPath.StartsWith(TEXT("/Game/"));
	}

	UObject* LoadAssetChecked(const FString& Path, UClass* ExpectedClass, bool bForWrite, FString& OutError)
	{
		FString ObjectPath;
		if (!NormalizeObjectPath(Path, ObjectPath, OutError))
		{
			return nullptr;
		}

		if (bForWrite && !IsUnderGameRoot(ObjectPath))
		{
			OutError = FString::Printf(TEXT("Refusing to modify '%s': only assets under /Game may be edited."), *ObjectPath);
			return nullptr;
		}

		// In-memory first so newly created (unsaved) assets are found.
		UObject* Object = FindObject<UObject>(nullptr, *ObjectPath);
		if (!Object)
		{
			const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
			if (!FPackageName::DoesPackageExist(PackageName))
			{
				OutError = FString::Printf(TEXT("No asset found at '%s'."), *ObjectPath);
				return nullptr;
			}
			Object = LoadObject<UObject>(nullptr, *ObjectPath);
		}

		if (!Object)
		{
			OutError = FString::Printf(TEXT("Failed to load '%s'."), *ObjectPath);
			return nullptr;
		}

		if (ExpectedClass && !Object->IsA(ExpectedClass))
		{
			OutError = FString::Printf(TEXT("'%s' is a %s, expected %s."),
				*ObjectPath, *Object->GetClass()->GetName(), *ExpectedClass->GetName());
			return nullptr;
		}
		return Object;
	}

	bool ValidateNewAssetLocation(const FString& PackagePath, const FString& AssetName, FString& OutPackageName, FString& OutError)
	{
		FString Folder = PackagePath.TrimStartAndEnd();
		while (Folder.EndsWith(TEXT("/")))
		{
			Folder.LeftChopInline(1);
		}

		if (Folder != TEXT("/Game") && !IsUnderGameRoot(Folder))
		{
			OutError = FString::Printf(TEXT("Refusing to create assets in '%s': only folders under /Game are allowed."), *PackagePath);
			return false;
		}

		FText NameError;
		if (AssetName.IsEmpty() || !FName::IsValidXName(AssetName, INVALID_OBJECTNAME_CHARACTERS INVALID_LONGPACKAGE_CHARACTERS, &NameError))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid asset name. %s"), *AssetName, *NameError.ToString());
			return false;
		}

		OutPackageName = Folder + TEXT("/") + AssetName;
		if (!FPackageName::IsValidLongPackageName(OutPackageName))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid package name."), *OutPackageName);
			return false;
		}

		if (FindPackage(nullptr, *OutPackageName) || FPackageName::DoesPackageExist(OutPackageName))
		{
			OutError = FString::Printf(TEXT("An asset already exists at '%s'. Pick a different name; existing assets are never overwritten."), *OutPackageName);
			return false;
		}

		IAssetRegistry& AssetRegistry = FAssetRegistryModule::GetRegistry();
		TArray<FAssetData> Existing;
		AssetRegistry.GetAssetsByPackageName(FName(*OutPackageName), Existing, /*bIncludeOnlyOnDiskAssets*/ false);
		if (!Existing.IsEmpty())
		{
			OutError = FString::Printf(TEXT("An asset already exists at '%s'. Pick a different name; existing assets are never overwritten."), *OutPackageName);
			return false;
		}
		return true;
	}

	USkeleton* ResolveSkeleton(const FString& Path, FString& OutError)
	{
		UObject* Object = LoadAssetChecked(Path, nullptr, /*bForWrite*/ false, OutError);
		if (!Object)
		{
			return nullptr;
		}

		USkeleton* Skeleton = nullptr;
		if (USkeleton* AsSkeleton = Cast<USkeleton>(Object))
		{
			Skeleton = AsSkeleton;
		}
		else if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Object))
		{
			Skeleton = Mesh->GetSkeleton();
		}
		else if (UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Object))
		{
			Skeleton = AnimBP->TargetSkeleton;
		}
		else if (UAnimationAsset* AnimAsset = Cast<UAnimationAsset>(Object))
		{
			Skeleton = AnimAsset->GetSkeleton();
		}
		else
		{
			OutError = FString::Printf(TEXT("'%s' is a %s. Pass a Skeleton, SkeletalMesh, AnimBlueprint or animation asset."),
				*Path, *Object->GetClass()->GetName());
			return nullptr;
		}

		if (!Skeleton)
		{
			OutError = FString::Printf(TEXT("'%s' has no skeleton (it may be a template Animation Blueprint)."), *Path);
		}
		return Skeleton;
	}

	// ---- Graphs, nodes, pins ---------------------------------------------------------------

	bool ParseGuid(const FString& GuidString, FGuid& OutGuid, FString& OutError)
	{
		if (!FGuid::Parse(GuidString.TrimStartAndEnd(), OutGuid) || !OutGuid.IsValid())
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid GUID. Use the node_guid values returned by anim_list_nodes."), *GuidString);
			return false;
		}
		return true;
	}

	static void CollectGraphsRecursive(UEdGraph* Graph, TArray<UEdGraph*>& OutGraphs, TSet<UEdGraph*>& Visited)
	{
		if (!Graph || Visited.Contains(Graph))
		{
			return;
		}
		Visited.Add(Graph);
		OutGraphs.Add(Graph);

		for (UEdGraph* Child : Graph->SubGraphs)
		{
			CollectGraphsRecursive(Child, OutGraphs, Visited);
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			for (UEdGraph* Child : Node->GetSubGraphs())
			{
				CollectGraphsRecursive(Child, OutGraphs, Visited);
			}
		}
	}

	static TArray<UEdGraph*> CollectAllGraphs(UBlueprint* Blueprint)
	{
		TArray<UEdGraph*> Roots;
		Blueprint->GetAllGraphs(Roots);

		TArray<UEdGraph*> Result;
		TSet<UEdGraph*> Visited;
		for (UEdGraph* Graph : Roots)
		{
			CollectGraphsRecursive(Graph, Result, Visited);
		}
		return Result;
	}

	UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& NameOrGuid, FString& OutError)
	{
		const FString Key = NameOrGuid.TrimStartAndEnd();
		TArray<UEdGraph*> Graphs = CollectAllGraphs(Blueprint);

		FGuid Guid;
		if (FGuid::Parse(Key, Guid))
		{
			for (UEdGraph* Graph : Graphs)
			{
				if (Graph->GraphGuid == Guid)
				{
					return Graph;
				}
			}
		}

		TArray<UEdGraph*> ByName;
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph->GetName().Equals(Key, ESearchCase::IgnoreCase))
			{
				ByName.Add(Graph);
			}
		}

		if (ByName.Num() == 1)
		{
			return ByName[0];
		}

		if (ByName.Num() > 1)
		{
			OutError = FString::Printf(TEXT("Graph name '%s' is ambiguous (%d matches). Pass the graph_guid from anim_list_graphs instead."), *Key, ByName.Num());
			return nullptr;
		}

		TArray<FString> Names;
		for (UEdGraph* Graph : Graphs)
		{
			Names.Add(Graph->GetName());
		}
		OutError = FString::Printf(TEXT("Graph '%s' not found. Available graphs: %s"), *Key, *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	UEdGraphNode* FindNode(UBlueprint* Blueprint, const FString& NodeGuid, FString& OutError)
	{
		FGuid Guid;
		if (!ParseGuid(NodeGuid, Guid, OutError))
		{
			return nullptr;
		}

		for (UEdGraph* Graph : CollectAllGraphs(Blueprint))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeGuid == Guid)
				{
					return Node;
				}
			}
		}
		OutError = FString::Printf(TEXT("No node with guid %s in '%s'."), *NodeGuid, *Blueprint->GetPathName());
		return nullptr;
	}

	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, const FString& Direction, FString& OutError)
	{
		EEdGraphPinDirection WantDir = EGPD_MAX;
		if (Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase))
		{
			WantDir = EGPD_Input;
		}
		else if (Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase))
		{
			WantDir = EGPD_Output;
		}
		else if (!Direction.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Invalid pin direction '%s'. Use 'input', 'output' or leave empty."), *Direction);
			return nullptr;
		}

		TArray<UEdGraphPin*> Matches;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase) &&
				(WantDir == EGPD_MAX || Pin->Direction == WantDir))
			{
				Matches.Add(Pin);
			}
		}

		if (Matches.Num() == 1)
		{
			return Matches[0];
		}

		TArray<FString> Available;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin)
			{
				Available.Add(FString::Printf(TEXT("%s (%s)"), *Pin->PinName.ToString(), *DirectionToString(Pin->Direction)));
			}
		}

		if (Matches.Num() > 1)
		{
			OutError = FString::Printf(TEXT("Pin name '%s' matches several pins on node %s; specify direction. Pins: %s"),
				*PinName, *GuidToString(Node->NodeGuid), *FString::Join(Available, TEXT(", ")));
		}
		else
		{
			OutError = FString::Printf(TEXT("Pin '%s' not found on node %s. Pins: %s"),
				*PinName, *GuidToString(Node->NodeGuid), *FString::Join(Available, TEXT(", ")));
		}
		return nullptr;
	}

	UEdGraphPin* FindFirstPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction && !Pin->bHidden)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	UEdGraphNode* SpawnNode(UEdGraph* Graph, UClass* NodeClass, const FVector2D& Position, TFunctionRef<void(UEdGraphNode*)> Init)
	{
		Graph->Modify();

		UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
		Graph->AddNode(Node, /*bUserAction*/ false, /*bSelectNewNode*/ false);
		Node->CreateNewGuid();
		Node->NodePosX = FMath::RoundToInt(Position.X);
		Node->NodePosY = FMath::RoundToInt(Position.Y);
		Init(Node);
		Node->PostPlacedNewNode();
		if (Node->Pins.IsEmpty())
		{
			Node->AllocateDefaultPins();
		}
		return Node;
	}

	UEdGraphNode* SpawnNode(UEdGraph* Graph, UClass* NodeClass, const FVector2D& Position)
	{
		return SpawnNode(Graph, NodeClass, Position, [](UEdGraphNode*) {});
	}

	void RemoveNode(UBlueprint* Blueprint, UEdGraphNode* Node)
	{
		if (UEdGraph* Graph = Node->GetGraph())
		{
			Graph->Modify();
		}
		Node->Modify();
		FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile*/ true);
	}

	UBlueprint* GetOwningBlueprint(const UEdGraphNode* Node)
	{
		return Node ? FBlueprintEditorUtils::FindBlueprintForGraph(Node->GetGraph()) : nullptr;
	}

	// ---- Serialization ---------------------------------------------------------------------

	FString GuidToString(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::DigitsWithHyphens);
	}

	FString DirectionToString(EEdGraphPinDirection Direction)
	{
		return Direction == EGPD_Input ? TEXT("input") : TEXT("output");
	}

	FString PinTypeToString(const FEdGraphPinType& PinType)
	{
		return UEdGraphSchema_K2::TypeToText(PinType).ToString();
	}

	TSharedRef<FJsonObject> PinToJson(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Json->SetStringField(TEXT("direction"), DirectionToString(Pin->Direction));
		Json->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		Json->SetStringField(TEXT("type"), PinTypeToString(Pin->PinType));
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			Json->SetStringField(TEXT("sub_category_object"), Pin->PinType.PinSubCategoryObject->GetPathName());
		}
		Json->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		if (Pin->DefaultObject)
		{
			Json->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
		}
		if (!Pin->DefaultTextValue.IsEmpty())
		{
			Json->SetStringField(TEXT("default_text"), Pin->DefaultTextValue.ToString());
		}
		Json->SetBoolField(TEXT("hidden"), Pin->bHidden);

		TArray<TSharedPtr<FJsonValue>> Links;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (Linked && Linked->GetOwningNodeUnchecked())
			{
				TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
				Link->SetStringField(TEXT("node_guid"), GuidToString(Linked->GetOwningNode()->NodeGuid));
				Link->SetStringField(TEXT("pin_name"), Linked->PinName.ToString());
				Links.Add(MakeShared<FJsonValueObject>(Link));
			}
		}
		Json->SetArrayField(TEXT("linked_to"), Links);
		return Json;
	}

	TSharedRef<FJsonObject> GraphToJson(const UEdGraph* Graph)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Graph->GetName());
		Json->SetStringField(TEXT("graph_guid"), GuidToString(Graph->GraphGuid));
		Json->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
		Json->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		if (const UEdGraphNode* OwnerNode = Cast<UEdGraphNode>(Graph->GetOuter()))
		{
			Json->SetStringField(TEXT("owner_node_guid"), GuidToString(OwnerNode->NodeGuid));
		}
		if (const UEdGraph* OuterGraph = Graph->GetTypedOuter<UEdGraph>())
		{
			Json->SetStringField(TEXT("parent_graph"), OuterGraph->GetName());
		}
		return Json;
	}

	TSharedRef<FJsonObject> NodeToJson(const UEdGraphNode* Node, bool bIncludePins)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("node_guid"), GuidToString(Node->NodeGuid));
		Json->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Json->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		Json->SetNumberField(TEXT("x"), Node->NodePosX);
		Json->SetNumberField(TEXT("y"), Node->NodePosY);
		if (const UEdGraph* Graph = Node->GetGraph())
		{
			Json->SetStringField(TEXT("graph"), Graph->GetName());
			Json->SetStringField(TEXT("graph_guid"), GuidToString(Graph->GraphGuid));
		}
		if (!Node->NodeComment.IsEmpty())
		{
			Json->SetStringField(TEXT("comment"), Node->NodeComment);
		}

		if (const UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node))
		{
			if (const UAnimStateNodeBase* From = Transition->GetPreviousState())
			{
				Json->SetStringField(TEXT("from_state_guid"), GuidToString(From->NodeGuid));
				Json->SetStringField(TEXT("from_state"), From->GetStateName());
			}
			if (const UAnimStateNodeBase* To = Transition->GetNextState())
			{
				Json->SetStringField(TEXT("to_state_guid"), GuidToString(To->NodeGuid));
				Json->SetStringField(TEXT("to_state"), To->GetStateName());
			}
			Json->SetNumberField(TEXT("crossfade_duration"), Transition->CrossfadeDuration);
			Json->SetNumberField(TEXT("priority_order"), Transition->PriorityOrder);
			Json->SetBoolField(TEXT("automatic_rule"), Transition->bAutomaticRuleBasedOnSequencePlayerInState);
			Json->SetNumberField(TEXT("automatic_rule_trigger_time"), Transition->AutomaticRuleTriggerTime);
		}
		else if (const UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(Node))
		{
			Json->SetStringField(TEXT("state_name"), State->GetStateName());
		}

		if (const UAnimGraphNode_StateMachineBase* StateMachine = Cast<UAnimGraphNode_StateMachineBase>(Node))
		{
			if (StateMachine->EditorStateMachineGraph)
			{
				Json->SetStringField(TEXT("state_machine_graph"), StateMachine->EditorStateMachineGraph->GetName());
				Json->SetStringField(TEXT("state_machine_graph_guid"), GuidToString(StateMachine->EditorStateMachineGraph->GraphGuid));
			}
		}

		if (const UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
		{
			if (const FStructProperty* NodeProperty = AnimNode->GetFNodeProperty())
			{
				Json->SetStringField(TEXT("node_struct_property"), NodeProperty->GetName());
			}
			if (const UAnimationAsset* Asset = AnimNode->GetAnimationAsset())
			{
				Json->SetStringField(TEXT("animation_asset"), Asset->GetPathName());
			}
		}

		TArray<TSharedPtr<FJsonValue>> SubGraphs;
		for (const UEdGraph* SubGraph : Node->GetSubGraphs())
		{
			if (SubGraph)
			{
				SubGraphs.Add(MakeShared<FJsonValueObject>(GraphToJson(SubGraph)));
			}
		}
		if (!SubGraphs.IsEmpty())
		{
			Json->SetArrayField(TEXT("sub_graphs"), SubGraphs);
		}

		if (bIncludePins)
		{
			TArray<TSharedPtr<FJsonValue>> Pins;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin)
				{
					Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
				}
			}
			Json->SetArrayField(TEXT("pins"), Pins);
		}
		return Json;
	}

	bool ParsePinType(const FString& TypeString, FEdGraphPinType& OutType, FString& OutError)
	{
		const FString Type = TypeString.TrimStartAndEnd().ToLower();
		OutType = FEdGraphPinType();

		auto SetStruct = [&OutType](UScriptStruct* Struct)
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = Struct;
		};

		if (Type == TEXT("bool") || Type == TEXT("boolean"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		}
		else if (Type == TEXT("byte"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		}
		else if (Type == TEXT("int") || Type == TEXT("int32") || Type == TEXT("integer"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Int;
		}
		else if (Type == TEXT("int64"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		}
		else if (Type == TEXT("float") || Type == TEXT("double") || Type == TEXT("real"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		}
		else if (Type == TEXT("name"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Name;
		}
		else if (Type == TEXT("string"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_String;
		}
		else if (Type == TEXT("text"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Text;
		}
		else if (Type == TEXT("vector"))
		{
			SetStruct(TBaseStructure<FVector>::Get());
		}
		else if (Type == TEXT("vector2d"))
		{
			SetStruct(TBaseStructure<FVector2D>::Get());
		}
		else if (Type == TEXT("rotator"))
		{
			SetStruct(TBaseStructure<FRotator>::Get());
		}
		else if (Type == TEXT("transform"))
		{
			SetStruct(TBaseStructure<FTransform>::Get());
		}
		else if (Type == TEXT("linearcolor"))
		{
			SetStruct(TBaseStructure<FLinearColor>::Get());
		}
		else if (Type.StartsWith(TEXT("object:")))
		{
			const FString ClassPath = TypeString.TrimStartAndEnd().RightChop(7);
			UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
			if (!Class)
			{
				Class = LoadObject<UClass>(nullptr, *ClassPath);
			}
			if (!Class)
			{
				OutError = FString::Printf(TEXT("Class '%s' not found. Use a full class path such as '/Script/Engine.AnimSequence'."), *ClassPath);
				return false;
			}
			OutType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutType.PinSubCategoryObject = Class;
		}
		else
		{
			OutError = FString::Printf(TEXT("Unsupported variable type '%s'. Supported: bool, byte, int, int64, float, name, string, text, vector, vector2d, rotator, transform, linearcolor, object:<class path>."), *TypeString);
			return false;
		}
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonArray(const TArray<TSharedRef<FJsonObject>>& Objects)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Objects.Num());
		for (const TSharedRef<FJsonObject>& Object : Objects)
		{
			Out.Add(MakeShared<FJsonValueObject>(Object));
		}
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonArray(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Strings.Num());
		for (const FString& String : Strings)
		{
			Out.Add(MakeShared<FJsonValueString>(String));
		}
		return Out;
	}
}
