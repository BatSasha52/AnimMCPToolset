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

	// ---- Editing. Every edit below works on a new copy of the animation unless in_place is true. --------------------------
	// The copy is created next to the source as '<Name>_Edited' (output_path 'auto') or at output_path; an existing asset is never
	// overwritten. To make several edits, edit once to get the copy, then pass the copy's path with in_place=true.
	// Edits are undoable and only mark the asset dirty; save with anim_save_asset.

	/**
	 * Removes the travel of a bone so the animation plays in place (e.g. a walk that moves forward). Only the chosen bone's keys
	 * change, and the travel is removed in component space, so it works whether the bone is the root or the hips below it.
	 * @param animation_path Path of the AnimSequence. With in_place=false it may be anywhere; the copy goes under /Game.
	 * @param bone Bone to fix. 'auto' = the root if the root moves, otherwise the hips (found by name: pelvis, hips, hip).
	 * @param axes Component-space axes to remove travel on: any of x, y, z, e.g. 'xy' (horizontal, the usual choice for walks and runs) or 'x'.
	 * @param mode 'linear' removes the straight-line drift from the first to the last frame and keeps sway and bob;
	 *        'flatten' holds the first frame's position on those axes (removes all motion on them).
	 * @param in_place false = edit a new copy (safe default); true = edit this asset (must be under /Game).
	 * @param output_path For a copy: full path of the new asset, e.g. '/Game/Anims/Walk_InPlace'. 'auto' = '<source folder>/<Name>_Edited'.
	 * @return Result: {path, source, in_place, bone, reason, axes, mode, keys_changed, before: {delta, distance}, after: {delta, distance}, warnings}.
	 */
	UFUNCTION(Category = "AnimMCP|AnimData", meta = (AICallable))
	static FAnimMCPResult anim_remove_bone_travel(const FString& animation_path, const FString& bone = TEXT("auto"), const FString& axes = TEXT("xy"), const FString& mode = TEXT("linear"),
		bool in_place = false, const FString& output_path = TEXT("auto"));

	/**
	 * Creates a float curve on an animation sequence, or replaces all keys of an existing one.
	 * @param animation_path Path of the AnimSequence.
	 * @param curve_name Curve name, e.g. 'Speed' or 'FootPlant_L'.
	 * @param keys JSON array of keys: [[time, value], ...] or [{"time": 0.0, "value": 1.0}, ...]. Times in seconds within the sequence, no duplicates.
	 * @param interpolation 'cubic' (smooth, automatic tangents), 'linear' or 'constant'.
	 * @param in_place false = edit a new copy (safe default); true = edit this asset (must be under /Game).
	 * @param output_path For a copy: full path of the new asset. 'auto' = '<source folder>/<Name>_Edited'.
	 * @return Result: {path, source, in_place, curve, created, key_count}.
	 */
	UFUNCTION(Category = "AnimMCP|AnimData", meta = (AICallable))
	static FAnimMCPResult anim_set_curve(const FString& animation_path, const FString& curve_name, const FString& keys, const FString& interpolation = TEXT("cubic"),
		bool in_place = false, const FString& output_path = TEXT("auto"));

	/**
	 * Removes a float curve from an animation sequence.
	 * @param animation_path Path of the AnimSequence.
	 * @param curve_name Name of an existing curve (see anim_get_animation_info).
	 * @param in_place false = edit a new copy (safe default); true = edit this asset (must be under /Game).
	 * @param output_path For a copy: full path of the new asset. 'auto' = '<source folder>/<Name>_Edited'.
	 * @return Result: {path, source, in_place, removed_curve}.
	 */
	UFUNCTION(Category = "AnimMCP|AnimData", meta = (AICallable))
	static FAnimMCPResult anim_remove_curve(const FString& animation_path, const FString& curve_name, bool in_place = false, const FString& output_path = TEXT("auto"));

	/**
	 * Adds a notify to an animation sequence or montage. Give either time or frame.
	 * A notify without a class is a named notify, received in Animation Blueprints as the event 'AnimNotify_<name>'.
	 * @param animation_path Path of an AnimSequence or AnimMontage.
	 * @param name Notify name, e.g. 'Footstep_L'. Required without notify_class; with a class, 'none' uses the class's name.
	 * @param time Time in seconds. -1 = use frame.
	 * @param frame Frame number. -1 = use time.
	 * @param track Notify track name; created if missing.
	 * @param notify_class 'none' for a named notify, or a UAnimNotify / UAnimNotifyState class (name or path, e.g. 'AnimNotify_PlaySound', '/Script/Engine.AnimNotifyState_Trail').
	 * @param duration Seconds, for notify states (must be > 0 for a state, 0 otherwise).
	 * @param in_place false = edit a new copy (safe default); true = edit this asset (must be under /Game).
	 * @param output_path For a copy: full path of the new asset. 'auto' = '<source folder>/<Name>_Edited'.
	 * @return Result: {path, source, in_place, notify: {guid, name, class, is_state, time, frame, duration, track}}.
	 */
	UFUNCTION(Category = "AnimMCP|AnimData", meta = (AICallable))
	static FAnimMCPResult anim_add_notify(const FString& animation_path, const FString& name, float time = -1.f, int32 frame = -1, const FString& track = TEXT("1"),
		const FString& notify_class = TEXT("none"), float duration = 0.f, bool in_place = false, const FString& output_path = TEXT("auto"));

	/**
	 * Changes a notify: its name, time (or frame), track or duration. Unchanged fields keep their values.
	 * @param animation_path Path of an AnimSequence or AnimMontage.
	 * @param notify The notify's guid from anim_get_animation_info, or 'index:<n>' for its position in the notifies list.
	 * @param name New name. 'none' = keep.
	 * @param time New time in seconds. -1 = keep (or use frame).
	 * @param frame New frame number. -1 = keep (or use time).
	 * @param track New track name (created if missing). 'none' = keep.
	 * @param duration New duration in seconds, for notify states. -1 = keep.
	 * @param in_place false = edit a new copy (safe default); true = edit this asset (must be under /Game).
	 * @param output_path For a copy: full path of the new asset. 'auto' = '<source folder>/<Name>_Edited'.
	 * @return Result: {path, source, in_place, notify: {guid, name, class, is_state, time, frame, duration, track}}.
	 */
	UFUNCTION(Category = "AnimMCP|AnimData", meta = (AICallable))
	static FAnimMCPResult anim_update_notify(const FString& animation_path, const FString& notify, const FString& name = TEXT("none"), float time = -1.f, int32 frame = -1,
		const FString& track = TEXT("none"), float duration = -1.f, bool in_place = false, const FString& output_path = TEXT("auto"));

	/**
	 * Removes a notify.
	 * @param animation_path Path of an AnimSequence or AnimMontage.
	 * @param notify The notify's guid from anim_get_animation_info, or 'index:<n>'.
	 * @param in_place false = edit a new copy (safe default); true = edit this asset (must be under /Game).
	 * @param output_path For a copy: full path of the new asset. 'auto' = '<source folder>/<Name>_Edited'.
	 * @return Result: {path, source, in_place, removed: {guid, name, time}}.
	 */
	UFUNCTION(Category = "AnimMCP|AnimData", meta = (AICallable))
	static FAnimMCPResult anim_remove_notify(const FString& animation_path, const FString& notify, bool in_place = false, const FString& output_path = TEXT("auto"));

	/**
	 * Creates an Animation Montage from a sequence, with a slot and sections. The asset is created in memory and marked dirty; save it with anim_save_asset.
	 * @param folder Destination folder under /Game.
	 * @param asset_name New asset name, e.g. 'AM_Attack'. Must not already exist.
	 * @param animation_path The AnimSequence the montage plays (may be anywhere, including /Engine).
	 * @param slot_name Montage slot, e.g. 'DefaultSlot' or 'UpperBody'. The Animation Blueprint needs a Slot node with this name.
	 * @param sections JSON array of sections: [{"name": "Start", "time": 0}, {"name": "Loop", "time": 0.5, "next": "Loop"}, {"name": "End", "time": 1.2}].
	 *        Without 'next' a section continues into the following one; "next": "" ends the montage there. A 'Default' section is added at 0
	 *        if no section starts at 0. 'none' = one 'Default' section.
	 * @return Result: {path, skeleton, slot_name, slot_on_skeleton, length, sections: [{name, time, next}], warnings}.
	 */
	UFUNCTION(Category = "AnimMCP|AnimData", meta = (AICallable))
	static FAnimMCPResult anim_create_montage(const FString& folder, const FString& asset_name, const FString& animation_path, const FString& slot_name = TEXT("DefaultSlot"),
		const FString& sections = TEXT("none"));
};
