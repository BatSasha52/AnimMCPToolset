// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#include "AnimStateMachineToolset.h"

#include "AnimMCPHelpers.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_AssetPlayerBase.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimStateConduitNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationTransitionGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "AnimMCPStateMachine"

namespace
{
	UAnimationStateMachineGraph* FindStateMachineGraph(UBlueprint* Blueprint, const FString& StateMachineGuid, FString& OutError)
	{
		UAnimGraphNode_StateMachineBase* Node = AnimMCP::FindNodeOfType<UAnimGraphNode_StateMachineBase>(Blueprint, StateMachineGuid, OutError);
		if (!Node)
		{
			return nullptr;
		}
		if (!Node->EditorStateMachineGraph)
		{
			OutError = TEXT("State machine node has no graph.");
			return nullptr;
		}
		return Node->EditorStateMachineGraph;
	}

	bool IsStateNameTaken(const UEdGraph* Graph, const FString& Name)
	{
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			const UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(Node);
			if (State && !State->IsA<UAnimStateTransitionNode>() && State->GetStateName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool ValidateName(const FString& Name, FString& OutError)
	{
		FText Reason;
		if (Name.TrimStartAndEnd().IsEmpty() || !FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS, &Reason))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid name. %s"), *Name, *Reason.ToString());
			return false;
		}
		return true;
	}

	/** Every transition node in the state's graph whose source or destination is the given state. */
	TArray<UAnimStateTransitionNode*> GetTransitionsTouching(const UAnimStateNodeBase* State)
	{
		TArray<UAnimStateTransitionNode*> Result;
		for (UEdGraphNode* Node : State->GetGraph()->Nodes)
		{
			UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node);
			if (Transition && (Transition->GetPreviousState() == State || Transition->GetNextState() == State))
			{
				Result.Add(Transition);
			}
		}
		return Result;
	}

	UAnimStateNodeBase* FindStateOrConduit(UBlueprint* Blueprint, const FString& Guid, FString& OutError)
	{
		UAnimStateNodeBase* State = AnimMCP::FindNodeOfType<UAnimStateNodeBase>(Blueprint, Guid, OutError);
		if (State && State->IsA<UAnimStateTransitionNode>())
		{
			OutError = FString::Printf(TEXT("Node %s is a transition, not a state or conduit."), *Guid);
			return nullptr;
		}
		return State;
	}

	UEdGraphNode* SpawnNamedStateNode(UAnimationStateMachineGraph* Graph, UClass* NodeClass, const FString& Name, const FVector2D& Position)
	{
		UEdGraphNode* Node = AnimMCP::SpawnNode(Graph, NodeClass, Position);
		// OnRenameNode renames the node's bound graph, which is where the state name lives.
		Node->OnRenameNode(Name);
		return Node;
	}

	// ---- Transition rules ------------------------------------------------------------------

	enum class ERuleKind : uint8 { Bool, NotBool, Compare, TimeRemaining, Always, Never };

	/** What a member variable can be used for in a rule. */
	enum class EVariableKind : uint8 { Missing, Bool, Real, Int, Int64, Byte, Other };

	/** A rule as the caller described it, before it is checked against the blueprint. */
	struct FRuleRequest
	{
		FString Rule;
		FString VariableName = TEXT("none");
		FString Comparison = TEXT(">");
		double Threshold = 0.0;
		float TriggerTime = -1.f;
	};

	/** A rule that has been validated and can be applied without further checks. */
	struct FResolvedRule
	{
		ERuleKind Kind = ERuleKind::Never;
		FName Variable;
		UFunction* Function = nullptr;  // Not_PreBool or the comparison function
		FString ThresholdText;
		float TriggerTime = -1.f;
	};

	/** Variables declared by a caller but not added to the blueprint yet (anim_build_state_machine). */
	using FPendingVariables = TMap<FName, FEdGraphPinType>;

	EVariableKind KindFromPinType(const FEdGraphPinType& Type)
	{
		if (Type.IsContainer())
		{
			return EVariableKind::Other;
		}
		const FName Category = Type.PinCategory;
		if (Category == UEdGraphSchema_K2::PC_Boolean)
		{
			return EVariableKind::Bool;
		}
		if (Category == UEdGraphSchema_K2::PC_Real || Category == UEdGraphSchema_K2::PC_Float || Category == UEdGraphSchema_K2::PC_Double)
		{
			return EVariableKind::Real;
		}
		if (Category == UEdGraphSchema_K2::PC_Int)
		{
			return EVariableKind::Int;
		}
		if (Category == UEdGraphSchema_K2::PC_Int64)
		{
			return EVariableKind::Int64;
		}
		if (Category == UEdGraphSchema_K2::PC_Byte && !Type.PinSubCategoryObject.IsValid())
		{
			return EVariableKind::Byte;  // enums are bytes too, but are not compared numerically here
		}
		return EVariableKind::Other;
	}

	/** Looks the variable up in pending declarations, the blueprint's own variables, then inherited properties. */
	EVariableKind GetVariableKind(UBlueprint* Blueprint, const FName Name, const FPendingVariables* Pending)
	{
		if (Pending)
		{
			if (const FEdGraphPinType* Type = Pending->Find(Name))
			{
				return KindFromPinType(*Type);
			}
		}
		const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name);
		if (VarIndex != INDEX_NONE)
		{
			return KindFromPinType(Blueprint->NewVariables[VarIndex].VarType);
		}

		UClass* SearchClass = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->GeneratedClass.Get();
		const FProperty* Property = SearchClass ? FindFProperty<FProperty>(SearchClass, Name) : nullptr;
		if (!Property)
		{
			return EVariableKind::Missing;
		}
		if (Property->IsA<FBoolProperty>())
		{
			return EVariableKind::Bool;
		}
		if (Property->IsA<FFloatProperty>() || Property->IsA<FDoubleProperty>())
		{
			return EVariableKind::Real;
		}
		if (Property->IsA<FIntProperty>())
		{
			return EVariableKind::Int;
		}
		if (Property->IsA<FInt64Property>())
		{
			return EVariableKind::Int64;
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			return ByteProperty->Enum ? EVariableKind::Other : EVariableKind::Byte;
		}
		return EVariableKind::Other;
	}

	/** Maps '>', '>=' ... to the KismetMathLibrary function prefix. */
	bool ParseComparison(const FString& Text, FString& OutPrefix)
	{
		static const TPair<const TCHAR*, const TCHAR*> Operators[] =
		{
			{ TEXT(">"), TEXT("Greater") }, { TEXT(">="), TEXT("GreaterEqual") },
			{ TEXT("<"), TEXT("Less") }, { TEXT("<="), TEXT("LessEqual") },
			{ TEXT("=="), TEXT("EqualEqual") }, { TEXT("!="), TEXT("NotEqual") },
		};
		const FString Trimmed = Text.TrimStartAndEnd();
		for (const TPair<const TCHAR*, const TCHAR*>& Operator : Operators)
		{
			if (Trimmed == Operator.Key)
			{
				OutPrefix = Operator.Value;
				return true;
			}
		}
		return false;
	}

	bool ResolveRule(UBlueprint* Blueprint, const FRuleRequest& Request, FResolvedRule& Out, FString& OutError, const FPendingVariables* Pending = nullptr)
	{
		const FString Rule = Request.Rule.TrimStartAndEnd().ToLower();
		if (Rule == TEXT("bool_variable"))             { Out.Kind = ERuleKind::Bool; }
		else if (Rule == TEXT("not_bool_variable"))    { Out.Kind = ERuleKind::NotBool; }
		else if (Rule == TEXT("compare"))              { Out.Kind = ERuleKind::Compare; }
		else if (Rule == TEXT("time_remaining"))       { Out.Kind = ERuleKind::TimeRemaining; }
		else if (Rule == TEXT("always"))               { Out.Kind = ERuleKind::Always; }
		else if (Rule == TEXT("never"))                { Out.Kind = ERuleKind::Never; }
		else
		{
			OutError = FString::Printf(TEXT("Unknown rule '%s'. Use bool_variable, not_bool_variable, compare, time_remaining, always or never."), *Request.Rule);
			return false;
		}
		Out.TriggerTime = Request.TriggerTime;

		if (Out.Kind == ERuleKind::Bool || Out.Kind == ERuleKind::NotBool)
		{
			Out.Variable = FName(*Request.VariableName.TrimStartAndEnd());
			if (AnimMCP::IsUnset(Request.VariableName) || GetVariableKind(Blueprint, Out.Variable, Pending) != EVariableKind::Bool)
			{
				OutError = FString::Printf(TEXT("Rule '%s' needs variable_name set to a bool member variable (got '%s'). Use anim_list_variables or anim_add_variable."), *Request.Rule, *Request.VariableName);
				return false;
			}
			if (Out.Kind == ERuleKind::NotBool)
			{
				Out.Function = UKismetMathLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Not_PreBool));
			}
		}
		else if (Out.Kind == ERuleKind::Compare)
		{
			Out.Variable = FName(*Request.VariableName.TrimStartAndEnd());
			const EVariableKind Kind = AnimMCP::IsUnset(Request.VariableName) ? EVariableKind::Missing : GetVariableKind(Blueprint, Out.Variable, Pending);
			if (Kind == EVariableKind::Missing)
			{
				OutError = FString::Printf(TEXT("Rule 'compare' needs variable_name set to a float or int member variable (got '%s'). Use anim_list_variables or anim_add_variable."), *Request.VariableName);
				return false;
			}
			if (Kind == EVariableKind::Bool || Kind == EVariableKind::Other)
			{
				OutError = FString::Printf(TEXT("Variable '%s' is not a float, int, int64 or byte, so it cannot be compared to a number. For a bool use rule bool_variable or not_bool_variable."), *Request.VariableName);
				return false;
			}

			FString Prefix;
			if (!ParseComparison(Request.Comparison, Prefix))
			{
				OutError = FString::Printf(TEXT("Unknown comparison '%s'. Use one of: >, >=, <, <=, ==, !=."), *Request.Comparison);
				return false;
			}

			const TCHAR* Suffix = TEXT("DoubleDouble");
			double Min = -DBL_MAX;
			double Max = DBL_MAX;
			switch (Kind)
			{
			case EVariableKind::Int:   Suffix = TEXT("IntInt");     Min = MIN_int32; Max = MAX_int32; break;
			case EVariableKind::Int64: Suffix = TEXT("Int64Int64"); Min = (double)MIN_int64; Max = (double)MAX_int64; break;
			case EVariableKind::Byte:  Suffix = TEXT("ByteByte");   Min = 0; Max = 255; break;
			default: break;
			}

			if (Kind == EVariableKind::Real)
			{
				Out.ThresholdText = FString::SanitizeFloat(Request.Threshold);
			}
			else
			{
				if (FMath::RoundToDouble(Request.Threshold) != Request.Threshold || Request.Threshold < Min || Request.Threshold > Max)
				{
					OutError = FString::Printf(TEXT("Variable '%s' is an integer type, so threshold must be a whole number in [%.0f, %.0f] (got %g)."), *Request.VariableName, Min, Max, Request.Threshold);
					return false;
				}
				Out.ThresholdText = FString::Printf(TEXT("%lld"), (int64)Request.Threshold);
			}

			const FName FunctionName(*FString::Printf(TEXT("%s_%s"), *Prefix, Suffix));
			Out.Function = UKismetMathLibrary::StaticClass()->FindFunctionByName(FunctionName);
			if (!Out.Function)
			{
				OutError = FString::Printf(TEXT("KismetMathLibrary.%s was not found in this engine version."), *FunctionName.ToString());
				return false;
			}
		}
		return true;
	}

	/**
	 * Replaces the transition's rule with a resolved rule. The caller owns the transaction.
	 * OutRuleNodes receives the nodes created in the rule graph.
	 */
	bool ApplyRule(UAnimBlueprint* AnimBP, UAnimStateTransitionNode* Transition, const FResolvedRule& Rule, TArray<UEdGraphNode*>& OutRuleNodes, FString& OutError)
	{
		if (Transition->IsBoundGraphShared())
		{
			OutError = TEXT("This transition uses shared rules. Unshare its rules in the editor before editing it here.");
			return false;
		}
		UAnimationTransitionGraph* RuleGraph = Cast<UAnimationTransitionGraph>(Transition->BoundGraph);
		UAnimGraphNode_TransitionResult* ResultNode = RuleGraph ? RuleGraph->GetResultNode() : nullptr;
		if (!ResultNode)
		{
			OutError = TEXT("Transition has no rule graph result node.");
			return false;
		}
		UEdGraphPin* ResultPin = AnimMCP::FindPin(ResultNode, TEXT("bCanEnterTransition"), TEXT("input"), OutError);
		if (!ResultPin)
		{
			return false;
		}

		Transition->Modify();
		RuleGraph->Modify();
		ResultNode->Modify();

		if (Rule.Kind == ERuleKind::TimeRemaining)
		{
			Transition->bAutomaticRuleBasedOnSequencePlayerInState = true;
			Transition->AutomaticRuleTriggerTime = Rule.TriggerTime;
			return true;
		}
		Transition->bAutomaticRuleBasedOnSequencePlayerInState = false;

		// Replace the rule graph: remove everything except the result node.
		TArray<UEdGraphNode*> ToRemove;
		for (UEdGraphNode* Node : RuleGraph->Nodes)
		{
			if (Node && Node != ResultNode)
			{
				ToRemove.Add(Node);
			}
		}
		for (UEdGraphNode* Node : ToRemove)
		{
			AnimMCP::RemoveNode(AnimBP, Node);
		}

		const UEdGraphSchema* Schema = RuleGraph->GetSchema();
		Schema->BreakPinLinks(*ResultPin, /*bSendsNodeNotifcation*/ true);

		if (Rule.Kind == ERuleKind::Always || Rule.Kind == ERuleKind::Never)
		{
			Schema->TrySetDefaultValue(*ResultPin, Rule.Kind == ERuleKind::Always ? TEXT("true") : TEXT("false"));
			return true;
		}

		const FVector2D ResultPos(ResultNode->NodePosX, ResultNode->NodePosY);
		UEdGraphNode* VarNode = AnimMCP::SpawnNode(RuleGraph, UK2Node_VariableGet::StaticClass(), ResultPos + FVector2D(-450.0, 0.0), [&](UEdGraphNode* NewNode)
		{
			CastChecked<UK2Node_VariableGet>(NewNode)->VariableReference.SetSelfMember(Rule.Variable);
		});
		OutRuleNodes.Add(VarNode);
		UEdGraphPin* VarPin = AnimMCP::FindPin(VarNode, Rule.Variable.ToString(), TEXT("output"), OutError);
		if (!VarPin)
		{
			OutError = FString::Printf(TEXT("Variable getter for '%s' has no output pin. Compile the blueprint once after adding the variable. %s"), *Rule.Variable.ToString(), *OutError);
			return false;
		}

		UEdGraphPin* Source = VarPin;
		if (Rule.Function)
		{
			UEdGraphNode* FunctionNode = AnimMCP::SpawnNode(RuleGraph, UK2Node_CallFunction::StaticClass(), ResultPos + FVector2D(-200.0, 0.0), [&](UEdGraphNode* NewNode)
			{
				CastChecked<UK2Node_CallFunction>(NewNode)->SetFromFunction(Rule.Function);
			});
			OutRuleNodes.Add(FunctionNode);
			UEdGraphPin* InA = AnimMCP::FindPin(FunctionNode, TEXT("A"), TEXT("input"), OutError);
			UEdGraphPin* Out = AnimMCP::FindPin(FunctionNode, TEXT("ReturnValue"), TEXT("output"), OutError);
			if (!InA || !Out || !Schema->TryCreateConnection(VarPin, InA))
			{
				OutError = FString::Printf(TEXT("Failed to wire %s in the transition rule."), *Rule.Function->GetName());
				return false;
			}
			if (Rule.Kind == ERuleKind::Compare)
			{
				UEdGraphPin* InB = AnimMCP::FindPin(FunctionNode, TEXT("B"), TEXT("input"), OutError);
				if (!InB)
				{
					return false;
				}
				Schema->TrySetDefaultValue(*InB, Rule.ThresholdText);
			}
			Source = Out;
		}

		if (!Schema->TryCreateConnection(Source, ResultPin))
		{
			OutError = TEXT("Failed to connect the rule to the transition result.");
			return false;
		}
		return true;
	}

	FString RuleKindToString(ERuleKind Kind)
	{
		switch (Kind)
		{
		case ERuleKind::Bool:          return TEXT("bool_variable");
		case ERuleKind::NotBool:       return TEXT("not_bool_variable");
		case ERuleKind::Compare:       return TEXT("compare");
		case ERuleKind::TimeRemaining: return TEXT("time_remaining");
		case ERuleKind::Always:        return TEXT("always");
		default:                       return TEXT("never");
		}
	}

	TArray<TSharedPtr<FJsonValue>> NodeGuidArray(const TArray<UEdGraphNode*>& Nodes)
	{
		TArray<FString> Guids;
		for (const UEdGraphNode* Node : Nodes)
		{
			Guids.Add(AnimMCP::GuidToString(Node->NodeGuid));
		}
		return AnimMCP::ToJsonArray(Guids);
	}
}

FAnimMCPResult UAnimStateMachineToolset::anim_add_state_machine(const FString& blueprint_path, const FString& graph, const FString& name, float x, float y)
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
	if (!ValidateName(name, Error))
	{
		return AnimMCP::Fail(Error);
	}
	if (!GetDefault<UAnimGraphNode_StateMachine>()->CanCreateUnderSpecifiedSchema(Graph->GetSchema()))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("A state machine cannot be placed in graph '%s'. Use an anim graph such as 'AnimGraph'."), *Graph->GetName()));
	}
	for (const UEdGraphNode* Existing : Graph->Nodes)
	{
		const UAnimGraphNode_StateMachineBase* ExistingSM = Cast<UAnimGraphNode_StateMachineBase>(Existing);
		if (ExistingSM && ExistingSM->EditorStateMachineGraph && ExistingSM->EditorStateMachineGraph->GetName().Equals(name, ESearchCase::IgnoreCase))
		{
			return AnimMCP::Fail(FString::Printf(TEXT("A state machine named '%s' already exists in '%s'."), *name, *Graph->GetName()));
		}
	}

	const FScopedTransaction Transaction(LOCTEXT("AddStateMachine", "AnimMCP: Add State Machine"));
	AnimBP->Modify();
	UEdGraphNode* Node = AnimMCP::SpawnNode(Graph, UAnimGraphNode_StateMachine::StaticClass(), FVector2D(x, y));
	Node->OnRenameNode(name);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	TSharedRef<FJsonObject> Payload = AnimMCP::NodeToJson(Node, /*bIncludePins*/ true);
	if (const UAnimationStateMachineGraph* SMGraph = CastChecked<UAnimGraphNode_StateMachineBase>(Node)->EditorStateMachineGraph)
	{
		if (SMGraph->EntryNode)
		{
			Payload->SetStringField(TEXT("entry_node_guid"), AnimMCP::GuidToString(SMGraph->EntryNode->NodeGuid));
		}
	}
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimStateMachineToolset::anim_add_state(const FString& blueprint_path, const FString& state_machine_guid, const FString& name, float x, float y, bool set_as_entry)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, state_machine_guid, Error);
	if (!SMGraph)
	{
		return AnimMCP::Fail(Error);
	}
	if (!ValidateName(name, Error))
	{
		return AnimMCP::Fail(Error);
	}
	if (IsStateNameTaken(SMGraph, name))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("A state or conduit named '%s' already exists in this state machine."), *name));
	}
	if (set_as_entry && !SMGraph->EntryNode)
	{
		return AnimMCP::Fail(TEXT("State machine has no Entry node."));
	}

	const FScopedTransaction Transaction(LOCTEXT("AddState", "AnimMCP: Add State"));
	AnimBP->Modify();
	UAnimStateNode* State = CastChecked<UAnimStateNode>(SpawnNamedStateNode(SMGraph, UAnimStateNode::StaticClass(), name, FVector2D(x, y)));

	if (set_as_entry)
	{
		UEdGraphPin* EntryPin = SMGraph->EntryNode->GetOutputPin();
		SMGraph->EntryNode->Modify();
		SMGraph->GetSchema()->BreakPinLinks(*EntryPin, /*bSendsNodeNotifcation*/ true);
		if (!SMGraph->GetSchema()->TryCreateConnection(EntryPin, State->GetInputPin()))
		{
			UE_LOG(LogAnimMCP, Warning, TEXT("Could not wire Entry to state '%s'."), *name);
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	TSharedRef<FJsonObject> Payload = AnimMCP::NodeToJson(State, /*bIncludePins*/ false);
	Payload->SetBoolField(TEXT("is_entry"), set_as_entry);
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimStateMachineToolset::anim_remove_state(const FString& blueprint_path, const FString& state_guid)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UAnimStateNodeBase* State = FindStateOrConduit(AnimBP, state_guid, Error);
	if (!State)
	{
		return AnimMCP::Fail(Error);
	}
	if (!State->CanUserDeleteNode())
	{
		return AnimMCP::Fail(TEXT("This state cannot be deleted."));
	}

	const FScopedTransaction Transaction(LOCTEXT("RemoveState", "AnimMCP: Remove State"));
	AnimBP->Modify();

	TArray<TSharedPtr<FJsonValue>> RemovedTransitions;
	for (UAnimStateTransitionNode* Transition : GetTransitionsTouching(State))
	{
		RemovedTransitions.Add(MakeShared<FJsonValueString>(AnimMCP::GuidToString(Transition->NodeGuid)));
		AnimMCP::RemoveNode(AnimBP, Transition);
	}
	AnimMCP::RemoveNode(AnimBP, State);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("removed_state_guid"), state_guid);
	Payload->SetArrayField(TEXT("removed_transition_guids"), RemovedTransitions);
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimStateMachineToolset::anim_add_transition(const FString& blueprint_path, const FString& from_state_guid, const FString& to_state_guid, float crossfade_duration)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UAnimStateNodeBase* From = FindStateOrConduit(AnimBP, from_state_guid, Error);
	if (!From)
	{
		return AnimMCP::Fail(Error);
	}
	UAnimStateNodeBase* To = FindStateOrConduit(AnimBP, to_state_guid, Error);
	if (!To)
	{
		return AnimMCP::Fail(Error);
	}
	UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(From->GetGraph());
	if (!SMGraph || To->GetGraph() != SMGraph)
	{
		return AnimMCP::Fail(TEXT("Both states must be in the same state machine."));
	}
	if (!From->GetOutputPin() || !To->GetInputPin())
	{
		return AnimMCP::Fail(TEXT("These nodes cannot be connected by a transition."));
	}
	if (crossfade_duration < 0.f)
	{
		return AnimMCP::Fail(TEXT("crossfade_duration must be >= 0."));
	}

	const FScopedTransaction Transaction(LOCTEXT("AddTransition", "AnimMCP: Add Transition"));
	AnimBP->Modify();
	From->Modify();
	To->Modify();

	const FVector2D Midpoint((From->NodePosX + To->NodePosX) * 0.5, (From->NodePosY + To->NodePosY) * 0.5);
	UAnimStateTransitionNode* Transition = CastChecked<UAnimStateTransitionNode>(
		AnimMCP::SpawnNode(SMGraph, UAnimStateTransitionNode::StaticClass(), Midpoint));
	Transition->CreateConnections(From, To);
	Transition->CrossfadeDuration = crossfade_duration;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	return AnimMCP::Ok(AnimMCP::NodeToJson(Transition, /*bIncludePins*/ false));
}

FAnimMCPResult UAnimStateMachineToolset::anim_remove_transition(const FString& blueprint_path, const FString& transition_guid)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UAnimStateTransitionNode* Transition = AnimMCP::FindNodeOfType<UAnimStateTransitionNode>(AnimBP, transition_guid, Error);
	if (!Transition)
	{
		return AnimMCP::Fail(Error);
	}

	const FScopedTransaction Transaction(LOCTEXT("RemoveTransition", "AnimMCP: Remove Transition"));
	AnimBP->Modify();
	AnimMCP::RemoveNode(AnimBP, Transition);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("removed_transition_guid"), transition_guid);
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimStateMachineToolset::anim_set_transition_rule(const FString& blueprint_path, const FString& transition_guid, const FString& rule, const FString& variable_name, float trigger_time, float crossfade_duration,
	const FString& comparison, float threshold)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UAnimStateTransitionNode* Transition = AnimMCP::FindNodeOfType<UAnimStateTransitionNode>(AnimBP, transition_guid, Error);
	if (!Transition)
	{
		return AnimMCP::Fail(Error);
	}
	if (Transition->IsBoundGraphShared())
	{
		return AnimMCP::Fail(TEXT("This transition uses shared rules. Unshare its rules in the editor before editing it here."));
	}

	FRuleRequest Request;
	Request.Rule = rule;
	Request.VariableName = variable_name;
	Request.Comparison = comparison;
	Request.Threshold = threshold;
	Request.TriggerTime = trigger_time;
	FResolvedRule Resolved;
	if (!ResolveRule(AnimBP, Request, Resolved, Error))
	{
		return AnimMCP::Fail(Error);
	}

	const FScopedTransaction Transaction(LOCTEXT("SetTransitionRule", "AnimMCP: Set Transition Rule"));
	AnimBP->Modify();
	if (crossfade_duration >= 0.f)
	{
		Transition->Modify();
		Transition->CrossfadeDuration = crossfade_duration;
	}

	TArray<UEdGraphNode*> RuleNodes;
	if (!ApplyRule(AnimBP, Transition, Resolved, RuleNodes, Error))
	{
		return AnimMCP::Fail(Error);
	}
	if (Resolved.Kind == ERuleKind::TimeRemaining)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	}
	else
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	}

	TSharedRef<FJsonObject> Payload = AnimMCP::NodeToJson(Transition, /*bIncludePins*/ false);
	Payload->SetStringField(TEXT("rule"), RuleKindToString(Resolved.Kind));
	Payload->SetArrayField(TEXT("rule_nodes"), NodeGuidArray(RuleNodes));
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimStateMachineToolset::anim_set_state_animation(const FString& blueprint_path, const FString& state_guid, const FString& asset_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UAnimStateNode* State = AnimMCP::FindNodeOfType<UAnimStateNode>(AnimBP, state_guid, Error);
	if (!State)
	{
		return AnimMCP::Fail(Error);
	}
	UAnimationAsset* Asset = AnimMCP::LoadAsset<UAnimationAsset>(asset_path, /*bForWrite*/ false, Error);
	if (!Asset)
	{
		return AnimMCP::Fail(Error);
	}

	UClass* PlayerClass = nullptr;
	if (Asset->IsA<UAnimMontage>())
	{
		return AnimMCP::Fail(TEXT("Montages cannot be played by a state. Use an AnimSequence or BlendSpace, or play the montage from a Slot node."));
	}
	else if (Asset->IsA<UAnimSequenceBase>())
	{
		PlayerClass = UAnimGraphNode_SequencePlayer::StaticClass();
	}
	else if (Asset->IsA<UBlendSpace>())
	{
		PlayerClass = UAnimGraphNode_BlendSpacePlayer::StaticClass();
	}
	else
	{
		return AnimMCP::Fail(FString::Printf(TEXT("'%s' is a %s. States can play AnimSequences or BlendSpaces."), *asset_path, *Asset->GetClass()->GetName()));
	}

	if (AnimBP->TargetSkeleton && Asset->GetSkeleton() && !AnimBP->TargetSkeleton->IsCompatibleForEditor(Asset->GetSkeleton()))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("'%s' uses skeleton %s, which is not compatible with the blueprint's skeleton %s."),
			*asset_path, *Asset->GetSkeleton()->GetPathName(), *AnimBP->TargetSkeleton->GetPathName()));
	}

	UEdGraph* StateGraph = State->BoundGraph;
	UEdGraphPin* PoseSink = State->GetPoseSinkPinInsideState();
	UAnimGraphNode_StateResult* ResultNode = State->GetResultNodeInsideState();
	if (!StateGraph || !PoseSink || !ResultNode)
	{
		return AnimMCP::Fail(TEXT("State has no inner graph or output pose."));
	}

	const FScopedTransaction Transaction(LOCTEXT("SetStateAnimation", "AnimMCP: Set State Animation"));
	AnimBP->Modify();
	StateGraph->Modify();
	ResultNode->Modify();

	// Drop asset players that only fed the output pose; other upstream nodes are left untouched.
	TArray<UEdGraphNode*> ToRemove;
	for (UEdGraphPin* Linked : PoseSink->LinkedTo)
	{
		UAnimGraphNode_AssetPlayerBase* OldPlayer = Cast<UAnimGraphNode_AssetPlayerBase>(Linked->GetOwningNode());
		if (OldPlayer && Linked->LinkedTo.Num() == 1)
		{
			ToRemove.Add(OldPlayer);
		}
	}
	StateGraph->GetSchema()->BreakPinLinks(*PoseSink, /*bSendsNodeNotifcation*/ true);
	for (UEdGraphNode* Node : ToRemove)
	{
		AnimMCP::RemoveNode(AnimBP, Node);
	}

	const FVector2D Position(ResultNode->NodePosX - 350.0, ResultNode->NodePosY);
	UAnimGraphNode_AssetPlayerBase* Player = CastChecked<UAnimGraphNode_AssetPlayerBase>(AnimMCP::SpawnNode(StateGraph, PlayerClass, Position));
	Player->SetAnimationAsset(Asset);

	UEdGraphPin* PlayerOut = AnimMCP::FindFirstPin(Player, EGPD_Output);
	if (!PlayerOut || !StateGraph->GetSchema()->TryCreateConnection(PlayerOut, PoseSink))
	{
		return AnimMCP::Fail(TEXT("Failed to connect the asset player to the state's output pose."));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	return AnimMCP::Ok(AnimMCP::NodeToJson(Player, /*bIncludePins*/ false));
}

FAnimMCPResult UAnimStateMachineToolset::anim_add_conduit(const FString& blueprint_path, const FString& state_machine_guid, const FString& name, float x, float y)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UAnimationStateMachineGraph* SMGraph = FindStateMachineGraph(AnimBP, state_machine_guid, Error);
	if (!SMGraph)
	{
		return AnimMCP::Fail(Error);
	}
	if (!ValidateName(name, Error))
	{
		return AnimMCP::Fail(Error);
	}
	if (IsStateNameTaken(SMGraph, name))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("A state or conduit named '%s' already exists in this state machine."), *name));
	}

	const FScopedTransaction Transaction(LOCTEXT("AddConduit", "AnimMCP: Add Conduit"));
	AnimBP->Modify();
	UEdGraphNode* Conduit = SpawnNamedStateNode(SMGraph, UAnimStateConduitNode::StaticClass(), name, FVector2D(x, y));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	return AnimMCP::Ok(AnimMCP::NodeToJson(Conduit, /*bIncludePins*/ false));
}

#undef LOCTEXT_NAMESPACE
