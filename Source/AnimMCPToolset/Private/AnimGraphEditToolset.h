// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "AnimMCPTypes.h"

#include "AnimGraphEditToolset.generated.h"

/**
 * Phase 2 - editing nodes, pins and links inside Animation Blueprint graphs (AnimGraph, EventGraph, state and transition graphs).
 * Nodes are identified by node_guid (from anim_list_nodes) and pins by name, never by index or position.
 * Every edit is a single undoable transaction and marks the asset dirty; nothing is saved until anim_save_asset is called.
 * Only assets under /Game can be edited. Compile with anim_compile_blueprint to validate changes.
 * Every tool returns {Success, Result, Error}.
 */
UCLASS(MinimalAPI)
class UAnimGraphEditToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Adds a node to a graph. Works for anim graph nodes (see anim_list_node_types) and for Blueprint nodes such as K2Node_VariableGet, K2Node_VariableSet and K2Node_CallFunction.
	 * State machine states, conduits and transitions must be created with the state machine tools instead.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param graph Graph name (e.g. 'AnimGraph') or graph_guid from anim_list_graphs.
	 * @param node_class Node class name, e.g. 'AnimGraphNode_TwoWayBlend', 'AnimGraphNode_SequencePlayer', 'K2Node_VariableGet', or a full path like '/Script/AnimGraph.AnimGraphNode_Slot'.
	 * @param x Horizontal position in graph units.
	 * @param y Vertical position in graph units.
	 * @param variable_name Required for K2Node_VariableGet/K2Node_VariableSet: name of a member variable of this blueprint (or its parent class). 'none' for other node types.
	 * @param function_name Required for K2Node_CallFunction: 'ClassName.FunctionName' (e.g. 'KismetMathLibrary.Not_PreBool') or just 'FunctionName' for a function on this blueprint. 'none' for other node types.
	 * @return Result: the new node with its node_guid and pins.
	 */
	UFUNCTION(Category = "AnimMCP|Graph", meta = (AICallable))
	static FAnimMCPResult anim_add_node(const FString& blueprint_path, const FString& graph, const FString& node_class, float x = 0.f, float y = 0.f, const FString& variable_name = TEXT("none"), const FString& function_name = TEXT("none"));

	/**
	 * Removes a node from its graph, breaking all of its links. Nodes the editor does not allow deleting (e.g. the Output Pose root) are rejected.
	 * Use anim_remove_state / anim_remove_transition for state machine nodes.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param node_guid GUID of the node to remove.
	 * @return Result: {removed_node_guid, disconnected}. disconnected lists every link the removal broke: [{from_node_guid, from_node, from_pin, to_node_guid, to_node, to_pin}].
	 */
	UFUNCTION(Category = "AnimMCP|Graph", meta = (AICallable))
	static FAnimMCPResult anim_remove_node(const FString& blueprint_path, const FString& node_guid);

	/**
	 * Moves a node. Purely cosmetic; does not affect behaviour.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param node_guid GUID of the node to move.
	 * @param x New horizontal position in graph units.
	 * @param y New vertical position in graph units.
	 * @return Result: {node_guid, x, y}.
	 */
	UFUNCTION(Category = "AnimMCP|Graph", meta = (AICallable))
	static FAnimMCPResult anim_set_node_position(const FString& blueprint_path, const FString& node_guid, float x, float y);

	/**
	 * Connects an output pin to an input pin. Both nodes must be in the same graph. The graph schema validates the link;
	 * if the input pin only accepts one link (e.g. a pose input), its existing link is replaced, and the result says which link that was.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param from_node_guid GUID of the node that owns the output pin.
	 * @param from_pin Name of the output pin, e.g. 'Pose' or 'ReturnValue'.
	 * @param to_node_guid GUID of the node that owns the input pin.
	 * @param to_pin Name of the input pin, e.g. 'Result', 'A', 'Alpha'.
	 * @return Result: {connected, replaced_existing_links, disconnected, message}. disconnected lists every link the connection replaced: [{from_node_guid, from_node, from_pin, to_node_guid, to_node, to_pin}].
	 */
	UFUNCTION(Category = "AnimMCP|Graph", meta = (AICallable))
	static FAnimMCPResult anim_connect_pins(const FString& blueprint_path, const FString& from_node_guid, const FString& from_pin, const FString& to_node_guid, const FString& to_pin);

	/**
	 * Breaks a link between two pins, or all links on one pin when to_node_guid is '*'.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param node_guid GUID of the node that owns the pin.
	 * @param pin Name of the pin.
	 * @param to_node_guid GUID of the node on the other end of the link. '*' = break every link on the pin.
	 * @param to_pin Name of the pin on the other end. Required when to_node_guid is a GUID.
	 * @return Result: {links_broken, disconnected: [{from_node_guid, from_node, from_pin, to_node_guid, to_node, to_pin}]}.
	 */
	UFUNCTION(Category = "AnimMCP|Graph", meta = (AICallable))
	static FAnimMCPResult anim_disconnect_pins(const FString& blueprint_path, const FString& node_guid, const FString& pin, const FString& to_node_guid = TEXT("*"), const FString& to_pin = TEXT("*"));

	/**
	 * Sets the literal default value of an unconnected input pin.
	 * Use Unreal text format: 'true'/'false', '0.5', 'X=1,Y=2,Z=3' for vectors, 'P=0,Y=90,R=0' for rotators, an asset path for object pins, an enum entry name for enum pins.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param node_guid GUID of the node.
	 * @param pin Name of the input pin.
	 * @param value New default value as text.
	 * @return Result: the updated pin.
	 */
	UFUNCTION(Category = "AnimMCP|Graph", meta = (AICallable))
	static FAnimMCPResult anim_set_pin_default(const FString& blueprint_path, const FString& node_guid, const FString& pin, const FString& value);

	/**
	 * Sets an editable property on a node using a dotted property path and Unreal text format.
	 * Anim graph nodes keep their runtime settings in a struct; 'Node' always refers to it (whatever the class names it; see node_struct_property in anim_get_node),
	 * so paths look like 'Node.bAlwaysUpdateChildren', 'Node.IKBone.BoneName', 'Node.LayerSetup[0].BranchFilters'.
	 * Use anim_get_node to inspect a node first. Only properties marked editable in the editor can be set.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param node_guid GUID of the node.
	 * @param property_path Dotted path to the property; array elements use [index].
	 * @param value New value in Unreal text format, e.g. '0.2', 'true', '(BoneName="hand_r")'.
	 * @return Result: {property_path, value, disconnected} with the value read back after the change. disconnected lists links lost because the change removed pins (e.g. fewer blend poses).
	 */
	UFUNCTION(Category = "AnimMCP|Graph", meta = (AICallable))
	static FAnimMCPResult anim_set_node_property(const FString& blueprint_path, const FString& node_guid, const FString& property_path, const FString& value);

	/**
	 * Assigns the animation asset played by an asset player node (Sequence Player, Blend Space Player, Sequence Evaluator, ...).
	 * The asset must be compatible with the blueprint's skeleton and with the node type (sequences for sequence players, blend spaces for blend space players).
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param node_guid GUID of the asset player node.
	 * @param asset_path Path of the animation asset, e.g. '/Game/Anims/Idle'.
	 * @return Result: the updated node.
	 */
	UFUNCTION(Category = "AnimMCP|Graph", meta = (AICallable))
	static FAnimMCPResult anim_set_sequence_player_asset(const FString& blueprint_path, const FString& node_guid, const FString& asset_path);
};
