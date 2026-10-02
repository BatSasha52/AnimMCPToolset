# AnimMCPToolset

An editor-only Unreal Engine plugin that lets an AI assistant inspect and edit **Animation Blueprints** over MCP (Model Context Protocol). It covers AnimGraph nodes and pins, state machines, states, transitions and rules, blend spaces, variables, compiling and saving.

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
   ```
5. Connect your MCP client to the Unreal MCP server. The endpoint and transport are configured by the Unreal MCP plugin, not by this plugin; see that plugin's settings in the editor.

## Conventions every tool follows

- **Uniform result.** Every tool returns `{ "success": bool, "result": { ... }, "error": "..." }`. On failure `result` is `{}` and `error` says what to fix.
- **Stable identifiers.** Nodes, states, transitions and state machines are identified by **node GUID** (`node_guid`). Pins are identified by **name**. Nothing is ever addressed by index or screen position. Graphs are identified by name (`AnimGraph`, `EventGraph`) or by `graph_guid`.
- **Undoable.** Every edit runs inside one `FScopedTransaction` and calls `Modify()` on the objects it touches, so **Ctrl+Z** in the editor undoes it.
- **No auto-save.** Edits only mark assets dirty. Nothing is written to disk until `anim_save_asset` is called.
- **Safe by construction.**
  - Assets are never deleted.
  - New assets can only be created under `/Game`, and creation fails if the name is already taken.
  - Only assets under `/Game` can be modified, compiled or saved.
  - Read-only references (a skeleton or an animation used as input) may come from anywhere, including `/Engine` or plugin content.
- **Game thread.** Tools refuse to run off the game thread.
- **Optional string parameters** use explicit defaults (`"*"`, `"none"`, `"auto"`, `">"`, `"AnimationAsset"`, `"AnimInstance"`, `"Default"`, `"DefaultSlot"`) instead of empty strings. The Toolset Registry in UE 5.8 only treats a parameter as optional if its schema carries a non-empty default. Empty strings are still accepted when passed explicitly.
- **Warnings, not silent acceptance.** When an edit is valid but cannot have an effect as things stand (for example a `time_remaining` rule leaving a state that has no animation), the tool still succeeds and lists the problem in `result.warnings`.

## Tools

Toolsets appear to MCP clients as `AnimMCPToolset.<Toolset>`, with tools inside them. Every parameter is documented in the tool schema the client receives. See [Calling tools through Unreal MCP](#calling-tools-through-unreal-mcp) for how to name a tool when calling it.

### Phase 1 — Inspect (`AnimInspectToolset`, read-only)

| Tool | Purpose |
|---|---|
| `anim_list_anim_blueprints(folder="/Game", skeleton_path="*")` | Find Animation Blueprints, optionally only those for one skeleton. Includes new, unsaved blueprints. |
| `anim_get_blueprint_info(blueprint_path)` | Skeleton, parent class, preview mesh, compile status, dirty state, counts. |
| `anim_list_graphs(blueprint_path)` | Every graph, including nested state machine, state, transition and conduit graphs. |
| `anim_list_nodes(blueprint_path, graph, include_pins=false)` | Nodes in a graph, with GUIDs, titles, positions and assets. |
| `anim_get_node(blueprint_path, node_guid)` | Full node detail: every pin with type, default and links, plus `node_struct_property`. |
| `anim_list_skeleton_bones(asset_path)` | Bone hierarchy of a Skeleton, SkeletalMesh, AnimBP or animation. |
| `anim_get_skeleton_info(asset_path, include_bones=true)` | **New in 0.2.** Bones, virtual bones, sockets, compatible skeletons and montage slot groups of a skeleton. |
| `anim_list_animation_assets(skeleton_path, folder="/Game", asset_class="AnimationAsset")` | Sequences, montages, blend spaces and other animation assets compatible with a skeleton. |
| `anim_list_variables(blueprint_path)` | Member variables with type, category and current default. |
| `anim_list_node_types(filter="*")` | Anim graph node classes accepted by `anim_add_node`. |

### Phase 2 — Graph editing (`AnimGraphEditToolset`)

| Tool | Purpose |
|---|---|
| `anim_add_node(blueprint_path, graph, node_class, x=0, y=0, variable_name="none", function_name="none")` | Place an anim node (`AnimGraphNode_TwoWayBlend`, …) or a Blueprint node (`K2Node_VariableGet`, `K2Node_CallFunction`, …). |
| `anim_remove_node(blueprint_path, node_guid)` | Delete a node. The root/output node cannot be deleted. |
| `anim_set_node_position(blueprint_path, node_guid, x, y)` | Move a node. |
| `anim_connect_pins(blueprint_path, from_node_guid, from_pin, to_node_guid, to_pin)` | Link an output pin to an input pin. The graph schema validates the link. |
| `anim_disconnect_pins(blueprint_path, node_guid, pin, to_node_guid="*", to_pin="*")` | Break one link, or every link on a pin. |
| `anim_set_pin_default(blueprint_path, node_guid, pin, value)` | Set an unconnected input pin's literal value. |
| `anim_set_node_property(blueprint_path, node_guid, property_path, value)` | Set an editable node property, e.g. `Node.IKBone.BoneName`. `Node` always means the node's runtime struct. |
| `anim_set_sequence_player_asset(blueprint_path, node_guid, asset_path)` | Assign the asset of a Sequence Player, Blend Space Player or other asset player node. Checks skeleton compatibility. |

### Phase 3 — State machines (`AnimStateMachineToolset`)

| Tool | Purpose |
|---|---|
| `anim_build_state_machine(blueprint_path, spec)` | **New in 0.2.** Build a whole state machine from one JSON spec (variables, states with animations, conduits, entry, transitions and rules) in one undoable step. The spec is fully validated first; nothing is created if anything is wrong, and every problem is listed. Returns every GUID created plus `warnings`. |
| `anim_add_state_machine(blueprint_path, graph, name, x=0, y=0)` | Add a named State Machine node. Returns its graph and entry node. |
| `anim_add_state(blueprint_path, state_machine_guid, name, x=0, y=0, set_as_entry=false)` | Add a state, optionally wiring Entry to it. |
| `anim_remove_state(blueprint_path, state_guid)` | Remove a state or conduit and all of its transitions. |
| `anim_add_transition(blueprint_path, from_state_guid, to_state_guid, crossfade_duration=0.2)` | Add a transition. Self-transitions are allowed. |
| `anim_remove_transition(blueprint_path, transition_guid)` | Remove a transition. |
| `anim_set_transition_rule(blueprint_path, transition_guid, rule, variable_name="none", trigger_time=-1, crossfade_duration=-1, comparison=">", threshold=0)` | Set the rule: `bool_variable`, `not_bool_variable`, `compare` (**new**: a float/int variable against a number, e.g. `Speed > 10`, built inside the rule graph), `time_remaining` (automatic, based on the source state's animation), `always` or `never`. Also accepts a conduit's GUID to set its entry rule. Returns `warnings` for rules that cannot fire. |
| `anim_set_state_animation(blueprint_path, state_guid, asset_path, node_type="auto", loop=true, play_rate=1, slot_name="DefaultSlot")` | Make a state play an animation. `node_type`: `auto`, `sequence_player`, `sequence_evaluator`, `blendspace_player`, `blendspace_evaluator` or `slot` (a montage Slot node, with the optional asset as its source). Defaults behave exactly as in 0.1. |
| `anim_add_conduit(blueprint_path, state_machine_guid, name, x=0, y=0)` | Add a conduit. |

`anim_build_state_machine` spec (only `name` and `states` are required; unknown fields are rejected so typos are caught):

```json
{
  "name": "Locomotion",
  "graph": "AnimGraph",
  "connect_to_output": true,
  "variables": [ { "name": "Speed", "type": "float", "default": "0", "category": "Locomotion" },
                 { "name": "bIsFalling", "type": "bool" } ],
  "entry_state": "Idle",
  "states": [ { "name": "Idle", "animation": "/Game/Anims/Idle" },
              { "name": "Run", "animation": "/Game/Anims/BS_Run", "play_rate": 1.2 },
              { "name": "Land", "animation": "/Game/Anims/Land", "loop": false } ],
  "conduits": [ { "name": "AirCheck", "rule": "bool_variable", "variable": "bIsFalling" } ],
  "transitions": [
    { "from": "Idle", "to": "Run", "rule": "compare", "variable": "Speed", "comparison": ">", "threshold": 10, "crossfade_duration": 0.2 },
    { "from": "Run", "to": "Idle", "rule": "compare", "variable": "Speed", "comparison": "<=", "threshold": 10 },
    { "from": "Land", "to": "Idle", "rule": "time_remaining", "trigger_time": 0.1 }
  ]
}
```

Variables are created if missing and reused if they already exist with the same type. States without `x`/`y` are laid out on a grid. State fields `node_type` and `slot_name` work as in `anim_set_state_animation`.

### Phase 4 — Assets, variables, build (`AnimAssetToolset`)

| Tool | Purpose |
|---|---|
| `anim_create_anim_blueprint(folder, asset_name, skeleton_path, parent_class="AnimInstance", preview_mesh_path="none", add_locomotion_vars=false)` | Create a new Animation Blueprint under `/Game`. **New:** `add_locomotion_vars=true` also adds `Speed`, `IsMoving` and `IsFalling` and wires the EventGraph to fill them from the owning pawn every frame (any pawn, no character class assumed). |
| `anim_add_variable(blueprint_path, name, type, default_value="none", variable_category="Default")` | Add a member variable. Types: `bool, byte, int, int64, float, name, string, text, vector, vector2d, rotator, transform, linearcolor, object:<class path>`. |
| `anim_remove_variable(blueprint_path, name, force=false)` | Remove a variable. Refuses while the variable is still used unless `force`. |
| `anim_set_variable_default(blueprint_path, name, value)` | Change a variable's default value. |
| `anim_create_blendspace(folder, asset_name, skeleton_path, dimensions=2, x_axis_name="Speed", x_min=0, x_max=600, x_grid=4, y_axis_name="Direction", y_min=-180, y_max=180, y_grid=4)` | Create a 1D or 2D blend space with axes configured. |
| `anim_add_blendspace_sample(blendspace_path, animation_path, x, y=0)` | Add a sample. Rejects positions out of range or already taken. |
| `anim_set_skeleton_compatible(skeleton_path, compatible_skeleton_path, compatible=true)` | **New in 0.2.** Add or remove a compatible skeleton, so animations made for one skeleton can be used with another. |
| `anim_compile_blueprint(blueprint_path)` | Compile and return status, error and warning counts, and messages. **New:** each message carries a `source` with the node GUID and title, its graph, and the state machine, state or transition it belongs to, e.g. `AnimGraph > state machine Locomotion > state JumpUp > Sequence Player`. |
| `anim_save_asset(asset_path)` | Save one asset to disk. This is the only tool that writes files. |

## Calling tools through Unreal MCP

With Unreal MCP's default tool search (`bEnableToolSearch`), the client sees three meta-tools: `list_toolsets`, `describe_toolset` and `call_tool`. Call a tool like this:

```json
{ "toolset_name": "AnimMCPToolset.AnimInspectToolset", "tool_name": "anim_list_anim_blueprints", "arguments": { "folder": "/Game" } }
```

`tool_name` must be the **short** name. `describe_toolset` prints fully qualified names such as `AnimMCPToolset.AnimInspectToolset.anim_list_anim_blueprints`, but passing that as `tool_name` fails with `Unknown tool …`. This is engine behaviour, not this plugin's: in UE 5.8.3, `call_tool` hands `tool_name` unchanged to the Toolset Registry, which looks tools up by their UFunction name (`FFunctionLibraryToolset::ExecuteToolInternal`), while the schema that `describe_toolset` prints uses qualified names. `call_tool`'s own parameter description says to pass the name "without toolset prefix". The smoke test checks both behaviours.

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

## Testing

`Tests/smoke_test.py` calls every tool through the Toolset Registry, the same entry point Unreal MCP uses, and checks the results, including rejection cases. Run it in a throwaway project that has this plugin and `PythonScriptPlugin` enabled (it creates assets under `/Game/AMCPTest`):

```
UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script=<plugin>/Tests/smoke_test.py -unattended -nullrhi
```

Results go to `$ANIMMCP_SMOKE_OUT` (default `<Project>/Saved/animmcp_smoke.txt`); the last line is the summary. Version 0.2.0: 163 checks, 0 failures on UE 5.8.3 (Win64).

## Limitations and notes

- **Experimental engine APIs.** This plugin is built on Toolset Registry and Unreal MCP as they exist in UE 5.8.3. Both are marked experimental and may change.
- **Partial changes on failure.** Inputs are validated before anything is modified. If a multi-step tool still fails part-way, the partial change stays inside a single transaction and can be undone with Ctrl+Z. `anim_build_state_machine` undoes it automatically when it is the outermost transaction.
- **`anim_build_state_machine` takes its spec as JSON text** (a string parameter). The 5.8 Toolset Registry has no verified way to take a free-form JSON object as a parameter.
- **`anim_set_transition_rule` replaces the whole rule graph** of the transition or conduit. It refuses transitions that use shared rules. `compare` works on float, int, int64 and non-enum byte variables; enums are not supported.
- **`anim_set_state_animation`** removes only the asset player or slot (and a slot's source player) that fed the state output directly. Other nodes inside the state are left alone.
- **`add_locomotion_vars`** treats a pawn as moving above 3 cm/s of horizontal speed. Its wiring is checked by compiling, not by playing in a level.
- **Property paths** support struct members and `[index]` into arrays. Maps and sets are not supported.
- **Only `/Game` content can be edited.** Assets inside plugin content roots (`/MyPlugin/...`) are read-only to these tools by design.
- **Verification scope.** The smoke test runs headless through the Toolset Registry. It does not cover calls through a live MCP client connection, the animation actually playing at runtime, or the values of node settings it cannot read back through the tools (for example a player's loop flag).

## Changelog

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
