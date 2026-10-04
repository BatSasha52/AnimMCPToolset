// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#include "AnimGraphEditToolset.h"

#include "AnimMCPHelpers.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_AssetPlayerBase.h"
#include "AnimGraphNode_Base.h"
#include "AnimStateNodeBase.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "AnimMCPGraphEdit"

namespace
{
	UFunction* ResolveFunction(UBlueprint* Blueprint, const FString& FunctionName, FString& OutError)
	{
		FString ClassName;
		FString FuncName = FunctionName.TrimStartAndEnd();
		UClass* OwnerClass = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->GeneratedClass.Get();

		if (FunctionName.Split(TEXT("."), &ClassName, &FuncName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			OwnerClass = ClassName.StartsWith(TEXT("/"))
				? FindObject<UClass>(nullptr, *ClassName)
				: FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
			if (!OwnerClass && ClassName.StartsWith(TEXT("U")))
			{
				OwnerClass = FindFirstObject<UClass>(*ClassName.RightChop(1), EFindFirstObjectOptions::NativeFirst);
			}
			if (!OwnerClass)
			{
				OutError = FString::Printf(TEXT("Class '%s' not found for function '%s'."), *ClassName, *FunctionName);
				return nullptr;
			}
		}

		UFunction* Function = OwnerClass ? OwnerClass->FindFunctionByName(FName(*FuncName)) : nullptr;
		if (!Function)
		{
			OutError = FString::Printf(TEXT("Function '%s' not found. Use 'ClassName.FunctionName', e.g. 'KismetMathLibrary.Not_PreBool'."), *FunctionName);
			return nullptr;
		}
		if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
		{
			OutError = FString::Printf(TEXT("Function '%s' is not Blueprint-callable."), *FunctionName);
			return nullptr;
		}
		return Function;
	}

	bool HasMemberVariable(UBlueprint* Blueprint, const FName VarName)
	{
		if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) != INDEX_NONE)
		{
			return true;
		}
		UClass* Class = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->GeneratedClass.Get();
		return Class && FindFProperty<FProperty>(Class, VarName) != nullptr;
	}

	bool IsObjectPinCategory(const FName Category)
	{
		return Category == UEdGraphSchema_K2::PC_Object
			|| Category == UEdGraphSchema_K2::PC_SoftObject
			|| Category == UEdGraphSchema_K2::PC_Class
			|| Category == UEdGraphSchema_K2::PC_SoftClass
			|| Category == UEdGraphSchema_K2::PC_Interface;
	}
}

FAnimMCPResult UAnimGraphEditToolset::anim_add_node(const FString& blueprint_path, const FString& graph, const FString& node_class, float x, float y, const FString& variable_name, const FString& function_name)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UEdGraph* Graph = AnimMCP::FindGraph(AnimBP, graph, Error);
	if (!Graph)
	{
		return AnimMCP::Fail(Error);
	}
	UClass* NodeClass = AnimMCP::ResolveNodeClass(node_class, Error);
	if (!NodeClass)
	{
		return AnimMCP::Fail(Error);
	}

	const UEdGraphNode* NodeCDO = NodeClass->GetDefaultObject<UEdGraphNode>();
	if (!NodeCDO->CanCreateUnderSpecifiedSchema(Graph->GetSchema()))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("%s cannot be placed in graph '%s' (%s)."),
			*NodeClass->GetName(), *Graph->GetName(), *Graph->GetSchema()->GetClass()->GetName()));
	}

	// Validate the extra configuration before touching anything.
	const bool bIsVariableNode = NodeClass->IsChildOf(UK2Node_Variable::StaticClass());
	const bool bIsCallFunction = NodeClass->IsChildOf(UK2Node_CallFunction::StaticClass());
	UFunction* Function = nullptr;
	if (bIsVariableNode)
	{
		if (AnimMCP::IsUnset(variable_name) || !HasMemberVariable(AnimBP, FName(*variable_name)))
		{
			return AnimMCP::Fail(FString::Printf(TEXT("%s needs variable_name set to an existing member variable (got '%s'). Use anim_list_variables."), *NodeClass->GetName(), *variable_name));
		}
	}
	else if (bIsCallFunction)
	{
		if (AnimMCP::IsUnset(function_name))
		{
			return AnimMCP::Fail(TEXT("K2Node_CallFunction needs function_name, e.g. 'KismetMathLibrary.Not_PreBool'."));
		}
		Function = ResolveFunction(AnimBP, function_name, Error);
		if (!Function)
		{
			return AnimMCP::Fail(Error);
		}
	}

	const FScopedTransaction Transaction(LOCTEXT("AddNode", "AnimMCP: Add Node"));
	AnimBP->Modify();

	UEdGraphNode* Node = AnimMCP::SpawnNode(Graph, NodeClass, FVector2D(x, y), [&](UEdGraphNode* NewNode)
	{
		if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(NewNode))
		{
			VarNode->VariableReference.SetSelfMember(FName(*variable_name));
		}
		else if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(NewNode))
		{
			CallNode->SetFromFunction(Function);
		}
	});

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	return AnimMCP::Ok(AnimMCP::NodeToJson(Node, /*bIncludePins*/ true));
}

FAnimMCPResult UAnimGraphEditToolset::anim_remove_node(const FString& blueprint_path, const FString& node_guid)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UEdGraphNode* Node = AnimMCP::FindNode(AnimBP, node_guid, Error);
	if (!Node)
	{
		return AnimMCP::Fail(Error);
	}
	if (Node->IsA<UAnimStateNodeBase>())
	{
		return AnimMCP::Fail(TEXT("This is a state machine node. Use anim_remove_state or anim_remove_transition so connected transitions are cleaned up."));
	}
	if (!Node->CanUserDeleteNode())
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Node %s (%s) cannot be deleted."), *node_guid, *Node->GetClass()->GetName()));
	}

	const FScopedTransaction Transaction(LOCTEXT("RemoveNode", "AnimMCP: Remove Node"));
	const AnimMCP::FLinkTracker Tracker({ Node->GetGraph() });
	AnimBP->Modify();
	AnimMCP::RemoveNode(AnimBP, Node);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("removed_node_guid"), node_guid);
	Payload->SetArrayField(TEXT("disconnected"), Tracker.Disconnected());
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimGraphEditToolset::anim_set_node_position(const FString& blueprint_path, const FString& node_guid, float x, float y)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UEdGraphNode* Node = AnimMCP::FindNode(AnimBP, node_guid, Error);
	if (!Node)
	{
		return AnimMCP::Fail(Error);
	}

	const FScopedTransaction Transaction(LOCTEXT("MoveNode", "AnimMCP: Move Node"));
	Node->Modify();
	Node->NodePosX = FMath::RoundToInt(x);
	Node->NodePosY = FMath::RoundToInt(y);
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("node_guid"), node_guid);
	Payload->SetNumberField(TEXT("x"), Node->NodePosX);
	Payload->SetNumberField(TEXT("y"), Node->NodePosY);
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimGraphEditToolset::anim_connect_pins(const FString& blueprint_path, const FString& from_node_guid, const FString& from_pin, const FString& to_node_guid, const FString& to_pin)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UEdGraphNode* FromNode = AnimMCP::FindNode(AnimBP, from_node_guid, Error);
	if (!FromNode)
	{
		return AnimMCP::Fail(Error);
	}
	UEdGraphNode* ToNode = AnimMCP::FindNode(AnimBP, to_node_guid, Error);
	if (!ToNode)
	{
		return AnimMCP::Fail(Error);
	}
	if (FromNode->GetGraph() != ToNode->GetGraph())
	{
		return AnimMCP::Fail(TEXT("Both nodes must be in the same graph."));
	}

	UEdGraphPin* FromPin = AnimMCP::FindPin(FromNode, from_pin, TEXT("output"), Error);
	if (!FromPin)
	{
		return AnimMCP::Fail(Error);
	}
	UEdGraphPin* ToPin = AnimMCP::FindPin(ToNode, to_pin, TEXT("input"), Error);
	if (!ToPin)
	{
		return AnimMCP::Fail(Error);
	}

	const UEdGraphSchema* Schema = FromNode->GetGraph()->GetSchema();
	const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
	if (Response.Response == CONNECT_RESPONSE_DISALLOW)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Cannot connect: %s"), *Response.Message.ToString()));
	}

	const FScopedTransaction Transaction(LOCTEXT("ConnectPins", "AnimMCP: Connect Pins"));
	const AnimMCP::FLinkTracker Tracker({ FromNode->GetGraph() });
	FromNode->Modify();
	ToNode->Modify();
	for (UEdGraphPin* Linked : ToPin->LinkedTo)
	{
		Linked->GetOwningNode()->Modify();
	}
	for (UEdGraphPin* Linked : FromPin->LinkedTo)
	{
		Linked->GetOwningNode()->Modify();
	}

	const bool bConnected = Schema->TryCreateConnection(FromPin, ToPin);
	if (!bConnected)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("The schema refused the connection. %s"), *Response.Message.ToString()));
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);

	const TArray<TSharedPtr<FJsonValue>> Disconnected = Tracker.Disconnected();
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetBoolField(TEXT("connected"), true);
	Payload->SetBoolField(TEXT("replaced_existing_links"), !Disconnected.IsEmpty());
	Payload->SetArrayField(TEXT("disconnected"), Disconnected);
	Payload->SetStringField(TEXT("message"), Response.Message.ToString());
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimGraphEditToolset::anim_disconnect_pins(const FString& blueprint_path, const FString& node_guid, const FString& pin, const FString& to_node_guid, const FString& to_pin)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UEdGraphNode* Node = AnimMCP::FindNode(AnimBP, node_guid, Error);
	if (!Node)
	{
		return AnimMCP::Fail(Error);
	}
	UEdGraphPin* Pin = AnimMCP::FindPin(Node, pin, TEXT(""), Error);
	if (!Pin)
	{
		return AnimMCP::Fail(Error);
	}

	const UEdGraphSchema* Schema = Node->GetGraph()->GetSchema();
	const AnimMCP::FLinkTracker Tracker({ Node->GetGraph() });

	if (AnimMCP::IsUnset(to_node_guid))
	{
		if (Pin->LinkedTo.IsEmpty())
		{
			return AnimMCP::Fail(TEXT("Pin has no links."));
		}

		const FScopedTransaction Transaction(LOCTEXT("BreakPinLinks", "AnimMCP: Break Pin Links"));
		Node->Modify();
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			Linked->GetOwningNode()->Modify();
		}
		Schema->BreakPinLinks(*Pin, /*bSendsNodeNotifcation*/ true);
	}
	else
	{
		UEdGraphNode* OtherNode = AnimMCP::FindNode(AnimBP, to_node_guid, Error);
		if (!OtherNode)
		{
			return AnimMCP::Fail(Error);
		}
		if (AnimMCP::IsUnset(to_pin))
		{
			return AnimMCP::Fail(TEXT("to_pin is required when to_node_guid is a GUID."));
		}
		UEdGraphPin* OtherPin = AnimMCP::FindPin(OtherNode, to_pin, TEXT(""), Error);
		if (!OtherPin)
		{
			return AnimMCP::Fail(Error);
		}
		if (!Pin->LinkedTo.Contains(OtherPin))
		{
			return AnimMCP::Fail(TEXT("These two pins are not linked."));
		}

		const FScopedTransaction Transaction(LOCTEXT("BreakSingleLink", "AnimMCP: Break Link"));
		Node->Modify();
		OtherNode->Modify();
		Schema->BreakSinglePinLink(Pin, OtherPin);
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	const TArray<TSharedPtr<FJsonValue>> Disconnected = Tracker.Disconnected();
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("links_broken"), Disconnected.Num());
	Payload->SetArrayField(TEXT("disconnected"), Disconnected);
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimGraphEditToolset::anim_set_pin_default(const FString& blueprint_path, const FString& node_guid, const FString& pin, const FString& value)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UEdGraphNode* Node = AnimMCP::FindNode(AnimBP, node_guid, Error);
	if (!Node)
	{
		return AnimMCP::Fail(Error);
	}
	UEdGraphPin* Pin = AnimMCP::FindPin(Node, pin, TEXT("input"), Error);
	if (!Pin)
	{
		return AnimMCP::Fail(Error);
	}
	if (!Pin->LinkedTo.IsEmpty())
	{
		return AnimMCP::Fail(TEXT("Pin is connected, so its default value is ignored. Disconnect it first with anim_disconnect_pins."));
	}

	const UEdGraphSchema* Schema = Node->GetGraph()->GetSchema();
	const FName Category = Pin->PinType.PinCategory;

	UObject* ObjectValue = nullptr;
	if (IsObjectPinCategory(Category) && !value.IsEmpty())
	{
		const bool bIsClassPin = Category == UEdGraphSchema_K2::PC_Class || Category == UEdGraphSchema_K2::PC_SoftClass;
		if (bIsClassPin)
		{
			ObjectValue = FindObject<UClass>(nullptr, *value);
			if (!ObjectValue)
			{
				ObjectValue = LoadObject<UClass>(nullptr, *value);
			}
		}
		else
		{
			ObjectValue = AnimMCP::LoadAssetChecked(value, nullptr, /*bForWrite*/ false, Error);
		}
		if (!ObjectValue)
		{
			return AnimMCP::Fail(Error.IsEmpty() ? FString::Printf(TEXT("Could not load '%s'."), *value) : Error);
		}
	}

	if (!AnimMCP::ValidateBoolText(Pin->PinType, value, Error))
	{
		return AnimMCP::Fail(Error);
	}

	// Only the K2 family implements IsPinDefaultValid; the base schema always reports "not implemented".
	if (Schema->IsA<UEdGraphSchema_K2>() && Category != UEdGraphSchema_K2::PC_Text)
	{
		const FString Validation = Schema->IsPinDefaultValid(Pin, ObjectValue ? FString() : value, ObjectValue, FText::GetEmpty());
		if (!Validation.IsEmpty())
		{
			return AnimMCP::Fail(FString::Printf(TEXT("Invalid default for pin '%s' (%s): %s"),
				*pin, *AnimMCP::PinTypeToString(Pin->PinType), *Validation));
		}
	}

	const FScopedTransaction Transaction(LOCTEXT("SetPinDefault", "AnimMCP: Set Pin Default"));
	Node->Modify();
	if (IsObjectPinCategory(Category))
	{
		Schema->TrySetDefaultObject(*Pin, ObjectValue);
	}
	else if (Category == UEdGraphSchema_K2::PC_Text)
	{
		Schema->TrySetDefaultText(*Pin, FText::FromString(value));
	}
	else
	{
		Schema->TrySetDefaultValue(*Pin, value);
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);

	return AnimMCP::Ok(AnimMCP::PinToJson(Pin));
}

FAnimMCPResult UAnimGraphEditToolset::anim_set_node_property(const FString& blueprint_path, const FString& node_guid, const FString& property_path, const FString& value)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UEdGraphNode* Node = AnimMCP::FindNode(AnimBP, node_guid, Error);
	if (!Node)
	{
		return AnimMCP::Fail(Error);
	}

	if (!AnimMCP::CanImportPropertyValue(Node, property_path, value, Error))
	{
		return AnimMCP::Fail(Error);
	}

	const FScopedTransaction Transaction(LOCTEXT("SetNodeProperty", "AnimMCP: Set Node Property"));
	const AnimMCP::FLinkTracker Tracker({ Node->GetGraph() });
	FString ReadBack;
	if (!AnimMCP::SetNodePropertyByPath(Node, property_path, value, ReadBack, Error))
	{
		return AnimMCP::Fail(Error);
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("node_guid"), node_guid);
	Payload->SetStringField(TEXT("property_path"), property_path);
	Payload->SetStringField(TEXT("value"), ReadBack);
	Payload->SetArrayField(TEXT("disconnected"), Tracker.Disconnected());
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimGraphEditToolset::anim_set_sequence_player_asset(const FString& blueprint_path, const FString& node_guid, const FString& asset_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UAnimGraphNode_AssetPlayerBase* PlayerNode = AnimMCP::FindNodeOfType<UAnimGraphNode_AssetPlayerBase>(AnimBP, node_guid, Error);
	if (!PlayerNode)
	{
		return AnimMCP::Fail(Error);
	}
	UAnimationAsset* Asset = AnimMCP::LoadAsset<UAnimationAsset>(asset_path, /*bForWrite*/ false, Error);
	if (!Asset)
	{
		return AnimMCP::Fail(Error);
	}

	const TSubclassOf<UAnimationAsset> ExpectedClass = PlayerNode->GetAnimationAssetClass();
	if (ExpectedClass && !Asset->IsA(ExpectedClass))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("%s expects a %s, but '%s' is a %s."),
			*PlayerNode->GetClass()->GetName(), *ExpectedClass->GetName(), *asset_path, *Asset->GetClass()->GetName()));
	}
	if (AnimBP->TargetSkeleton && Asset->GetSkeleton() && !AnimBP->TargetSkeleton->IsCompatibleForEditor(Asset->GetSkeleton()))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("'%s' uses skeleton %s, which is not compatible with the blueprint's skeleton %s."),
			*asset_path, *Asset->GetSkeleton()->GetPathName(), *AnimBP->TargetSkeleton->GetPathName()));
	}

	const FScopedTransaction Transaction(LOCTEXT("SetPlayerAsset", "AnimMCP: Set Animation Asset"));
	PlayerNode->Modify();
	PlayerNode->SetAnimationAsset(Asset);
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);

	return AnimMCP::Ok(AnimMCP::NodeToJson(PlayerNode, /*bIncludePins*/ false));
}

#undef LOCTEXT_NAMESPACE
