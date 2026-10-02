// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "AnimMCPTypes.h"

#include "AnimInspectToolset.generated.h"

/**
 * Phase 1 - read-only inspection of Animation Blueprints, skeletons and animation assets.
 * Use these tools first to discover asset paths, graph names, node GUIDs and pin names before editing.
 * Nodes are always identified by node_guid and pins by name. Nothing here modifies any asset.
 * Every tool returns {Success, Result, Error}.
 */
UCLASS(MinimalAPI)
class UAnimInspectToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Lists Animation Blueprint assets in the project.
	 * @param folder Content folder to search recursively, e.g. '/Game' or '/Game/Characters'.
	 * @param skeleton_path Only return Animation Blueprints targeting this skeleton (Skeleton, SkeletalMesh or AnimBlueprint path). '*' = any skeleton.
	 * @return Result.anim_blueprints: [{path, name, skeleton, parent_class}].
	 */
	UFUNCTION(Category = "AnimMCP|Inspect", meta = (AICallable))
	static FAnimMCPResult anim_list_anim_blueprints(const FString& folder = TEXT("/Game"), const FString& skeleton_path = TEXT("*"));

	/**
	 * Returns summary information about one Animation Blueprint: target skeleton, parent class, preview mesh, compile status, graph and variable counts.
	 * @param blueprint_path Asset path of the Animation Blueprint, e.g. '/Game/Characters/ABP_Hero'.
	 * @return Result: {path, skeleton, parent_class, preview_mesh, status, is_template, graph_count, variable_count}.
	 */
	UFUNCTION(Category = "AnimMCP|Inspect", meta = (AICallable))
	static FAnimMCPResult anim_get_blueprint_info(const FString& blueprint_path);

	/**
	 * Lists every graph in an Animation Blueprint, including nested state machine, state, transition and conduit graphs.
	 * @param blueprint_path Asset path of the Animation Blueprint.
	 * @return Result.graphs: [{name, graph_guid, class, node_count, owner_node_guid?, parent_graph?}]. Use name or graph_guid with other tools.
	 */
	UFUNCTION(Category = "AnimMCP|Inspect", meta = (AICallable))
	static FAnimMCPResult anim_list_graphs(const FString& blueprint_path);

	/**
	 * Lists the nodes in one graph.
	 * @param blueprint_path Asset path of the Animation Blueprint.
	 * @param graph Graph name (e.g. 'AnimGraph', 'EventGraph') or graph_guid from anim_list_graphs.
	 * @param include_pins If true, include every pin with its type, default value and links. Larger output.
	 * @return Result.nodes: [{node_guid, class, title, x, y, state_name?, animation_asset?, sub_graphs?, pins?}].
	 */
	UFUNCTION(Category = "AnimMCP|Inspect", meta = (AICallable))
	static FAnimMCPResult anim_list_nodes(const FString& blueprint_path, const FString& graph, bool include_pins = false);

	/**
	 * Returns full details for one node, including all pins (name, direction, type, default value, links to other node_guid/pin_name).
	 * @param blueprint_path Asset path of the Animation Blueprint.
	 * @param node_guid GUID of the node, as returned by anim_list_nodes.
	 * @return Result: node object with pins.
	 */
	UFUNCTION(Category = "AnimMCP|Inspect", meta = (AICallable))
	static FAnimMCPResult anim_get_node(const FString& blueprint_path, const FString& node_guid);

	/**
	 * Lists the bones of a skeleton in hierarchy order.
	 * @param asset_path Path of a Skeleton, SkeletalMesh, AnimBlueprint or animation asset; its skeleton is used.
	 * @return Result: {skeleton, bones: [{index, name, parent}]}.
	 */
	UFUNCTION(Category = "AnimMCP|Inspect", meta = (AICallable))
	static FAnimMCPResult anim_list_skeleton_bones(const FString& asset_path);

	/**
	 * Describes a skeleton: bones, virtual bones, sockets, compatible skeletons and montage slot groups. Read-only.
	 * Use it to check why animations do not show up for a mesh (compatible_skeletons) or which slot names a Slot node can use.
	 * @param asset_path Path of a Skeleton, SkeletalMesh, AnimBlueprint or animation asset; its skeleton is used.
	 * @param include_bones If false, bones are counted but not listed (useful for large skeletons).
	 * @return Result: {skeleton, bone_count, bones: [{index, name, parent}], virtual_bones: [{name, source, target}],
	 *         sockets: [{name, bone, location, rotation, scale}], compatible_skeletons, use_retarget_modes_from_compatible, slot_groups: [{group, slots}]}.
	 */
	UFUNCTION(Category = "AnimMCP|Inspect", meta = (AICallable))
	static FAnimMCPResult anim_get_skeleton_info(const FString& asset_path, bool include_bones = true);

	/**
	 * Lists animation assets (sequences, montages, blend spaces, aim offsets, ...) compatible with a skeleton.
	 * @param skeleton_path Path of a Skeleton, SkeletalMesh or AnimBlueprint whose skeleton to match.
	 * @param folder Content folder to search recursively.
	 * @param asset_class Class filter, e.g. 'AnimSequence', 'BlendSpace', 'AnimMontage'. 'AnimationAsset' = all animation assets.
	 * @return Result.assets: [{path, name, class}].
	 */
	UFUNCTION(Category = "AnimMCP|Inspect", meta = (AICallable))
	static FAnimMCPResult anim_list_animation_assets(const FString& skeleton_path, const FString& folder = TEXT("/Game"), const FString& asset_class = TEXT("AnimationAsset"));

	/**
	 * Lists the member variables declared in an Animation Blueprint.
	 * @param blueprint_path Asset path of the Animation Blueprint.
	 * @return Result.variables: [{name, type, default_value, category}].
	 */
	UFUNCTION(Category = "AnimMCP|Inspect", meta = (AICallable))
	static FAnimMCPResult anim_list_variables(const FString& blueprint_path);

	/**
	 * Lists animation graph node classes that can be passed to anim_add_node (e.g. AnimGraphNode_TwoWayBlend, AnimGraphNode_LayeredBoneBlend, AnimGraphNode_TwoBoneIK).
	 * @param filter Case-insensitive substring to filter class names, e.g. 'Blend' or 'IK'. '*' = all.
	 * @return Result.node_types: [{class, path, description}].
	 */
	UFUNCTION(Category = "AnimMCP|Inspect", meta = (AICallable))
	static FAnimMCPResult anim_list_node_types(const FString& filter = TEXT("*"));
};
