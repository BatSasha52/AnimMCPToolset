// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#include "AnimDataToolset.h"

#include "AnimMCPHelpers.h"

#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Curves/RichCurve.h"
#include "Dom/JsonObject.h"
#include "Factories/AnimMontageFactory.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "AnimMCPData"

namespace
{
	TArray<TSharedPtr<FJsonValue>> VectorToJson(const FVector& V)
	{
		return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) };
	}

	TArray<FString> SplitList(const FString& Text)
	{
		TArray<FString> Items;
		Text.ParseIntoArray(Items, TEXT(","));
		for (FString& Item : Items)
		{
			Item.TrimStartAndEndInline();
		}
		Items.RemoveAll([](const FString& Item) { return Item.IsEmpty(); });
		return Items;
	}

	/**
	 * Reads bone transforms from a sequence's source data (the animation data model). Bones without a track use the
	 * skeleton's reference pose, which is also what the engine plays for them.
	 */
	class FBoneSampler
	{
	public:
		FBoneSampler(const UAnimSequence* InSequence)
			: Sequence(InSequence)
			, Model(InSequence->GetDataModel())
			, RefSkeleton(InSequence->GetSkeleton()->GetReferenceSkeleton())
		{
			TArray<FName> TrackNames;
			Model->GetBoneTrackNames(TrackNames);
			Tracked.Append(TrackNames);
		}

		const FReferenceSkeleton& GetRefSkeleton() const { return RefSkeleton; }
		bool HasTrack(int32 BoneIndex) const { return Tracked.Contains(RefSkeleton.GetBoneName(BoneIndex)); }

		FFrameTime ToFrameTime(double Seconds) const
		{
			return Model->GetFrameRate().AsFrameTime(FMath::Clamp(Seconds, 0.0, Model->GetPlayLength()));
		}

		FTransform Local(int32 BoneIndex, const FFrameTime& Time) const
		{
			const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
			if (!Tracked.Contains(BoneName))
			{
				return RefSkeleton.GetRefBonePose()[BoneIndex];
			}
			return Model->EvaluateBoneTrackTransform(BoneName, Time, Sequence->Interpolation);
		}

		FTransform Component(int32 BoneIndex, const FFrameTime& Time) const
		{
			FTransform Result = Local(BoneIndex, Time);
			for (int32 Parent = RefSkeleton.GetParentIndex(BoneIndex); Parent != INDEX_NONE; Parent = RefSkeleton.GetParentIndex(Parent))
			{
				Result = Result * Local(Parent, Time);
			}
			return Result;
		}

		FTransform Local(int32 BoneIndex, double Seconds) const { return Local(BoneIndex, ToFrameTime(Seconds)); }
		FTransform Component(int32 BoneIndex, double Seconds) const { return Component(BoneIndex, ToFrameTime(Seconds)); }

	private:
		const UAnimSequence* Sequence;
		const IAnimationDataModel* Model;
		const FReferenceSkeleton& RefSkeleton;
		TSet<FName> Tracked;
	};

	/** The hips bone by naming convention ('pelvis', 'hips', 'hip'), or INDEX_NONE. Root is excluded. */
	int32 FindHipsBone(const FReferenceSkeleton& RefSkeleton)
	{
		static const TCHAR* const Exact[] = { TEXT("pelvis"), TEXT("hips"), TEXT("hip") };
		for (const TCHAR* Name : Exact)
		{
			for (int32 Index = 1; Index < RefSkeleton.GetNum(); ++Index)
			{
				const FString BoneName = RefSkeleton.GetBoneName(Index).ToString();
				// Match 'pelvis' as well as prefixed names such as 'mixamorig:Hips' or 'DEF-pelvis'.
				if (BoneName.Equals(Name, ESearchCase::IgnoreCase) || BoneName.EndsWith(FString(TEXT(":")) + Name, ESearchCase::IgnoreCase) || BoneName.EndsWith(FString(TEXT("-")) + Name, ESearchCase::IgnoreCase))
				{
					return Index;
				}
			}
		}
		return INDEX_NONE;
	}

	bool ResolveBones(const FReferenceSkeleton& RefSkeleton, const TArray<FString>& Names, TArray<int32>& OutIndices, FString& OutError)
	{
		TArray<FString> Missing;
		for (const FString& Name : Names)
		{
			const int32 Index = RefSkeleton.FindBoneIndex(FName(*Name));
			if (Index == INDEX_NONE)
			{
				Missing.Add(Name);
			}
			else
			{
				OutIndices.AddUnique(Index);
			}
		}
		if (!Missing.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Bone(s) not in the skeleton: %s. Use anim_list_skeleton_bones to see bone names."), *FString::Join(Missing, TEXT(", ")));
			return false;
		}
		return true;
	}

	TSharedRef<FJsonObject> DeltaToJson(const FVector& Delta)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("x"), Delta.X);
		Json->SetNumberField(TEXT("y"), Delta.Y);
		Json->SetNumberField(TEXT("z"), Delta.Z);
		return Json;
	}

	/** Where a bone goes over the whole sequence, in component space. */
	TSharedRef<FJsonObject> MeasureTravel(const FBoneSampler& Sampler, int32 BoneIndex, const IAnimationDataModel* Model)
	{
		const int32 NumKeys = FMath::Max(Model->GetNumberOfKeys(), 1);
		FVector Previous = Sampler.Component(BoneIndex, FFrameTime(0)).GetLocation();
		const FVector Start = Previous;
		double PathLength = 0.0;
		for (int32 Key = 1; Key < NumKeys; ++Key)
		{
			const FVector Location = Sampler.Component(BoneIndex, FFrameTime(Key)).GetLocation();
			PathLength += FVector::Distance(Previous, Location);
			Previous = Location;
		}
		const FVector End = Previous;
		const FVector Delta = End - Start;
		const TSharedRef<FJsonObject> DeltaJson = DeltaToJson(Delta);

		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("bone"), Sampler.GetRefSkeleton().GetBoneName(BoneIndex).ToString());
		Json->SetBoolField(TEXT("has_track"), Sampler.HasTrack(BoneIndex));
		Json->SetArrayField(TEXT("start"), VectorToJson(Start));
		Json->SetArrayField(TEXT("end"), VectorToJson(End));
		Json->SetObjectField(TEXT("delta"), DeltaJson);
		Json->SetNumberField(TEXT("distance"), Delta.Size());
		Json->SetNumberField(TEXT("horizontal_distance"), Delta.Size2D());
		Json->SetNumberField(TEXT("path_length"), PathLength);
		return Json;
	}

	template <typename EnumType>
	FString EnumName(EnumType Value)
	{
		return StaticEnum<EnumType>()->GetNameStringByValue((int64)Value);
	}

	// ---- Edit targets ----------------------------------------------------------------------

	/** Where an edit goes: the asset itself, or a copy that does not exist yet. Planned (and checked) before anything is created. */
	struct FEditTarget
	{
		bool bInPlace = false;
		FString Folder;
		FString Name;
	};

	bool PlanEditTarget(const UObject* Source, bool bInPlace, const FString& OutputPath, FEditTarget& Out, FString& OutError)
	{
		const FString SourcePackage = Source->GetOutermost()->GetName();
		const bool bAutoOutput = AnimMCP::IsUnset(OutputPath) || OutputPath.TrimStartAndEnd().Equals(TEXT("auto"), ESearchCase::IgnoreCase);
		if (bInPlace)
		{
			if (!AnimMCP::IsUnderGameRoot(SourcePackage))
			{
				OutError = FString::Printf(TEXT("Refusing to modify '%s' in place: only assets under /Game may be edited. Leave in_place=false to edit a copy."), *Source->GetPathName());
				return false;
			}
			if (!bAutoOutput)
			{
				OutError = TEXT("output_path is only used when in_place is false. Leave it at 'auto'.");
				return false;
			}
			Out.bInPlace = true;
			return true;
		}

		if (bAutoOutput)
		{
			if (!AnimMCP::IsUnderGameRoot(SourcePackage))
			{
				OutError = FString::Printf(TEXT("'%s' is outside /Game, so the edited copy needs an explicit output_path under /Game, e.g. '/Game/Animations/%s_Edited'."), *Source->GetPathName(), *Source->GetName());
				return false;
			}
			Out.Folder = FPackageName::GetLongPackagePath(SourcePackage);
			Out.Name = Source->GetName() + TEXT("_Edited");
		}
		else
		{
			FString ObjectPath;
			if (!AnimMCP::NormalizeObjectPath(OutputPath, ObjectPath, OutError))
			{
				return false;
			}
			const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
			Out.Folder = FPackageName::GetLongPackagePath(PackageName);
			Out.Name = FPackageName::GetLongPackageAssetName(PackageName);
		}

		FString PackageName;
		if (!AnimMCP::ValidateNewAssetLocation(Out.Folder, Out.Name, PackageName, OutError))
		{
			if (bAutoOutput)
			{
				OutError += TEXT(" Pass output_path to choose another name, or in_place=true to edit the source.");
			}
			return false;
		}
		return true;
	}

	/**
	 * Returns the object to edit: the source, or a new copy. The copy is made the way the editor's Duplicate does it, minus the
	 * save AssetTools performs when source control is enabled: nothing is written to disk here.
	 */
	template <typename T>
	T* ApplyEditTarget(T* Source, const FEditTarget& Target, FString& OutError)
	{
		if (Target.bInPlace)
		{
			return Source;
		}
		ObjectTools::FPackageGroupName PGN;
		PGN.PackageName = Target.Folder / Target.Name;
		PGN.ObjectName = Target.Name;
		TSet<UPackage*> RefusedToLoad;
		UObject* Copy = ObjectTools::DuplicateSingleObject(Source, PGN, RefusedToLoad, /*bPromptToOverwrite*/ false);
		if (!Copy)
		{
			OutError = FString::Printf(TEXT("Could not copy '%s' to '%s'."), *Source->GetPathName(), *PGN.PackageName);
			return nullptr;
		}
		const bool bWasAsset = Copy->IsAsset();
		Copy->SetFlags(RF_Public | RF_Standalone);
		if (!bWasAsset && Copy->IsAsset())
		{
			FAssetRegistryModule::AssetCreated(Copy);
		}
		Copy->MarkPackageDirty();
		return CastChecked<T>(Copy);
	}

	void AddTargetFields(const TSharedRef<FJsonObject>& Payload, const UObject* Source, const UObject* Edited)
	{
		Payload->SetStringField(TEXT("path"), Edited->GetPathName());
		Payload->SetStringField(TEXT("source"), Source->GetPathName());
		Payload->SetBoolField(TEXT("in_place"), Source == Edited);
		Payload->SetBoolField(TEXT("saved"), false);
	}

	/** Loads an animation for editing: read-only access is enough when a copy will be edited. */
	template <typename T>
	T* LoadForEdit(const FString& Path, bool bInPlace, FString& OutError)
	{
		return AnimMCP::LoadAsset<T>(Path, /*bForWrite*/ bInPlace, OutError);
	}

	// ---- Notifies --------------------------------------------------------------------------

	/** Finds a notify by guid or 'index:<n>'. */
	int32 FindNotifyIndex(const UAnimSequenceBase* Anim, const FString& Key, FString& OutError)
	{
		const FString Trimmed = Key.TrimStartAndEnd();
		if (Trimmed.StartsWith(TEXT("index:"), ESearchCase::IgnoreCase))
		{
			const FString IndexText = Trimmed.RightChop(6).TrimStartAndEnd();
			const int32 Index = IndexText.IsNumeric() ? FCString::Atoi(*IndexText) : INDEX_NONE;
			if (!Anim->Notifies.IsValidIndex(Index))
			{
				OutError = FString::Printf(TEXT("'%s' is not a valid notify index (the animation has %d notifies)."), *Key, Anim->Notifies.Num());
				return INDEX_NONE;
			}
			return Index;
		}
		FGuid Guid;
		if (!FGuid::Parse(Trimmed, Guid) || !Guid.IsValid())
		{
			OutError = FString::Printf(TEXT("'%s' is neither a notify guid nor 'index:<n>'. Use anim_get_animation_info to list notifies."), *Key);
			return INDEX_NONE;
		}
		const int32 Index = Anim->Notifies.IndexOfByPredicate([&Guid](const FAnimNotifyEvent& Notify) { return Notify.Guid == Guid; });
		if (Index == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("No notify with guid %s in '%s'."), *Key, *Anim->GetPathName());
		}
		return Index;
	}

	/** Resolves time / frame into seconds. Exactly one of them may be set (>= 0) unless both are optional. */
	bool ResolveNotifyTime(const UAnimSequenceBase* Anim, float Time, int32 Frame, bool bRequired, TOptional<float>& OutTime, FString& OutError)
	{
		if (Time >= 0.f && Frame >= 0)
		{
			OutError = TEXT("Give either time or frame, not both.");
			return false;
		}
		if (Time < 0.f && Frame < 0)
		{
			if (bRequired)
			{
				OutError = TEXT("Give the notify's time (seconds) or frame.");
				return false;
			}
			return true;
		}
		const float Seconds = Frame >= 0 ? (float)Anim->GetSamplingFrameRate().AsSeconds(FFrameNumber(Frame)) : Time;
		if (Seconds > Anim->GetPlayLength() + UE_KINDA_SMALL_NUMBER)
		{
			OutError = FString::Printf(TEXT("%s is past the end of the animation (%g seconds, %d frames)."),
				Frame >= 0 ? *FString::Printf(TEXT("Frame %d"), Frame) : *FString::Printf(TEXT("Time %g"), Time), Anim->GetPlayLength(), Anim->GetNumberOfSampledKeys() - 1);
			return false;
		}
		OutTime = FMath::Min(Seconds, Anim->GetPlayLength());
		return true;
	}

	int32 FindOrAddNotifyTrack(UAnimSequenceBase* Anim, const FName TrackName)
	{
		const int32 Existing = Anim->AnimNotifyTracks.IndexOfByPredicate([TrackName](const FAnimNotifyTrack& Track) { return Track.TrackName == TrackName; });
		if (Existing != INDEX_NONE)
		{
			return Existing;
		}
		return Anim->AnimNotifyTracks.Add(FAnimNotifyTrack(TrackName, FLinearColor::White));
	}

	/** Places a notify (and the end of a notify state) at a time, the way the notify editor does. */
	void PlaceNotify(UAnimSequenceBase* Anim, FAnimNotifyEvent& Notify, float Time)
	{
		Notify.Link(Anim, Time);
		Notify.TriggerTimeOffset = GetTriggerTimeOffsetForType(Anim->CalculateOffsetForNotify(Time));
	}

	void SetNotifyDuration(UAnimSequenceBase* Anim, FAnimNotifyEvent& Notify, float Duration)
	{
		Notify.SetDuration(Duration);
		Notify.EndLink.Link(Anim, Notify.EndLink.GetTime());
		Notify.EndTriggerTimeOffset = GetTriggerTimeOffsetForType(Anim->CalculateOffsetForNotify(Notify.EndLink.GetTime()));
	}

	TSharedRef<FJsonObject> NotifyToJson(const UAnimSequenceBase* Anim, const FAnimNotifyEvent& Notify)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("guid"), AnimMCP::GuidToString(Notify.Guid));
		Item->SetStringField(TEXT("name"), Notify.NotifyName.ToString());
		const UObject* NotifyObject = Notify.Notify ? static_cast<const UObject*>(Notify.Notify) : static_cast<const UObject*>(Notify.NotifyStateClass);
		Item->SetStringField(TEXT("class"), NotifyObject ? NotifyObject->GetClass()->GetPathName() : FString());
		Item->SetBoolField(TEXT("is_state"), Notify.NotifyStateClass != nullptr);
		Item->SetNumberField(TEXT("time"), Notify.GetTime());
		Item->SetNumberField(TEXT("frame"), Anim->GetSamplingFrameRate().AsFrameTime(Notify.GetTime()).AsDecimal());
		Item->SetNumberField(TEXT("duration"), Notify.GetDuration());
		Item->SetNumberField(TEXT("track_index"), Notify.TrackIndex);
		Item->SetStringField(TEXT("track"), Anim->AnimNotifyTracks.IsValidIndex(Notify.TrackIndex) ? Anim->AnimNotifyTracks[Notify.TrackIndex].TrackName.ToString() : FString());
		return Item;
	}

	/** Resolves a UAnimNotify or UAnimNotifyState class by name or path. */
	UClass* ResolveNotifyClass(const FString& Name, FString& OutError)
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
		if (!Class || !(Class->IsChildOf(UAnimNotify::StaticClass()) || Class->IsChildOf(UAnimNotifyState::StaticClass())))
		{
			OutError = FString::Printf(TEXT("'%s' is not an AnimNotify or AnimNotifyState class. Use a name like 'AnimNotify_PlaySound' or a path; for a Blueprint notify use its generated class path ('/Game/Notifies/BP_Step.BP_Step_C')."), *Name);
			return nullptr;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			OutError = FString::Printf(TEXT("'%s' is abstract or deprecated."), *Name);
			return nullptr;
		}
		return Class;
	}
}

FAnimMCPResult UAnimDataToolset::anim_get_animation_info(const FString& asset_path, const FString& travel_bones)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimSequenceBase* Anim = AnimMCP::LoadAsset<UAnimSequenceBase>(asset_path, /*bForWrite*/ false, Error);
	if (!Anim)
	{
		return AnimMCP::Fail(Error);
	}
	const IAnimationDataModel* Model = Anim->GetDataModel();
	UAnimSequence* Sequence = Cast<UAnimSequence>(Anim);
	USkeleton* Skeleton = Anim->GetSkeleton();

	// Travel bones are validated before anything is reported.
	TArray<int32> TravelBones;
	FString HipsNote;
	if (Sequence && Skeleton && Model && !AnimMCP::IsUnset(travel_bones))
	{
		const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
		if (travel_bones.TrimStartAndEnd().Equals(TEXT("auto"), ESearchCase::IgnoreCase))
		{
			if (RefSkeleton.GetNum() > 0)
			{
				TravelBones.Add(0);
			}
			const int32 Hips = FindHipsBone(RefSkeleton);
			if (Hips != INDEX_NONE)
			{
				TravelBones.Add(Hips);
			}
			else
			{
				HipsNote = TEXT("No bone named pelvis, hips or hip was found; pass travel_bones to measure the hips.");
			}
		}
		else if (!ResolveBones(RefSkeleton, SplitList(travel_bones), TravelBones, Error))
		{
			return AnimMCP::Fail(Error);
		}
	}
	else if (!Sequence && !AnimMCP::IsUnset(travel_bones) && !travel_bones.TrimStartAndEnd().Equals(TEXT("auto"), ESearchCase::IgnoreCase))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("'%s' is a %s; travel can only be measured on an AnimSequence."), *asset_path, *Anim->GetClass()->GetName()));
	}

	const FFrameRate FrameRate = Model ? Model->GetFrameRate() : Anim->GetSamplingFrameRate();
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("path"), Anim->GetPathName());
	Payload->SetStringField(TEXT("class"), Anim->GetClass()->GetName());
	Payload->SetStringField(TEXT("skeleton"), Skeleton ? Skeleton->GetPathName() : FString());
	Payload->SetNumberField(TEXT("length"), Anim->GetPlayLength());
	Payload->SetNumberField(TEXT("frame_rate"), FrameRate.AsDecimal());
	Payload->SetNumberField(TEXT("frame_count"), Model ? Model->GetNumberOfFrames() : FMath::Max(Anim->GetNumberOfSampledKeys() - 1, 0));
	Payload->SetNumberField(TEXT("key_count"), Model ? Model->GetNumberOfKeys() : Anim->GetNumberOfSampledKeys());
	Payload->SetStringField(TEXT("additive_type"), EnumName(Anim->GetAdditiveAnimType()));

	if (Sequence)
	{
		Payload->SetStringField(TEXT("interpolation"), EnumName(Sequence->Interpolation));
		TSharedRef<FJsonObject> RootMotion = MakeShared<FJsonObject>();
		RootMotion->SetBoolField(TEXT("enabled"), Sequence->bEnableRootMotion);
		RootMotion->SetStringField(TEXT("root_lock"), EnumName(Sequence->RootMotionRootLock.GetValue()));
		RootMotion->SetBoolField(TEXT("force_root_lock"), Sequence->bForceRootLock);
		Payload->SetObjectField(TEXT("root_motion"), RootMotion);
		Payload->SetNumberField(TEXT("bone_track_count"), Model ? Model->GetNumBoneTracks() : 0);
	}

	TArray<TSharedRef<FJsonObject>> Curves;
	if (Model)
	{
		for (const FFloatCurve& Curve : Model->GetFloatCurves())
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Curve.GetName().ToString());
			Item->SetNumberField(TEXT("key_count"), Curve.FloatCurve.GetNumKeys());
			float MinValue = 0.f;
			float MaxValue = 0.f;
			if (Curve.FloatCurve.GetNumKeys() > 0)
			{
				Curve.FloatCurve.GetValueRange(MinValue, MaxValue);
			}
			Item->SetNumberField(TEXT("min_value"), MinValue);
			Item->SetNumberField(TEXT("max_value"), MaxValue);
			Curves.Add(Item);
		}
	}
	Payload->SetArrayField(TEXT("curves"), AnimMCP::ToJsonArray(Curves));

	TArray<FString> TrackNames;
	for (const FAnimNotifyTrack& Track : Anim->AnimNotifyTracks)
	{
		TrackNames.Add(Track.TrackName.ToString());
	}
	Payload->SetArrayField(TEXT("notify_tracks"), AnimMCP::ToJsonArray(TrackNames));

	TArray<TSharedRef<FJsonObject>> Notifies;
	for (const FAnimNotifyEvent& Notify : Anim->Notifies)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("guid"), AnimMCP::GuidToString(Notify.Guid));
		Item->SetStringField(TEXT("name"), Notify.NotifyName.ToString());
		const UObject* NotifyObject = Notify.Notify ? static_cast<const UObject*>(Notify.Notify) : static_cast<const UObject*>(Notify.NotifyStateClass);
		Item->SetStringField(TEXT("class"), NotifyObject ? NotifyObject->GetClass()->GetPathName() : FString());
		Item->SetBoolField(TEXT("is_state"), Notify.NotifyStateClass != nullptr);
		Item->SetNumberField(TEXT("time"), Notify.GetTime());
		Item->SetNumberField(TEXT("frame"), FrameRate.AsFrameTime(Notify.GetTime()).AsDecimal());
		Item->SetNumberField(TEXT("duration"), Notify.GetDuration());
		Item->SetNumberField(TEXT("track_index"), Notify.TrackIndex);
		Item->SetStringField(TEXT("track"), TrackNames.IsValidIndex(Notify.TrackIndex) ? TrackNames[Notify.TrackIndex] : FString());
		Notifies.Add(Item);
	}
	Payload->SetArrayField(TEXT("notifies"), AnimMCP::ToJsonArray(Notifies));

	if (!TravelBones.IsEmpty())
	{
		const FBoneSampler Sampler(Sequence);
		TArray<TSharedRef<FJsonObject>> Travel;
		TArray<FString> Measured;
		for (const int32 BoneIndex : TravelBones)
		{
			Travel.Add(MeasureTravel(Sampler, BoneIndex, Model));
			Measured.Add(Sampler.GetRefSkeleton().GetBoneName(BoneIndex).ToString());
		}
		Payload->SetArrayField(TEXT("travel"), AnimMCP::ToJsonArray(Travel));
		Payload->SetArrayField(TEXT("bones_measured"), AnimMCP::ToJsonArray(Measured));
	}
	if (!HipsNote.IsEmpty())
	{
		Payload->SetStringField(TEXT("note"), HipsNote);
	}
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimDataToolset::anim_sample_bones(const FString& asset_path, float time, const FString& bones, const FString& space)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimSequence* Sequence = AnimMCP::LoadAsset<UAnimSequence>(asset_path, /*bForWrite*/ false, Error);
	if (!Sequence)
	{
		return AnimMCP::Fail(Error);
	}
	if (!Sequence->GetSkeleton() || !Sequence->GetDataModel())
	{
		return AnimMCP::Fail(FString::Printf(TEXT("'%s' has no skeleton or no source data."), *asset_path));
	}
	const bool bComponent = space.TrimStartAndEnd().Equals(TEXT("component"), ESearchCase::IgnoreCase);
	if (!bComponent && !space.TrimStartAndEnd().Equals(TEXT("parent"), ESearchCase::IgnoreCase))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Unknown space '%s'. Use 'component' or 'parent'."), *space));
	}
	const IAnimationDataModel* Model = Sequence->GetDataModel();
	const double Length = Model->GetPlayLength();
	if (time < 0.f || time > Length + UE_KINDA_SMALL_NUMBER)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("time %g is outside the sequence (0 to %g seconds)."), time, Length));
	}

	const FBoneSampler Sampler(Sequence);
	const FReferenceSkeleton& RefSkeleton = Sampler.GetRefSkeleton();
	TArray<int32> BoneIndices;
	if (AnimMCP::IsUnset(bones))
	{
		for (int32 Index = 0; Index < RefSkeleton.GetNum(); ++Index)
		{
			BoneIndices.Add(Index);
		}
	}
	else if (!ResolveBones(RefSkeleton, SplitList(bones), BoneIndices, Error))
	{
		return AnimMCP::Fail(Error);
	}

	TArray<TSharedRef<FJsonObject>> Items;
	for (const int32 BoneIndex : BoneIndices)
	{
		const FTransform Transform = bComponent ? Sampler.Component(BoneIndex, time) : Sampler.Local(BoneIndex, time);
		const FRotator Rotator = Transform.Rotator();
		const FQuat Quat = Transform.GetRotation();
		const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);

		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), RefSkeleton.GetBoneName(BoneIndex).ToString());
		Item->SetNumberField(TEXT("index"), BoneIndex);
		Item->SetStringField(TEXT("parent"), ParentIndex != INDEX_NONE ? RefSkeleton.GetBoneName(ParentIndex).ToString() : FString());
		Item->SetBoolField(TEXT("has_track"), Sampler.HasTrack(BoneIndex));
		Item->SetArrayField(TEXT("translation"), VectorToJson(Transform.GetLocation()));
		Item->SetArrayField(TEXT("rotation"), VectorToJson(FVector(Rotator.Pitch, Rotator.Yaw, Rotator.Roll)));
		Item->SetArrayField(TEXT("quaternion"), { MakeShared<FJsonValueNumber>(Quat.X), MakeShared<FJsonValueNumber>(Quat.Y), MakeShared<FJsonValueNumber>(Quat.Z), MakeShared<FJsonValueNumber>(Quat.W) });
		Item->SetArrayField(TEXT("scale"), VectorToJson(Transform.GetScale3D()));
		Items.Add(Item);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("path"), Sequence->GetPathName());
	Payload->SetNumberField(TEXT("time"), time);
	Payload->SetNumberField(TEXT("frame"), Model->GetFrameRate().AsFrameTime(time).AsDecimal());
	Payload->SetStringField(TEXT("space"), bComponent ? TEXT("component") : TEXT("parent"));
	Payload->SetArrayField(TEXT("bones"), AnimMCP::ToJsonArray(Items));
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimDataToolset::anim_remove_bone_travel(const FString& animation_path, const FString& bone, const FString& axes, const FString& mode, bool in_place, const FString& output_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimSequence* Source = LoadForEdit<UAnimSequence>(animation_path, in_place, Error);
	if (!Source)
	{
		return AnimMCP::Fail(Error);
	}
	if (!Source->GetSkeleton() || !Source->GetDataModel())
	{
		return AnimMCP::Fail(FString::Printf(TEXT("'%s' has no skeleton or no source data."), *animation_path));
	}

	// Axes and mode.
	FVector Mask = FVector::ZeroVector;
	const FString AxesText = axes.TrimStartAndEnd().ToLower();
	for (const TCHAR Axis : AxesText)
	{
		if (Axis == TEXT('x')) { Mask.X = 1.0; }
		else if (Axis == TEXT('y')) { Mask.Y = 1.0; }
		else if (Axis == TEXT('z')) { Mask.Z = 1.0; }
		else if (Axis != TEXT(',') && Axis != TEXT(' '))
		{
			return AnimMCP::Fail(FString::Printf(TEXT("axes '%s' may only contain x, y and z, e.g. 'xy'."), *axes));
		}
	}
	if (Mask.IsZero())
	{
		return AnimMCP::Fail(TEXT("axes must name at least one of x, y, z, e.g. 'xy'."));
	}
	const FString Mode = mode.TrimStartAndEnd().ToLower();
	const bool bFlatten = Mode == TEXT("flatten");
	if (!bFlatten && Mode != TEXT("linear"))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Unknown mode '%s'. Use 'linear' (remove the start-to-end drift) or 'flatten' (hold the first frame)."), *mode));
	}

	// The bone, and its travel as things stand.
	const IAnimationDataModel* Model = Source->GetDataModel();
	const FBoneSampler Sampler(Source);
	const FReferenceSkeleton& RefSkeleton = Sampler.GetRefSkeleton();
	const int32 NumKeys = Model->GetNumberOfKeys();
	auto Travel = [&](int32 BoneIndex)
	{
		return Sampler.Component(BoneIndex, FFrameTime(FMath::Max(NumKeys - 1, 0))).GetLocation() - Sampler.Component(BoneIndex, FFrameTime(0)).GetLocation();
	};

	int32 BoneIndex = INDEX_NONE;
	FString Reason;
	if (bone.TrimStartAndEnd().Equals(TEXT("auto"), ESearchCase::IgnoreCase))
	{
		if (RefSkeleton.GetNum() > 0 && Sampler.HasTrack(0) && (Travel(0) * Mask).Size() > 0.01)
		{
			BoneIndex = 0;
			Reason = TEXT("auto: the root bone moves");
		}
		else
		{
			BoneIndex = FindHipsBone(RefSkeleton);
			Reason = TEXT("auto: the root does not move, so the hips were used");
			if (BoneIndex == INDEX_NONE)
			{
				return AnimMCP::Fail(TEXT("The root bone does not move and no hips bone (pelvis, hips, hip) was found. Pass bone explicitly."));
			}
		}
	}
	else
	{
		TArray<int32> Found;
		if (!ResolveBones(RefSkeleton, { bone.TrimStartAndEnd() }, Found, Error))
		{
			return AnimMCP::Fail(Error);
		}
		BoneIndex = Found[0];
		Reason = TEXT("given");
	}
	const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
	if (!Sampler.HasTrack(BoneIndex))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Bone '%s' has no keys in '%s', so it does not move."), *BoneName.ToString(), *animation_path));
	}
	if (NumKeys < 2)
	{
		return AnimMCP::Fail(TEXT("The animation has a single key; there is no travel to remove."));
	}
	// Measured before the edit: with in_place the sampler reads the live data.
	const FVector TravelBefore = Travel(BoneIndex);
	const FVector RootTravelBefore = Travel(0);
	const FVector Drift = TravelBefore * Mask;
	if (!bFlatten && Drift.Size() < 0.01)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Bone '%s' already ends where it starts on axes '%s' (drift %.4f cm); nothing to remove."), *BoneName.ToString(), *axes, Drift.Size()));
	}

	// New keys: move the bone in component space, then express it back in its parent's space. Rotation and scale stay.
	const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
	const FVector Start = Sampler.Component(BoneIndex, FFrameTime(0)).GetLocation();
	TArray<FVector> Positions;
	TArray<FQuat> Rotations;
	TArray<FVector> Scales;
	for (int32 Key = 0; Key < NumKeys; ++Key)
	{
		const FTransform Local = Sampler.Local(BoneIndex, FFrameTime(Key));
		const FTransform ParentCS = ParentIndex != INDEX_NONE ? Sampler.Component(ParentIndex, FFrameTime(Key)) : FTransform::Identity;
		const FVector Location = (Local * ParentCS).GetLocation();
		const FVector Offset = bFlatten ? (Location - Start) * Mask : Drift * ((double)Key / (NumKeys - 1));
		Positions.Add(ParentCS.InverseTransformPosition(Location - Offset));
		Rotations.Add(Local.GetRotation());
		Scales.Add(Local.GetScale3D());
	}

	FEditTarget Target;
	if (!PlanEditTarget(Source, in_place, output_path, Target, Error))
	{
		return AnimMCP::Fail(Error);
	}
	UAnimSequence* Sequence = ApplyEditTarget(Source, Target, Error);
	if (!Sequence)
	{
		return AnimMCP::Fail(Error);
	}

	bool bSet = false;
	{
		const FScopedTransaction Transaction(LOCTEXT("RemoveBoneTravel", "AnimMCP: Remove Bone Travel"));
		Sequence->Modify();
		IAnimationDataController& Controller = Sequence->GetController();
		IAnimationDataController::FScopedBracket Bracket(Controller, LOCTEXT("RemoveBoneTravelBracket", "Remove bone travel"));
		bSet = Controller.SetBoneTrackKeys(BoneName, Positions, Rotations, Scales);
	}
	if (!bSet)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("The animation data controller rejected the new keys for '%s'.%s"), *BoneName.ToString(),
			Target.bInPlace ? TEXT("") : *FString::Printf(TEXT(" The copy '%s' was created but not changed."), *Sequence->GetPathName())));
	}
	Sequence->MarkPackageDirty();

	const FBoneSampler After(Sequence);
	const FVector DriftAfter = (After.Component(BoneIndex, FFrameTime(NumKeys - 1)).GetLocation() - After.Component(BoneIndex, FFrameTime(0)).GetLocation());
	TArray<FString> Warnings;
	if (BoneIndex == 0 && Sequence->bEnableRootMotion)
	{
		Warnings.Add(TEXT("Root motion is enabled and the root no longer travels on these axes, so the character will not move from root motion. Turn root motion off or move the character in code."));
	}
	if (BoneIndex != 0 && !RootTravelBefore.IsNearlyZero(0.01))
	{
		Warnings.Add(TEXT("The root bone also moves in this animation; only the chosen bone was changed."));
	}

	TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
	Before->SetObjectField(TEXT("delta"), DeltaToJson(TravelBefore));
	Before->SetNumberField(TEXT("distance"), TravelBefore.Size());
	TSharedRef<FJsonObject> AfterJson = MakeShared<FJsonObject>();
	AfterJson->SetObjectField(TEXT("delta"), DeltaToJson(DriftAfter));
	AfterJson->SetNumberField(TEXT("distance"), DriftAfter.Size());

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	AddTargetFields(Payload, Source, Sequence);
	Payload->SetStringField(TEXT("bone"), BoneName.ToString());
	Payload->SetStringField(TEXT("reason"), Reason);
	Payload->SetStringField(TEXT("axes"), AxesText);
	Payload->SetStringField(TEXT("mode"), Mode);
	Payload->SetNumberField(TEXT("keys_changed"), NumKeys);
	Payload->SetObjectField(TEXT("before"), Before);
	Payload->SetObjectField(TEXT("after"), AfterJson);
	Payload->SetArrayField(TEXT("warnings"), AnimMCP::ToJsonArray(Warnings));
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimDataToolset::anim_set_curve(const FString& animation_path, const FString& curve_name, const FString& keys, const FString& interpolation, bool in_place, const FString& output_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimSequence* Source = LoadForEdit<UAnimSequence>(animation_path, in_place, Error);
	if (!Source)
	{
		return AnimMCP::Fail(Error);
	}
	if (!Source->GetDataModel())
	{
		return AnimMCP::Fail(FString::Printf(TEXT("'%s' has no source data."), *animation_path));
	}
	FText NameError;
	if (curve_name.TrimStartAndEnd().IsEmpty() || !FName::IsValidXName(curve_name, INVALID_NAME_CHARACTERS, &NameError))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("'%s' is not a valid curve name. %s"), *curve_name, *NameError.ToString()));
	}

	ERichCurveInterpMode InterpMode = RCIM_Cubic;
	const FString Interp = interpolation.TrimStartAndEnd().ToLower();
	if (Interp == TEXT("linear")) { InterpMode = RCIM_Linear; }
	else if (Interp == TEXT("constant")) { InterpMode = RCIM_Constant; }
	else if (Interp != TEXT("cubic"))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Unknown interpolation '%s'. Use cubic, linear or constant."), *interpolation));
	}

	// Keys: [[t, v], ...] or [{"time": t, "value": v}, ...]
	TArray<TSharedPtr<FJsonValue>> Items;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(keys);
	if (!FJsonSerializer::Deserialize(Reader, Items) || Items.IsEmpty())
	{
		return AnimMCP::Fail(TEXT("keys must be a non-empty JSON array such as [[0, 0], [0.5, 1]] or [{\"time\": 0, \"value\": 0}]."));
	}
	const float Length = Source->GetPlayLength();
	TArray<FRichCurveKey> CurveKeys;
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		double KeyTime = 0.0;
		double KeyValue = 0.0;
		const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
		const TSharedPtr<FJsonObject>* Object = nullptr;
		bool bParsed = false;
		if (Items[Index]->TryGetArray(Pair) && Pair->Num() == 2)
		{
			bParsed = (*Pair)[0]->TryGetNumber(KeyTime) && (*Pair)[1]->TryGetNumber(KeyValue);
		}
		else if (Items[Index]->TryGetObject(Object))
		{
			bParsed = (*Object)->TryGetNumberField(TEXT("time"), KeyTime) && (*Object)->TryGetNumberField(TEXT("value"), KeyValue) && (*Object)->Values.Num() == 2;
		}
		if (!bParsed)
		{
			return AnimMCP::Fail(FString::Printf(TEXT("keys[%d] must be [time, value] or {\"time\": t, \"value\": v}."), Index));
		}
		if (KeyTime < 0.0 || KeyTime > Length + UE_KINDA_SMALL_NUMBER)
		{
			return AnimMCP::Fail(FString::Printf(TEXT("keys[%d]: time %g is outside the animation (0 to %g seconds)."), Index, KeyTime, Length));
		}
		if (CurveKeys.ContainsByPredicate([KeyTime](const FRichCurveKey& Key) { return FMath::IsNearlyEqual(Key.Time, (float)KeyTime); }))
		{
			return AnimMCP::Fail(FString::Printf(TEXT("keys[%d]: there is already a key at time %g."), Index, KeyTime));
		}
		FRichCurveKey Key((float)FMath::Min(KeyTime, (double)Length), (float)KeyValue);
		Key.InterpMode = InterpMode;
		CurveKeys.Add(Key);
	}
	CurveKeys.Sort([](const FRichCurveKey& A, const FRichCurveKey& B) { return A.Time < B.Time; });

	FEditTarget Target;
	if (!PlanEditTarget(Source, in_place, output_path, Target, Error))
	{
		return AnimMCP::Fail(Error);
	}
	UAnimSequence* Sequence = ApplyEditTarget(Source, Target, Error);
	if (!Sequence)
	{
		return AnimMCP::Fail(Error);
	}

	const FAnimationCurveIdentifier CurveId(FName(*curve_name.TrimStartAndEnd()), ERawCurveTrackTypes::RCT_Float);
	const bool bCreated = Sequence->GetDataModel()->FindFloatCurve(CurveId) == nullptr;
	bool bSet = false;
	{
		const FScopedTransaction Transaction(LOCTEXT("SetCurve", "AnimMCP: Set Curve"));
		Sequence->Modify();
		IAnimationDataController& Controller = Sequence->GetController();
		IAnimationDataController::FScopedBracket Bracket(Controller, LOCTEXT("SetCurveBracket", "Set curve keys"));
		if (bCreated)
		{
			Controller.AddCurve(CurveId, AACF_DefaultCurve);
		}
		bSet = Controller.SetCurveKeys(CurveId, CurveKeys);
	}
	if (!bSet)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("The animation data controller rejected curve '%s'.%s"), *curve_name,
			Target.bInPlace ? TEXT("") : *FString::Printf(TEXT(" The copy '%s' was created but not changed."), *Sequence->GetPathName())));
	}
	Sequence->MarkPackageDirty();

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	AddTargetFields(Payload, Source, Sequence);
	Payload->SetStringField(TEXT("curve"), CurveId.CurveName.ToString());
	Payload->SetBoolField(TEXT("created"), bCreated);
	Payload->SetNumberField(TEXT("key_count"), CurveKeys.Num());
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimDataToolset::anim_remove_curve(const FString& animation_path, const FString& curve_name, bool in_place, const FString& output_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimSequence* Source = LoadForEdit<UAnimSequence>(animation_path, in_place, Error);
	if (!Source)
	{
		return AnimMCP::Fail(Error);
	}
	const FAnimationCurveIdentifier CurveId(FName(*curve_name.TrimStartAndEnd()), ERawCurveTrackTypes::RCT_Float);
	if (!Source->GetDataModel() || !Source->GetDataModel()->FindFloatCurve(CurveId))
	{
		TArray<FString> Names;
		if (Source->GetDataModel())
		{
			for (const FFloatCurve& Curve : Source->GetDataModel()->GetFloatCurves())
			{
				Names.Add(Curve.GetName().ToString());
			}
		}
		return AnimMCP::Fail(FString::Printf(TEXT("'%s' has no curve '%s'. Curves: %s."), *animation_path, *curve_name, Names.IsEmpty() ? TEXT("none") : *FString::Join(Names, TEXT(", "))));
	}

	FEditTarget Target;
	if (!PlanEditTarget(Source, in_place, output_path, Target, Error))
	{
		return AnimMCP::Fail(Error);
	}
	UAnimSequence* Sequence = ApplyEditTarget(Source, Target, Error);
	if (!Sequence)
	{
		return AnimMCP::Fail(Error);
	}

	bool bRemoved = false;
	{
		const FScopedTransaction Transaction(LOCTEXT("RemoveCurve", "AnimMCP: Remove Curve"));
		Sequence->Modify();
		IAnimationDataController& Controller = Sequence->GetController();
		IAnimationDataController::FScopedBracket Bracket(Controller, LOCTEXT("RemoveCurveBracket", "Remove curve"));
		bRemoved = Controller.RemoveCurve(CurveId);
	}
	if (!bRemoved)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("The animation data controller could not remove curve '%s'."), *curve_name));
	}
	Sequence->MarkPackageDirty();

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	AddTargetFields(Payload, Source, Sequence);
	Payload->SetStringField(TEXT("removed_curve"), CurveId.CurveName.ToString());
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimDataToolset::anim_add_notify(const FString& animation_path, const FString& name, float time, int32 frame, const FString& track, const FString& notify_class, float duration,
	bool in_place, const FString& output_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimSequenceBase* Source = LoadForEdit<UAnimSequenceBase>(animation_path, in_place, Error);
	if (!Source)
	{
		return AnimMCP::Fail(Error);
	}
	TOptional<float> Time;
	if (!ResolveNotifyTime(Source, time, frame, /*bRequired*/ true, Time, Error))
	{
		return AnimMCP::Fail(Error);
	}
	UClass* Class = nullptr;
	if (!AnimMCP::IsUnset(notify_class))
	{
		Class = ResolveNotifyClass(notify_class, Error);
		if (!Class)
		{
			return AnimMCP::Fail(Error);
		}
	}
	else if (AnimMCP::IsUnset(name))
	{
		return AnimMCP::Fail(TEXT("A named notify needs a name (or pass notify_class)."));
	}
	const bool bState = Class && Class->IsChildOf(UAnimNotifyState::StaticClass());
	if (bState && duration <= 0.f)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("%s is a notify state; give it a duration > 0."), *Class->GetName()));
	}
	if (!bState && duration != 0.f)
	{
		return AnimMCP::Fail(TEXT("duration only applies to notify states (notify_class derived from AnimNotifyState). Leave it at 0."));
	}
	if (bState && Time.GetValue() + duration > Source->GetPlayLength() + UE_KINDA_SMALL_NUMBER)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("The notify state would end at %g, past the end of the animation (%g)."), Time.GetValue() + duration, Source->GetPlayLength()));
	}
	const FString TrackName = AnimMCP::IsUnset(track) ? FString(TEXT("1")) : track.TrimStartAndEnd();

	FEditTarget Target;
	if (!PlanEditTarget(Source, in_place, output_path, Target, Error))
	{
		return AnimMCP::Fail(Error);
	}
	UAnimSequenceBase* Anim = ApplyEditTarget(Source, Target, Error);
	if (!Anim)
	{
		return AnimMCP::Fail(Error);
	}

	FGuid NewGuid;
	{
		const FScopedTransaction Transaction(LOCTEXT("AddNotify", "AnimMCP: Add Notify"));
		Anim->Modify();
		const int32 TrackIndex = FindOrAddNotifyTrack(Anim, FName(*TrackName));
		FAnimNotifyEvent& Notify = Anim->Notifies.AddDefaulted_GetRef();
		PlaceNotify(Anim, Notify, Time.GetValue());
		Notify.TrackIndex = TrackIndex;
		Notify.Guid = FGuid::NewGuid();
		NewGuid = Notify.Guid;
		if (bState)
		{
			Notify.NotifyStateClass = NewObject<UAnimNotifyState>(Anim, Class, NAME_None, RF_Transactional);
			Notify.NotifyName = FName(*Notify.NotifyStateClass->GetNotifyName());
			SetNotifyDuration(Anim, Notify, duration);
		}
		else if (Class)
		{
			Notify.Notify = NewObject<UAnimNotify>(Anim, Class, NAME_None, RF_Transactional);
			Notify.NotifyName = FName(*Notify.Notify->GetNotifyName());
		}
		if (!AnimMCP::IsUnset(name))
		{
			Notify.NotifyName = FName(*name.TrimStartAndEnd());
		}
		Anim->SortNotifies();
		Anim->RefreshCacheData();
		Anim->MarkPackageDirty();
	}

	const int32 Index = Anim->Notifies.IndexOfByPredicate([&NewGuid](const FAnimNotifyEvent& Notify) { return Notify.Guid == NewGuid; });
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	AddTargetFields(Payload, Source, Anim);
	Payload->SetObjectField(TEXT("notify"), NotifyToJson(Anim, Anim->Notifies[Index]));
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimDataToolset::anim_update_notify(const FString& animation_path, const FString& notify, const FString& name, float time, int32 frame, const FString& track, float duration,
	bool in_place, const FString& output_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimSequenceBase* Source = LoadForEdit<UAnimSequenceBase>(animation_path, in_place, Error);
	if (!Source)
	{
		return AnimMCP::Fail(Error);
	}
	const int32 SourceIndex = FindNotifyIndex(Source, notify, Error);
	if (SourceIndex == INDEX_NONE)
	{
		return AnimMCP::Fail(Error);
	}
	const FAnimNotifyEvent& Existing = Source->Notifies[SourceIndex];
	TOptional<float> Time;
	if (!ResolveNotifyTime(Source, time, frame, /*bRequired*/ false, Time, Error))
	{
		return AnimMCP::Fail(Error);
	}
	const bool bState = Existing.NotifyStateClass != nullptr;
	if (duration >= 0.f && !bState)
	{
		return AnimMCP::Fail(TEXT("duration only applies to notify states. Leave it at -1."));
	}
	if (bState && duration == 0.f)
	{
		return AnimMCP::Fail(TEXT("A notify state needs a duration > 0."));
	}
	const float NewTime = Time.Get(Existing.GetTime());
	const float NewDuration = duration >= 0.f ? duration : Existing.GetDuration();
	if (bState && NewTime + NewDuration > Source->GetPlayLength() + UE_KINDA_SMALL_NUMBER)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("The notify state would end at %g, past the end of the animation (%g)."), NewTime + NewDuration, Source->GetPlayLength()));
	}
	if (AnimMCP::IsUnset(name) && !Time.IsSet() && AnimMCP::IsUnset(track) && duration < 0.f)
	{
		return AnimMCP::Fail(TEXT("Nothing to change: give a new name, time or frame, track or duration."));
	}
	FEditTarget Target;
	if (!PlanEditTarget(Source, in_place, output_path, Target, Error))
	{
		return AnimMCP::Fail(Error);
	}
	UAnimSequenceBase* Anim = ApplyEditTarget(Source, Target, Error);
	if (!Anim)
	{
		return AnimMCP::Fail(Error);
	}

	FGuid Guid;
	{
		const FScopedTransaction Transaction(LOCTEXT("UpdateNotify", "AnimMCP: Update Notify"));
		Anim->Modify();
		FAnimNotifyEvent& Notify = Anim->Notifies[SourceIndex];  // a copy keeps the notify order
		if (!Notify.Guid.IsValid())
		{
			Notify.Guid = FGuid::NewGuid();
		}
		Guid = Notify.Guid;
		if (!AnimMCP::IsUnset(name))
		{
			Notify.NotifyName = FName(*name.TrimStartAndEnd());
		}
		if (!AnimMCP::IsUnset(track))
		{
			Notify.TrackIndex = FindOrAddNotifyTrack(Anim, FName(*track.TrimStartAndEnd()));
		}
		if (Time.IsSet())
		{
			PlaceNotify(Anim, Notify, NewTime);
		}
		if (bState && (Time.IsSet() || duration >= 0.f))
		{
			SetNotifyDuration(Anim, Notify, NewDuration);
		}
		Anim->SortNotifies();
		Anim->RefreshCacheData();
		Anim->MarkPackageDirty();
	}

	const int32 Index = Anim->Notifies.IndexOfByPredicate([&Guid](const FAnimNotifyEvent& Notify) { return Notify.Guid == Guid; });
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	AddTargetFields(Payload, Source, Anim);
	Payload->SetObjectField(TEXT("notify"), NotifyToJson(Anim, Anim->Notifies[Index]));
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimDataToolset::anim_remove_notify(const FString& animation_path, const FString& notify, bool in_place, const FString& output_path)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	UAnimSequenceBase* Source = LoadForEdit<UAnimSequenceBase>(animation_path, in_place, Error);
	if (!Source)
	{
		return AnimMCP::Fail(Error);
	}
	const int32 Index = FindNotifyIndex(Source, notify, Error);
	if (Index == INDEX_NONE)
	{
		return AnimMCP::Fail(Error);
	}

	FEditTarget Target;
	if (!PlanEditTarget(Source, in_place, output_path, Target, Error))
	{
		return AnimMCP::Fail(Error);
	}
	UAnimSequenceBase* Anim = ApplyEditTarget(Source, Target, Error);
	if (!Anim)
	{
		return AnimMCP::Fail(Error);
	}

	TSharedRef<FJsonObject> Removed = MakeShared<FJsonObject>();
	{
		const FScopedTransaction Transaction(LOCTEXT("RemoveNotify", "AnimMCP: Remove Notify"));
		Anim->Modify();
		const FAnimNotifyEvent& Notify = Anim->Notifies[Index];
		Removed->SetStringField(TEXT("guid"), AnimMCP::GuidToString(Notify.Guid));
		Removed->SetStringField(TEXT("name"), Notify.NotifyName.ToString());
		Removed->SetNumberField(TEXT("time"), Notify.GetTime());
		Anim->Notifies.RemoveAt(Index);
		Anim->RefreshCacheData();
		Anim->MarkPackageDirty();
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	AddTargetFields(Payload, Source, Anim);
	Payload->SetObjectField(TEXT("removed"), Removed);
	return AnimMCP::Ok(Payload);
}

FAnimMCPResult UAnimDataToolset::anim_create_montage(const FString& folder, const FString& asset_name, const FString& animation_path, const FString& slot_name, const FString& sections)
{
	ANIMMCP_REQUIRE_GAME_THREAD();

	FString Error;
	FString PackageName;
	if (!AnimMCP::ValidateNewAssetLocation(folder, asset_name, PackageName, Error))
	{
		return AnimMCP::Fail(Error);
	}
	UAnimSequence* Sequence = AnimMCP::LoadAsset<UAnimSequence>(animation_path, /*bForWrite*/ false, Error);
	if (!Sequence)
	{
		return AnimMCP::Fail(Error);
	}
	if (!Sequence->GetSkeleton())
	{
		return AnimMCP::Fail(FString::Printf(TEXT("'%s' has no skeleton."), *animation_path));
	}
	const FName SlotName(*slot_name.TrimStartAndEnd());
	FText NameError;
	if (SlotName.IsNone() || !FName::IsValidXName(SlotName.ToString(), INVALID_NAME_CHARACTERS, &NameError))
	{
		return AnimMCP::Fail(FString::Printf(TEXT("'%s' is not a valid slot name. %s"), *slot_name, *NameError.ToString()));
	}

	// Sections: validated in full before the asset is created.
	struct FSectionSpec
	{
		FName Name;
		float Time = 0.f;
		bool bHasNext = false;
		FName Next;
	};
	TArray<FSectionSpec> Sections;
	const float Length = Sequence->GetPlayLength();
	if (!AnimMCP::IsUnset(sections))
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(sections);
		if (!FJsonSerializer::Deserialize(Reader, Items))
		{
			return AnimMCP::Fail(TEXT("sections must be a JSON array such as [{\"name\": \"Start\", \"time\": 0}, {\"name\": \"Loop\", \"time\": 0.5, \"next\": \"Loop\"}]."));
		}
		TArray<FString> Problems;
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString Name;
			double Time = 0.0;
			if (!Items[Index]->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("name"), Name) || !(*Object)->TryGetNumberField(TEXT("time"), Time))
			{
				Problems.Add(FString::Printf(TEXT("sections[%d]: needs 'name' and 'time'."), Index));
				continue;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Object)->Values)
			{
				if (Field.Key != TEXT("name") && Field.Key != TEXT("time") && Field.Key != TEXT("next"))
				{
					Problems.Add(FString::Printf(TEXT("sections[%d]: unknown field '%s'. Allowed: name, time, next."), Index, *Field.Key));
				}
			}
			FSectionSpec Section;
			Section.Name = FName(*Name.TrimStartAndEnd());
			Section.Time = (float)Time;
			if (Section.Name.IsNone() || !FName::IsValidXName(Section.Name.ToString(), INVALID_NAME_CHARACTERS))
			{
				Problems.Add(FString::Printf(TEXT("sections[%d]: '%s' is not a valid section name."), Index, *Name));
			}
			if (Time < 0.0 || Time > Length + UE_KINDA_SMALL_NUMBER)
			{
				Problems.Add(FString::Printf(TEXT("sections[%d]: time %g is outside the animation (0 to %g seconds)."), Index, Time, Length));
			}
			if (Sections.ContainsByPredicate([&Section](const FSectionSpec& Other) { return Other.Name == Section.Name; }))
			{
				Problems.Add(FString::Printf(TEXT("sections[%d]: the name '%s' is used twice."), Index, *Name));
			}
			if (Sections.ContainsByPredicate([&Section](const FSectionSpec& Other) { return FMath::IsNearlyEqual(Other.Time, Section.Time); }))
			{
				Problems.Add(FString::Printf(TEXT("sections[%d]: another section already starts at %g."), Index, Time));
			}
			FString Next;
			if ((*Object)->TryGetStringField(TEXT("next"), Next))
			{
				Section.bHasNext = true;
				Section.Next = AnimMCP::IsUnset(Next) ? NAME_None : FName(*Next.TrimStartAndEnd());
			}
			Sections.Add(Section);
		}
		for (const FSectionSpec& Section : Sections)
		{
			if (!Section.Next.IsNone() && !Sections.ContainsByPredicate([&Section](const FSectionSpec& Other) { return Other.Name == Section.Next; }))
			{
				Problems.Add(FString::Printf(TEXT("section '%s': next section '%s' is not in the list."), *Section.Name.ToString(), *Section.Next.ToString()));
			}
		}
		if (!Problems.IsEmpty())
		{
			return AnimMCP::Fail(FString::Printf(TEXT("Nothing was created. The sections have %d problem(s):\n- %s"), Problems.Num(), *FString::Join(Problems, TEXT("\n- "))));
		}
	}
	Sections.Sort([](const FSectionSpec& A, const FSectionSpec& B) { return A.Time < B.Time; });
	if (Sections.IsEmpty() || Sections[0].Time > UE_KINDA_SMALL_NUMBER)
	{
		const FName DefaultName = Sections.ContainsByPredicate([](const FSectionSpec& Section) { return Section.Name == TEXT("Default"); }) ? FName(TEXT("Start")) : FName(TEXT("Default"));
		Sections.Insert({ DefaultName, 0.f, false, NAME_None }, 0);
	}

	UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
	Factory->SourceAnimation = Sequence;
	Factory->TargetSkeleton = Sequence->GetSkeleton();

	const FScopedTransaction Transaction(LOCTEXT("CreateMontage", "AnimMCP: Create Montage"));
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UAnimMontage* Montage = Cast<UAnimMontage>(AssetTools.CreateAsset(asset_name, FPackageName::GetLongPackagePath(PackageName), UAnimMontage::StaticClass(), Factory, NAME_None, /*bOverwriteExisting*/ false));
	if (!Montage)
	{
		return AnimMCP::Fail(FString::Printf(TEXT("Failed to create montage '%s'."), *PackageName));
	}

	Montage->Modify();
	Montage->SlotAnimTracks[0].SlotName = SlotName;
	Montage->CompositeSections.Reset();
	for (const FSectionSpec& Section : Sections)
	{
		Montage->AddAnimCompositeSection(Section.Name, Section.Time);  // links each section to the next by default
	}
	if (!Montage->CompositeSections.IsEmpty())
	{
		Montage->CompositeSections.Last().NextSectionName = NAME_None;
	}
	for (const FSectionSpec& Section : Sections)
	{
		const int32 SectionIndex = Montage->GetSectionIndex(Section.Name);
		if (Section.bHasNext && SectionIndex != INDEX_NONE)
		{
			Montage->CompositeSections[SectionIndex].NextSectionName = Section.Next;
		}
	}
	Montage->PostEditChange();
	Montage->MarkPackageDirty();

	TArray<FString> Warnings;
	const bool bSlotOnSkeleton = Sequence->GetSkeleton()->ContainsSlotName(SlotName);
	if (!bSlotOnSkeleton)
	{
		Warnings.Add(FString::Printf(TEXT("Slot '%s' is not registered on skeleton %s. The montage still plays through a Slot node with that name; add the slot in the Anim Slot Manager to pick it from lists."),
			*SlotName.ToString(), *Sequence->GetSkeleton()->GetName()));
	}

	TArray<TSharedRef<FJsonObject>> SectionItems;
	for (const FCompositeSection& Section : Montage->CompositeSections)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Section.SectionName.ToString());
		Item->SetNumberField(TEXT("time"), Section.GetTime());
		Item->SetStringField(TEXT("next"), Section.NextSectionName.IsNone() ? FString() : Section.NextSectionName.ToString());
		SectionItems.Add(Item);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("path"), Montage->GetPathName());
	Payload->SetStringField(TEXT("skeleton"), Sequence->GetSkeleton()->GetPathName());
	Payload->SetStringField(TEXT("animation"), Sequence->GetPathName());
	Payload->SetStringField(TEXT("slot_name"), Montage->SlotAnimTracks[0].SlotName.ToString());
	Payload->SetBoolField(TEXT("slot_on_skeleton"), bSlotOnSkeleton);
	Payload->SetNumberField(TEXT("length"), Montage->GetPlayLength());
	Payload->SetArrayField(TEXT("sections"), AnimMCP::ToJsonArray(SectionItems));
	Payload->SetArrayField(TEXT("warnings"), AnimMCP::ToJsonArray(Warnings));
	Payload->SetBoolField(TEXT("saved"), false);
	return AnimMCP::Ok(Payload);
}

#undef LOCTEXT_NAMESPACE
