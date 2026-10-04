// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#include "AnimAssetToolset.h"

#include "AnimMCPHelpers.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateConduitNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateMachineGraph.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/NavMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "EdGraphToken.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Factories/BlendSpaceFactory1D.h"
#include "Factories/BlendSpaceFactoryNew.h"
#include "FileHelpers.h"
#include "IAssetTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/StringOutputDevice.h"
#include "ObjectTools.h"
#include "Misc/UObjectToken.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "AnimMCPAssets"

namespace
{
	UClass* ResolveAnimInstanceClass(const FString& Name, FString& OutError)
	{
		if (AnimMCP::IsUnset(Name))
		{
			return UAnimInstance::StaticClass();
		}

		UClass* Class = nullptr;
		if (Name.StartsWith(TEXT("/Script/")))
		{
			Class = FindObject<UClass>(nullptr, *Name);
		}
		else if (Name.StartsWith(TEXT("/")))
		{
			// An Animation Blueprint (or its generated class) used as parent.
			FString Ignored;
			if (UAnimBlueprint* ParentBP = AnimMCP::LoadAsset<UAnimBlueprint>(Name, /*bForWrite*/ false, Ignored))
			{
				Class = ParentBP->GeneratedClass;
			}
			else
			{
				Class = LoadObject<UClass>(nullptr, *Name);
			}
		}
		else
		{
			Class = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst);
			if (!Class && Name.StartsWith(TEXT("U")))
			{
				Class = FindFirstObject<UClass>(*Name.RightChop(1), EFindFirstObjectOptions::NativeFirst);
			}
		}

		if (!Class || !Class->IsChildOf(UAnimInstance::StaticClass()))
		{
			OutError = FString::Printf(TEXT("'%s' is not an AnimInstance class or Animation Blueprint."), *Name);
			return nullptr;
		}
		return Class;
	}

	UClass* GetSearchClass(UBlueprint* Blueprint)
	{
		return Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->GeneratedClass.Get();
	}

	/** Fills state machine context (state machine, state, transition) for a node by walking up its graphs. */
	void AddGraphContext(const UEdGraphNode* Node, const TSharedRef<FJsonObject>& Json, TArray<FString>& Path)
	{
		auto Describe = [&Json, &Path](const UEdGraphNode* Owner)
		{
			if (const UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Owner))
			{
				const FString Label = FString::Printf(TEXT("%s -> %s"),
					Transition->GetPreviousState() ? *Transition->GetPreviousState()->GetStateName() : TEXT("?"),
					Transition->GetNextState() ? *Transition->GetNextState()->GetStateName() : TEXT("?"));
				if (!Json->HasField(TEXT("transition")))
				{
					Json->SetStringField(TEXT("transition"), Label);
					Json->SetStringField(TEXT("transition_guid"), AnimMCP::GuidToString(Transition->NodeGuid));
				}
				Path.Insert(TEXT("transition ") + Label, 0);
			}
			else if (const UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(Owner))
			{
				if (!Json->HasField(TEXT("state")))
				{
					Json->SetStringField(TEXT("state"), State->GetStateName());
					Json->SetStringField(TEXT("state_guid"), AnimMCP::GuidToString(State->NodeGuid));
				}
				Path.Insert(FString::Printf(TEXT("%s %s"), State->IsA<UAnimStateConduitNode>() ? TEXT("conduit") : TEXT("state"), *State->GetStateName()), 0);
			}
			else if (const UAnimGraphNode_StateMachineBase* Machine = Cast<UAnimGraphNode_StateMachineBase>(Owner))
			{
				const FString Name = Machine->EditorStateMachineGraph ? Machine->EditorStateMachineGraph->GetName() : Machine->GetName();
				if (!Json->HasField(TEXT("state_machine")))
				{
					Json->SetStringField(TEXT("state_machine"), Name);
					Json->SetStringField(TEXT("state_machine_guid"), AnimMCP::GuidToString(Machine->NodeGuid));
				}
				Path.Insert(TEXT("state machine ") + Name, 0);
			}
		};

		// The node itself may be a state, conduit or transition (e.g. "will never be taken").
		Describe(Node);
		const UEdGraph* Graph = Node->GetGraph();
		while (Graph)
		{
			const UEdGraphNode* Owner = Cast<UEdGraphNode>(Graph->GetOuter());
			if (!Owner)
			{
				Path.Insert(Graph->GetName(), 0);
				break;
			}
			Describe(Owner);
			Graph = Owner->GetGraph();
		}
	}

	/** The node (and pin) a compiler message points at, with its place in the blueprint. Null if the message names no node. */
	TSharedPtr<FJsonObject> DescribeMessageSource(const FTokenizedMessage& Message)
	{
		for (const TSharedRef<IMessageToken>& Token : Message.GetMessageTokens())
		{
			const UObject* Object = nullptr;
			const UEdGraphPin* Pin = nullptr;
			if (Token->GetType() == EMessageToken::EdGraph)
			{
				const FEdGraphToken& GraphToken = static_cast<const FEdGraphToken&>(Token.Get());
				Object = GraphToken.GetGraphObject();
				Pin = GraphToken.GetPin();
				if (!Object && Pin)
				{
					Object = Pin->GetOwningNodeUnchecked();
				}
			}
			else if (Token->GetType() == EMessageToken::Object)
			{
				Object = static_cast<const FUObjectToken&>(Token.Get()).GetObject().Get();
			}

			const UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
			if (!Node)
			{
				continue;
			}
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("node_guid"), AnimMCP::GuidToString(Node->NodeGuid));
			Json->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			Json->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
			if (const UEdGraph* Graph = Node->GetGraph())
			{
				Json->SetStringField(TEXT("graph"), Graph->GetName());
				Json->SetStringField(TEXT("graph_guid"), AnimMCP::GuidToString(Graph->GraphGuid));
			}
			if (Pin)
			{
				Json->SetStringField(TEXT("pin"), Pin->PinName.ToString());
			}
			TArray<FString> Path;
			AddGraphContext(Node, Json, Path);
			if (!Node->IsA<UAnimStateNodeBase>())
			{
				Path.Add(Json->GetStringField(TEXT("node_title")));
			}
			Json->SetStringField(TEXT("location"), FString::Join(Path, TEXT(" > ")));
			return Json;
		}
		return nullptr;
	}

	/** The result of compiling a blueprint: status, counts and every message with the node it points at. */
	struct FCompileReport
	{
		FString Status;
		int32 NumErrors = 0;
		int32 NumWarnings = 0;
		TArray<TSharedRef<FJsonObject>> Messages;

		TSharedRef<FJsonObject> ToJson() const
		{
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("status"), Status);
			Json->SetNumberField(TEXT("num_errors"), NumErrors);
			Json->SetNumberField(TEXT("num_warnings"), NumWarnings);
			Json->SetArrayField(TEXT("messages"), AnimMCP::ToJsonArray(Messages));
			return Json;
		}

		/** Messages in this report whose severity and text do not appear in Other. */
		TArray<TSharedRef<FJsonObject>> NotIn(const FCompileReport& Other) const
		{
			TArray<TSharedRef<FJsonObject>> Result;
			for (const TSharedRef<FJsonObject>& Message : Messages)
			{
				const bool bFound = Other.Messages.ContainsByPredicate([&Message](const TSharedRef<FJsonObject>& Candidate)
				{
					return Candidate->GetStringField(TEXT("severity")) == Message->GetStringField(TEXT("severity"))
						&& Candidate->GetStringField(TEXT("message")) == Message->GetStringField(TEXT("message"));
				});
				if (!bFound)
				{
					Result.Add(Message);
				}
			}
			return Result;
		}
	};

	FCompileReport CompileAndReport(UBlueprint* Blueprint)
	{
		FCompilerResultsLog Results;
		Results.SetSourcePath(Blueprint->GetPathName());
		Results.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

		FCompileReport Report;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("severity"), FTokenizedMessage::GetSeverityText(Message->GetSeverity()).ToString());
			Item->SetStringField(TEXT("message"), Message->ToText().ToString());
			if (const TSharedPtr<FJsonObject> Source = DescribeMessageSource(*Message))
			{
				Item->SetObjectField(TEXT("source"), Source);
			}
			Report.Messages.Add(Item);
		}
		switch (Blueprint->Status)
		{
		case BS_UpToDate:             Report.Status = TEXT("up_to_date"); break;
		case BS_UpToDateWithWarnings: Report.Status = TEXT("up_to_date_with_warnings"); break;
		case BS_Error:                Report.Status = TEXT("error"); break;
		case BS_Dirty:                Report.Status = TEXT("dirty"); break;
		default:                      Report.Status = TEXT("unknown"); break;
		}
		Report.NumErrors = Results.NumErrors;
		Report.NumWarnings = Results.NumWarnings;
		return Report;
	}

	// ---- Locomotion starter variables ----------------------------------------------------

	const TCHAR* const LocomotionVariableNames[] = { TEXT("Speed"), TEXT("IsMoving"), TEXT("IsFalling") };

	/** Speed below which IsMoving is false, in cm/s. Small enough for slow walks, large enough to ignore jitter. */
	constexpr double IsMovingThreshold = 3.0;

	/** The parent class must not already define the starter variables. */
	bool CheckLocomotionVariables(const UClass* ParentClass, FString& OutError)
	{
		for (const TCHAR* Name : LocomotionVariableNames)
		{
			if (ParentClass && FindFProperty<FProperty>(ParentClass, Name))
			{
				OutError = FString::Printf(TEXT("add_locomotion_vars: the parent class %s already has a property named '%s'. Create the blueprint without add_locomotion_vars and use the inherited variables."), *ParentClass->GetName(), Name);
				return false;
			}
		}
		return true;
	}

	/** Turns the editor's disabled placeholder nodes (Event Blueprint Update Animation, Try Get Pawn Owner) into live nodes. */
	void EnableGhostNode(UEdGraphNode* Node)
	{
		if (Node->IsAutomaticallyPlacedGhostNode())
		{
			Node->Modify();
			Node->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction*/ false);
			Node->NodeComment.Empty();
		}
	}

	/**
	 * Adds Speed, IsMoving and IsFalling and fills them every frame from the owning pawn:
	 *   Event Blueprint Update Animation -> Branch(IsValid(TryGetPawnOwner))
	 *     -> Speed = VSizeXY(Pawn.GetVelocity) -> IsMoving = Speed > threshold
	 *     -> Branch(IsValid(Pawn.GetMovementComponent)) -> IsFalling = MovementComponent.IsFalling
	 * Only APawn / UNavMovementComponent API is used, so any pawn works, not just Characters. The caller owns the transaction.
	 */
	bool AddLocomotionSetup(UAnimBlueprint* AnimBP, TArray<FString>& OutNodeGuids, FString& OutError)
	{
		FEdGraphPinType FloatType;
		FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
		FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		FEdGraphPinType BoolType;
		BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;

		AnimBP->Modify();
		for (const TCHAR* Name : LocomotionVariableNames)
		{
			const bool bIsSpeed = FCString::Strcmp(Name, TEXT("Speed")) == 0;
			if (!FBlueprintEditorUtils::AddMemberVariable(AnimBP, Name, bIsSpeed ? FloatType : BoolType, bIsSpeed ? TEXT("0.0") : TEXT("false")))
			{
				OutError = FString::Printf(TEXT("Failed to add variable '%s'."), Name);
				return false;
			}
			FBlueprintEditorUtils::SetBlueprintVariableCategory(AnimBP, Name, nullptr, FText::FromString(TEXT("Locomotion")));
		}
		// Regenerates the skeleton class so the getters and setters below get their value pins.
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

		UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(AnimBP);
		if (!EventGraph)
		{
			OutError = TEXT("The blueprint has no EventGraph.");
			return false;
		}
		const UEdGraphSchema* Schema = EventGraph->GetSchema();

		const FName UpdateEventName = GET_FUNCTION_NAME_CHECKED(UAnimInstance, BlueprintUpdateAnimation);
		UK2Node_Event* Event = FBlueprintEditorUtils::FindOverrideForFunction(AnimBP, UAnimInstance::StaticClass(), UpdateEventName);
		if (!Event)
		{
			int32 NodePosY = 0;
			Event = FKismetEditorUtilities::AddDefaultEventNode(AnimBP, EventGraph, UpdateEventName, UAnimInstance::StaticClass(), NodePosY);
		}
		if (!Event || Event->GetGraph() != EventGraph)
		{
			OutError = TEXT("Could not find or create Event Blueprint Update Animation in the EventGraph.");
			return false;
		}
		EnableGhostNode(Event);
		UEdGraphPin* EventThen = Event->FindPin(UEdGraphSchema_K2::PN_Then);
		if (!EventThen || !EventThen->LinkedTo.IsEmpty())
		{
			OutError = TEXT("Event Blueprint Update Animation is already wired; add_locomotion_vars only sets up a fresh blueprint.");
			return false;
		}

		const FVector2D Origin(Event->NodePosX, Event->NodePosY);
		TArray<UEdGraphNode*> Created;
		auto Call = [&](UClass* Owner, FName FunctionName, const FVector2D& Offset) -> UEdGraphNode*
		{
			UFunction* Function = Owner->FindFunctionByName(FunctionName);
			if (!Function)
			{
				return nullptr;
			}
			UEdGraphNode* Node = AnimMCP::SpawnNode(EventGraph, UK2Node_CallFunction::StaticClass(), Origin + Offset, [Function](UEdGraphNode* NewNode)
			{
				CastChecked<UK2Node_CallFunction>(NewNode)->SetFromFunction(Function);
			});
			Created.Add(Node);
			return Node;
		};
		auto Variable = [&](UClass* NodeClass, const TCHAR* Name, const FVector2D& Offset) -> UEdGraphNode*
		{
			UEdGraphNode* Node = AnimMCP::SpawnNode(EventGraph, NodeClass, Origin + Offset, [Name](UEdGraphNode* NewNode)
			{
				CastChecked<UK2Node_Variable>(NewNode)->VariableReference.SetSelfMember(Name);
			});
			Created.Add(Node);
			return Node;
		};
		auto Branch = [&](const FVector2D& Offset) -> UK2Node_IfThenElse*
		{
			UEdGraphNode* Node = AnimMCP::SpawnNode(EventGraph, UK2Node_IfThenElse::StaticClass(), Origin + Offset);
			Created.Add(Node);
			return CastChecked<UK2Node_IfThenElse>(Node);
		};

		// Reuse the editor's placeholder Try Get Pawn Owner if it is there.
		UEdGraphNode* PawnNode = nullptr;
		const FName TryGetPawnOwnerName = GET_FUNCTION_NAME_CHECKED(UAnimInstance, TryGetPawnOwner);
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
			if (CallNode && CallNode->GetTargetFunction() && CallNode->GetTargetFunction()->GetFName() == TryGetPawnOwnerName)
			{
				PawnNode = Node;
				EnableGhostNode(Node);
				break;
			}
		}
		if (!PawnNode)
		{
			PawnNode = Call(UAnimInstance::StaticClass(), TryGetPawnOwnerName, FVector2D(0.0, 200.0));
		}

		UEdGraphNode* PawnValid = Call(UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, IsValid), FVector2D(250.0, 200.0));
		UK2Node_IfThenElse* PawnBranch = Branch(FVector2D(450.0, 0.0));
		UEdGraphNode* Velocity = Call(AActor::StaticClass(), GET_FUNCTION_NAME_CHECKED(AActor, GetVelocity), FVector2D(250.0, 350.0));
		UEdGraphNode* SpeedXY = Call(UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, VSizeXY), FVector2D(500.0, 350.0));
		UEdGraphNode* SetSpeed = Variable(UK2Node_VariableSet::StaticClass(), TEXT("Speed"), FVector2D(700.0, 0.0));
		UEdGraphNode* GetSpeed = Variable(UK2Node_VariableGet::StaticClass(), TEXT("Speed"), FVector2D(750.0, 250.0));
		UEdGraphNode* Greater = Call(UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Greater_DoubleDouble), FVector2D(950.0, 250.0));
		UEdGraphNode* SetMoving = Variable(UK2Node_VariableSet::StaticClass(), TEXT("IsMoving"), FVector2D(1150.0, 0.0));
		UEdGraphNode* Movement = Call(APawn::StaticClass(), GET_FUNCTION_NAME_CHECKED(APawn, GetMovementComponent), FVector2D(1150.0, 350.0));
		UEdGraphNode* MovementValid = Call(UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, IsValid), FVector2D(1450.0, 250.0));
		UK2Node_IfThenElse* MovementBranch = Branch(FVector2D(1650.0, 0.0));
		UEdGraphNode* Falling = Call(UNavMovementComponent::StaticClass(), GET_FUNCTION_NAME_CHECKED(UNavMovementComponent, IsFalling), FVector2D(1650.0, 350.0));
		UEdGraphNode* SetFalling = Variable(UK2Node_VariableSet::StaticClass(), TEXT("IsFalling"), FVector2D(1900.0, 0.0));
		for (const UEdGraphNode* Node : Created)
		{
			if (!Node)
			{
				OutError = TEXT("A function used by the locomotion setup was not found in this engine version.");
				return false;
			}
		}

		// Every link is checked so a failure names exactly what could not be wired.
		struct FLink { UEdGraphNode* From; FName FromPin; UEdGraphNode* To; FName ToPin; };
		const FLink Links[] =
		{
			{ Event, UEdGraphSchema_K2::PN_Then, PawnBranch, UEdGraphSchema_K2::PN_Execute },
			{ PawnNode, UEdGraphSchema_K2::PN_ReturnValue, PawnValid, TEXT("Object") },
			{ PawnValid, UEdGraphSchema_K2::PN_ReturnValue, PawnBranch, UEdGraphSchema_K2::PN_Condition },
			{ PawnBranch, UEdGraphSchema_K2::PN_Then, SetSpeed, UEdGraphSchema_K2::PN_Execute },
			{ PawnNode, UEdGraphSchema_K2::PN_ReturnValue, Velocity, UEdGraphSchema_K2::PN_Self },
			{ Velocity, UEdGraphSchema_K2::PN_ReturnValue, SpeedXY, TEXT("A") },
			{ SpeedXY, UEdGraphSchema_K2::PN_ReturnValue, SetSpeed, TEXT("Speed") },
			{ SetSpeed, UEdGraphSchema_K2::PN_Then, SetMoving, UEdGraphSchema_K2::PN_Execute },
			{ GetSpeed, TEXT("Speed"), Greater, TEXT("A") },
			{ Greater, UEdGraphSchema_K2::PN_ReturnValue, SetMoving, TEXT("IsMoving") },
			{ SetMoving, UEdGraphSchema_K2::PN_Then, MovementBranch, UEdGraphSchema_K2::PN_Execute },
			{ PawnNode, UEdGraphSchema_K2::PN_ReturnValue, Movement, UEdGraphSchema_K2::PN_Self },
			{ Movement, UEdGraphSchema_K2::PN_ReturnValue, MovementValid, TEXT("Object") },
			{ MovementValid, UEdGraphSchema_K2::PN_ReturnValue, MovementBranch, UEdGraphSchema_K2::PN_Condition },
			{ MovementBranch, UEdGraphSchema_K2::PN_Then, SetFalling, UEdGraphSchema_K2::PN_Execute },
			{ Movement, UEdGraphSchema_K2::PN_ReturnValue, Falling, UEdGraphSchema_K2::PN_Self },
			{ Falling, UEdGraphSchema_K2::PN_ReturnValue, SetFalling, TEXT("IsFalling") },
		};
		for (const FLink& Link : Links)
		{
			UEdGraphPin* FromPin = Link.From->FindPin(Link.FromPin, EGPD_Output);
			UEdGraphPin* ToPin = Link.To->FindPin(Link.ToPin, EGPD_Input);
			if (!FromPin || !ToPin || !Schema->TryCreateConnection(FromPin, ToPin))
			{
				OutError = FString::Printf(TEXT("Failed to wire %s.%s -> %s.%s in the EventGraph."),
					*Link.From->GetNodeTitle(ENodeTitleType::ListView).ToString(), *Link.FromPin.ToString(),
					*Link.To->GetNodeTitle(ENodeTitleType::ListView).ToString(), *Link.ToPin.ToString());
				return false;
			}
		}
		if (UEdGraphPin* Threshold = Greater->FindPin(TEXT("B"), EGPD_Input))
		{
			Schema->TrySetDefaultValue(*Threshold, FString::SanitizeFloat(IsMovingThreshold));
		}

		OutNodeGuids.Add(AnimMCP::GuidToString(Event->NodeGuid));
		OutNodeGuids.Add(AnimMCP::GuidToString(PawnNode->NodeGuid));
		for (const UEdGraphNode* Node : Created)
		{
			if (Node != PawnNode)
			{
				OutNodeGuids.Add(AnimMCP::GuidToString(Node->NodeGuid));
			}
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
		return true;
	}

	UObject* CreateAssetWithFactory(const FString& Folder, const FString& AssetName, UClass* AssetClass, UFactory* Factory)
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		FString CleanFolder = Folder.TrimStartAndEnd();
		while (CleanFolder.EndsWith(TEXT("/")))
		{
			CleanFolder.LeftChopInline(1);
		}
		return AssetTools.CreateAsset(AssetName, CleanFolder, AssetClass, Factory, NAME_None, /*bOverwriteExisting*/ false);
	}

	bool SetBlendParameter(UBlendSpace* BlendSpace, int32 Index, const FString& Name, float Min, float Max, int32 Grid, FString& OutError)
	{
		if (Max <= Min)
		{
			OutError = FString::Printf(TEXT("Axis '%s': max (%g) must be greater than min (%g)."), *Name, Max, Min);
			return false;
		}
		if (Grid < 1)
		{
			OutError = FString::Printf(TEXT("Axis '%s': grid divisions must be >= 1."), *Name);
			return false;
		}

		// BlendParameters is protected; reach it through reflection like the details panel does.
		FStructProperty* Property = FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), TEXT("BlendParameters"));
		if (!Property || Property->Struct != FBlendParameter::StaticStruct() || Index >= Property->ArrayDim)
		{
			OutError = TEXT("Could not access UBlendSpace::BlendParameters in this engine version.");
			return false;
		}
		FBlendParameter* Parameter = Property->ContainerPtrToValuePtr<FBlendParameter>(BlendSpace, Index);
		Parameter->DisplayName = Name;
		Parameter->Min = Min;
		Parameter->Max = Max;
		Parameter->GridNum = Grid;
		return true;
	}
}

FAnimMCPResult UAnimAssetToolset::anim_create_anim_blueprint(const FString& folder, const FString& asset_name, const FString& skeleton_path, const FString& parent_class, const FString& preview_mesh_path, bool add_locomotion_vars)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	FString PackageName;
	if (!AnimMCP::ValidateNewAssetLocation(folder, asset_name, PackageName, Error))
	{
		return AnimMCP::Fail(Error);
	}
	USkeleton* Skeleton = AnimMCP::ResolveSkeleton(skeleton_path, Error);
	if (!Skeleton)
	{
		return AnimMCP::Fail(Error);
	}
	UClass* ParentClass = ResolveAnimInstanceClass(parent_class, Error);
	if (!ParentClass)
	{
		return AnimMCP::Fail(Error);
	}
	USkeletalMesh* PreviewMesh = nullptr;
	if (!AnimMCP::IsUnset(preview_mesh_path))
	{
		PreviewMesh = AnimMCP::LoadAsset<USkeletalMesh>(preview_mesh_path, /*bForWrite*/ false, Error);
		if (!PreviewMesh)
		{
			return AnimMCP::Fail(Error);
		}
		if (PreviewMesh->GetSkeleton() && !Skeleton->IsCompatibleForEditor(PreviewMesh->GetSkeleton()))
		{
			return AnimMCP::Fail(TEXT("Preview mesh uses a skeleton that is not compatible with the target skeleton."));
		}
	}

	if (add_locomotion_vars && !CheckLocomotionVariables(ParentClass, Error))
	{
		return AnimMCP::Fail(Error);
	}

	UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
	Factory->BlueprintType = BPTYPE_Normal;
	Factory->ParentClass = ParentClass;
	Factory->TargetSkeleton = Skeleton;
	Factory->PreviewSkeletalMesh = PreviewMesh;

	const FScopedTransaction Transaction(LOCTEXT("CreateAnimBP", "AnimMCP: Create Animation Blueprint"));
	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(CreateAssetWithFactory(folder, asset_name, UAnimBlueprint::StaticClass(), Factory));
	if (!AnimBP)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Failed to create Animation Blueprint '%s'."), *PackageName));
	}
	AnimBP->MarkPackageDirty();

	TArray<FString> LocomotionNodes;
	if (add_locomotion_vars && !AddLocomotionSetup(AnimBP, LocomotionNodes, Error))
	{
		// The asset exists in memory but is unsaved; it is not deleted (assets are never deleted by this plugin).
		return AnimMCP::Fail(FString::Printf(TEXT("Animation Blueprint created at '%s' but the locomotion setup failed: %s Ctrl+Z undoes the partial setup."), *AnimBP->GetPathName(), *Error));
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("path"), AnimBP->GetPathName());
	Payload->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Payload->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
	Payload->SetBoolField(TEXT("saved"), false);
	if (add_locomotion_vars)
	{
		TArray<FString> Names;
		for (const TCHAR* Name : LocomotionVariableNames)
		{
			Names.Add(Name);
		}
		Payload->SetArrayField(TEXT("locomotion_variables"), AnimMCP::ToJsonArray(Names));
		Payload->SetArrayField(TEXT("locomotion_nodes"), AnimMCP::ToJsonArray(LocomotionNodes));
	}
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimAssetToolset::anim_add_variable(const FString& blueprint_path, const FString& name, const FString& type, const FString& in_default_value, const FString& variable_category)
{
	ANIMMCP_REQUIRE_GAME_THREAD();
	const FString default_value = AnimMCP::IsUnset(in_default_value) ? FString() : in_default_value;

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}

	FText NameError;
	if (name.IsEmpty() || !FName::IsValidXName(name, INVALID_OBJECTNAME_CHARACTERS, &NameError))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("'%s' is not a valid variable name. %s"), *name, *NameError.ToString()));
	}
	const FName VarName(*name);
	UClass* SearchClass = GetSearchClass(AnimBP);
	if (FBlueprintEditorUtils::FindNewVariableIndex(AnimBP, VarName) != INDEX_NONE || (SearchClass && FindFProperty<FProperty>(SearchClass, VarName)))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("A variable or property named '%s' already exists."), *name));
	}

	FEdGraphPinType PinType;
	if (!AnimMCP::ParsePinType(type, PinType, Error))
	{
		return AnimMCP::Fail(Error);
	}
	if (!AnimMCP::ValidateDefaultValue(PinType, VarName, default_value, Error))
	{
		return AnimMCP::Fail(Error);
	}

	const FScopedTransaction Transaction(LOCTEXT("AddVariable", "AnimMCP: Add Variable"));
	AnimBP->Modify();
	if (!FBlueprintEditorUtils::AddMemberVariable(AnimBP, VarName, PinType, default_value))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Failed to add variable '%s'."), *name));
	}
	if (!AnimMCP::IsUnset(variable_category) && !variable_category.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
	{
		FBlueprintEditorUtils::SetBlueprintVariableCategory(AnimBP, VarName, nullptr, FText::FromString(variable_category));
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("name"), name);
	Payload->SetStringField(TEXT("type"), AnimMCP::PinTypeToString(PinType));
	Payload->SetStringField(TEXT("default_value"), default_value);
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimAssetToolset::anim_remove_variable(const FString& blueprint_path, const FString& name, bool force)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	const FName VarName(*name);
	if (FBlueprintEditorUtils::FindNewVariableIndex(AnimBP, VarName) == INDEX_NONE)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("'%s' is not a variable declared on this blueprint (inherited variables cannot be removed here)."), *name));
	}

	const bool bUsed = FBlueprintEditorUtils::IsVariableUsed(AnimBP, VarName);
	if (bUsed && !force)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Variable '%s' is still used by graph nodes. Remove those nodes first, or pass force=true."), *name));
	}

	const FScopedTransaction Transaction(LOCTEXT("RemoveVariable", "AnimMCP: Remove Variable"));
	AnimBP->Modify();
	FBlueprintEditorUtils::RemoveMemberVariable(AnimBP, VarName);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetBoolField(TEXT("removed"), true);
	Payload->SetBoolField(TEXT("was_used"), bUsed);
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimAssetToolset::anim_set_variable_default(const FString& blueprint_path, const FString& name, const FString& value)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	const FName VarName(*name);
	const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(AnimBP, VarName);
	if (VarIndex == INDEX_NONE)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("'%s' is not a variable declared on this blueprint."), *name));
	}
	FBPVariableDescription& Variable = AnimBP->NewVariables[VarIndex];
	if (!AnimMCP::ValidateDefaultValue(Variable.VarType, VarName, value, Error))
	{
		return AnimMCP::Fail(Error);
	}

	// After compilation the class default object is authoritative, so update it as well.
	UObject* CDO = AnimBP->GeneratedClass ? AnimBP->GeneratedClass->GetDefaultObject(false) : nullptr;
	FProperty* Property = CDO ? AnimBP->GeneratedClass->FindPropertyByName(VarName) : nullptr;

	const FScopedTransaction Transaction(LOCTEXT("SetVariableDefault", "AnimMCP: Set Variable Default"));
	AnimBP->Modify();
	Variable.DefaultValue = value;
	if (Property)
	{
		FStringOutputDevice ImportErrors;
		CDO->Modify();
		Property->ImportText_InContainer(*value, CDO, CDO, PPF_None, &ImportErrors);
		if (!ImportErrors.IsEmpty())
		{
			UE_LOG(LogAnimMCP, Warning, TEXT("Default for '%s' stored, but applying it to the class default object reported: %s"), *name, *ImportErrors);
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("name"), name);
	Payload->SetStringField(TEXT("default_value"), value);
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimAssetToolset::anim_create_blendspace(const FString& folder, const FString& asset_name, const FString& skeleton_path, int32 dimensions,
	const FString& x_axis_name, float x_min, float x_max, int32 x_grid,
	const FString& y_axis_name, float y_min, float y_max, int32 y_grid)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	if (dimensions != 1 && dimensions != 2)
	{
		return AnimMCP::Fail(TEXT("dimensions must be 1 or 2."));
	}

	FString Error;
	FString PackageName;
	if (!AnimMCP::ValidateNewAssetLocation(folder, asset_name, PackageName, Error))
	{
		return AnimMCP::Fail(Error);
	}
	USkeleton* Skeleton = AnimMCP::ResolveSkeleton(skeleton_path, Error);
	if (!Skeleton)
	{
		return AnimMCP::Fail(Error);
	}
	if (x_max <= x_min || x_grid < 1 || (dimensions == 2 && (y_max <= y_min || y_grid < 1)))
	{
		return AnimMCP::Fail(TEXT("Each axis needs max > min and grid >= 1."));
	}

	UFactory* Factory = nullptr;
	UClass* AssetClass = nullptr;
	if (dimensions == 1)
	{
		UBlendSpaceFactory1D* Factory1D = NewObject<UBlendSpaceFactory1D>();
		Factory1D->TargetSkeleton = Skeleton;
		Factory = Factory1D;
		AssetClass = UBlendSpace1D::StaticClass();
	}
	else
	{
		UBlendSpaceFactoryNew* Factory2D = NewObject<UBlendSpaceFactoryNew>();
		Factory2D->TargetSkeleton = Skeleton;
		Factory = Factory2D;
		AssetClass = UBlendSpace::StaticClass();
	}

	const FScopedTransaction Transaction(LOCTEXT("CreateBlendSpace", "AnimMCP: Create Blend Space"));
	UBlendSpace* BlendSpace = Cast<UBlendSpace>(CreateAssetWithFactory(folder, asset_name, AssetClass, Factory));
	if (!BlendSpace)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Failed to create blend space '%s'."), *PackageName));
	}

	BlendSpace->Modify();
	if (!SetBlendParameter(BlendSpace, 0, x_axis_name, x_min, x_max, x_grid, Error)
		|| (dimensions == 2 && !SetBlendParameter(BlendSpace, 1, y_axis_name, y_min, y_max, y_grid, Error)))
	{
		// The asset exists in memory but is unsaved; it is not deleted (assets are never deleted by this plugin).
		return AnimMCP::Fail(FString::Printf(TEXT("Blend space created at '%s' but axis setup failed: %s"), *BlendSpace->GetPathName(), *Error));
	}
	BlendSpace->PostEditChange();
	BlendSpace->MarkPackageDirty();

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("path"), BlendSpace->GetPathName());
	Payload->SetStringField(TEXT("class"), BlendSpace->GetClass()->GetName());
	Payload->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Payload->SetBoolField(TEXT("saved"), false);
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimAssetToolset::anim_add_blendspace_sample(const FString& blendspace_path, const FString& animation_path, float x, float y)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UBlendSpace* BlendSpace = AnimMCP::LoadAsset<UBlendSpace>(blendspace_path, /*bForWrite*/ true, Error);
	if (!BlendSpace)
	{
		return AnimMCP::Fail(Error);
	}
	UAnimSequence* Sequence = AnimMCP::LoadAsset<UAnimSequence>(animation_path, /*bForWrite*/ false, Error);
	if (!Sequence)
	{
		return AnimMCP::Fail(Error);
	}
	if (BlendSpace->GetSkeleton() && Sequence->GetSkeleton() && !BlendSpace->GetSkeleton()->IsCompatibleForEditor(Sequence->GetSkeleton()))
	{
		return AnimMCP::Fail(TEXT("The animation's skeleton is not compatible with the blend space's skeleton."));
	}

	const bool b1D = BlendSpace->IsA<UBlendSpace1D>();
	const FVector SampleValue(x, b1D ? 0.f : y, 0.f);
	if (!BlendSpace->IsSampleWithinBounds(SampleValue))
	{
		const FBlendParameter& X = BlendSpace->GetBlendParameter(0);
		const FBlendParameter& Y = BlendSpace->GetBlendParameter(1);
		return AnimMCP::Fail(b1D
			? FString::Printf(TEXT("Sample x=%g is outside the axis range [%g, %g]."), x, X.Min, X.Max)
			: FString::Printf(TEXT("Sample (%g, %g) is outside the axis ranges x=[%g, %g], y=[%g, %g]."), x, y, X.Min, X.Max, Y.Min, Y.Max));
	}
	if (!BlendSpace->ValidateSampleValue(SampleValue))
	{
		return AnimMCP::Fail(TEXT("A sample already exists at that position."));
	}

	const FScopedTransaction Transaction(LOCTEXT("AddBlendSample", "AnimMCP: Add Blend Space Sample"));
	BlendSpace->Modify();
	const int32 SampleIndex = BlendSpace->AddSample(Sequence, SampleValue);
	if (SampleIndex == INDEX_NONE)
	{
		return AnimMCP::Fail(TEXT("The blend space rejected the sample (it may be additive while the blend space is not, or vice versa)."));
	}
	BlendSpace->ValidateSampleData();
	BlendSpace->ResampleData();
	BlendSpace->PostEditChange();
	BlendSpace->MarkPackageDirty();

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("sample_index"), SampleIndex);
	Payload->SetNumberField(TEXT("sample_count"), BlendSpace->GetNumberOfBlendSamples());
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimAssetToolset::anim_compile_blueprint(const FString& blueprint_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}

	return AnimMCP::Ok(CompileAndReport(AnimBP).ToJson());
}

FAnimMCPResult UAnimAssetToolset::anim_save_asset(const FString& asset_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UObject* Asset = AnimMCP::LoadAssetChecked(asset_path, nullptr, /*bForWrite*/ true, Error);
	if (!Asset)
	{
		return AnimMCP::Fail(Error);
	}

	UPackage* Package = Asset->GetOutermost();
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("path"), Asset->GetPathName());

	if (!Package->IsDirty())
	{
		Payload->SetBoolField(TEXT("saved"), false);
		return AnimMCP::Ok(Payload);
	}

	if (!UEditorLoadingAndSavingUtils::SavePackages({ Package }, /*bOnlyDirty*/ true))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Saving '%s' failed. The file may be read-only or checked out by someone else."), *Package->GetName()));
	}
	Payload->SetBoolField(TEXT("saved"), true);
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimAssetToolset::anim_set_skeleton_compatible(const FString& skeleton_path, const FString& compatible_skeleton_path, bool compatible)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	USkeleton* Skeleton = AnimMCP::ResolveSkeleton(skeleton_path, Error);
	if (!Skeleton)
	{
		return AnimMCP::Fail(Error);
	}
	if (!AnimMCP::IsUnderGameRoot(Skeleton->GetPathName()))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Refusing to modify '%s': only assets under /Game may be edited."), *Skeleton->GetPathName()));
	}
	USkeleton* Other = AnimMCP::ResolveSkeleton(compatible_skeleton_path, Error);
	if (!Other)
	{
		return AnimMCP::Fail(Error);
	}
	if (Other == Skeleton)
	{
		return AnimMCP::Fail(TEXT("A skeleton is always compatible with itself; pass a different compatible_skeleton_path."));
	}

	const TSoftObjectPtr<USkeleton> OtherRef(Other);
	const bool bListed = Skeleton->GetCompatibleSkeletons().Contains(OtherRef);
	const bool bChange = compatible != bListed;
	if (bChange)
	{
		const FScopedTransaction Transaction(LOCTEXT("SetSkeletonCompatible", "AnimMCP: Set Compatible Skeleton"));
		Skeleton->Modify();
		if (compatible)
		{
			Skeleton->AddCompatibleSkeleton(Other);
		}
		else
		{
			Skeleton->RemoveCompatibleSkeleton(Other);
		}
		Skeleton->MarkPackageDirty();
	}

	TArray<FString> Compatible;
	for (const TSoftObjectPtr<USkeleton>& Entry : Skeleton->GetCompatibleSkeletons())
	{
		Compatible.Add(Entry.ToSoftObjectPath().ToString());
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Payload->SetBoolField(TEXT("changed"), bChange);
	Payload->SetArrayField(TEXT("compatible_skeletons"), AnimMCP::ToJsonArray(Compatible));
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimAssetToolset::anim_rename_anim_blueprint(const FString& blueprint_path, const FString& new_name, const FString& new_folder)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	const FString OldPackageName = AnimBP->GetOutermost()->GetName();
	const FString OldPath = AnimBP->GetPathName();
	FString Folder;
	if (AnimMCP::IsUnset(new_folder) || new_folder.TrimStartAndEnd().Equals(TEXT("auto"), ESearchCase::IgnoreCase))
	{
		Folder = FPackageName::GetLongPackagePath(OldPackageName);
	}
	else if (!AnimMCP::NormalizeFolder(new_folder, Folder, Error))
	{
		return AnimMCP::Fail(Error);
	}
	const FString NewName = new_name.TrimStartAndEnd();
	FString NewPackageName;
	if (!AnimMCP::ValidateNewAssetLocation(Folder, NewName, NewPackageName, Error))
	{
		return AnimMCP::Fail(Error);
	}

	// Referencers are read before the rename, from the asset registry (saved packages).
	TArray<FName> Referencers;
	FAssetRegistryModule::GetRegistry().GetReferencers(FName(*OldPackageName), Referencers);
	TArray<FString> ReferencerNames;
	for (const FName& Referencer : Referencers)
	{
		if (Referencer.ToString() != OldPackageName)
		{
			ReferencerNames.Add(Referencer.ToString());
		}
	}

	// ObjectTools::RenameSingleObject is the editor's own rename without the extras IAssetTools::RenameAssets adds:
	// that function saves the renamed asset, its redirector and referencing packages, and deletes the old package when no
	// redirector is needed. Here a redirector is always left and nothing is saved or deleted.
	ObjectTools::FPackageGroupName PGN;
	PGN.PackageName = NewPackageName;
	PGN.ObjectName = NewName;
	TSet<UPackage*> RefusedToLoad;
	FText RenameError;
	if (!ObjectTools::RenameSingleObject(AnimBP, PGN, RefusedToLoad, RenameError, nullptr, /*bLeaveRedirector*/ true))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Rename failed: %s"), *RenameError.ToString()));
	}
	AnimBP->MarkPackageDirty();
	if (UPackage* OldPackage = FindPackage(nullptr, *OldPackageName))
	{
		OldPackage->MarkPackageDirty();
	}

	const FCompileReport Compile = CompileAndReport(AnimBP);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("old_path"), OldPath);
	Payload->SetStringField(TEXT("new_path"), AnimBP->GetPathName());
	Payload->SetStringField(TEXT("redirector"), OldPath);
	Payload->SetArrayField(TEXT("referencers"), AnimMCP::ToJsonArray(ReferencerNames));
	Payload->SetArrayField(TEXT("save_to_finish"), AnimMCP::ToJsonArray(TArray<FString>{ NewPackageName, OldPackageName }));
	Payload->SetObjectField(TEXT("compile"), Compile.ToJson());
	Payload->SetBoolField(TEXT("saved"), false);
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimAssetToolset::anim_reparent_anim_blueprint(const FString& blueprint_path, const FString& new_parent)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimBlueprint* AnimBP = AnimMCP::LoadAsset<UAnimBlueprint>(blueprint_path, /*bForWrite*/ true, Error);
	if (!AnimBP)
	{
		return AnimMCP::Fail(Error);
	}
	UClass* NewParent = ResolveAnimInstanceClass(new_parent, Error);
	if (!NewParent)
	{
		return AnimMCP::Fail(Error);
	}
	if (NewParent == AnimBP->ParentClass)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("%s is already the parent class."), *NewParent->GetName()));
	}
	// Walk the blueprints' declared parents as well as the compiled classes: after an undo, or before a compile, a generated
	// class can still have its old super class, and a cycle that slips through crashes the compiler.
	int32 Guard = 0;
	for (UClass* Ancestor = NewParent; Ancestor && Guard++ < 1024; )
	{
		const UBlueprint* AncestorBP = Cast<UBlueprint>(Ancestor->ClassGeneratedBy);
		if (AncestorBP == AnimBP || Ancestor == AnimBP->GeneratedClass || Ancestor == AnimBP->SkeletonGeneratedClass)
		{
			return AnimMCP::Fail(FString::Printf(TEXT("%s is this blueprint or derives from it; a blueprint cannot be its own ancestor."), *NewParent->GetName()));
		}
		Ancestor = AncestorBP ? AncestorBP->ParentClass.Get() : Ancestor->GetSuperClass();
	}
	if ((AnimBP->GeneratedClass && NewParent->IsChildOf(AnimBP->GeneratedClass)) || (AnimBP->SkeletonGeneratedClass && NewParent->IsChildOf(AnimBP->SkeletonGeneratedClass)))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("%s is this blueprint or derives from it; a blueprint cannot be its own ancestor."), *NewParent->GetName()));
	}
	if (const UAnimBlueprint* ParentBP = Cast<UAnimBlueprint>(NewParent->ClassGeneratedBy))
	{
		if (!ParentBP->bIsTemplate && ParentBP->TargetSkeleton && AnimBP->TargetSkeleton && !AnimBP->TargetSkeleton->IsCompatibleForEditor(ParentBP->TargetSkeleton))
		{
			return AnimMCP::Fail(FString::Printf(TEXT("%s targets skeleton %s, which is not compatible with this blueprint's skeleton %s. Use anim_set_skeleton_compatible first, or pick another parent."),
				*ParentBP->GetName(), *ParentBP->TargetSkeleton->GetPathName(), *AnimBP->TargetSkeleton->GetPathName()));
		}
	}

	const FCompileReport Before = CompileAndReport(AnimBP);
	UClass* OldParent = AnimBP->ParentClass;
	FCompileReport After;
	{
		// Mirrors FBlueprintEditor::ReparentBlueprint_NewParentChosen, without its dialogs and namespace import bookkeeping.
		const FScopedTransaction Transaction(LOCTEXT("ReparentAnimBP", "AnimMCP: Reparent Animation Blueprint"));
		AnimBP->Modify();
		AnimBP->ParentClass = NewParent;
		FBlueprintEditorUtils::RefreshAllNodes(AnimBP);
		FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
		if (UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(AnimBP->GeneratedClass))
		{
			GeneratedClass->PrepareToConformSparseClassData(NewParent->GetSparseClassDataStruct());
		}
		After = CompileAndReport(AnimBP);
	}

	auto BySeverity = [](const TArray<TSharedRef<FJsonObject>>& Messages, const TCHAR* Severity)
	{
		TArray<TSharedRef<FJsonObject>> Result;
		for (const TSharedRef<FJsonObject>& Message : Messages)
		{
			if (Message->GetStringField(TEXT("severity")).Equals(Severity, ESearchCase::IgnoreCase))
			{
				Result.Add(Message);
			}
		}
		return Result;
	};
	const TArray<TSharedRef<FJsonObject>> Added = After.NotIn(Before);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("path"), AnimBP->GetPathName());
	Payload->SetStringField(TEXT("old_parent"), OldParent ? OldParent->GetPathName() : FString());
	Payload->SetStringField(TEXT("new_parent"), NewParent->GetPathName());
	Payload->SetObjectField(TEXT("compile"), After.ToJson());
	Payload->SetArrayField(TEXT("new_errors"), AnimMCP::ToJsonArray(BySeverity(Added, TEXT("Error"))));
	Payload->SetArrayField(TEXT("new_warnings"), AnimMCP::ToJsonArray(BySeverity(Added, TEXT("Warning"))));
	Payload->SetArrayField(TEXT("fixed"), AnimMCP::ToJsonArray(Before.NotIn(After)));
	Payload->SetBoolField(TEXT("broke"), Before.NumErrors == 0 && After.NumErrors > 0);
	return AnimMCP::Ok(Payload);
}

#undef LOCTEXT_NAMESPACE
