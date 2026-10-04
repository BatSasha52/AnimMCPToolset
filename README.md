# AnimMCPToolset

An editor-only Unreal Engine plugin that lets an AI assistant inspect and edit **Animation Blueprints** over MCP (Model Context Protocol). It covers AnimGraph nodes and pins, state machines, states, transitions and rules, blend spaces, variables, compiling and saving, and the contents of animations: curves, notifies, root and hips travel, bone transforms and montages.

It works with any skeleton, any character and any Animation Blueprint in any project. Nothing in it is tied to a specific game.

The tools are registered with Epic's **Toolset Registry**, and Epic's **Unreal MCP** plugin serves them to MCP clients such as Claude Code, Claude Desktop or Cursor.

---

## Requirements

| | |
|---|---|
| Engine | Unreal Engine **5.8** (built and tested against 5.8.3). Toolset Registry and Unreal MCP are *experimental* engine plugins, so their APIs may change in later versions. |
| **Toolset Registry** plugin (`ToolsetRegistry`) | Ships with UE 5.8 under `Engine/Plugins/Experimental/ToolsetRegistry`. Disabled by default. Provides the `UToolsetDefinition` base class that the tools are built on. |
| **Unreal MCP** plugin (`ModelContextProtocol`) | Ships with UE 5.8 under `Engine/Plugins/Experimental/ModelContextProtocol`. Disabled by default. Runs the MCP server inside the editor and exposes every registered toolset to MCP clients. |
| Platform | Editor builds only (Win64 tested). The module is `Editor` type with `TargetAllowList: ["Editor"]`, so it never ships in a packaged game. |

`AnimMCPToolset.uplugin` declares both plugins as dependencies, so enabling AnimMCPToolset enables them as well. The module links only against `ToolsetRegistry`. Unreal MCP discovers toolsets through the registry at runtime, so no link dependency on `ModelContextProtocol` is needed.

## Setup

1. Put the plugin in your project's `Plugins/` folder, either as a git submodule or as a copy:
   ```bash
   git submodule add <this-repo-url> Plugins/AnimMCPToolset
   ```
2. Enable it in your `.uproject` (or in **Edit → Plugins**):
   ```json
   "Plugins": [
     { "Name": "AnimMCPToolset", "Enabled": true }
   ]
   ```
3. Regenerate project files and build the editor target. The plugin is C++, so a content-only project needs to be converted to C++ first, or you can use prebuilt binaries.
4. Start the editor. The log should show:
   ```
   LogToolsetRegistry: Display: Registering Toolset AnimMCPToolset.AnimInspectToolset
   LogToolsetRegistry: Display: Registering Toolset AnimMCPToolset.AnimGraphEditToolset
   LogToolsetRegistry: Display: Registering Toolset AnimMCPToolset.AnimStateMachineToolset
   LogToolsetRegistry: Display: Registering Toolset AnimMCPToolset.AnimAssetToolset
   LogToolsetRegistry: Display: Registering Toolset AnimMCPToolset.AnimDataToolset
   ```
5. Connect your MCP client to the Unreal MCP server. The endpoint and transport are configured by the Unreal MCP plugin, not by this plugin; see that plugin's settings in the editor.

## Conventions every tool follows

- **Uniform result.** Every tool returns `{ "success": bool, "result": { ... }, "error": "..." }`. On failure `result` is `{}` and `error` says what to fix.
- **Stable identifiers.** Nodes, states, transitions and state machines are identified by **node GUID** (`node_guid`). Pins are identified by **name**. Nothing is ever addressed by index or screen position. Graphs are identified by name (`AnimGraph`, `EventGraph`) or by `graph_guid`.
- **Undoable.** Every edit runs inside one `FScopedTransaction` and calls `Modify()` on the objects it touches, so **Ctrl+Z** in the editor undoes it. Animation data edits also go through `IAnimationDataController` inside a scoped bracket, so the animation data model is notified and restored on undo. The one exception is `anim_rename_anim_blueprint`: the editor does not record renames, so rename back instead.
- **No auto-save.** Edits only mark assets dirty. Nothing is written to disk until `anim_save_asset` is called.
- **Safe by construction.**
  - Assets are never deleted.
  - New assets can only be created under `/Game`, and creation fails if the name is already taken.
  - Only assets under `/Game` can be modified, compiled or saved.
  - Read-only references (a skeleton or an animation used as input) may come from anywhere, including `/Engine` or plugin content.
- **Animation edits work on a copy by default.** `anim_remove_bone_travel`, `anim_set_curve`, `anim_remove_curve` and the notify tools write to a new copy (`<Name>_Edited` next to the source, or `output_path`) unless `in_place=true`. A copy never overwrites an existing asset. To make several edits, make the first one to get the copy, then pass the copy with `in_place=true`.
- **Nothing disconnected silently.** Every tool that breaks or replaces links returns `disconnected`: each lost link with node GUID, node title and pin name on both ends. Tools that replace nodes also return `removed_nodes`.
- **Game thread.** Tools refuse to run off the game thread.
- **Optional string parameters** use explicit defaults (`"*"`, `"none"`, `"auto"`, `">"`, `"AnimationAsset"`, `"AnimInstance"`, `"Default"`, `"DefaultSlot"`) instead of empty strings. The Toolset Registry in UE 5.8 only treats a parameter as optional if its schema carries a non-empty default. Empty strings are still accepted when passed explicitly.
- **Warnings, not silent acceptance.** When an edit is valid but cannot have an effect as things stand (for example a `time_remaining` rule leaving a state that has no animation), the tool still succeeds and lists the problem in `result.warnings`.

## Tools

Toolsets appear to MCP clients as `AnimMCPToolset.<Toolset>`, with tools inside them. Tools and options new in 0.3 are marked **0.3**. Every parameter is documented in the tool schema the client receives. See [Calling tools through Unreal MCP](#calling-tools-through-unreal-mcp) for how to name a tool when calling it.

### Phase 1 — Inspect (`AnimInspectToolset`, read-only)

| Tool | Purpose |
|---|---|
| `anim_list_anim_blueprints(folder="/Game", skeleton_path="*")` | Find Animation Blueprints, optionally only those for one skeleton. Includes new, unsaved blueprints. |
| `anim_get_blueprint_info(blueprint_path)` | Skeleton, parent class, preview mesh, compile status, dirty state, counts. |
| `anim_list_graphs(blueprint_path)` | Every graph, including nested state machine, state, transition and conduit graphs. |
| `anim_list_nodes(blueprint_path, graph, include_pins=false)` | Nodes in a graph, with GUIDs, titles, positions and assets. |
| `anim_get_node(blueprint_path, node_guid)` | Full node detail: every pin with type, default and links, plus `node_struct_property`. |
| `anim_list_skeleton_bones(asset_path)` | Bone hierarchy of a Skeleton, SkeletalMesh, AnimBP or animation. |
| `anim_get_skeleton_info(asset_path, include_bones=true)` | Bones, virtual bones, sockets, compatible skeletons and montage slot groups of a skeleton. |
| `anim_list_animation_assets(skeleton_path, folder="/Game", asset_class="AnimationAsset")` | Sequences, montages, blend spaces and other animation assets compatible with a skeleton. |
| `anim_list_variables(blueprint_path)` | Member variables with type, category and current default. |
| `anim_list_node_types(filter="*")` | Anim graph node classes accepted by `anim_add_node`. |

### Phase 2 — Graph editing (`AnimGraphEditToolset`)

| Tool | Purpose |
|---|---|
| `anim_add_node(blueprint_path, graph, node_class, x=0, y=0, variable_name="none", function_name="none")` | Place an anim node (`AnimGraphNode_TwoWayBlend`, …) or a Blueprint node (`K2Node_VariableGet`, `K2Node_CallFunction`, …). |
| `anim_remove_node(blueprint_path, node_guid)` | Delete a node. The root/output node cannot be deleted. |
| `anim_set_node_position(blueprint_path, node_guid, x, y)` | Move a node. |
| `anim_connect_pins(blueprint_path, from_node_guid, from_pin, to_node_guid, to_pin)` | Link an output pin to an input pin. The graph schema validates the link. **0.3:** `disconnected` lists every link the connection replaced (e.g. the previous input of a pose pin). |
| `anim_disconnect_pins(blueprint_path, node_guid, pin, to_node_guid="*", to_pin="*")` | Break one link, or every link on a pin. **0.3:** `disconnected` names each broken link. |
| `anim_set_pin_default(blueprint_path, node_guid, pin, value)` | Set an unconnected input pin's literal value. |
| `anim_set_node_property(blueprint_path, node_guid, property_path, value)` | Set an editable node property, e.g. `Node.IKBone.BoneName`. `Node` always means the node's runtime struct. |
| `anim_set_sequence_player_asset(blueprint_path, node_guid, asset_path)` | Assign the asset of a Sequence Player, Blend Space Player or other asset player node. Checks skeleton compatibility. |

### Phase 3 — State machines (`AnimStateMachineToolset`)

| Tool | Purpose |
|---|---|
| `anim_build_state_machine(blueprint_path, spec)` | Build a whole state machine from one JSON spec (variables, states with animations, conduits, entry, transitions and rules) in one undoable step. The spec is fully validated first; nothing is created if anything is wrong, and every problem is listed. Returns every GUID created plus `warnings`. **0.3:** `"from": "*"` wildcard transitions, per-state pin bindings (`bind`), extra nodes chained inside a state (`nodes`), combined `and`/`or`/`not` rule conditions, transition `priority`, `blend_mode` and `blend_curve`. |
| `anim_add_state_machine(blueprint_path, graph, name, x=0, y=0)` | Add a named State Machine node. Returns its graph and entry node. |
| `anim_add_state(blueprint_path, state_machine_guid, name, x=0, y=0, set_as_entry=false)` | Add a state, optionally wiring Entry to it. |
| `anim_remove_state(blueprint_path, state_guid)` | Remove a state or conduit and all of its transitions. |
| `anim_add_transition(blueprint_path, from_state_guid, to_state_guid, crossfade_duration=0.2)` | Add a transition. Self-transitions are allowed. |
| `anim_remove_transition(blueprint_path, transition_guid)` | Remove a transition. |
| `anim_set_transition_rule(blueprint_path, transition_guid, rule, variable_name="none", trigger_time=-1, crossfade_duration=-1, comparison=">", threshold=0)` | Set the rule: `bool_variable`, `not_bool_variable`, `compare` (a float/int variable against a number, e.g. `Speed > 10`, built inside the rule graph), `time_remaining` (automatic, based on the source state's animation), `always` or `never`. **0.3:** `condition`, with `condition` = a JSON condition combining bool and compare checks with `and`/`or`/`not`. Also accepts a conduit's GUID to set its entry rule. Returns `warnings` for rules that cannot fire, and the old rule's `removed_nodes` and `disconnected` links. |
| `anim_set_state_animation(blueprint_path, state_guid, asset_path, node_type="auto", loop=true, play_rate=1, slot_name="DefaultSlot")` | Make a state play an animation. `node_type`: `auto`, `sequence_player`, `sequence_evaluator`, `blendspace_player`, `blendspace_evaluator` or `slot` (a montage Slot node, with the optional asset as its source). |
| `anim_add_conduit(blueprint_path, state_machine_guid, name, x=0, y=0)` | Add a conduit. |

`anim_build_state_machine` spec (only `name` and `states` are required; unknown fields are rejected so typos are caught):

```json
{
  "name": "Locomotion",
  "graph": "AnimGraph",
  "connect_to_output": true,
  "variables": [ { "name": "Speed", "type": "float", "default": "0", "category": "Locomotion" },
                 { "name": "Direction", "type": "float" },
                 { "name": "bIsFalling", "type": "bool" }, { "name": "bOnLadder", "type": "bool" } ],
  "entry_state": "Idle",
  "states": [ { "name": "Idle", "animation": "/Game/Anims/Idle" },
              { "name": "Move", "animation": "/Game/Anims/BS_Move", "bind": { "X": "Direction", "Y": "Speed" } },
              { "name": "Attack", "animation": "/Game/Anims/Attack_Loop",
                "nodes": [ { "class": "AnimGraphNode_Slot", "properties": { "Node.SlotName": "UpperBody" } },
                           { "class": "AnimGraphNode_ModifyCurve", "properties": { "Node.CurveMap": "((\"Lean\", 1.0))" } } ] },
              { "name": "Fall", "animation": "/Game/Anims/Fall_Loop" },
              { "name": "Land", "animation": "/Game/Anims/Land", "loop": false } ],
  "conduits": [ { "name": "AirCheck", "rule": "bool_variable", "variable": "bIsFalling" } ],
  "transitions": [
    { "from": "Idle", "to": "Move", "priority": 1, "blend_mode": "HermiteCubic", "crossfade_duration": 0.2,
      "rule": { "and": [ { "compare": "Speed", "comparison": ">", "threshold": 10 }, { "not": { "bool": "bIsFalling" } } ] } },
    { "from": "Move", "to": "Idle", "rule": "compare", "variable": "Speed", "comparison": "<=", "threshold": 10 },
    { "from": "*", "to": "Fall", "priority": 0, "blend_curve": "/Game/Curves/CV_FallBlend",
      "rule": { "and": [ { "bool": "bIsFalling" }, { "not": { "bool": "bOnLadder" } } ] } },
    { "from": "Fall", "to": "Land", "rule": "not_bool_variable", "variable": "bIsFalling" },
    { "from": "Land", "to": "Idle", "rule": "time_remaining", "trigger_time": 0.1 }
  ]
}
```

- **Variables** are created if missing and reused if they already exist with the same type. States without `x`/`y` are laid out on a grid. State fields `node_type` and `slot_name` work as in `anim_set_state_animation`.
- **`bind`** maps a pin of the state's animation player to a variable: `X`/`Y` of a blend space, `ExplicitTime` of an evaluator, `PlayRate`, and so on. Pins hidden by default are exposed automatically. The variable must exist (or be declared in `variables`) and its type must fit the pin.
- **`nodes`** are anim nodes chained in order between the state's animation and its output pose (animation → nodes[0] → nodes[1] → Output Pose). Each takes a `class`, `properties` by path (as in `anim_set_node_property`) and its own `bind`.
- **Rules** may be a condition object instead of a rule name: `{"bool": Var}`, `{"compare": Var, "comparison": ">", "threshold": 10}`, `{"not": cond}`, `{"and": [cond, ...]}` or `{"or": [cond, ...]}`, nested up to 8 levels. They are built from Get, comparison, NOT, AND and OR nodes in the rule graph.
- **`"from": "*"`** adds one transition from every state (not conduits) to the target, except the target itself. An explicit transition between the same two states replaces the wildcard one.
- **`priority`**: when several transitions out of a state are true at once, the lowest is taken (engine default 1). **`blend_mode`** is an `EAlphaBlendOption` name (`Linear`, `Cubic`, `HermiteCubic`, `Sinusoidal`, `QuadraticInOut`, … `Custom`). **`blend_curve`** is a CurveFloat asset and implies `Custom`.

### Phase 4 — Assets, variables, build (`AnimAssetToolset`)

| Tool | Purpose |
|---|---|
| `anim_create_anim_blueprint(folder, asset_name, skeleton_path, parent_class="AnimInstance", preview_mesh_path="none", add_locomotion_vars=false)` | Create a new Animation Blueprint under `/Game`. `add_locomotion_vars=true` also adds `Speed`, `IsMoving` and `IsFalling` and wires the EventGraph to fill them from the owning pawn every frame (any pawn, no character class assumed). |
| `anim_add_variable(blueprint_path, name, type, default_value="none", variable_category="Default")` | Add a member variable. Types: `bool, byte, int, int64, float, name, string, text, vector, vector2d, rotator, transform, linearcolor, object:<class path>`. |
| `anim_remove_variable(blueprint_path, name, force=false)` | Remove a variable. Refuses while the variable is still used unless `force`. |
| `anim_set_variable_default(blueprint_path, name, value)` | Change a variable's default value. |
| `anim_create_blendspace(folder, asset_name, skeleton_path, dimensions=2, x_axis_name="Speed", x_min=0, x_max=600, x_grid=4, y_axis_name="Direction", y_min=-180, y_max=180, y_grid=4)` | Create a 1D or 2D blend space with axes configured. |
| `anim_add_blendspace_sample(blendspace_path, animation_path, x, y=0)` | Add a sample. Rejects positions out of range or already taken. |
| `anim_set_skeleton_compatible(skeleton_path, compatible_skeleton_path, compatible=true)` | Add or remove a compatible skeleton, so animations made for one skeleton can be used with another. |
| `anim_rename_anim_blueprint(blueprint_path, new_name, new_folder="auto")` | **0.3.** Rename or move an Animation Blueprint, compile it and report what broke. Leaves a redirector at the old path, saves nothing, deletes nothing; `save_to_finish` lists the two packages to save. Lists referencers known to the asset registry. Not undoable (the editor does not record renames). |
| `anim_reparent_anim_blueprint(blueprint_path, new_parent)` | **0.3.** Change the parent class as the editor's Reparent Blueprint does, compile, and report `new_errors`, `new_warnings`, `fixed` and `broke` (compared with a compile just before). Rejects cycles, non-AnimInstance parents and parents on incompatible skeletons. |
| `anim_compile_blueprint(blueprint_path)` | Compile and return status, error and warning counts, and messages. Each message carries a `source` with the node GUID and title, its graph, and the state machine, state or transition it belongs to, e.g. `AnimGraph > state machine Locomotion > state JumpUp > Sequence Player`. |
| `anim_save_asset(asset_path)` | Save one asset to disk. This is the only tool that writes files. |

### Phase 5 — Animation data (`AnimDataToolset`, new in 0.3)

| Tool | Purpose |
|---|---|
| `anim_get_animation_info(asset_path, travel_bones="auto")` | Length, frame rate, frame and key counts, additive type, interpolation, root motion settings, float curves, notify tracks and notifies (with GUID, time, frame, duration, track), and how far the root and hips travel: start, end, `delta` per axis, distance, horizontal distance and path length, in component space. Read-only. |
| `anim_sample_bones(asset_path, time, bones="*", space="component")` | Bone transforms at a time in component or parent space: translation, rotation (pitch, yaw, roll), quaternion, scale. Read-only. |
| `anim_remove_bone_travel(animation_path, bone="auto", axes="xy", mode="linear", in_place=false, output_path="auto")` | Make an animation play in place. Removes a bone's travel on the chosen component-space axes: `linear` removes the first-to-last-frame drift and keeps sway, `flatten` holds the first frame. `bone="auto"` uses the root if it moves, otherwise the hips. |
| `anim_set_curve(animation_path, curve_name, keys, interpolation="cubic", in_place=false, output_path="auto")` | Create a float curve or replace its keys. `keys` is JSON, `[[time, value], ...]`. |
| `anim_remove_curve(animation_path, curve_name, in_place=false, output_path="auto")` | Remove a float curve. |
| `anim_add_notify(animation_path, name, time=-1, frame=-1, track="1", notify_class="none", duration=0, in_place=false, output_path="auto")` | Add a named notify, a notify class (`AnimNotify_PlaySound`, or a Blueprint notify's `_C` class) or a notify state with a duration, by time or frame, on a track (created if missing). Works on sequences and montages. |
| `anim_update_notify(animation_path, notify, name="none", time=-1, frame=-1, track="none", duration=-1, in_place=false, output_path="auto")` | Rename, move, re-track or resize a notify, addressed by its GUID (or `index:<n>`). |
| `anim_remove_notify(animation_path, notify, in_place=false, output_path="auto")` | Remove a notify by GUID (or `index:<n>`). |
| `anim_create_montage(folder, asset_name, animation_path, slot_name="DefaultSlot", sections="none")` | Create a montage from a sequence with a slot and sections (`[{"name", "time", "next"}]`). Sections continue into the next one unless `next` says otherwise; a `Default` section is added at 0 if needed. Warns if the slot is not registered on the skeleton. |

## Calling tools through Unreal MCP

With Unreal MCP's default tool search (`bEnableToolSearch`), the client sees three meta-tools: `list_toolsets`, `describe_toolset` and `call_tool`. Call a tool like this:

```json
{ "toolset_name": "AnimMCPToolset.AnimInspectToolset", "tool_name": "anim_list_anim_blueprints", "arguments": { "folder": "/Game" } }
```

`tool_name` must be the **short** name (`anim_list_anim_blueprints`). Dotted full names are a known issue in the bridge, not in this plugin, and AnimMCPToolset does not work around it. `describe_toolset` prints fully qualified, dotted names such as `AnimMCPToolset.AnimInspectToolset.anim_list_anim_blueprints`, but `call_tool` passes `tool_name` unchanged to the Toolset Registry, which looks tools up by their short UFunction name. A dotted name therefore fails with `Unknown tool …`. `call_tool`'s own parameter description says to pass the name "without toolset prefix". If your client copies the dotted name from `describe_toolset`, strip everything up to the last `.`. The smoke test checks both behaviours, so a fix in the bridge will show up there.

## Example prompts

**Phase 1 — Inspect**
> List the Animation Blueprints for the skeleton `/Game/Characters/Mannequin/SK_Mannequin_Skeleton`. For the first one, show me its AnimGraph nodes and which animations its locomotion states use.

**Skeleton info** (`anim_get_skeleton_info`)
> Show me the sockets, compatible skeletons and montage slots of `/Game/Characters/Hero/SK_Hero_Skeleton`. Skip the bone list.

**Phase 2 — Graph editing**
> In `/Game/Characters/Hero/ABP_Hero`, add a Layered Blend Per Bone node between the locomotion state machine and the Output Pose. Feed the upper-body slot into Blend Pose 0 and set its branch filter bone to `spine_01`. Then compile.

**Phase 3 — State machines**
> In `ABP_Hero`, add a state machine called "Locomotion" that drives the Output Pose. Give it states Idle (entry) and Run, playing `/Game/Anims/Idle` and `/Game/Anims/Run_Fwd`. Add a bool variable `bIsMoving` and transitions Idle→Run when `bIsMoving` is true and Run→Idle when it is false. Compile, then save.

**Compare rules** (`anim_set_transition_rule` with `rule="compare"`)
> In `ABP_Hero`, change the Idle→Run rule to "Speed greater than 10" and the Run→Idle rule to "Speed at most 10". Don't add any helper bools.

**Whole state machine in one call** (`anim_build_state_machine`)
> In `ABP_Hero`, build a "Locomotion" state machine wired to the Output Pose in one step: float `Speed` and bool `bIsFalling`; states Idle, Walk, JumpLoop and Land with their animations from `/Game/Anims`, Land not looping; Idle↔Walk on Speed > 10 / Speed ≤ 10, Walk→JumpLoop when `bIsFalling`, JumpLoop→Land when not falling, Land→Idle when its animation ends. Tell me any warnings.

**State animation options** (`anim_set_state_animation`)
> Make the Land state of `ABP_Hero` play `/Game/Anims/Land` once (no looping) at 1.3× speed, and make the Emote state a `DefaultSlot` montage slot over the Idle animation.

**Diagnostics** (`anim_compile_blueprint` sources and rule `warnings`)
> Compile `ABP_Hero` and, for every warning or note, tell me which state machine, state or transition it comes from.

**Compatible skeletons** (`anim_set_skeleton_compatible`)
> The animations in `/Game/Anims/Mixamo` use `SK_Mixamo_Skeleton`. Make that skeleton compatible with `/Game/Characters/Hero/SK_Hero_Skeleton`, check the animations now list for the hero skeleton, then save the hero skeleton.

**Phase 4 — Assets**
> Create a 1D blend space `BS_Walk_Run` in `/Game/Characters/Hero/Animation` for the hero skeleton. Use a Speed axis from 0 to 600 with samples Idle at 0, Walk at 200 and Run at 600. Make the Run state of `ABP_Hero` play it, compile, and save both assets.

**Locomotion starter variables** (`anim_create_anim_blueprint` with `add_locomotion_vars`)
> Create `ABP_Guard` in `/Game/Characters/Guard` for the guard skeleton with the locomotion starter variables, then build an Idle/Walk state machine that uses `IsMoving`.

### New in 0.3

**Wildcards, bindings, extra nodes, conditions, blend settings** (`anim_build_state_machine`)
> In `ABP_Hero`, build a "Locomotion" state machine: Idle, a Move state playing `BS_Move` with X driven by `Direction` and Y by `Speed`, and an Attack state with an `UpperBody` slot after its animation. Any state goes to Fall when `bIsFalling` and not `bOnLadder`, with priority 0 and a linear blend.

**Combined rule** (`anim_set_transition_rule` with `rule="condition"`)
> Change the Idle→Run rule of `ABP_Hero` to "Speed above 10 and not falling".

**Disconnected links** (`anim_connect_pins` and the other link-replacing tools)
> Plug the new Layered Blend Per Bone into the Output Pose of `ABP_Hero` and tell me exactly which link that replaced.

**Animation info** (`anim_get_animation_info`)
> How long is `/Game/Anims/Run_Fwd`, at what frame rate, does it have root motion, and how far do the root and the pelvis travel forward? List its curves and notifies.

**Bone sampling** (`anim_sample_bones`)
> Where are `hand_l` and `hand_r` in component space at 0.4 s in `/Game/Anims/Attack_01`?

**In place** (`anim_remove_bone_travel`)
> `/Game/Anims/Walk_Fwd` moves the character forward through the hips. Make an in-place copy called `Walk_Fwd_InPlace`, keep the bob, and show the travel before and after.

**Curves** (`anim_set_curve`)
> Add a curve `FootPlant_L` to `/Game/Anims/Walk_Fwd_InPlace` (edit it in place) that is 1 from 0.0 to 0.3 s and 0 from 0.5 s, with constant interpolation.

**Remove a curve** (`anim_remove_curve`)
> Remove the `Lean` curve from `/Game/Anims/Run_Fwd`, working on a copy.

**Notifies** (`anim_add_notify`)
> In `/Game/Anims/Walk_Fwd_InPlace`, add `Footstep_L` at frame 8 and `Footstep_R` at frame 23 on a track called Feet, and an `AnimNotifyState_Trail` from 0.2 s for 0.3 s.

**Change a notify** (`anim_update_notify`)
> Move `Footstep_R` in `/Game/Anims/Walk_Fwd_InPlace` to frame 24 and rename it `Step_R`.

**Remove a notify** (`anim_remove_notify`)
> Remove the old PlaySound notify from `/Game/Anims/Jump_Start`, editing it in place.

**Montages** (`anim_create_montage`)
> Make `AM_Attack` from `/Game/Anims/Attack_01` on the `UpperBody` slot with sections Windup at 0, Loop at 0.4 that loops, and Recover at 1.1.

**Rename** (`anim_rename_anim_blueprint`)
> Rename `/Game/Characters/Hero/ABP_Hero` to `ABP_Hero_Main`, tell me what references it and whether it still compiles, then save both packages.

**Reparent** (`anim_reparent_anim_blueprint`)
> Reparent `ABP_Hero_Main` onto `/Script/MyGame.HeroAnimInstance` and tell me what broke compared with before.

## Testing

`Tests/smoke_test.py` calls every tool through the Toolset Registry, the same entry point Unreal MCP uses, and checks the results, including rejection cases. Run it in a throwaway project that has this plugin and `PythonScriptPlugin` enabled (it creates assets under `/Game/AMCPTest`):

```
UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script=<plugin>/Tests/smoke_test.py -unattended -nullrhi
```

The `-run=pythonscript` commandlet has no undo buffer, so the undo checks are reported as `SKIP` there. To include them, run the full editor headless; the script quits it when done:

```
UnrealEditor-Cmd.exe <Project>.uproject -ExecutePythonScript=<plugin>/Tests/smoke_test.py -unattended -nullrhi -nosplash -nosound
```

Results go to `$ANIMMCP_SMOKE_OUT` (default `<Project>/Saved/animmcp_smoke.txt`); the last line is the summary. Version 0.3.0 on UE 5.8.3 (Win64): commandlet 462 checks, 0 failures, 2 skipped; full editor 478 checks, 0 failures.

## Limitations and notes

- **Experimental engine APIs.** This plugin is built on Toolset Registry and Unreal MCP as they exist in UE 5.8.3. Both are marked experimental and may change.
- **Partial changes on failure.** Inputs are validated before anything is modified. If a multi-step tool still fails part-way, the partial change stays inside a single transaction and can be undone with Ctrl+Z. `anim_build_state_machine` undoes it automatically when it is the outermost transaction.
- **`anim_build_state_machine` takes its spec as JSON text** (a string parameter). The 5.8 Toolset Registry has no verified way to take a free-form JSON object as a parameter.
- **`anim_set_transition_rule` replaces the whole rule graph** of the transition or conduit. It refuses transitions that use shared rules. `compare` works on float, int, int64 and non-enum byte variables; enums are not supported.
- **`anim_set_state_animation`** removes only the asset player or slot (and a slot's source player) that fed the state output directly. Other nodes inside the state are left alone.
- **`add_locomotion_vars`** treats a pawn as moving above 3 cm/s of horizontal speed. Its wiring is checked by compiling, not by playing in a level.
- **Property paths** support struct members and `[index]` into arrays. Maps and sets are not supported.
- **Only `/Game` content can be edited.** Assets inside plugin content roots (`/MyPlugin/...`) are read-only to these tools by design.
- **Animation data is read from source data.** `anim_get_animation_info`, `anim_sample_bones` and `anim_remove_bone_travel` use the animation data model (the keys as authored), not the compressed runtime data, and evaluate bones without keys at the reference pose. Retargeting, additive application and root-motion extraction settings are not applied when sampling.
- **Hips detection is by name.** `auto` looks for a bone named `pelvis`, `hips` or `hip` (also with a `prefix:` or `prefix-`). For other rigs, pass the bone explicitly.
- **`anim_remove_bone_travel`** changes only the chosen bone's keys. It removes travel in component space and writes it back in the bone's parent space, so moving parents are handled. Rotation and scale keys are kept. `linear` assumes the travel to remove is the straight line from the first to the last frame.
- **Curves** are float curves on animation sequences. Transform curves and curves on montages are not edited. Curve names are not added to the skeleton's curve metadata, which UE 5.3+ does not require.
- **Notifies** without a class are named notifies (`AnimNotify_<Name>` events in Animation Blueprints); their names are not added to the skeleton's notify list. Older notifies may have no GUID; address them with `index:<n>`. `anim_update_notify` gives such a notify a GUID.
- **Montages** are created with one slot track and one segment. If the slot is not registered on the skeleton, the result warns but the skeleton is not modified. The engine itself registers slot names on the skeleton in memory when a montage is loaded or an Animation Blueprint with that Slot node is compiled.
- **Copies and renames avoid the AssetTools helpers that save.** Copies use `ObjectTools::DuplicateSingleObject`, not `IAssetTools::DuplicateAsset`, which saves the new asset when source control is enabled. Renames use `ObjectTools::RenameSingleObject` with a redirector, not `IAssetTools::RenameAssets`, which saves packages and deletes the old package when no redirector is needed. The rename does not fix up soft references in other assets; they resolve through the redirector until you run Fix Up Redirectors.
- **Rename referencers** come from the asset registry, so they cover saved assets. References from assets created in this session and never saved are not listed.
- **Reparent** does not repeat the editor's namespace-import bookkeeping (a Blueprint namespaces feature) or its confirmation dialogs.
- **Verification scope.** The smoke test runs headless through the Toolset Registry. It does not cover calls through a live MCP client connection, the animation actually playing at runtime, or the values of node settings it cannot read back through the tools (for example a player's loop flag). The automatic rollback of a failed `anim_build_state_machine` is not exercised, because no valid spec fails while it is being applied.

## Changelog

**0.3.0**
- `anim_build_state_machine`: `"from": "*"` wildcard transitions, per-state pin bindings (`bind`), extra nodes inside states (`nodes`, with properties by path), combined `and`/`or`/`not` rule conditions, transition `priority`, `blend_mode` and `blend_curve`. Still validated in full before anything is created.
- `anim_set_transition_rule`: `rule="condition"`.
- Diagnostics: every tool that breaks or replaces links reports them in `disconnected` (node GUID, title and pin on both ends), and replaced nodes in `removed_nodes`.
- New `AnimDataToolset`: `anim_get_animation_info`, `anim_sample_bones`, `anim_remove_bone_travel`, `anim_set_curve`, `anim_remove_curve`, `anim_add_notify`, `anim_update_notify`, `anim_remove_notify`, `anim_create_montage`. Edits go to a copy unless `in_place=true`.
- `anim_rename_anim_blueprint` and `anim_reparent_anim_blueprint`, each with a compile and a report of what broke.
- `anim_get_node` reports a transition's blend mode and curve.
- Smoke test: about 300 new checks, and a full-editor mode that also checks undo.

**0.2.0**
- `compare` transition rules (`Speed > 10`) built inside the rule graph, with no helper bools.
- `anim_build_state_machine`: a whole state machine from one validated JSON spec, in one undoable step.
- `anim_set_state_animation`: `node_type` (players, evaluators, montage slot), `loop`, `play_rate`, `slot_name`.
- Conduit entry rules, `warnings` for rules that cannot fire, and node, state and transition sources on compile messages.
- `anim_get_skeleton_info` and `anim_set_skeleton_compatible`.
- `add_locomotion_vars` on `anim_create_anim_blueprint`.
- Folder arguments with a trailing slash now work; documented how to name tools through `call_tool`.
- End-to-end smoke test added to the repo.

**0.1.0** — first release.

## License

MIT. See [LICENSE](LICENSE).
