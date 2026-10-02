# End-to-end smoke test for AnimMCPToolset. Calls every tool through the Toolset Registry, the same
# entry point Unreal MCP uses. Run it in a throwaway project that has this plugin and PythonScriptPlugin:
#
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script=<path>/smoke_test.py -unattended -nullrhi
#
# It creates assets under /Game/AMCPTest, so the project must not already contain that folder.
# Results go to $ANIMMCP_SMOKE_OUT (default <Project>/Saved/animmcp_smoke.txt); the last line is the summary.
import json, os, unreal

LOG = []
OUT = open(os.environ.get("ANIMMCP_SMOKE_OUT") or os.path.join(unreal.Paths.project_saved_dir(), "animmcp_smoke.txt"), "w", encoding="utf-8")
def log(msg):
    LOG.append(msg)
    OUT.write(msg + "\n"); OUT.flush()

def call(toolset, tool, **args):
    r = unreal.ToolsetRegistry.execute_tool("AnimMCPToolset." + toolset, tool, json.dumps(args))
    if not r.is_complete:
        log("FAIL %s: not complete synchronously" % tool); return None
    if r.error:
        log("FAIL %s: registry error %s" % (tool, r.error)); return None
    return json.loads(r.value)["returnValue"]

def ok(toolset, tool, **args):
    env = call(toolset, tool, **args)
    if env is None: return None
    if env.get("success"):
        log("OK   %s" % tool)
        return env.get("result")
    log("FAIL %s: %s" % (tool, env.get("error")))
    return None

def expect_fail(toolset, tool, **args):
    env = call(toolset, tool, **args)
    if env is not None and not env.get("success"):
        log("OK   %s rejected as expected: %s" % (tool, env.get("error")))
    else:
        log("FAIL %s should have been rejected" % tool)

schemas = json.loads(unreal.ToolsetRegistry.get_all_toolset_json_schemas())
ours = [s for s in schemas if s["name"].startswith("AnimMCPToolset.")]
log("toolsets: " + ", ".join("%s(%d)" % (s["name"], len(s["tools"])) for s in ours))
sample = [t for s in ours for t in s["tools"] if t["name"].endswith("anim_add_state")][0]
log("sample schema: " + json.dumps(sample)[:1500])

SK = "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton"
IDLE = "/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle"
WALK = "/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd"
BP = "/Game/AMCPTest/ABP_Test"

r = ok("AnimInspectToolset", "anim_list_skeleton_bones", asset_path=SK); r and log("bones=%d first=%s" % (len(r["bones"]), r["bones"][0]["name"]))
r = ok("AnimInspectToolset", "anim_list_animation_assets", skeleton_path=SK, folder="/Engine/Tutorial"); r and log("assets=%s" % [a["name"] for a in r["assets"]])
r = ok("AnimInspectToolset", "anim_list_node_types", filter="Blend"); r and log("blend node types=%d" % len(r["node_types"]))

ok("AnimAssetToolset", "anim_create_anim_blueprint", folder="/Game/AMCPTest", asset_name="ABP_Test", skeleton_path=SK)
expect_fail("AnimAssetToolset", "anim_create_anim_blueprint", folder="/Game/AMCPTest", asset_name="ABP_Test", skeleton_path=SK)
expect_fail("AnimAssetToolset", "anim_create_anim_blueprint", folder="/Engine/Foo", asset_name="ABP_X", skeleton_path=SK)
expect_fail("AnimGraphEditToolset", "anim_add_node", blueprint_path="/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_AnimBlueprint", graph="AnimGraph", node_class="AnimGraphNode_TwoWayBlend")
r = ok("AnimInspectToolset", "anim_list_anim_blueprints", folder="/Game", skeleton_path=SK); r and log("abps=%s" % r["anim_blueprints"])
ok("AnimAssetToolset", "anim_add_variable", blueprint_path=BP, name="bIsMoving", type="bool", default_value="false")
ok("AnimAssetToolset", "anim_add_variable", blueprint_path=BP, name="Speed", type="float", default_value="0.0", variable_category="Locomotion")
expect_fail("AnimAssetToolset", "anim_add_variable", blueprint_path=BP, name="Bad", type="bool", default_value="notabool")
ok("AnimAssetToolset", "anim_compile_blueprint", blueprint_path=BP)
ok("AnimAssetToolset", "anim_set_variable_default", blueprint_path=BP, name="Speed", value="150.0")
r = ok("AnimInspectToolset", "anim_list_variables", blueprint_path=BP); r and log("vars=%s" % r["variables"])
r = ok("AnimInspectToolset", "anim_get_blueprint_info", blueprint_path=BP); r and log("info=%s" % r)
r = ok("AnimInspectToolset", "anim_list_graphs", blueprint_path=BP); r and log("graphs=%s" % [g["name"] for g in r["graphs"]])

r = ok("AnimInspectToolset", "anim_list_nodes", blueprint_path=BP, graph="AnimGraph")
root = [n for n in r["nodes"] if n["class"] == "AnimGraphNode_Root"][0]["node_guid"]
sm = ok("AnimStateMachineToolset", "anim_add_state_machine", blueprint_path=BP, graph="AnimGraph", name="Locomotion", x=-400, y=0)
log("sm title=%s graph=%s pins=%s" % (sm["title"], sm.get("state_machine_graph"), [p["name"] for p in sm["pins"]]))
ok("AnimGraphEditToolset", "anim_connect_pins", blueprint_path=BP, from_node_guid=sm["node_guid"], from_pin="Pose", to_node_guid=root, to_pin="Result")
idle = ok("AnimStateMachineToolset", "anim_add_state", blueprint_path=BP, state_machine_guid=sm["node_guid"], name="Idle", x=200, y=0, set_as_entry=True)
walk = ok("AnimStateMachineToolset", "anim_add_state", blueprint_path=BP, state_machine_guid=sm["node_guid"], name="Walk", x=500, y=0)
expect_fail("AnimStateMachineToolset", "anim_add_state", blueprint_path=BP, state_machine_guid=sm["node_guid"], name="Idle")
log("state names: %s / %s" % (idle["state_name"], walk["state_name"]))
ok("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP, state_guid=idle["node_guid"], asset_path=IDLE)
ok("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP, state_guid=walk["node_guid"], asset_path=WALK)
ok("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP, state_guid=walk["node_guid"], asset_path=WALK)
t1 = ok("AnimStateMachineToolset", "anim_add_transition", blueprint_path=BP, from_state_guid=idle["node_guid"], to_state_guid=walk["node_guid"], crossfade_duration=0.25)
t2 = ok("AnimStateMachineToolset", "anim_add_transition", blueprint_path=BP, from_state_guid=walk["node_guid"], to_state_guid=idle["node_guid"])
ok("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP, transition_guid=t1["node_guid"], rule="bool_variable", variable_name="bIsMoving")
ok("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP, transition_guid=t2["node_guid"], rule="not_bool_variable", variable_name="bIsMoving")
expect_fail("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP, transition_guid=t2["node_guid"], rule="bool_variable", variable_name="Speed")
# --- v0.2 step 1: compare rules on float/int variables, built inside the transition graph
ok("AnimAssetToolset", "anim_add_variable", blueprint_path=BP, name="Count", type="int", default_value="0")
t_cmp = ok("AnimStateMachineToolset", "anim_add_transition", blueprint_path=BP, from_state_guid=walk["node_guid"], to_state_guid=walk["node_guid"])
r = ok("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP, transition_guid=t1["node_guid"], rule="compare", variable_name="Speed", comparison=">", threshold=10)
r and log("compare rule nodes=%s" % r.get("rule_nodes"))
for op in (">=", "<", "<=", "==", "!="):
    ok("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP, transition_guid=t_cmp["node_guid"], rule="compare", variable_name="Count", comparison=op, threshold=3)
r = ok("AnimInspectToolset", "anim_list_graphs", blueprint_path=BP)
rule_graph = [g for g in r["graphs"] if g.get("owner_node_guid") == t1["node_guid"]][0]
r = ok("AnimInspectToolset", "anim_list_nodes", blueprint_path=BP, graph=rule_graph["graph_guid"], include_pins=True)
cmp_nodes = [n for n in r["nodes"] if n["class"] == "K2Node_CallFunction"]
b_pin = [p for p in cmp_nodes[0]["pins"] if p["name"] == "B"][0] if cmp_nodes else {}
log("%s compare node in rule graph: %s B=%s" % ("OK  " if len(cmp_nodes) == 1 and float(b_pin.get("default_value") or "nan") == 10 else "FAIL", [n["title"] for n in cmp_nodes], b_pin.get("default_value")))
expect_fail("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP, transition_guid=t_cmp["node_guid"], rule="compare", variable_name="Count", comparison=">", threshold=2.5)
expect_fail("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP, transition_guid=t_cmp["node_guid"], rule="compare", variable_name="Speed", comparison="=>", threshold=1)
expect_fail("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP, transition_guid=t_cmp["node_guid"], rule="compare", variable_name="bIsMoving", comparison=">", threshold=1)
expect_fail("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP, transition_guid=t_cmp["node_guid"], rule="compare", variable_name="NoSuchVar", comparison=">", threshold=1)
r = ok("AnimAssetToolset", "anim_compile_blueprint", blueprint_path=BP); r and log("%s compile with compare rules: errors=%d warnings=%d" % ("OK  " if r["num_errors"] == 0 else "FAIL", r["num_errors"], r["num_warnings"]))
ok("AnimStateMachineToolset", "anim_remove_transition", blueprint_path=BP, transition_guid=t_cmp["node_guid"])
ok("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP, transition_guid=t1["node_guid"], rule="bool_variable", variable_name="bIsMoving")

c = ok("AnimStateMachineToolset", "anim_add_conduit", blueprint_path=BP, state_machine_guid=sm["node_guid"], name="Branch", x=350, y=200)
t3 = ok("AnimStateMachineToolset", "anim_add_transition", blueprint_path=BP, from_state_guid=idle["node_guid"], to_state_guid=c["node_guid"])
ok("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP, transition_guid=t3["node_guid"], rule="time_remaining", trigger_time=0.3)
ok("AnimStateMachineToolset", "anim_remove_transition", blueprint_path=BP, transition_guid=t3["node_guid"])
ok("AnimStateMachineToolset", "anim_remove_state", blueprint_path=BP, state_guid=c["node_guid"])

blend = ok("AnimGraphEditToolset", "anim_add_node", blueprint_path=BP, graph="AnimGraph", node_class="AnimGraphNode_TwoWayBlend", x=-200, y=300)
ok("AnimGraphEditToolset", "anim_set_pin_default", blueprint_path=BP, node_guid=blend["node_guid"], pin="Alpha", value="0.5")
r = ok("AnimGraphEditToolset", "anim_set_node_property", blueprint_path=BP, node_guid=blend["node_guid"], property_path="Node.bAlwaysUpdateChildren", value="true"); r and log("prop readback=%s" % r["value"])
expect_fail("AnimGraphEditToolset", "anim_set_node_property", blueprint_path=BP, node_guid=blend["node_guid"], property_path="Node.NoSuchProp", value="1")
sp = ok("AnimGraphEditToolset", "anim_add_node", blueprint_path=BP, graph="AnimGraph", node_class="AnimGraphNode_SequencePlayer", x=-500, y=300)
ok("AnimGraphEditToolset", "anim_set_sequence_player_asset", blueprint_path=BP, node_guid=sp["node_guid"], asset_path=IDLE)
ok("AnimGraphEditToolset", "anim_connect_pins", blueprint_path=BP, from_node_guid=sp["node_guid"], from_pin="Pose", to_node_guid=blend["node_guid"], to_pin="A")
ok("AnimGraphEditToolset", "anim_disconnect_pins", blueprint_path=BP, node_guid=blend["node_guid"], pin="A")
ok("AnimGraphEditToolset", "anim_set_node_position", blueprint_path=BP, node_guid=blend["node_guid"], x=0, y=500)
gv = ok("AnimGraphEditToolset", "anim_add_node", blueprint_path=BP, graph="EventGraph", node_class="K2Node_VariableGet", variable_name="Speed", x=0, y=0)
gv and log("var get pins=%s" % [p["name"] for p in gv["pins"]])
ok("AnimGraphEditToolset", "anim_remove_node", blueprint_path=BP, node_guid=gv["node_guid"])
ok("AnimGraphEditToolset", "anim_remove_node", blueprint_path=BP, node_guid=sp["node_guid"])
ok("AnimGraphEditToolset", "anim_remove_node", blueprint_path=BP, node_guid=blend["node_guid"])
expect_fail("AnimGraphEditToolset", "anim_remove_node", blueprint_path=BP, node_guid=root)
r = ok("AnimInspectToolset", "anim_get_node", blueprint_path=BP, node_guid=t1["node_guid"]); r and log("t1=%s" % {k: r.get(k) for k in ("from_state","to_state","crossfade_duration")})

r = ok("AnimAssetToolset", "anim_compile_blueprint", blueprint_path=BP); r and log("compile: %s errors=%d warnings=%d msgs=%s" % (r["status"], r["num_errors"], r["num_warnings"], r["messages"][:5]))

ok("AnimAssetToolset", "anim_create_blendspace", folder="/Game/AMCPTest", asset_name="BS_Test", skeleton_path=SK, dimensions=1, x_axis_name="Speed", x_min=0, x_max=300, x_grid=4)
ok("AnimAssetToolset", "anim_add_blendspace_sample", blendspace_path="/Game/AMCPTest/BS_Test", animation_path=IDLE, x=0)
r = ok("AnimAssetToolset", "anim_add_blendspace_sample", blendspace_path="/Game/AMCPTest/BS_Test", animation_path=WALK, x=300); r and log("samples=%s" % r)
expect_fail("AnimAssetToolset", "anim_add_blendspace_sample", blendspace_path="/Game/AMCPTest/BS_Test", animation_path=WALK, x=999)
ok("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP, state_guid=walk["node_guid"], asset_path="/Game/AMCPTest/BS_Test")
r = ok("AnimAssetToolset", "anim_compile_blueprint", blueprint_path=BP); r and log("compile2: %s errors=%d warnings=%d msgs=%s" % (r["status"], r["num_errors"], r["num_warnings"], r["messages"][:5]))
r = ok("AnimAssetToolset", "anim_save_asset", asset_path=BP); r and log("save=%s" % r)
r = ok("AnimAssetToolset", "anim_save_asset", asset_path="/Game/AMCPTest/BS_Test"); r and log("save=%s" % r)
ok("AnimAssetToolset", "anim_remove_variable", blueprint_path=BP, name="Speed", force=True)
expect_fail("AnimAssetToolset", "anim_remove_variable", blueprint_path=BP, name="bIsMoving")

# --- v0.2 step 2: anim_build_state_machine
BP2 = "/Game/AMCPTest/ABP_Build"
ok("AnimAssetToolset", "anim_create_anim_blueprint", folder="/Game/AMCPTest", asset_name="ABP_Build", skeleton_path=SK)
spec = {"name": "Loco", "connect_to_output": True,
        "variables": [{"name": "Speed", "type": "float", "default": "0"}, {"name": "bFalling", "type": "bool"}],
        "entry_state": "Idle",
        "states": [{"name": "Idle", "animation": IDLE}, {"name": "Walk", "animation": WALK, "loop": True, "play_rate": 1.5},
                   {"name": "Land", "animation": IDLE, "loop": False}],
        "conduits": [{"name": "Air"}],
        "transitions": [{"from": "Idle", "to": "Walk", "rule": "compare", "variable": "Speed", "comparison": ">", "threshold": 10},
                        {"from": "Walk", "to": "Idle", "rule": "compare", "variable": "Speed", "comparison": "<=", "threshold": 10},
                        {"from": "Walk", "to": "Air", "rule": "bool_variable", "variable": "bFalling"},
                        {"from": "Air", "to": "Land", "rule": "always"},
                        {"from": "Land", "to": "Idle", "rule": "time_remaining", "trigger_time": 0.2}]}
r = ok("AnimStateMachineToolset", "anim_build_state_machine", blueprint_path=BP2, spec=json.dumps(spec))
if r:
    log("%s build result: states=%d conduits=%d transitions=%d vars=%s connected=%s" % (
        "OK  " if len(r["states"]) == 3 and len(r["transitions"]) == 5 and r["connected_to_output"] else "FAIL",
        len(r["states"]), len(r["conduits"]), len(r["transitions"]), r["variables_created"], r["connected_to_output"]))
    walk_player = [s for s in r["states"] if s["name"] == "Walk"][0]["player_node_guid"]
    n = ok("AnimInspectToolset", "anim_get_node", blueprint_path=BP2, node_guid=walk_player); n and log("walk player: %s" % n.get("animation_asset"))
r = ok("AnimAssetToolset", "anim_compile_blueprint", blueprint_path=BP2); r and log("%s compile built SM: errors=%d warnings=%d msgs=%s" % ("OK  " if r["num_errors"] == 0 else "FAIL", r["num_errors"], r["num_warnings"], r["messages"][:3]))
bad = {"name": "Loco2", "states": [{"name": "A", "animation": "/Game/Nope"}], "transitions": [{"from": "A", "to": "B", "rule": "compare", "variable": "Missing"}], "typo": 1}
expect_fail("AnimStateMachineToolset", "anim_build_state_machine", blueprint_path=BP2, spec=json.dumps(bad))
r = ok("AnimInspectToolset", "anim_list_nodes", blueprint_path=BP2, graph="AnimGraph")
r and log("%s nothing created on bad spec: state machines=%d" % ("OK  " if len([x for x in r["nodes"] if x["class"] == "AnimGraphNode_StateMachine"]) == 1 else "FAIL", len([x for x in r["nodes"] if x["class"] == "AnimGraphNode_StateMachine"])))
expect_fail("AnimStateMachineToolset", "anim_build_state_machine", blueprint_path=BP2, spec="{not json")

# --- v0.2 step 3: anim_set_state_animation node types, loop, play rate
r = ok("AnimInspectToolset", "anim_list_nodes", blueprint_path=BP2, graph="AnimGraph")
sm2 = [x for x in r["nodes"] if x["class"] == "AnimGraphNode_StateMachine"][0]["node_guid"]
st = ok("AnimStateMachineToolset", "anim_add_state", blueprint_path=BP2, state_machine_guid=sm2, name="Options", x=300, y=600)
r = ok("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP2, state_guid=st["node_guid"], asset_path=WALK)
r and log("%s default node: %s" % ("OK  " if r["class"] == "AnimGraphNode_SequencePlayer" else "FAIL", r["class"]))
r = ok("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP2, state_guid=st["node_guid"], asset_path=WALK, loop=False, play_rate=0.5)
r and log("%s non-default loop/play rate accepted: %s" % ("OK  " if r["class"] == "AnimGraphNode_SequencePlayer" else "FAIL", r["class"]))  # values themselves are not readable through the tools
r = ok("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP2, state_guid=st["node_guid"], asset_path=WALK, node_type="sequence_evaluator", loop=False)
r and log("%s evaluator: %s" % ("OK  " if r["class"] == "AnimGraphNode_SequenceEvaluator" else "FAIL", r["class"]))
r = ok("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP2, state_guid=st["node_guid"], asset_path="/Game/AMCPTest/BS_Test", node_type="blendspace_player", play_rate=2)
r and log("%s blend space player: %s" % ("OK  " if r["class"] == "AnimGraphNode_BlendSpacePlayer" else "FAIL", r["class"]))
r = ok("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP2, state_guid=st["node_guid"], asset_path="/Game/AMCPTest/BS_Test", node_type="blendspace_evaluator")
r and log("%s blend space evaluator: %s" % ("OK  " if r["class"] == "AnimGraphNode_BlendSpaceEvaluator" else "FAIL", r["class"]))
r = ok("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP2, state_guid=st["node_guid"], asset_path=IDLE, node_type="slot", slot_name="DefaultSlot")
r and log("%s slot: %s player=%s" % ("OK  " if r["class"] == "AnimGraphNode_Slot" and r.get("player_node_guid") else "FAIL", r["class"], r.get("player_node_guid")))
r = ok("AnimInspectToolset", "anim_list_nodes", blueprint_path=BP2, graph="Options")
r and log("%s state graph after replacements: %s" % ("OK  " if sorted(x["class"] for x in r["nodes"]) == ["AnimGraphNode_SequencePlayer", "AnimGraphNode_Slot", "AnimGraphNode_StateResult"] else "FAIL", sorted(x["class"] for x in r["nodes"])))
r = ok("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP2, state_guid=st["node_guid"], asset_path="none", node_type="slot")
expect_fail("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP2, state_guid=st["node_guid"], asset_path=WALK, node_type="sequence_evaluator", play_rate=2)
expect_fail("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP2, state_guid=st["node_guid"], asset_path=WALK, node_type="blendspace_player")
expect_fail("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP2, state_guid=st["node_guid"], asset_path=WALK, node_type="wobble")
expect_fail("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP2, state_guid=st["node_guid"], asset_path="none")
spec3 = {"name": "Spec3", "states": [{"name": "A", "animation": WALK, "node_type": "sequence_evaluator"}, {"name": "B", "node_type": "slot", "slot_name": "DefaultSlot"}]}
r = ok("AnimStateMachineToolset", "anim_build_state_machine", blueprint_path=BP2, spec=json.dumps(spec3))
r and log("%s spec node types: %s" % ("OK  " if r["states"][1].get("slot_node_guid") and r["states"][0].get("player_node_guid") else "FAIL", r["states"]))
r = ok("AnimAssetToolset", "anim_compile_blueprint", blueprint_path=BP2); r and log("%s compile after node types: errors=%d warnings=%d" % ("OK  " if r["num_errors"] == 0 else "FAIL", r["num_errors"], r["num_warnings"]))

# --- v0.2 step 4: dead-rule warnings, conduit rules, compile message sources
spec4 = {"name": "Jump", "variables": [{"name": "bJump", "type": "bool"}],
         "states": [{"name": "Ground", "animation": IDLE}, {"name": "JumpUp"}, {"name": "JumpDown", "animation": WALK, "loop": False}],
         "conduits": [{"name": "Gate", "rule": "bool_variable", "variable": "bJump"}],
         "transitions": [{"from": "Ground", "to": "Gate", "rule": "always"}, {"from": "Gate", "to": "JumpUp", "rule": "always"},
                         {"from": "JumpUp", "to": "JumpDown", "rule": "time_remaining"},
                         {"from": "JumpDown", "to": "Ground", "rule": "time_remaining"}, {"from": "JumpDown", "to": "JumpUp"}]}
r = ok("AnimStateMachineToolset", "anim_build_state_machine", blueprint_path=BP2, spec=json.dumps(spec4))
if r:
    w = r["warnings"]
    log("%s build warnings: %s" % ("OK  " if len(w) == 2 and any("JumpUp" in x and "time_remaining" in x for x in w) and any("no rule" in x for x in w) else "FAIL", w))
    log("%s conduit rule: %s" % ("OK  " if r["conduits"][0]["rule"] == "bool_variable" and len(r["conduits"][0]["rule_nodes"]) == 1 else "FAIL", r["conduits"][0]))
    jt = {(t["from"], t["to"]): t["node_guid"] for t in r["transitions"]}
    st_guid = {s["name"]: s["node_guid"] for s in r["states"]}
    t = ok("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP2, transition_guid=jt[("JumpUp", "JumpDown")], rule="time_remaining", trigger_time=0.1)
    t and log("%s dead rule accepted with warning: %s" % ("OK  " if len(t["warnings"]) == 1 else "FAIL", t["warnings"]))
    t = ok("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP2, transition_guid=jt[("JumpDown", "Ground")], rule="time_remaining")
    t and log("%s live rule has no warning: %s" % ("OK  " if t["warnings"] == [] else "FAIL", t["warnings"]))
    t = ok("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP2, transition_guid=r["conduits"][0]["node_guid"], rule="compare", variable_name="Speed", comparison=">", threshold=1)
    t and log("%s conduit rule via anim_set_transition_rule: %s" % ("OK  " if t["rule"] == "compare" else "FAIL", t["rule"]))
    expect_fail("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP2, transition_guid=r["conduits"][0]["node_guid"], rule="time_remaining")
    expect_fail("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP2, transition_guid=st_guid["Ground"], rule="always")
    c = ok("AnimAssetToolset", "anim_compile_blueprint", blueprint_path=BP2)
    if c:
        srcs = [m.get("source") for m in c["messages"]]
        log("compile messages: %s" % [(m["message"][:60], (m.get("source") or {}).get("location")) for m in c["messages"]])
        log("%s every compile message has a source: %d/%d" % ("OK  " if c["messages"] and all(srcs) else "FAIL", len([x for x in srcs if x]), len(srcs)))
        never = [m for m in c["messages"] if "never be taken" in m["message"]]
        # The engine only warns for conduits here; a transition with no rule is silent at compile time (anim_build_state_machine warns instead).
        log("%s 'never taken' names the conduit: %s" % ("OK  " if never and (never[0].get("source") or {}).get("location") == "AnimGraph > state machine Loco > conduit Air" else "FAIL", never and never[0]["source"].get("location")))
        ignored = [m for m in c["messages"] if "visible but ignored" in m["message"]]
        log("%s 'visible but ignored' names the state: %s" % ("OK  " if ignored and (ignored[0].get("source") or {}).get("state") == "Options" else "FAIL", ignored and ignored[0]["source"].get("location")))

fails = [l for l in LOG if l.startswith("FAIL")]
log("SUMMARY: %d checks, %d failures" % (len([l for l in LOG if l.startswith(("OK", "FAIL"))]), len(fails)))
