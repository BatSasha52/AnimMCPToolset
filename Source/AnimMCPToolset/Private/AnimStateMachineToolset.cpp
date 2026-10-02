// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#include "AnimStateMachineToolset.h"

#include "AnimMCPHelpers.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_AssetPlayerBase.h"
#include "AnimGraphNode_BlendSpaceEvaluator.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequenceEvaluator.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_Slot.h"
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
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/ITransaction.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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
	// ---- State animations ------------------------------------------------------------------

	/** What a state should play, as requested; ResolveStateAnimation fills in the resolved fields. */
	struct FStateAnimation
	{
		FString AssetPath = TEXT("none");
		FString NodeType = TEXT("auto");
		bool bLoop = true;
		float PlayRate = 1.f;
		FString SlotName = TEXT("DefaultSlot");

		// Resolved
		UAnimationAsset* Asset = nullptr;
		UClass* PlayerClass = nullptr;  // null for a slot with no source animation
		bool bSlot = false;
	};

	/** Validates the request: loads the asset, checks the skeleton and the node type, and picks the node classes. */
	bool ResolveStateAnimation(UAnimBlueprint* AnimBP, FStateAnimation& Anim, FString& OutError)
	{
		const FString Type = Anim.NodeType.TrimStartAndEnd().ToLower();
		const bool bAuto = Type.IsEmpty() || Type == TEXT("auto");
		const bool bSequencePlayer = Type == TEXT("sequence_player");
		const bool bSequenceEvaluator = Type == TEXT("sequence_evaluator");
		const bool bBlendSpacePlayer = Type == TEXT("blendspace_player");
		const bool bBlendSpaceEvaluator = Type == TEXT("blendspace_evaluator");
		Anim.bSlot = Type == TEXT("slot");
		if (!bAuto && !bSequencePlayer && !bSequenceEvaluator && !bBlendSpacePlayer && !bBlendSpaceEvaluator && !Anim.bSlot)
		{
			OutError = FString::Printf(TEXT("Unknown node_type '%s'. Use auto, sequence_player, sequence_evaluator, blendspace_player, blendspace_evaluator or slot."), *Anim.NodeType);
			return false;
		}
		if ((bSequenceEvaluator || bBlendSpaceEvaluator) && Anim.PlayRate != 1.f)
		{
			OutError = TEXT("play_rate does not apply to evaluators: an evaluator plays the time you drive into its time pin. Leave play_rate at 1.");
			return false;
		}
		if (Anim.bSlot && Anim.SlotName.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("node_type 'slot' needs a slot_name, e.g. 'DefaultSlot'.");
			return false;
		}

		if (AnimMCP::IsUnset(Anim.AssetPath))
		{
			if (Anim.bSlot)
			{
				if (!Anim.bLoop || Anim.PlayRate != 1.f)
				{
					OutError = TEXT("loop and play_rate apply to the slot's source animation; pass asset_path as well.");
					return false;
				}
				return true;  // A slot with nothing plugged into its source.
			}
			OutError = TEXT("asset_path is required (it may only be 'none' for node_type 'slot').");
			return false;
		}

		Anim.Asset = AnimMCP::LoadAsset<UAnimationAsset>(Anim.AssetPath, /*bForWrite*/ false, OutError);
		if (!Anim.Asset)
		{
			return false;
		}
		if (Anim.Asset->IsA<UAnimMontage>())
		{
			OutError = FString::Printf(TEXT("'%s' is a montage. Montages are not played by a state node: use node_type 'slot' with the montage's slot name and play the montage at runtime (Play Montage / Montage_Play)."), *Anim.AssetPath);
			return false;
		}

		const bool bIsSequence = Anim.Asset->IsA<UAnimSequenceBase>();
		const bool bIsBlendSpace = Anim.Asset->IsA<UBlendSpace>();
		if (!bIsSequence && !bIsBlendSpace)
		{
			OutError = FString::Printf(TEXT("'%s' is a %s. States can play AnimSequences or BlendSpaces."), *Anim.AssetPath, *Anim.Asset->GetClass()->GetName());
			return false;
		}
		if ((bSequencePlayer || bSequenceEvaluator) && !bIsSequence)
		{
			OutError = FString::Printf(TEXT("node_type '%s' needs an AnimSequence, but '%s' is a %s."), *Type, *Anim.AssetPath, *Anim.Asset->GetClass()->GetName());
			return false;
		}
		if ((bBlendSpacePlayer || bBlendSpaceEvaluator) && !bIsBlendSpace)
		{
			OutError = FString::Printf(TEXT("node_type '%s' needs a BlendSpace, but '%s' is a %s."), *Type, *Anim.AssetPath, *Anim.Asset->GetClass()->GetName());
			return false;
		}

		if (bSequenceEvaluator)
		{
			Anim.PlayerClass = UAnimGraphNode_SequenceEvaluator::StaticClass();
		}
		else if (bBlendSpaceEvaluator)
		{
			Anim.PlayerClass = UAnimGraphNode_BlendSpaceEvaluator::StaticClass();
		}
		else
		{
			Anim.PlayerClass = bIsSequence ? UAnimGraphNode_SequencePlayer::StaticClass() : UAnimGraphNode_BlendSpacePlayer::StaticClass();
		}

		if (AnimBP->TargetSkeleton && Anim.Asset->GetSkeleton() && !AnimBP->TargetSkeleton->IsCompatibleForEditor(Anim.Asset->GetSkeleton()))
		{
			OutError = FString::Printf(TEXT("'%s' uses skeleton %s, which is not compatible with the blueprint's skeleton %s."),
				*Anim.AssetPath, *Anim.Asset->GetSkeleton()->GetPathName(), *AnimBP->TargetSkeleton->GetPathName());
			return false;
		}
		return true;
	}

	/** Sets one member of an anim node's runtime struct the way the details panel does (works for folded properties). */
	bool SetNodeStructValue(UAnimGraphNode_Base* Node, const FName PropertyName, const FString& Value, FString& OutError)
	{
		FStructProperty* NodeProperty = Node->GetFNodeProperty();
		FProperty* Property = NodeProperty ? FindFProperty<FProperty>(NodeProperty->Struct, PropertyName) : nullptr;
		if (!Property)
		{
			OutError = FString::Printf(TEXT("%s has no property '%s'."), *Node->GetClass()->GetName(), *PropertyName.ToString());
			return false;
		}
		void* NodeData = NodeProperty->ContainerPtrToValuePtr<void>(Node);
		Node->PreEditChange(NodeProperty);
		const TCHAR* ImportResult = Property->ImportText_Direct(*Value, Property->ContainerPtrToValuePtr<void>(NodeData), Node, PPF_None);
		FPropertyChangedEvent ChangedEvent(NodeProperty, EPropertyChangeType::ValueSet);
		Node->PostEditChangeProperty(ChangedEvent);
		if (!ImportResult)
		{
			OutError = FString::Printf(TEXT("Could not set %s to '%s'."), *PropertyName.ToString(), *Value);
			return false;
		}
		return true;
	}

	/** True for nodes this plugin places in front of a state's output: asset players and slots. */
	bool IsReplaceablePoseNode(const UEdGraphNode* Node)
	{
		return Node && (Node->IsA<UAnimGraphNode_AssetPlayerBase>() || Node->IsA<UAnimGraphNode_Slot>());
	}

	/**
	 * Replaces the asset player (or slot) wired to the state's output pose with new nodes. The caller owns the transaction.
	 * Other nodes inside the state are left alone. OutPoseNode is the node wired to the output; OutPlayer the asset player (may be null for an empty slot).
	 */
	bool ApplyStateAnimation(UAnimBlueprint* AnimBP, UAnimStateNode* State, const FStateAnimation& Anim,
		UAnimGraphNode_Base*& OutPoseNode, UAnimGraphNode_Base*& OutPlayer, FString& OutError)
	{
		OutPoseNode = nullptr;
		OutPlayer = nullptr;
		UEdGraph* StateGraph = State->BoundGraph;
		UEdGraphPin* PoseSink = State->GetPoseSinkPinInsideState();
		UAnimGraphNode_StateResult* ResultNode = State->GetResultNodeInsideState();
		if (!StateGraph || !PoseSink || !ResultNode)
		{
			OutError = TEXT("State has no inner graph or output pose.");
			return false;
		}
		StateGraph->Modify();
		ResultNode->Modify();
		const UEdGraphSchema* Schema = StateGraph->GetSchema();

		// Drop asset players / slots that only fed the output pose (and a player that only fed such a slot).
		TArray<UEdGraphNode*> ToRemove;
		for (UEdGraphPin* Linked : PoseSink->LinkedTo)
		{
			UEdGraphNode* OldNode = Linked->GetOwningNode();
			if (!IsReplaceablePoseNode(OldNode) || Linked->LinkedTo.Num() != 1)
			{
				continue;
			}
			ToRemove.Add(OldNode);
			if (OldNode->IsA<UAnimGraphNode_Slot>())
			{
				if (UEdGraphPin* SourcePin = AnimMCP::FindFirstPin(OldNode, EGPD_Input))
				{
					for (UEdGraphPin* SourceLink : SourcePin->LinkedTo)
					{
						UEdGraphNode* SourceNode = SourceLink->GetOwningNode();
						if (SourceNode->IsA<UAnimGraphNode_AssetPlayerBase>() && SourceLink->LinkedTo.Num() == 1)
						{
							ToRemove.Add(SourceNode);
						}
					}
				}
			}
		}
		Schema->BreakPinLinks(*PoseSink, /*bSendsNodeNotifcation*/ true);
		for (UEdGraphNode* Node : ToRemove)
		{
			AnimMCP::RemoveNode(AnimBP, Node);
		}

		const FVector2D OutputPos(ResultNode->NodePosX - 350.0, ResultNode->NodePosY);
		if (Anim.PlayerClass)
		{
			const FVector2D PlayerPos = Anim.bSlot ? OutputPos - FVector2D(300.0, 0.0) : OutputPos;
			UAnimGraphNode_AssetPlayerBase* Player = CastChecked<UAnimGraphNode_AssetPlayerBase>(AnimMCP::SpawnNode(StateGraph, Anim.PlayerClass, PlayerPos));
			Player->SetAnimationAsset(Anim.Asset);
			OutPlayer = Player;

			// Only touch settings that differ from the node defaults, so a plain call creates exactly what the editor would.
			FName LoopProperty = TEXT("bLoopAnimation");
			if (Anim.PlayerClass->IsChildOf(UAnimGraphNode_BlendSpaceBase::StaticClass()))
			{
				LoopProperty = TEXT("bLoop");
			}
			else if (Anim.PlayerClass->IsChildOf(UAnimGraphNode_SequenceEvaluator::StaticClass()))
			{
				LoopProperty = TEXT("bShouldLoop");
			}
			if (!Anim.bLoop && !SetNodeStructValue(Player, LoopProperty, TEXT("False"), OutError))
			{
				return false;
			}
			if (Anim.PlayRate != 1.f && !SetNodeStructValue(Player, TEXT("PlayRate"), FString::SanitizeFloat(Anim.PlayRate), OutError))
			{
				return false;
			}
			OutPoseNode = Player;
		}

		if (Anim.bSlot)
		{
			UAnimGraphNode_Slot* Slot = CastChecked<UAnimGraphNode_Slot>(AnimMCP::SpawnNode(StateGraph, UAnimGraphNode_Slot::StaticClass(), OutputPos));
			if (!SetNodeStructValue(Slot, TEXT("SlotName"), Anim.SlotName.TrimStartAndEnd(), OutError))
			{
				return false;
			}
			if (OutPlayer)
			{
				UEdGraphPin* PlayerOut = AnimMCP::FindFirstPin(OutPlayer, EGPD_Output);
				UEdGraphPin* SlotSource = AnimMCP::FindFirstPin(Slot, EGPD_Input);
				if (!PlayerOut || !SlotSource || !Schema->TryCreateConnection(PlayerOut, SlotSource))
				{
					OutError = TEXT("Failed to connect the source animation to the slot.");
					return false;
				}
			}
			OutPoseNode = Slot;
		}

		UEdGraphPin* PoseOut = OutPoseNode ? AnimMCP::FindFirstPin(OutPoseNode, EGPD_Output) : nullptr;
		if (!PoseOut || !Schema->TryCreateConnection(PoseOut, PoseSink))
		{
			OutError = TEXT("Failed to connect the new node to the state's output pose.");
			return false;
		}
		return true;
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

FAnimMCPResult UAnimStateMachineToolset::anim_set_state_animation(const FString& blueprint_path, const FString& state_guid, const FString& asset_path,
	const FString& node_type, bool loop, float play_rate, const FString& slot_name)
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
	FStateAnimation Anim;
	Anim.AssetPath = asset_path;
	Anim.NodeType = node_type;
	Anim.bLoop = loop;
	Anim.PlayRate = play_rate;
	Anim.SlotName = slot_name;
	if (!ResolveStateAnimation(AnimBP, Anim, Error))
	{
		return AnimMCP::Fail(Error);
	}
	if (!State->BoundGraph || !State->GetPoseSinkPinInsideState() || !State->GetResultNodeInsideState())
	{
		return AnimMCP::Fail(TEXT("State has no inner graph or output pose."));
	}

	const FScopedTransaction Transaction(LOCTEXT("SetStateAnimation", "AnimMCP: Set State Animation"));
	AnimBP->Modify();
	UAnimGraphNode_Base* PoseNode = nullptr;
	UAnimGraphNode_Base* Player = nullptr;
	if (!ApplyStateAnimation(AnimBP, State, Anim, PoseNode, Player, Error))
	{
		return AnimMCP::Fail(Error);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	TSharedRef<FJsonObject> Payload = AnimMCP::NodeToJson(PoseNode, /*bIncludePins*/ false);
	if (Player)
	{
		Payload->SetStringField(TEXT("player_node_guid"), AnimMCP::GuidToString(Player->NodeGuid));
	}
	if (Anim.bSlot)
	{
		Payload->SetStringField(TEXT("slot_node_guid"), AnimMCP::GuidToString(PoseNode->NodeGuid));
	}
	return AnimMCP::Ok(Payload);
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

namespace
{
	// ---- anim_build_state_machine spec ----------------------------------------------------

	struct FSpecVariable
	{
		FName Name;
		FEdGraphPinType Type;
		FString Default;
		FString Category;
	};

	struct FSpecNode
	{
		FString Name;
		bool bConduit = false;
		bool bHasAnimation = false;
		FStateAnimation Animation;
		TOptional<FVector2D> Position;
	};

	struct FSpecTransition
	{
		int32 From = INDEX_NONE;
		int32 To = INDEX_NONE;
		float Crossfade = 0.2f;
		bool bHasRule = false;
		FResolvedRule Rule;
	};

	struct FStateMachineSpec
	{
		FString Name;
		UEdGraph* Graph = nullptr;
		FVector2D Position = FVector2D::ZeroVector;
		bool bConnectToOutput = false;
		UEdGraphPin* OutputPin = nullptr;
		TArray<FSpecVariable> NewVariables;
		TArray<FString> ReusedVariables;
		TArray<FSpecNode> Nodes;  // states first, then conduits
		int32 EntryIndex = INDEX_NONE;
		TArray<FSpecTransition> Transitions;
	};

	/** Reads optional typed fields from a spec object and records every problem instead of stopping at the first. */
	struct FSpecReader
	{
		TArray<FString>& Problems;

		void CheckKeys(const TSharedPtr<FJsonObject>& Object, const FString& Where, const TArray<FString>& Allowed) const
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
			{
				if (!Allowed.ContainsByPredicate([&Field](const FString& Key) { return Key.Equals(Field.Key, ESearchCase::IgnoreCase); }))
				{
					Problems.Add(FString::Printf(TEXT("%s: unknown field '%s'. Allowed: %s."), *Where, *Field.Key, *FString::Join(Allowed, TEXT(", "))));
				}
			}
		}

		TSharedPtr<FJsonValue> Find(const TSharedPtr<FJsonObject>& Object, const FString& Key) const
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
			{
				if (Field.Key.Equals(Key, ESearchCase::IgnoreCase))
				{
					return Field.Value;
				}
			}
			return nullptr;
		}

		bool String(const TSharedPtr<FJsonObject>& Object, const FString& Where, const FString& Key, FString& Out) const
		{
			const TSharedPtr<FJsonValue> Value = Find(Object, Key);
			if (!Value.IsValid() || Value->IsNull())
			{
				return false;
			}
			if (Value->Type != EJson::String)
			{
				Problems.Add(FString::Printf(TEXT("%s: '%s' must be a string."), *Where, *Key));
				return false;
			}
			Out = Value->AsString();
			return true;
		}

		bool Number(const TSharedPtr<FJsonObject>& Object, const FString& Where, const FString& Key, double& Out) const
		{
			const TSharedPtr<FJsonValue> Value = Find(Object, Key);
			if (!Value.IsValid() || Value->IsNull())
			{
				return false;
			}
			if (Value->Type != EJson::Number)
			{
				Problems.Add(FString::Printf(TEXT("%s: '%s' must be a number."), *Where, *Key));
				return false;
			}
			Out = Value->AsNumber();
			return true;
		}

		bool Bool(const TSharedPtr<FJsonObject>& Object, const FString& Where, const FString& Key, bool& Out) const
		{
			const TSharedPtr<FJsonValue> Value = Find(Object, Key);
			if (!Value.IsValid() || Value->IsNull())
			{
				return false;
			}
			if (Value->Type != EJson::Boolean)
			{
				Problems.Add(FString::Printf(TEXT("%s: '%s' must be true or false."), *Where, *Key));
				return false;
			}
			Out = Value->AsBool();
			return true;
		}

		TArray<TSharedPtr<FJsonObject>> ObjectArray(const TSharedPtr<FJsonObject>& Object, const FString& Key) const
		{
			TArray<TSharedPtr<FJsonObject>> Result;
			const TSharedPtr<FJsonValue> Value = Find(Object, Key);
			if (!Value.IsValid() || Value->IsNull())
			{
				return Result;
			}
			if (Value->Type != EJson::Array)
			{
				Problems.Add(FString::Printf(TEXT("'%s' must be an array."), *Key));
				return Result;
			}
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				if (Items[Index].IsValid() && Items[Index]->Type == EJson::Object)
				{
					Result.Add(Items[Index]->AsObject());
				}
				else
				{
					Problems.Add(FString::Printf(TEXT("%s[%d] must be an object."), *Key, Index));
				}
			}
			return Result;
		}
	};

	/** Parses and validates the whole spec against the blueprint. Nothing is modified. Returns false with every problem listed. */
	bool ParseStateMachineSpec(UAnimBlueprint* AnimBP, const FString& SpecText, FStateMachineSpec& Out, FString& OutError)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SpecText);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = FString::Printf(TEXT("spec is not valid JSON: %s"), *Reader->GetErrorMessage());
			return false;
		}

		TArray<FString> Problems;
		const FSpecReader Read{ Problems };
		Read.CheckKeys(Root, TEXT("spec"), { TEXT("name"), TEXT("graph"), TEXT("x"), TEXT("y"), TEXT("connect_to_output"), TEXT("variables"), TEXT("entry_state"), TEXT("states"), TEXT("conduits"), TEXT("transitions") });

		// State machine node.
		FString Error;
		if (!Read.String(Root, TEXT("spec"), TEXT("name"), Out.Name))
		{
			Problems.Add(TEXT("spec: 'name' (the state machine name) is required."));
		}
		else if (!ValidateName(Out.Name, Error))
		{
			Problems.Add(FString::Printf(TEXT("spec.name: %s"), *Error));
		}

		FString GraphName = TEXT("AnimGraph");
		Read.String(Root, TEXT("spec"), TEXT("graph"), GraphName);
		Out.Graph = AnimMCP::FindGraph(AnimBP, GraphName, Error);
		if (!Out.Graph)
		{
			Problems.Add(FString::Printf(TEXT("spec.graph: %s"), *Error));
		}
		else if (!GetDefault<UAnimGraphNode_StateMachine>()->CanCreateUnderSpecifiedSchema(Out.Graph->GetSchema()))
		{
			Problems.Add(FString::Printf(TEXT("spec.graph: a state machine cannot be placed in '%s'. Use an anim graph such as 'AnimGraph'."), *Out.Graph->GetName()));
			Out.Graph = nullptr;
		}
		if (Out.Graph)
		{
			for (const UEdGraphNode* Existing : Out.Graph->Nodes)
			{
				const UAnimGraphNode_StateMachineBase* ExistingSM = Cast<UAnimGraphNode_StateMachineBase>(Existing);
				if (ExistingSM && ExistingSM->EditorStateMachineGraph && ExistingSM->EditorStateMachineGraph->GetName().Equals(Out.Name, ESearchCase::IgnoreCase))
				{
					Problems.Add(FString::Printf(TEXT("spec.name: a state machine named '%s' already exists in '%s'."), *Out.Name, *Out.Graph->GetName()));
				}
			}
		}

		double X = 0.0, Y = 0.0;
		Read.Number(Root, TEXT("spec"), TEXT("x"), X);
		Read.Number(Root, TEXT("spec"), TEXT("y"), Y);
		Out.Position = FVector2D(X, Y);

		Read.Bool(Root, TEXT("spec"), TEXT("connect_to_output"), Out.bConnectToOutput);
		if (Out.bConnectToOutput && Out.Graph)
		{
			for (UEdGraphNode* Node : Out.Graph->Nodes)
			{
				if (Node && Node->IsA<UAnimGraphNode_Root>())
				{
					Out.OutputPin = AnimMCP::FindFirstPin(Node, EGPD_Input);
					break;
				}
			}
			if (!Out.OutputPin)
			{
				Problems.Add(FString::Printf(TEXT("spec.connect_to_output: graph '%s' has no Output Pose node."), *Out.Graph->GetName()));
			}
		}

		// Variables.
		FPendingVariables Pending;
		const TArray<TSharedPtr<FJsonObject>> Variables = Read.ObjectArray(Root, TEXT("variables"));
		for (int32 Index = 0; Index < Variables.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>& Item = Variables[Index];
			const FString Where = FString::Printf(TEXT("variables[%d]"), Index);
			Read.CheckKeys(Item, Where, { TEXT("name"), TEXT("type"), TEXT("default"), TEXT("category") });

			FSpecVariable Variable;
			FString Name, Type;
			if (!Read.String(Item, Where, TEXT("name"), Name) || !Read.String(Item, Where, TEXT("type"), Type))
			{
				Problems.Add(FString::Printf(TEXT("%s: 'name' and 'type' are required."), *Where));
				continue;
			}
			FText NameError;
			if (!FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS, &NameError))
			{
				Problems.Add(FString::Printf(TEXT("%s: '%s' is not a valid variable name. %s"), *Where, *Name, *NameError.ToString()));
				continue;
			}
			Variable.Name = FName(*Name);
			if (!AnimMCP::ParsePinType(Type, Variable.Type, Error))
			{
				Problems.Add(FString::Printf(TEXT("%s: %s"), *Where, *Error));
				continue;
			}
			if (Pending.Contains(Variable.Name) || Out.ReusedVariables.Contains(Name))
			{
				Problems.Add(FString::Printf(TEXT("%s: variable '%s' is listed twice."), *Where, *Name));
				continue;
			}

			const EVariableKind Existing = GetVariableKind(AnimBP, Variable.Name, nullptr);
			if (Existing != EVariableKind::Missing)
			{
				// Already there: reuse it if it holds the same kind of value.
				if (Existing != KindFromPinType(Variable.Type) || Existing == EVariableKind::Other)
				{
					Problems.Add(FString::Printf(TEXT("%s: '%s' already exists with a different type. Remove it, pick another name, or leave it out of 'variables' to use it as is."), *Where, *Name));
				}
				else
				{
					Out.ReusedVariables.Add(Name);
				}
				continue;
			}

			FString Default;
			if (Read.String(Item, Where, TEXT("default"), Default) && AnimMCP::IsUnset(Default))
			{
				Default.Reset();
			}
			if (!AnimMCP::ValidateDefaultValue(Variable.Type, Variable.Name, Default, Error))
			{
				Problems.Add(FString::Printf(TEXT("%s: %s"), *Where, *Error));
				continue;
			}
			Variable.Default = Default;
			Read.String(Item, Where, TEXT("category"), Variable.Category);
			Pending.Add(Variable.Name, Variable.Type);
			Out.NewVariables.Add(MoveTemp(Variable));
		}

		// States and conduits share one name space.
		auto ReadNodes = [&](const TCHAR* Key, bool bConduit)
		{
			const TArray<TSharedPtr<FJsonObject>> Items = Read.ObjectArray(Root, Key);
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>& Item = Items[Index];
				const FString Where = FString::Printf(TEXT("%s[%d]"), Key, Index);
				if (bConduit)
				{
					Read.CheckKeys(Item, Where, { TEXT("name"), TEXT("x"), TEXT("y") });
				}
				else
				{
					Read.CheckKeys(Item, Where, { TEXT("name"), TEXT("animation"), TEXT("node_type"), TEXT("loop"), TEXT("play_rate"), TEXT("slot_name"), TEXT("x"), TEXT("y") });
				}

				FSpecNode Node;
				Node.bConduit = bConduit;
				if (!Read.String(Item, Where, TEXT("name"), Node.Name))
				{
					Problems.Add(FString::Printf(TEXT("%s: 'name' is required."), *Where));
					continue;
				}
				if (!ValidateName(Node.Name, Error))
				{
					Problems.Add(FString::Printf(TEXT("%s: %s"), *Where, *Error));
					continue;
				}
				if (Out.Nodes.ContainsByPredicate([&Node](const FSpecNode& Other) { return Other.Name.Equals(Node.Name, ESearchCase::IgnoreCase); }))
				{
					Problems.Add(FString::Printf(TEXT("%s: the name '%s' is used twice (state and conduit names must be unique)."), *Where, *Node.Name));
					continue;
				}

				double NodeX = 0.0, NodeY = 0.0;
				const bool bHasX = Read.Number(Item, Where, TEXT("x"), NodeX);
				const bool bHasY = Read.Number(Item, Where, TEXT("y"), NodeY);
				if (bHasX || bHasY)
				{
					Node.Position = FVector2D(NodeX, NodeY);
				}

				if (!bConduit)
				{
					FStateAnimation& Anim = Node.Animation;
					Read.String(Item, Where, TEXT("animation"), Anim.AssetPath);
					Read.String(Item, Where, TEXT("node_type"), Anim.NodeType);
					Read.String(Item, Where, TEXT("slot_name"), Anim.SlotName);
					Read.Bool(Item, Where, TEXT("loop"), Anim.bLoop);
					double PlayRate = 1.0;
					if (Read.Number(Item, Where, TEXT("play_rate"), PlayRate))
					{
						Anim.PlayRate = (float)PlayRate;
					}
					const bool bWantsSlot = Anim.NodeType.TrimStartAndEnd().Equals(TEXT("slot"), ESearchCase::IgnoreCase);
					if (!AnimMCP::IsUnset(Anim.AssetPath) || bWantsSlot)
					{
						Node.bHasAnimation = true;
						if (!ResolveStateAnimation(AnimBP, Anim, Error))
						{
							Problems.Add(FString::Printf(TEXT("%s: %s"), *Where, *Error));
						}
					}
					else if (!Anim.bLoop || Anim.PlayRate != 1.f || (!AnimMCP::IsUnset(Anim.NodeType) && !Anim.NodeType.Equals(TEXT("auto"), ESearchCase::IgnoreCase)))
					{
						Problems.Add(FString::Printf(TEXT("%s: 'node_type', 'loop' and 'play_rate' need an 'animation'."), *Where));
					}
				}
				Out.Nodes.Add(MoveTemp(Node));
			}
		};
		ReadNodes(TEXT("states"), /*bConduit*/ false);
		const int32 NumStates = Out.Nodes.Num();
		ReadNodes(TEXT("conduits"), /*bConduit*/ true);
		if (NumStates == 0)
		{
			Problems.Add(TEXT("spec: 'states' must contain at least one state."));
		}

		auto FindNodeIndex = [&Out](const FString& Name)
		{
			return Out.Nodes.IndexOfByPredicate([&Name](const FSpecNode& Node) { return Node.Name.Equals(Name, ESearchCase::IgnoreCase); });
		};

		FString EntryName;
		if (Read.String(Root, TEXT("spec"), TEXT("entry_state"), EntryName))
		{
			Out.EntryIndex = FindNodeIndex(EntryName);
			if (Out.EntryIndex == INDEX_NONE || Out.Nodes[Out.EntryIndex].bConduit)
			{
				Problems.Add(FString::Printf(TEXT("spec.entry_state: '%s' is not one of the states."), *EntryName));
				Out.EntryIndex = INDEX_NONE;
			}
		}
		else if (NumStates > 0)
		{
			Out.EntryIndex = 0;
		}

		// Transitions.
		const TArray<TSharedPtr<FJsonObject>> Transitions = Read.ObjectArray(Root, TEXT("transitions"));
		for (int32 Index = 0; Index < Transitions.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>& Item = Transitions[Index];
			const FString Where = FString::Printf(TEXT("transitions[%d]"), Index);
			Read.CheckKeys(Item, Where, { TEXT("from"), TEXT("to"), TEXT("crossfade_duration"), TEXT("rule"), TEXT("variable"), TEXT("variable_name"), TEXT("comparison"), TEXT("threshold"), TEXT("trigger_time") });

			FSpecTransition Transition;
			FString From, To;
			if (!Read.String(Item, Where, TEXT("from"), From) || !Read.String(Item, Where, TEXT("to"), To))
			{
				Problems.Add(FString::Printf(TEXT("%s: 'from' and 'to' are required."), *Where));
				continue;
			}
			Transition.From = FindNodeIndex(From);
			Transition.To = FindNodeIndex(To);
			if (Transition.From == INDEX_NONE)
			{
				Problems.Add(FString::Printf(TEXT("%s: 'from' state '%s' is not in the spec."), *Where, *From));
			}
			if (Transition.To == INDEX_NONE)
			{
				Problems.Add(FString::Printf(TEXT("%s: 'to' state '%s' is not in the spec."), *Where, *To));
			}

			double Crossfade = 0.2;
			if (Read.Number(Item, Where, TEXT("crossfade_duration"), Crossfade) && Crossfade < 0.0)
			{
				Problems.Add(FString::Printf(TEXT("%s: crossfade_duration must be >= 0."), *Where));
			}
			Transition.Crossfade = (float)Crossfade;

			FRuleRequest Request;
			if (Read.String(Item, Where, TEXT("rule"), Request.Rule))
			{
				if (!Read.String(Item, Where, TEXT("variable"), Request.VariableName))
				{
					Read.String(Item, Where, TEXT("variable_name"), Request.VariableName);
				}
				Read.String(Item, Where, TEXT("comparison"), Request.Comparison);
				Read.Number(Item, Where, TEXT("threshold"), Request.Threshold);
				double TriggerTime = -1.0;
				if (Read.Number(Item, Where, TEXT("trigger_time"), TriggerTime))
				{
					Request.TriggerTime = (float)TriggerTime;
				}
				if (ResolveRule(AnimBP, Request, Transition.Rule, Error, &Pending))
				{
					Transition.bHasRule = true;
				}
				else
				{
					Problems.Add(FString::Printf(TEXT("%s: %s"), *Where, *Error));
				}
			}
			Out.Transitions.Add(Transition);
		}

		if (!Problems.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Nothing was created. The spec has %d problem(s):\n- %s"), Problems.Num(), *FString::Join(Problems, TEXT("\n- ")));
			return false;
		}
		return true;
	}

	/** Creates everything the validated spec describes. The caller owns the transaction. */
	bool ApplyStateMachineSpec(UAnimBlueprint* AnimBP, const FStateMachineSpec& Spec, const TSharedRef<FJsonObject>& Payload, FString& OutError)
	{
		AnimBP->Modify();

		// Variables first, so rule getters can find them. Marking the blueprint structurally modified regenerates the skeleton class.
		TArray<FString> CreatedVariables;
		for (const FSpecVariable& Variable : Spec.NewVariables)
		{
			if (!FBlueprintEditorUtils::AddMemberVariable(AnimBP, Variable.Name, Variable.Type, Variable.Default))
			{
				OutError = FString::Printf(TEXT("Failed to add variable '%s'."), *Variable.Name.ToString());
				return false;
			}
			if (!AnimMCP::IsUnset(Variable.Category) && !Variable.Category.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
			{
				FBlueprintEditorUtils::SetBlueprintVariableCategory(AnimBP, Variable.Name, nullptr, FText::FromString(Variable.Category));
			}
			CreatedVariables.Add(Variable.Name.ToString());
		}
		if (!CreatedVariables.IsEmpty())
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
		}

		// State machine node.
		UEdGraphNode* MachineNode = AnimMCP::SpawnNode(Spec.Graph, UAnimGraphNode_StateMachine::StaticClass(), Spec.Position);
		MachineNode->OnRenameNode(Spec.Name);
		UAnimationStateMachineGraph* SMGraph = CastChecked<UAnimGraphNode_StateMachineBase>(MachineNode)->EditorStateMachineGraph;
		if (!SMGraph || !SMGraph->EntryNode)
		{
			OutError = TEXT("The new state machine has no graph or Entry node.");
			return false;
		}
		if (Spec.OutputPin)
		{
			UEdGraphPin* PosePin = AnimMCP::FindFirstPin(MachineNode, EGPD_Output);
			Spec.OutputPin->GetOwningNode()->Modify();
			if (!PosePin || !Spec.Graph->GetSchema()->TryCreateConnection(PosePin, Spec.OutputPin))
			{
				OutError = TEXT("Failed to connect the state machine to the Output Pose.");
				return false;
			}
		}

		// States and conduits, laid out on a grid unless the spec gives positions.
		TArray<UAnimStateNodeBase*> Created;
		TArray<TSharedRef<FJsonObject>> StateItems;
		TArray<TSharedRef<FJsonObject>> ConduitItems;
		for (int32 Index = 0; Index < Spec.Nodes.Num(); ++Index)
		{
			const FSpecNode& Node = Spec.Nodes[Index];
			const FVector2D Position = Node.Position.Get(FVector2D(300.0 + 350.0 * (Index % 4), 250.0 * (Index / 4)));
			UClass* NodeClass = Node.bConduit ? UAnimStateConduitNode::StaticClass() : UAnimStateNode::StaticClass();
			UAnimStateNodeBase* StateNode = CastChecked<UAnimStateNodeBase>(SpawnNamedStateNode(SMGraph, NodeClass, Node.Name, Position));
			Created.Add(StateNode);

			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), StateNode->GetStateName());
			Item->SetStringField(TEXT("node_guid"), AnimMCP::GuidToString(StateNode->NodeGuid));
			if (Node.bHasAnimation)
			{
				UAnimGraphNode_Base* PoseNode = nullptr;
				UAnimGraphNode_Base* Player = nullptr;
				if (!ApplyStateAnimation(AnimBP, CastChecked<UAnimStateNode>(StateNode), Node.Animation, PoseNode, Player, OutError))
				{
					OutError = FString::Printf(TEXT("State '%s': %s"), *Node.Name, *OutError);
					return false;
				}
				if (Player)
				{
					Item->SetStringField(TEXT("player_node_guid"), AnimMCP::GuidToString(Player->NodeGuid));
					Item->SetStringField(TEXT("animation"), Node.Animation.Asset->GetPathName());
				}
				if (Node.Animation.bSlot)
				{
					Item->SetStringField(TEXT("slot_node_guid"), AnimMCP::GuidToString(PoseNode->NodeGuid));
				}
			}
			(Node.bConduit ? ConduitItems : StateItems).Add(Item);
		}

		if (Spec.EntryIndex != INDEX_NONE)
		{
			UEdGraphPin* EntryPin = SMGraph->EntryNode->GetOutputPin();
			SMGraph->EntryNode->Modify();
			if (!SMGraph->GetSchema()->TryCreateConnection(EntryPin, Created[Spec.EntryIndex]->GetInputPin()))
			{
				OutError = FString::Printf(TEXT("Failed to wire Entry to state '%s'."), *Spec.Nodes[Spec.EntryIndex].Name);
				return false;
			}
		}

		TArray<TSharedRef<FJsonObject>> TransitionItems;
		for (const FSpecTransition& SpecTransition : Spec.Transitions)
		{
			UAnimStateNodeBase* From = Created[SpecTransition.From];
			UAnimStateNodeBase* To = Created[SpecTransition.To];
			const FVector2D Midpoint((From->NodePosX + To->NodePosX) * 0.5, (From->NodePosY + To->NodePosY) * 0.5);
			UAnimStateTransitionNode* Transition = CastChecked<UAnimStateTransitionNode>(AnimMCP::SpawnNode(SMGraph, UAnimStateTransitionNode::StaticClass(), Midpoint));
			Transition->CreateConnections(From, To);
			Transition->CrossfadeDuration = SpecTransition.Crossfade;

			TArray<UEdGraphNode*> RuleNodes;
			if (SpecTransition.bHasRule && !ApplyRule(AnimBP, Transition, SpecTransition.Rule, RuleNodes, OutError))
			{
				OutError = FString::Printf(TEXT("Transition %s -> %s: %s"), *From->GetStateName(), *To->GetStateName(), *OutError);
				return false;
			}

			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("from"), From->GetStateName());
			Item->SetStringField(TEXT("to"), To->GetStateName());
			Item->SetStringField(TEXT("node_guid"), AnimMCP::GuidToString(Transition->NodeGuid));
			Item->SetStringField(TEXT("rule"), SpecTransition.bHasRule ? RuleKindToString(SpecTransition.Rule.Kind) : FString(TEXT("never")));
			Item->SetArrayField(TEXT("rule_nodes"), NodeGuidArray(RuleNodes));
			TransitionItems.Add(Item);
		}

		Payload->SetObjectField(TEXT("state_machine"), AnimMCP::NodeToJson(MachineNode, /*bIncludePins*/ false));
		Payload->SetStringField(TEXT("state_machine_graph_guid"), AnimMCP::GuidToString(SMGraph->GraphGuid));
		Payload->SetStringField(TEXT("entry_node_guid"), AnimMCP::GuidToString(SMGraph->EntryNode->NodeGuid));
		Payload->SetBoolField(TEXT("connected_to_output"), Spec.OutputPin != nullptr);
		Payload->SetArrayField(TEXT("variables_created"), AnimMCP::ToJsonArray(CreatedVariables));
		Payload->SetArrayField(TEXT("variables_reused"), AnimMCP::ToJsonArray(Spec.ReusedVariables));
		Payload->SetArrayField(TEXT("states"), AnimMCP::ToJsonArray(StateItems));
		Payload->SetArrayField(TEXT("conduits"), AnimMCP::ToJsonArray(ConduitItems));
		Payload->SetArrayField(TEXT("transitions"), AnimMCP::ToJsonArray(TransitionItems));
		return true;
	}
}

FAnimMCPResult UAnimStateMachineToolset::anim_build_state_machine(const FString& blueprint_path, const FString& spec)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}

	FStateMachineSpec Spec;
	if (!ParseStateMachineSpec(AnimBP, spec, Spec, Error))
	{
		return AnimMCP::Fail(Error);
	}

	// Rolling back with undo is only safe when this is the outermost transaction.
	const bool bCanRollBack = GEditor && !GUndo;
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	bool bApplied = false;
	{
		const FScopedTransaction Transaction(LOCTEXT("BuildStateMachine", "AnimMCP: Build State Machine"));
		bApplied = ApplyStateMachineSpec(AnimBP, Spec, Payload, Error);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	}
	if (!bApplied)
	{
		const bool bRolledBack = bCanRollBack && GEditor->UndoTransaction(/*bCanRedo*/ false);
		return AnimMCP::Fail(FString::Printf(TEXT("Building the state machine failed: %s %s"), *Error,
			bRolledBack ? TEXT("All changes were rolled back.") : TEXT("Partial changes remain; press Ctrl+Z in the editor to undo them.")));
	}

	Payload->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
	return AnimMCP::Ok(Payload);
}

#undef LOCTEXT_NAMESPACE
