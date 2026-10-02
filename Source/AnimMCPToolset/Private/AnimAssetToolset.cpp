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
#include "AssetToolsModule.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphToken.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Factories/BlendSpaceFactory1D.h"
#include "Factories/BlendSpaceFactoryNew.h"
#include "FileHelpers.h"
#include "IAssetTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/StringOutputDevice.h"
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

FAnimMCPResult UAnimAssetToolset::anim_create_anim_blueprint(const FString& folder, const FString& asset_name, const FString& skeleton_path, const FString& parent_class, const FString& preview_mesh_path)
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

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("path"), AnimBP->GetPathName());
	Payload->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Payload->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
	Payload->SetBoolField(TEXT("saved"), false);
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

	FCompilerResultsLog Results;
	Results.SetSourcePath(AnimBP->GetPathName());
	Results.bSilentMode = true;
	FKismetEditorUtilities::CompileBlueprint(AnimBP, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

	TArray<TSharedPtr<FJsonValue>> Messages;
	for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("severity"), FTokenizedMessage::GetSeverityText(Message->GetSeverity()).ToString());
		Item->SetStringField(TEXT("message"), Message->ToText().ToString());
		if (const TSharedPtr<FJsonObject> Source = DescribeMessageSource(*Message))
		{
			Item->SetObjectField(TEXT("source"), Source);
		}
		Messages.Add(MakeShared<FJsonValueObject>(Item));
	}

	FString Status;
	switch (AnimBP->Status)
	{
	case BS_UpToDate:             Status = TEXT("up_to_date"); break;
	case BS_UpToDateWithWarnings: Status = TEXT("up_to_date_with_warnings"); break;
	case BS_Error:                Status = TEXT("error"); break;
	case BS_Dirty:                Status = TEXT("dirty"); break;
	default:                      Status = TEXT("unknown"); break;
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("status"), Status);
	Payload->SetNumberField(TEXT("num_errors"), Results.NumErrors);
	Payload->SetNumberField(TEXT("num_warnings"), Results.NumWarnings);
	Payload->SetArrayField(TEXT("messages"), Messages);
	return AnimMCP::Ok(Payload);
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

#undef LOCTEXT_NAMESPACE
