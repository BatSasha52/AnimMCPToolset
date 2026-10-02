// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "AnimMCPTypes.h"

#include "AnimAssetToolset.generated.h"

/**
 * Phase 4 - creating Animation Blueprints and blend spaces, managing blueprint variables, compiling and saving.
 * New assets can only be created under /Game and never overwrite an existing asset. Assets are never deleted.
 * Changes are only written to disk by anim_save_asset; every other tool just marks assets dirty.
 * Every tool returns {Success, Result, Error}.
 */
UCLASS(MinimalAPI)
class UAnimAssetToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Creates a new Animation Blueprint for a skeleton. The asset is created in memory and marked dirty; call anim_save_asset to write it.
	 * @param folder Destination folder under /Game, e.g. '/Game/Characters/Hero/Animation'.
	 * @param asset_name New asset name, e.g. 'ABP_Hero'. Must not already exist.
	 * @param skeleton_path Path of the target Skeleton, or of a SkeletalMesh whose skeleton to use.
	 * @param parent_class Parent AnimInstance class: a class name ('AnimInstance'), a class path ('/Script/MyGame.MyAnimInstance'), or another Animation Blueprint path.
	 * @param preview_mesh_path SkeletalMesh to use as the editor preview mesh. 'none' = leave unset.
	 * @return Result: {path, skeleton, parent_class}.
	 */
	UFUNCTION(Category = "AnimMCP|Assets", meta = (AICallable))
	static FAnimMCPResult anim_create_anim_blueprint(const FString& folder, const FString& asset_name, const FString& skeleton_path, const FString& parent_class = TEXT("AnimInstance"), const FString& preview_mesh_path = TEXT("none"));

	/**
	 * Adds a member variable to an Animation Blueprint (e.g. a bool 'bIsInAir' or float 'Speed' to drive transitions and blends).
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param name Variable name. Must not already exist on the blueprint or its parent class.
	 * @param type One of: bool, byte, int, int64, float, name, string, text, vector, vector2d, rotator, transform, linearcolor, or 'object:<class path>' (e.g. 'object:/Script/Engine.AnimSequence').
	 * @param default_value Default in Unreal text format, e.g. 'true', '0.0', 'X=0,Y=0,Z=0'. 'none' = the type's zero value.
	 * @param variable_category Category shown in the My Blueprint panel. 'Default' = no category.
	 * @return Result: {name, type, default_value}.
	 */
	UFUNCTION(Category = "AnimMCP|Assets", meta = (AICallable))
	static FAnimMCPResult anim_add_variable(const FString& blueprint_path, const FString& name, const FString& type, const FString& default_value = TEXT("none"), const FString& variable_category = TEXT("Default"));

	/**
	 * Removes a member variable declared on this blueprint. By default refuses if any graph still reads or writes it.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param name Variable name.
	 * @param force If true, remove even if the variable is still used; affected nodes will error on compile until fixed.
	 * @return Result: {removed, was_used}.
	 */
	UFUNCTION(Category = "AnimMCP|Assets", meta = (AICallable))
	static FAnimMCPResult anim_remove_variable(const FString& blueprint_path, const FString& name, bool force = false);

	/**
	 * Sets the default value of a member variable declared on this blueprint.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param name Variable name.
	 * @param value New default in Unreal text format, e.g. 'true', '250.0', 'P=0,Y=90,R=0', or an asset path for object variables.
	 * @return Result: {name, default_value}.
	 */
	UFUNCTION(Category = "AnimMCP|Assets", meta = (AICallable))
	static FAnimMCPResult anim_set_variable_default(const FString& blueprint_path, const FString& name, const FString& value);

	/**
	 * Creates a new blend space for a skeleton (1D or 2D) with the given axes. Add samples with anim_add_blendspace_sample.
	 * @param folder Destination folder under /Game.
	 * @param asset_name New asset name, e.g. 'BS_Locomotion'. Must not already exist.
	 * @param skeleton_path Path of the target Skeleton, or of a SkeletalMesh whose skeleton to use.
	 * @param dimensions 1 for a 1D blend space (X axis only) or 2 for a 2D blend space.
	 * @param x_axis_name Display name of the horizontal axis, e.g. 'Speed'.
	 * @param x_min Minimum horizontal axis value.
	 * @param x_max Maximum horizontal axis value. Must be greater than x_min.
	 * @param x_grid Number of grid divisions on the horizontal axis (>= 1).
	 * @param y_axis_name Display name of the vertical axis (2D only), e.g. 'Direction'.
	 * @param y_min Minimum vertical axis value (2D only).
	 * @param y_max Maximum vertical axis value (2D only). Must be greater than y_min.
	 * @param y_grid Number of grid divisions on the vertical axis (2D only, >= 1).
	 * @return Result: {path, class, skeleton}.
	 */
	UFUNCTION(Category = "AnimMCP|Assets", meta = (AICallable))
	static FAnimMCPResult anim_create_blendspace(const FString& folder, const FString& asset_name, const FString& skeleton_path, int32 dimensions = 2,
		const FString& x_axis_name = TEXT("Speed"), float x_min = 0.f, float x_max = 600.f, int32 x_grid = 4,
		const FString& y_axis_name = TEXT("Direction"), float y_min = -180.f, float y_max = 180.f, int32 y_grid = 4);

	/**
	 * Adds an animation sample to a blend space at the given axis coordinates.
	 * @param blendspace_path Asset path of the blend space (must be under /Game).
	 * @param animation_path Path of an AnimSequence using a compatible skeleton.
	 * @param x Horizontal axis value; must be inside the axis range.
	 * @param y Vertical axis value; ignored for 1D blend spaces.
	 * @return Result: {sample_index, sample_count}.
	 */
	UFUNCTION(Category = "AnimMCP|Assets", meta = (AICallable))
	static FAnimMCPResult anim_add_blendspace_sample(const FString& blendspace_path, const FString& animation_path, float x, float y = 0.f);

	/**
	 * Compiles an Animation Blueprint and reports errors and warnings. Does not save.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @return Result: {status, num_errors, num_warnings, messages: [{severity, message, source}]}. Success is true even when the blueprint has compile errors; check num_errors.
	 *         source (when the message names a node): {node_guid, node_title, node_class, graph, graph_guid, pin, state_machine, state, transition (+ their _guid), location},
	 *         where location reads like 'AnimGraph > state machine Locomotion > state JumpUp > Sequence Player'.
	 */
	UFUNCTION(Category = "AnimMCP|Assets", meta = (AICallable))
	static FAnimMCPResult anim_compile_blueprint(const FString& blueprint_path);

	/**
	 * Saves one asset to disk. This is the only tool that writes files. Works for any asset under /Game (Animation Blueprints, blend spaces, ...).
	 * @param asset_path Asset path to save, e.g. '/Game/Characters/ABP_Hero'.
	 * @return Result: {path, saved}. saved is false if the asset had no unsaved changes.
	 */
	UFUNCTION(Category = "AnimMCP|Assets", meta = (AICallable))
	static FAnimMCPResult anim_save_asset(const FString& asset_path);
};
