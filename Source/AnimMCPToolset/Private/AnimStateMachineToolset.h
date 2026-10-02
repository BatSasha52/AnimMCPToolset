// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "AnimMCPTypes.h"

#include "AnimStateMachineToolset.generated.h"

/**
 * Phase 3 - building animation state machines: state machine nodes, states, conduits, transitions and transition rules.
 * State machines, states, conduits and transitions are all identified by node_guid. Use anim_list_nodes on the
 * state machine graph (state_machine_graph from the state machine node) to find them.
 * Every edit is a single undoable transaction and marks the asset dirty; nothing is saved until anim_save_asset is called.
 * Every tool returns {Success, Result, Error}.
 */
UCLASS(MinimalAPI)
class UAnimStateMachineToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Adds a State Machine node to an anim graph (usually 'AnimGraph'). Connect its 'Pose' output to the next node or the Output Pose with anim_connect_pins.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param graph Anim graph name or graph_guid to place the state machine in, e.g. 'AnimGraph'.
	 * @param name Name for the state machine, e.g. 'Locomotion'. Must be unique among state machines in that graph.
	 * @param x Horizontal position in graph units.
	 * @param y Vertical position in graph units.
	 * @return Result: the new node, including node_guid, state_machine_graph and state_machine_graph_guid.
	 */
	UFUNCTION(Category = "AnimMCP|StateMachine", meta = (AICallable))
	static FAnimMCPResult anim_add_state_machine(const FString& blueprint_path, const FString& graph, const FString& name, float x = 0.f, float y = 0.f);

	/**
	 * Adds a state to a state machine. A new state has an empty pose; give it an animation with anim_set_state_animation.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param state_machine_guid node_guid of the State Machine node (from anim_add_state_machine or anim_list_nodes).
	 * @param name State name, e.g. 'Idle'. Must be unique within the state machine.
	 * @param x Horizontal position inside the state machine graph.
	 * @param y Vertical position inside the state machine graph.
	 * @param set_as_entry If true, the state machine's Entry node is wired to this state, replacing any previous entry state.
	 * @return Result: the new state node with node_guid and its inner graph.
	 */
	UFUNCTION(Category = "AnimMCP|StateMachine", meta = (AICallable))
	static FAnimMCPResult anim_add_state(const FString& blueprint_path, const FString& state_machine_guid, const FString& name, float x = 0.f, float y = 0.f, bool set_as_entry = false);

	/**
	 * Removes a state or conduit and every transition into or out of it.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param state_guid node_guid of the state or conduit.
	 * @return Result: {removed_state_guid, removed_transition_guids}.
	 */
	UFUNCTION(Category = "AnimMCP|StateMachine", meta = (AICallable))
	static FAnimMCPResult anim_remove_state(const FString& blueprint_path, const FString& state_guid);

	/**
	 * Adds a transition between two states or conduits in the same state machine. A new transition's rule is false
	 * until set with anim_set_transition_rule. A transition from a state to itself is allowed.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param from_state_guid node_guid of the source state or conduit.
	 * @param to_state_guid node_guid of the destination state or conduit.
	 * @param crossfade_duration Blend time in seconds.
	 * @return Result: the new transition node with node_guid.
	 */
	UFUNCTION(Category = "AnimMCP|StateMachine", meta = (AICallable))
	static FAnimMCPResult anim_add_transition(const FString& blueprint_path, const FString& from_state_guid, const FString& to_state_guid, float crossfade_duration = 0.2f);

	/**
	 * Removes a transition.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param transition_guid node_guid of the transition.
	 * @return Result: {removed_transition_guid}.
	 */
	UFUNCTION(Category = "AnimMCP|StateMachine", meta = (AICallable))
	static FAnimMCPResult anim_remove_transition(const FString& blueprint_path, const FString& transition_guid);

	/**
	 * Sets when a transition may be taken. This replaces the whole rule graph of the transition.
	 * rule values:
	 *  'bool_variable' - take the transition while a bool member variable is true (needs variable_name);
	 *  'not_bool_variable' - take it while that bool variable is false (needs variable_name);
	 *  'compare' - take it while a float or int member variable compares true against a number, e.g. Speed > 10
	 *              (needs variable_name, comparison and threshold). The comparison is built inside the rule graph, so no helper bool is needed;
	 *  'time_remaining' - automatic rule: take it when the source state's animation is about to finish (uses trigger_time);
	 *  'always' - always true; 'never' - always false.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param transition_guid node_guid of the transition.
	 * @param rule One of: bool_variable, not_bool_variable, compare, time_remaining, always, never.
	 * @param variable_name Member variable the rule reads: a bool for bool_variable / not_bool_variable, a float, int, int64 or byte for compare. Create it with anim_add_variable if needed. 'none' for other rules.
	 * @param trigger_time For time_remaining: seconds before the end of the source animation at which to transition. Negative = use the crossfade duration.
	 * @param crossfade_duration Optional new blend time in seconds. Negative = leave unchanged.
	 * @param comparison For compare: one of >, >=, <, <=, ==, !=. The rule is 'variable <comparison> threshold'.
	 * @param threshold For compare: the number to compare against. Must be a whole number when the variable is an int, int64 or byte.
	 * @return Result: the updated transition node plus rule and rule_nodes (node_guids created in the rule graph).
	 */
	UFUNCTION(Category = "AnimMCP|StateMachine", meta = (AICallable))
	static FAnimMCPResult anim_set_transition_rule(const FString& blueprint_path, const FString& transition_guid, const FString& rule, const FString& variable_name = TEXT("none"), float trigger_time = -1.f, float crossfade_duration = -1.f,
		const FString& comparison = TEXT(">"), float threshold = 0.f);

	/**
	 * Makes a state play an animation: creates a Sequence Player (for sequences) or Blend Space Player (for blend spaces)
	 * inside the state and wires it to the state's output pose, replacing any asset player previously wired there.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param state_guid node_guid of the state.
	 * @param asset_path Path of an AnimSequence or BlendSpace compatible with the blueprint's skeleton.
	 * @return Result: the asset player node created inside the state.
	 */
	UFUNCTION(Category = "AnimMCP|StateMachine", meta = (AICallable))
	static FAnimMCPResult anim_set_state_animation(const FString& blueprint_path, const FString& state_guid, const FString& asset_path);

	/**
	 * Adds a conduit (a branching point with its own rule and no pose) to a state machine.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param state_machine_guid node_guid of the State Machine node.
	 * @param name Conduit name. Must be unique within the state machine.
	 * @param x Horizontal position inside the state machine graph.
	 * @param y Vertical position inside the state machine graph.
	 * @return Result: the new conduit node with node_guid.
	 */
	UFUNCTION(Category = "AnimMCP|StateMachine", meta = (AICallable))
	static FAnimMCPResult anim_add_conduit(const FString& blueprint_path, const FString& state_machine_guid, const FString& name, float x = 0.f, float y = 0.f);
};
