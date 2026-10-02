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
- **Optional string parameters** use explicit defaults (`"*"`, `"none"`, `"AnimationAsset"`, `"AnimInstance"`, `"Default"`) instead of empty strings. The Toolset Registry in UE 5.8 only treats a parameter as optional if its schema carries a non-empty default. Empty strings are still accepted when passed explicitly.

## Tools

Toolsets appear to MCP clients as `AnimMCPToolset.<Toolset>`, with tools inside them, for example `AnimMCPToolset.AnimInspectToolset.anim_list_nodes`. Every parameter is documented in the tool schema the client receives.

### Phase 1 — Inspect (`AnimInspectToolset`, read-only)

| Tool | Purpose |
|---|---|
| `anim_list_anim_blueprints(folder="/Game", skeleton_path="*")` | Find Animation Blueprints, optionally only those for one skeleton. |
| `anim_get_blueprint_info(blueprint_path)` | Skeleton, parent class, preview mesh, compile status, dirty state, counts. |
| `anim_list_graphs(blueprint_path)` | Every graph, including nested state machine, state, transition and conduit graphs. |
| `anim_list_nodes(blueprint_path, graph, include_pins=false)` | Nodes in a graph, with GUIDs, titles, positions and assets. |
| `anim_get_node(blueprint_path, node_guid)` | Full node detail: every pin with type, default and links, plus `node_struct_property`. |
| `anim_list_skeleton_bones(asset_path)` | Bone hierarchy of a Skeleton, SkeletalMesh, AnimBP or animation. |
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
| `anim_add_state_machine(blueprint_path, graph, name, x=0, y=0)` | Add a named State Machine node. Returns its graph and entry node. |
| `anim_add_state(blueprint_path, state_machine_guid, name, x=0, y=0, set_as_entry=false)` | Add a state, optionally wiring Entry to it. |
| `anim_remove_state(blueprint_path, state_guid)` | Remove a state or conduit and all of its transitions. |
| `anim_add_transition(blueprint_path, from_state_guid, to_state_guid, crossfade_duration=0.2)` | Add a transition. Self-transitions are allowed. |
| `anim_remove_transition(blueprint_path, transition_guid)` | Remove a transition. |
| `anim_set_transition_rule(blueprint_path, transition_guid, rule, variable_name="none", trigger_time=-1, crossfade_duration=-1)` | Set the rule: `bool_variable`, `not_bool_variable`, `time_remaining` (automatic, based on the source state's animation), `always` or `never`. |
| `anim_set_state_animation(blueprint_path, state_guid, asset_path)` | Make a state play a sequence or blend space. Creates the player and wires it to the state output. |
| `anim_add_conduit(blueprint_path, state_machine_guid, name, x=0, y=0)` | Add a conduit. |

### Phase 4 — Assets, variables, build (`AnimAssetToolset`)

| Tool | Purpose |
|---|---|
| `anim_create_anim_blueprint(folder, asset_name, skeleton_path, parent_class="AnimInstance", preview_mesh_path="none")` | Create a new Animation Blueprint under `/Game`. |
| `anim_add_variable(blueprint_path, name, type, default_value="none", variable_category="Default")` | Add a member variable. Types: `bool, byte, int, int64, float, name, string, text, vector, vector2d, rotator, transform, linearcolor, object:<class path>`. |
| `anim_remove_variable(blueprint_path, name, force=false)` | Remove a variable. Refuses while the variable is still used unless `force`. |
| `anim_set_variable_default(blueprint_path, name, value)` | Change a variable's default value. |
| `anim_create_blendspace(folder, asset_name, skeleton_path, dimensions=2, x_axis_name="Speed", x_min=0, x_max=600, x_grid=4, y_axis_name="Direction", y_min=-180, y_max=180, y_grid=4)` | Create a 1D or 2D blend space with axes configured. |
| `anim_add_blendspace_sample(blendspace_path, animation_path, x, y=0)` | Add a sample. Rejects positions out of range or already taken. |
| `anim_compile_blueprint(blueprint_path)` | Compile and return status, error and warning counts, and messages. |
| `anim_save_asset(asset_path)` | Save one asset to disk. This is the only tool that writes files. |

## Example prompts

**Phase 1 — Inspect**
> List the Animation Blueprints for the skeleton `/Game/Characters/Mannequin/SK_Mannequin_Skeleton`. For the first one, show me its AnimGraph nodes and which animations its locomotion states use.

**Phase 2 — Graph editing**
> In `/Game/Characters/Hero/ABP_Hero`, add a Layered Blend Per Bone node between the locomotion state machine and the Output Pose. Feed the upper-body slot into Blend Pose 0 and set its branch filter bone to `spine_01`. Then compile.

**Phase 3 — State machines**
> In `ABP_Hero`, add a state machine called "Locomotion" that drives the Output Pose. Give it states Idle (entry) and Run, playing `/Game/Anims/Idle` and `/Game/Anims/Run_Fwd`. Add a bool variable `bIsMoving` and transitions Idle→Run when `bIsMoving` is true and Run→Idle when it is false. Compile, then save.

**Phase 4 — Assets**
> Create a 1D blend space `BS_Walk_Run` in `/Game/Characters/Hero/Animation` for the hero skeleton. Use a Speed axis from 0 to 600 with samples Idle at 0, Walk at 200 and Run at 600. Make the Run state of `ABP_Hero` play it, compile, and save both assets.

## Limitations and notes

- **Experimental engine APIs.** This plugin is built on Toolset Registry and Unreal MCP as they exist in UE 5.8.3. Both are marked experimental and may change.
- **Partial changes on failure.** Inputs are validated before anything is modified. In the rare case a multi-step tool (for example `anim_set_transition_rule` or `anim_set_state_animation`) fails part-way, the partial change stays inside a single transaction and can be undone with Ctrl+Z.
- **`anim_set_transition_rule` replaces the whole rule graph** of the transition. It refuses transitions that use shared rules.
- **`anim_set_state_animation`** removes only the asset player that fed the state output directly. Other nodes inside the state are left alone.
- **Property paths** support struct members and `[index]` into arrays. Maps and sets are not supported.
- **Only `/Game` content can be edited.** Assets inside plugin content roots (`/MyPlugin/...`) are read-only to these tools by design.
- **Verification scope.** Every tool was exercised end to end through the Toolset Registry (`ToolsetRegistry.execute_tool`) in UE 5.8.3:
  - creating an AnimBP;
  - building a two-state locomotion machine with bool and NOT-bool rules;
  - conduits and automatic rules;
  - blend spaces;
  - compiling clean (0 errors, 0 warnings) and saving.

  Calls through a live MCP client connection were not part of that automated test.

## License

MIT. See [LICENSE](LICENSE).
