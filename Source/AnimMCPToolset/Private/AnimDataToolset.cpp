// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#include "AnimDataToolset.h"

#include "AnimMCPHelpers.h"

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"

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

		FTransform Local(int32 BoneIndex, double Time) const
		{
			const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
			if (!Tracked.Contains(BoneName))
			{
				return RefSkeleton.GetRefBonePose()[BoneIndex];
			}
			const double Clamped = FMath::Clamp(Time, 0.0, Model->GetPlayLength());
			return Model->EvaluateBoneTrackTransform(BoneName, Model->GetFrameRate().AsFrameTime(Clamped), Sequence->Interpolation);
		}

		FTransform Component(int32 BoneIndex, double Time) const
		{
			FTransform Result = Local(BoneIndex, Time);
			for (int32 Parent = RefSkeleton.GetParentIndex(BoneIndex); Parent != INDEX_NONE; Parent = RefSkeleton.GetParentIndex(Parent))
			{
				Result = Result * Local(Parent, Time);
			}
			return Result;
		}

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

	/** Where a bone goes over the whole sequence, in component space. */
	TSharedRef<FJsonObject> MeasureTravel(const FBoneSampler& Sampler, int32 BoneIndex, const IAnimationDataModel* Model)
	{
		const int32 NumKeys = FMath::Max(Model->GetNumberOfKeys(), 1);
		const double Length = Model->GetPlayLength();
		FVector Previous = Sampler.Component(BoneIndex, 0.0).GetLocation();
		const FVector Start = Previous;
		double PathLength = 0.0;
		for (int32 Key = 1; Key < NumKeys; ++Key)
		{
			const FVector Location = Sampler.Component(BoneIndex, Model->GetFrameRate().AsSeconds(FFrameNumber(Key))).GetLocation();
			PathLength += FVector::Distance(Previous, Location);
			Previous = Location;
		}
		const FVector End = NumKeys > 1 ? Previous : Sampler.Component(BoneIndex, Length).GetLocation();
		const FVector Delta = End - Start;

		TSharedRef<FJsonObject> DeltaJson = MakeShared<FJsonObject>();
		DeltaJson->SetNumberField(TEXT("x"), Delta.X);
		DeltaJson->SetNumberField(TEXT("y"), Delta.Y);
		DeltaJson->SetNumberField(TEXT("z"), Delta.Z);

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

#undef LOCTEXT_NAMESPACE
