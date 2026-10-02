// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#include "AnimGraphEditToolset.h"

#include "AnimMCPHelpers.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_AssetPlayerBase.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_CustomTransitionResult.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNodeBase.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/StringOutputDevice.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "AnimMCPGraphEdit"

namespace
{
	UClass* ResolveNodeClass(const FString& Name, FString& OutError)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		UClass* Class = nullptr;
		if (Trimmed.StartsWith(TEXT("/")))
		{
			Class = FindObject<UClass>(nullptr, *Trimmed);
			if (!Class)
			{
				Class = LoadObject<UClass>(nullptr, *Trimmed);
			}
		}
		else
		{
			Class = FindFirstObject<UClass>(*Trimmed, EFindFirstObjectOptions::NativeFirst);
			if (!Class && Trimmed.StartsWith(TEXT("U")))
			{
				Class = FindFirstObject<UClass>(*Trimmed.RightChop(1), EFindFirstObjectOptions::NativeFirst);
			}
		}

		if (!Class || !Class->IsChildOf(UEdGraphNode::StaticClass()))
		{
			OutError = FString::Printf(TEXT("'%s' is not a graph node class. Use anim_list_node_types to see valid anim node classes."), *Name);
			return nullptr;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			OutError = FString::Printf(TEXT("'%s' is abstract or deprecated and cannot be placed."), *Name);
			return nullptr;
		}
		if (Class->IsChildOf(UAnimGraphNode_Root::StaticClass())
			|| Class->IsChildOf(UAnimGraphNode_StateResult::StaticClass())
			|| Class->IsChildOf(UAnimGraphNode_TransitionResult::StaticClass())
			|| Class->IsChildOf(UAnimGraphNode_CustomTransitionResult::StaticClass())
			|| Class->IsChildOf(UAnimStateEntryNode::StaticClass()))
		{
			OutError = FString::Printf(TEXT("'%s' is a result/entry node that the editor creates automatically; it cannot be added manually."), *Name);
			return nullptr;
		}
		if (Class->IsChildOf(UAnimStateNodeBase::StaticClass()))
		{
			OutError = TEXT("States, conduits and transitions must be created with anim_add_state, anim_add_conduit and anim_add_transition.");
			return nullptr;
		}
		return Class;
	}

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

	struct FResolvedProperty
	{
		FProperty* TopProperty = nullptr;
		FProperty* LeafProperty = nullptr;
		void* LeafValue = nullptr;
	};

	/** Walks 'A.B[2].C' from Object down to the leaf value. Every segment must be editor-visible. */
	bool ResolvePropertyPath(UObject* Object, const FString& Path, FResolvedProperty& Out, FString& OutError)
	{
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("."));
		if (Segments.IsEmpty())
		{
			OutError = TEXT("property_path is empty.");
			return false;
		}

		UStruct* Struct = Object->GetClass();
		void* Container = Object;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FString Name = Segments[Index];
			int32 ArrayIndex = INDEX_NONE;
			int32 BracketPos;
			if (Name.FindChar(TEXT('['), BracketPos) && Name.EndsWith(TEXT("]")))
			{
				const FString IndexText = Name.Mid(BracketPos + 1, Name.Len() - BracketPos - 2);
				if (!IndexText.IsNumeric())
				{
					OutError = FString::Printf(TEXT("Invalid array index in '%s'."), *Segments[Index]);
					return false;
				}
				ArrayIndex = FCString::Atoi(*IndexText);
				Name.LeftInline(BracketPos);
			}

			FProperty* Property = FindFProperty<FProperty>(Struct, FName(*Name));
			if (!Property && Index == 0 && Name == TEXT("Node"))
			{
				if (const UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Object))
				{
					Property = AnimNode->GetFNodeProperty();
				}
			}
			if (!Property)
			{
				TArray<FString> Available;
				for (TFieldIterator<FProperty> It(Struct); It; ++It)
				{
					if (It->HasAnyPropertyFlags(CPF_Edit))
					{
						Available.Add(It->GetName());
					}
				}
				OutError = FString::Printf(TEXT("Property '%s' not found on %s. Editable properties: %s"),
					*Name, *Struct->GetName(), *FString::Join(Available, TEXT(", ")));
				return false;
			}
			if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_EditConst))
			{
				OutError = FString::Printf(TEXT("Property '%s' is not editable."), *Name);
				return false;
			}
			if (Index == 0)
			{
				Out.TopProperty = Property;
			}

			void* Value = Property->ContainerPtrToValuePtr<void>(Container);
			if (ArrayIndex != INDEX_NONE)
			{
				FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
				if (!ArrayProperty)
				{
					OutError = FString::Printf(TEXT("'%s' is not an array."), *Name);
					return false;
				}
				FScriptArrayHelper Helper(ArrayProperty, Value);
				if (!Helper.IsValidIndex(ArrayIndex))
				{
					OutError = FString::Printf(TEXT("Index %d out of range for '%s' (size %d)."), ArrayIndex, *Name, Helper.Num());
					return false;
				}
				Value = Helper.GetRawPtr(ArrayIndex);
				Property = ArrayProperty->Inner;
			}

			if (Index == Segments.Num() - 1)
			{
				Out.LeafProperty = Property;
				Out.LeafValue = Value;
				return true;
			}

			FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (!StructProperty)
			{
				OutError = FString::Printf(TEXT("'%s' is not a struct, so '%s' cannot be resolved inside it."), *Name, *Segments[Index + 1]);
				return false;
			}
			Struct = StructProperty->Struct;
			Container = Value;
		}
		return false;
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
	UClass* NodeClass = ResolveNodeClass(node_class, Error);
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
	AnimBP->Modify();
	AnimMCP::RemoveNode(AnimBP, Node);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("removed_node_guid"), node_guid);
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

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetBoolField(TEXT("connected"), true);
	Payload->SetBoolField(TEXT("replaced_existing_links"),
		Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB);
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
	int32 LinksBroken = 0;

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
		LinksBroken = Pin->LinkedTo.Num();
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
		LinksBroken = 1;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("links_broken"), LinksBroken);
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

	FResolvedProperty Resolved;
	if (!ResolvePropertyPath(Node, property_path, Resolved, Error))
	{
		return AnimMCP::Fail(Error);
	}

	// Import into a scratch copy first so a bad value never touches the node.
	FStringOutputDevice ImportErrors;
	void* Scratch = FMemory::Malloc(Resolved.LeafProperty->GetElementSize(), Resolved.LeafProperty->GetMinAlignment());
	Resolved.LeafProperty->InitializeValue(Scratch);
	Resolved.LeafProperty->CopySingleValue(Scratch, Resolved.LeafValue);
	const TCHAR* ImportResult = Resolved.LeafProperty->ImportText_Direct(*value, Scratch, Node, PPF_None, &ImportErrors);
	Resolved.LeafProperty->DestroyValue(Scratch);
	FMemory::Free(Scratch);
	if (!ImportResult || !ImportErrors.IsEmpty())
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Could not parse '%s' for %s (%s). %s"),
			*value, *property_path, *Resolved.LeafProperty->GetCPPType(), *ImportErrors));
	}

	const FScopedTransaction Transaction(LOCTEXT("SetNodeProperty", "AnimMCP: Set Node Property"));
	Node->Modify();
	Node->PreEditChange(Resolved.TopProperty);
	Resolved.LeafProperty->ImportText_Direct(*value, Resolved.LeafValue, Node, PPF_None);
	FPropertyChangedEvent ChangedEvent(Resolved.TopProperty, EPropertyChangeType::ValueSet);
	Node->PostEditChangeProperty(ChangedEvent);
	Node->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);

	// Re-resolve: ReconstructNode may have reallocated storage.
	FString ReadBack;
	if (ResolvePropertyPath(Node, property_path, Resolved, Error))
	{
		Resolved.LeafProperty->ExportTextItem_Direct(ReadBack, Resolved.LeafValue, nullptr, Node, PPF_None);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("node_guid"), node_guid);
	Payload->SetStringField(TEXT("property_path"), property_path);
	Payload->SetStringField(TEXT("value"), ReadBack);
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
