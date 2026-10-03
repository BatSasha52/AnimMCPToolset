// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "AnimMCPTypes.h"

#include "AnimDataToolset.generated.h"

/**
 * Phase 5 - animation data: reading and editing the contents of animation sequences (length, frames, curves, notifies,
 * root motion, bone transforms) and creating montages.
 * Bone data is read from the sequence's source data (the animation data model), not from the compressed runtime data.
 * Every tool returns {Success, Result, Error}.
 */
UCLASS(MinimalAPI)
class UAnimDataToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Describes an animation: length, frame rate, frame count, curves, notifies, root motion settings, and how far the root and hips travel.
	 * Works for sequences, montages and composites; bone tracks and travel are only reported for sequences. Read-only.
	 * @param asset_path Path of an AnimSequence, AnimMontage or AnimComposite (may be anywhere, including /Engine).
	 * @param travel_bones Bones to measure travel for, comma-separated (e.g. 'root,pelvis'). 'auto' = the root bone plus the hips, found by name
	 *        ('pelvis', 'hips' or 'hip'); bones_measured says which were used. 'none' = skip travel.
	 * @return Result: {path, class, skeleton, length, frame_rate, frame_count, key_count, additive_type, interpolation,
	 *         root_motion: {enabled, root_lock, force_root_lock}, bone_track_count,
	 *         curves: [{name, key_count, min_value, max_value}], notify_tracks: [names],
	 *         notifies: [{guid, name, class, is_state, time, frame, duration, track_index, track}],
	 *         bones_measured, travel: [{bone, has_track, start, end, delta: {x, y, z}, distance, horizontal_distance, path_length}]}.
	 *         Travel is in component space (cm): delta = end - start per axis, distance = |delta|, path_length = distance covered frame by frame.
	 */
	UFUNCTION(Category = "AnimMCP|AnimData", meta = (AICallable))
	static FAnimMCPResult anim_get_animation_info(const FString& asset_path, const FString& travel_bones = TEXT("auto"));

	/**
	 * Samples bone transforms of an animation sequence at a time. Bones without animation keys use the skeleton's reference pose. Read-only.
	 * @param asset_path Path of an AnimSequence.
	 * @param time Time in seconds, between 0 and the sequence length. For a frame number use frame / frame_rate (see anim_get_animation_info).
	 * @param bones Bone names, comma-separated (e.g. 'root,pelvis,hand_r'). '*' = every bone of the skeleton.
	 * @param space 'component' (relative to the skeletal mesh component, the usual choice) or 'parent' (relative to the parent bone, as keyed).
	 * @return Result: {time, frame, space, bones: [{name, index, parent, has_track, translation: [x, y, z], rotation: [pitch, yaw, roll],
	 *         quaternion: [x, y, z, w], scale: [x, y, z]}]}.
	 */
	UFUNCTION(Category = "AnimMCP|AnimData", meta = (AICallable))
	static FAnimMCPResult anim_sample_bones(const FString& asset_path, float time, const FString& bones = TEXT("*"), const FString& space = TEXT("component"));
};
