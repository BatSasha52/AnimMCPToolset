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
	 * Sets when a transition may be taken, or when a conduit may be passed through. This replaces the whole rule graph.
	 * A rule that is accepted but cannot fire as things stand (for example time_remaining when the source state has no animation)
	 * still succeeds and is reported in the result's warnings.
	 * rule values:
	 *  'bool_variable' - take the transition while a bool member variable is true (needs variable_name);
	 *  'not_bool_variable' - take it while that bool variable is false (needs variable_name);
	 *  'compare' - take it while a float or int member variable compares true against a number, e.g. Speed > 10
	 *              (needs variable_name, comparison and threshold). The comparison is built inside the rule graph, so no helper bool is needed;
	 *  'time_remaining' - automatic rule: take it when the source state's animation is about to finish (uses trigger_time);
	 *  'always' - always true; 'never' - always false.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param transition_guid node_guid of the transition, or of a conduit to set the conduit's entry rule (time_remaining does not apply to conduits).
	 * @param rule One of: bool_variable, not_bool_variable, compare, time_remaining, always, never.
	 * @param variable_name Member variable the rule reads: a bool for bool_variable / not_bool_variable, a float, int, int64 or byte for compare. Create it with anim_add_variable if needed. 'none' for other rules.
	 * @param trigger_time For time_remaining: seconds before the end of the source animation at which to transition. Negative = use the crossfade duration.
	 * @param crossfade_duration Optional new blend time in seconds (transitions only). Negative = leave unchanged.
	 * @param comparison For compare: one of >, >=, <, <=, ==, !=. The rule is 'variable <comparison> threshold'.
	 * @param threshold For compare: the number to compare against. Must be a whole number when the variable is an int, int64 or byte.
	 * @return Result: the updated transition (or conduit) node plus rule, rule_nodes (node_guids created in the rule graph) and warnings.
	 */
	UFUNCTION(Category = "AnimMCP|StateMachine", meta = (AICallable))
	static FAnimMCPResult anim_set_transition_rule(const FString& blueprint_path, const FString& transition_guid, const FString& rule, const FString& variable_name = TEXT("none"), float trigger_time = -1.f, float crossfade_duration = -1.f,
		const FString& comparison = TEXT(">"), float threshold = 0.f);

	/**
	 * Makes a state play an animation: creates the node inside the state and wires it to the state's output pose,
	 * replacing the asset player or slot previously wired there. With only asset_path this creates a looping Sequence Player
	 * (for sequences) or Blend Space Player (for blend spaces) at play rate 1, exactly like the editor default.
	 * node_type values:
	 *  'auto' - Sequence Player or Blend Space Player depending on the asset;
	 *  'sequence_player', 'blendspace_player' - the same, but checks the asset type;
	 *  'sequence_evaluator', 'blendspace_evaluator' - plays the time you drive into its Explicit Time / Normalized Time pin (play_rate must stay 1);
	 *  'slot' - a montage Slot node named slot_name wired to the output; asset_path (optional) becomes the slot's source pose.
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param state_guid node_guid of the state.
	 * @param asset_path Path of an AnimSequence or BlendSpace compatible with the blueprint's skeleton. May be 'none' only for node_type 'slot'.
	 * @param node_type One of: auto, sequence_player, sequence_evaluator, blendspace_player, blendspace_evaluator, slot.
	 * @param loop Whether the animation loops. False is typical for one-shot states such as Land or JumpStart.
	 * @param play_rate Play rate multiplier for players (negative plays in reverse). Must be 1 for evaluators.
	 * @param slot_name For node_type 'slot': the montage slot, e.g. 'DefaultSlot' or 'UpperBody'.
	 * @return Result: the node wired to the state output, plus player_node_guid and (for slots) slot_node_guid.
	 */
	UFUNCTION(Category = "AnimMCP|StateMachine", meta = (AICallable))
	static FAnimMCPResult anim_set_state_animation(const FString& blueprint_path, const FString& state_guid, const FString& asset_path,
		const FString& node_type = TEXT("auto"), bool loop = true, float play_rate = 1.f, const FString& slot_name = TEXT("DefaultSlot"));

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

	/**
	 * Builds a whole state machine from one JSON spec in a single undoable step: variables, the state machine node,
	 * states with their animations, conduits, the entry state, transitions and their rules.
	 * The whole spec is validated first; if anything is wrong nothing is created and the error lists every problem found.
	 * Spec (JSON text). Only 'name' and 'states' are required:
	 * {
	 *   "name": "Locomotion",              // state machine name
	 *   "graph": "AnimGraph",              // anim graph to place it in (default AnimGraph)
	 *   "x": 0, "y": 0,                    // state machine node position
	 *   "connect_to_output": true,         // wire its Pose to the graph's Output Pose (default false)
	 *   "variables": [ {"name": "Speed", "type": "float", "default": "0", "category": "Locomotion"} ],
	 *                                      // created if missing, reused if they already exist with the same type
	 *   "entry_state": "Idle",             // default: the first state
	 *   "states": [ {"name": "Idle", "animation": "/Game/Anims/Idle", "loop": true, "play_rate": 1.0, "x": 300, "y": 0} ],
	 *                                      // animation is optional; node_type and slot_name work as in anim_set_state_animation;
	 *                                      // positions default to a grid
	 *   "conduits": [ {"name": "Branch", "rule": "bool_variable", "variable": "bIsFalling"} ],  // rule fields as for transitions
	 *   "transitions": [ {"from": "Idle", "to": "Run", "rule": "compare", "variable": "Speed", "comparison": ">", "threshold": 10,
	 *                     "crossfade_duration": 0.2} ]
	 *                                      // rule fields as in anim_set_transition_rule: rule, variable, comparison, threshold, trigger_time
	 * }
	 * @param blueprint_path Asset path of the Animation Blueprint (must be under /Game).
	 * @param spec The state machine description as JSON text (see above).
	 * @return Result: {state_machine, state_machine_graph_guid, entry_node_guid, variables_created, variables_reused, states: [{name, node_guid, player_node_guid}], conduits: [{name, node_guid, rule, rule_nodes}], transitions: [{from, to, node_guid, rule, rule_nodes}], warnings}.
	 *         warnings lists rules that were built but cannot fire as things stand (missing rules, time_remaining from a state with no animation).
	 */
	UFUNCTION(Category = "AnimMCP|StateMachine", meta = (AICallable))
	static FAnimMCPResult anim_build_state_machine(const FString& blueprint_path, const FString& spec);
};
