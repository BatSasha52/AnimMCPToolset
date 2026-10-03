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
        return env.get("error") or ""
    log("FAIL %s should have been rejected" % tool)
    return ""

def check(cond, what, detail=""):
    log("%s %s%s" % ("OK  " if cond else "FAIL", what, (": %s" % (detail,)) if detail != "" else ""))
    return cond

def pins_of(node):
    return {p["name"]: p for p in node.get("pins", [])}

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

# --- v0.2 step 5: skeleton tools
r = ok("AnimInspectToolset", "anim_get_skeleton_info", asset_path=SK)
r and log("%s skeleton info: bones=%d listed=%d sockets=%d virtual=%d compatible=%s slot_groups=%s" % (
    "OK  " if r["bone_count"] == len(r["bones"]) > 0 else "FAIL", r["bone_count"], len(r["bones"]), len(r["sockets"]), len(r["virtual_bones"]), r["compatible_skeletons"], r["slot_groups"]))
r = ok("AnimInspectToolset", "anim_get_skeleton_info", asset_path=SK, include_bones=False)
r and log("%s include_bones=false: %s" % ("OK  " if "bones" not in r and r["bone_count"] > 0 else "FAIL", sorted(r.keys())))
SK_COPY = "/Game/AMCPTest/SK_Copy"
unreal.EditorAssetLibrary.duplicate_asset(SK, SK_COPY)
before = ok("AnimInspectToolset", "anim_list_animation_assets", skeleton_path=SK_COPY, folder="/Engine/Tutorial/SubEditors/TutorialAssets")
r = ok("AnimAssetToolset", "anim_set_skeleton_compatible", skeleton_path=SK_COPY, compatible_skeleton_path=SK)
after = ok("AnimInspectToolset", "anim_list_animation_assets", skeleton_path=SK_COPY, folder="/Engine/Tutorial/SubEditors/TutorialAssets")
if r and before is not None and after is not None:
    log("%s compatible skeleton makes anims usable: changed=%s list=%s anims before=%d after=%d" % (
        "OK  " if r["changed"] and len(before["assets"]) == 0 and len(after["assets"]) > 0 else "FAIL", r["changed"], r["compatible_skeletons"], len(before["assets"]), len(after["assets"])))
r = ok("AnimAssetToolset", "anim_set_skeleton_compatible", skeleton_path=SK_COPY, compatible_skeleton_path=SK)
r and log("%s adding twice is a no-op: changed=%s" % ("OK  " if not r["changed"] else "FAIL", r["changed"]))
r = ok("AnimAssetToolset", "anim_set_skeleton_compatible", skeleton_path=SK_COPY, compatible_skeleton_path=SK, compatible=False)
r and log("%s remove: changed=%s list=%s" % ("OK  " if r["changed"] and r["compatible_skeletons"] == [] else "FAIL", r["changed"], r["compatible_skeletons"]))
expect_fail("AnimAssetToolset", "anim_set_skeleton_compatible", skeleton_path=SK, compatible_skeleton_path=SK_COPY)
expect_fail("AnimAssetToolset", "anim_set_skeleton_compatible", skeleton_path=SK_COPY, compatible_skeleton_path=SK_COPY)

# --- v0.2 step 7: add_locomotion_vars
BP3 = "/Game/AMCPTest/ABP_Loco"
r = ok("AnimAssetToolset", "anim_create_anim_blueprint", folder="/Game/AMCPTest", asset_name="ABP_Loco", skeleton_path=SK, add_locomotion_vars=True)
r and log("%s locomotion setup: vars=%s nodes=%d" % ("OK  " if r.get("locomotion_variables") == ["Speed", "IsMoving", "IsFalling"] and len(r.get("locomotion_nodes", [])) >= 14 else "FAIL", r.get("locomotion_variables"), len(r.get("locomotion_nodes", []))))
r = ok("AnimInspectToolset", "anim_list_variables", blueprint_path=BP3)
r and log("%s locomotion vars: %s" % ("OK  " if [(v["name"], v["category"]) for v in r["variables"]] == [("Speed", "Locomotion"), ("IsMoving", "Locomotion"), ("IsFalling", "Locomotion")] else "FAIL", [(v["name"], v["type"]) for v in r["variables"]]))
r = ok("AnimInspectToolset", "anim_list_nodes", blueprint_path=BP3, graph="EventGraph", include_pins=True)
if r:
    ev = [n for n in r["nodes"] if n["class"] == "K2Node_Event"]
    then_links = [p["linked_to"] for p in ev[0]["pins"] if p["name"] == "then"] if ev else []
    log("%s update event wired: events=%d then_links=%s titles=%s" % ("OK  " if len(ev) == 1 and then_links and then_links[0] else "FAIL", len(ev), then_links, sorted(n["title"] for n in r["nodes"])))
r = ok("AnimAssetToolset", "anim_compile_blueprint", blueprint_path=BP3); r and log("%s compile locomotion ABP: errors=%d warnings=%d msgs=%s" % ("OK  " if r["num_errors"] == 0 and r["num_warnings"] == 0 else "FAIL", r["num_errors"], r["num_warnings"], r["messages"][:3]))
spec7 = {"name": "Loco", "connect_to_output": True, "states": [{"name": "Idle", "animation": IDLE}, {"name": "Walk", "animation": WALK}],
         "transitions": [{"from": "Idle", "to": "Walk", "rule": "bool_variable", "variable": "IsMoving"}, {"from": "Walk", "to": "Idle", "rule": "compare", "variable": "Speed", "comparison": "<", "threshold": 3}]}
r = ok("AnimStateMachineToolset", "anim_build_state_machine", blueprint_path=BP3, spec=json.dumps(spec7))
r and log("%s state machine on locomotion vars: warnings=%s" % ("OK  " if r["warnings"] == [] else "FAIL", r["warnings"]))
r = ok("AnimAssetToolset", "anim_compile_blueprint", blueprint_path=BP3); r and log("%s compile full locomotion ABP: errors=%d warnings=%d" % ("OK  " if r["num_errors"] == 0 and r["num_warnings"] == 0 else "FAIL", r["num_errors"], r["num_warnings"]))
expect_fail("AnimAssetToolset", "anim_create_anim_blueprint", folder="/Game/AMCPTest", asset_name="ABP_LocoChild", skeleton_path=SK, parent_class=BP3, add_locomotion_vars=True)
r = ok("AnimAssetToolset", "anim_create_anim_blueprint", folder="/Game/AMCPTest", asset_name="ABP_Plain", skeleton_path=SK)
r and log("%s default create has no locomotion setup: %s" % ("OK  " if "locomotion_variables" not in r else "FAIL", sorted(r.keys())))

# --- v0.2: listing checks (the ABP_Batman report)
r = ok("AnimInspectToolset", "anim_list_anim_blueprints", folder="/Game/AMCPTest/")
names = sorted(a["name"] for a in r["anim_blueprints"]) if r else []
log("%s trailing slash and unsaved new ABPs listed: %s" % ("OK  " if {"ABP_Test", "ABP_Build", "ABP_Loco", "ABP_Plain"} <= set(names) else "FAIL", names))
expect_fail("AnimInspectToolset", "anim_list_anim_blueprints", folder="Game/AMCPTest")

# --- v0.2 step 6: name resolution. The registry schema (what describe_toolset prints) uses qualified names,
# but tools are looked up by their short name, so the qualified name is rejected. Engine behaviour, documented in the README.
qualified = [t["name"] for s in schemas if s["name"] == "AnimMCPToolset.AnimInspectToolset" for t in s["tools"] if t["name"].endswith("anim_list_anim_blueprints")]
rq = unreal.ToolsetRegistry.execute_tool("AnimMCPToolset.AnimInspectToolset", qualified[0] if qualified else "", "{}")
rs = unreal.ToolsetRegistry.execute_tool("AnimMCPToolset.AnimInspectToolset", "anim_list_anim_blueprints", "{}")
log("%s qualified tool name: schema=%s qualified_error=%r short_error=%r" % (
    "OK  " if qualified and qualified[0].startswith("AnimMCPToolset.") and "Unknown tool" in (rq.error or "") and not rs.error else "FAIL",
    qualified, rq.error, rs.error))

# --- v0.3 step 1: anim_build_state_machine upgrades (wildcard, bindings, extra nodes, combined conditions, priority, blend settings)
BP4 = "/Game/AMCPTest/ABP_Spec"
BS = "/Game/AMCPTest/BS_Test"
ok("AnimAssetToolset", "anim_create_anim_blueprint", folder="/Game/AMCPTest", asset_name="ABP_Spec", skeleton_path=SK)
curve = unreal.AssetToolsHelpers.get_asset_tools().create_asset("CV_Blend", "/Game/AMCPTest", unreal.CurveFloat, unreal.CurveFloatFactory())
check(curve is not None, "blend curve asset created for the test")

def state_machines(bp):
    r = ok("AnimInspectToolset", "anim_list_nodes", blueprint_path=bp, graph="AnimGraph")
    return [n for n in r["nodes"] if n["class"] == "AnimGraphNode_StateMachine"] if r else []

spec5 = {"name": "Loco", "connect_to_output": True,
         "variables": [{"name": "Speed", "type": "float"}, {"name": "Rate", "type": "float", "default": "1"}, {"name": "Lean", "type": "float"},
                       {"name": "bFalling", "type": "bool"}, {"name": "bLadder", "type": "bool"}],
         "states": [{"name": "Idle", "animation": IDLE},
                    {"name": "Move", "animation": BS, "bind": {"X": "Speed"}},
                    {"name": "Walk", "animation": WALK, "bind": {"playrate": "Rate"},
                     "nodes": [{"class": "AnimGraphNode_Slot", "properties": {"Node.SlotName": "UpperBody"}},
                               {"class": "AnimGraphNode_ModifyCurve", "properties": {"Node.CurveMap": "((\"Lean\", 1.0))"}, "bind": {"Alpha": "Lean"}}]},
                    {"name": "Fall", "animation": IDLE}],
         "transitions": [{"from": "Idle", "to": "Move", "priority": 2, "blend_mode": "Linear",
                          "rule": {"and": [{"compare": "Speed", "comparison": ">", "threshold": 10}, {"not": {"bool": "bFalling"}}]}},
                         {"from": "Move", "to": "Walk", "blend_curve": "/Game/AMCPTest/CV_Blend",
                          "rule": {"or": [{"bool": "bLadder"}, {"compare": "Speed", "comparison": "<", "threshold": 5}, {"bool": "bFalling"}]}},
                         {"from": "*", "to": "Fall", "rule": "bool_variable", "variable": "bFalling", "priority": 0, "crossfade_duration": 0.1},
                         {"from": "Idle", "to": "Fall", "rule": "always"}]}
r = ok("AnimStateMachineToolset", "anim_build_state_machine", blueprint_path=BP4, spec=json.dumps(spec5))
if r:
    tr = {(t["from"], t["to"]): t for t in r["transitions"]}
    wild = sorted(t["from"] for t in r["transitions"] if t.get("from_wildcard"))
    check(len(r["transitions"]) == 5 and wild == ["Move", "Walk"] and not tr[("Idle", "Fall")].get("from_wildcard"),
          "wildcard expands to every other state, explicit Idle->Fall wins", sorted(tr.keys()))
    check(all(tr[(f, "Fall")]["priority"] == 0 for f in wild) and tr[("Idle", "Fall")]["priority"] == 1 and tr[("Idle", "Fall")]["rule"] == "always",
          "wildcard transitions carry the spec's priority and rule", [(k, v["priority"], v["rule"]) for k, v in tr.items()])
    t = tr[("Idle", "Move")]
    check(t["rule"] == "condition" and t["priority"] == 2 and t["blend_mode"] == "Linear" and len(t["rule_nodes"]) == 5,
          "and/not/compare condition, priority 2, blend Linear", (t["rule"], t["priority"], t["blend_mode"], len(t["rule_nodes"])))
    t = tr[("Move", "Walk")]
    n = ok("AnimInspectToolset", "anim_get_node", blueprint_path=BP4, node_guid=t["node_guid"])
    check(n and n.get("blend_mode") == "Custom" and n.get("blend_curve", "").endswith("CV_Blend") and len(t["rule_nodes"]) == 6,
          "or condition, blend_curve implies Custom", n and (n.get("blend_mode"), n.get("blend_curve"), len(t["rule_nodes"])))
    n = ok("AnimInspectToolset", "anim_get_node", blueprint_path=BP4, node_guid=tr[("Walk", "Fall")]["node_guid"])
    check(n and n["priority_order"] == 0 and abs(n["crossfade_duration"] - 0.1) < 1e-6, "transition node holds priority and crossfade", n and (n["priority_order"], n["crossfade_duration"]))
    rg = [g for g in ok("AnimInspectToolset", "anim_list_graphs", blueprint_path=BP4)["graphs"] if g.get("owner_node_guid") == tr[("Idle", "Move")]["node_guid"]][0]
    rn = ok("AnimInspectToolset", "anim_list_nodes", blueprint_path=BP4, graph=rg["graph_guid"])
    calls = [x["title"] for x in rn["nodes"] if x["class"] == "K2Node_CallFunction"] if rn else []
    check(len(calls) == 3 and any("AND" in x for x in calls) and any("NOT" in x for x in calls) and any(">" in x for x in calls),
          "rule graph has >, NOT, AND", calls)

    st = {s["name"]: s for s in r["states"]}
    move = st["Move"]
    check([(b["pin"], b["variable"]) for b in move.get("bindings", [])] == [("X", "Speed")], "Move binds X to Speed", move.get("bindings"))
    p = ok("AnimInspectToolset", "anim_get_node", blueprint_path=BP4, node_guid=move["player_node_guid"])
    xpin = pins_of(p).get("X", {}) if p else {}
    check(p and p["class"] == "AnimGraphNode_BlendSpacePlayer" and [l["node_guid"] for l in xpin.get("linked_to", [])] == [move["bindings"][0]["getter_node_guid"]],
          "blend space X pin is driven by the Speed getter", xpin.get("linked_to"))
    walk_s = st["Walk"]
    check([x["class"] for x in walk_s.get("nodes", [])] == ["AnimGraphNode_Slot", "AnimGraphNode_ModifyCurve"], "Walk extra nodes in order", walk_s.get("nodes"))
    check(sorted((b["pin"], b["variable"]) for b in walk_s.get("bindings", [])) == [("Alpha", "Lean"), ("PlayRate", "Rate")],
          "PlayRate (hidden by default) and ModifyCurve Alpha are bound", walk_s.get("bindings"))
    g = ok("AnimInspectToolset", "anim_list_nodes", blueprint_path=BP4, graph="Walk", include_pins=True)
    if g:
        by_class = {x["class"]: x for x in g["nodes"]}
        slot_guid, mc_guid, player_guid = walk_s["nodes"][0]["node_guid"], walk_s["nodes"][1]["node_guid"], walk_s["player_node_guid"]
        result_in = [p for p in by_class["AnimGraphNode_StateResult"]["pins"] if p["direction"] == "input"][0]
        chain_ok = ([l["node_guid"] for l in result_in["linked_to"]] == [mc_guid]
                    and [l["node_guid"] for l in pins_of(by_class["AnimGraphNode_ModifyCurve"])["SourcePose"]["linked_to"]] == [slot_guid]
                    and [l["node_guid"] for l in pins_of(by_class["AnimGraphNode_Slot"])["Source"]["linked_to"]] == [player_guid])
        check(chain_ok, "chain player -> Slot -> ModifyCurve -> Output Pose", sorted(by_class.keys()))
        check("UpperBody" in by_class["AnimGraphNode_Slot"]["title"], "Slot name set through properties", by_class["AnimGraphNode_Slot"]["title"])
        rate = pins_of(by_class["AnimGraphNode_SequencePlayer"]).get("PlayRate", {})
        check(rate and not rate["hidden"] and rate["linked_to"], "PlayRate pin exposed and linked", rate.get("linked_to"))
    c = ok("AnimAssetToolset", "anim_compile_blueprint", blueprint_path=BP4)
    c and check(c["num_errors"] == 0, "compile spec with bindings, nodes and conditions", (c["num_errors"], c["num_warnings"], [m["message"][:80] for m in c["messages"]][:4]))

    # anim_set_transition_rule with a combined condition
    t = ok("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP4, transition_guid=tr[("Idle", "Fall")]["node_guid"], rule="condition",
           condition=json.dumps({"or": [{"bool": "bLadder"}, {"not": {"bool": "bFalling"}}]}))
    t and check(t["rule"] == "condition" and len(t["rule_nodes"]) == 4, "anim_set_transition_rule condition", (t["rule"], len(t["rule_nodes"])))
    expect_fail("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP4, transition_guid=tr[("Idle", "Fall")]["node_guid"], rule="bool_variable", variable_name="bFalling", condition='{"bool": "bFalling"}')
    expect_fail("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP4, transition_guid=tr[("Idle", "Fall")]["node_guid"], rule="condition", condition="{nope")
    expect_fail("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP4, transition_guid=tr[("Idle", "Fall")]["node_guid"], rule="condition")
    expect_fail("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP4, transition_guid=tr[("Idle", "Fall")]["node_guid"], rule="condition", condition='{"and": [{"bool": "bFalling"}]}')
    c = ok("AnimAssetToolset", "anim_compile_blueprint", blueprint_path=BP4)
    c and check(c["num_errors"] == 0, "compile after condition rule", c["num_errors"])

# Atomic: every problem is listed and nothing is created.
before = len(state_machines(BP4))
bad5 = {"name": "Bad", "variables": [{"name": "bJump", "type": "bool"}],
        "states": [{"name": "A", "animation": BS, "bind": {"Z": "Speed", "X": "bJump"}},
                   {"name": "B", "nodes": [{"class": "NoSuchNode"}, {"class": "AnimGraphNode_Slot", "properties": {"Node.NoProp": "1"}}]},
                   {"name": "C", "bind": {"X": "Speed"}}],
        "transitions": [{"from": "A", "to": "B", "blend_mode": "Wobbly"},
                        {"from": "B", "to": "A", "blend_mode": "Linear", "blend_curve": "/Game/AMCPTest/CV_Blend"},
                        {"from": "A", "to": "*"},
                        {"from": "A", "to": "C", "priority": 1.5, "rule": {"bool": "bJump", "not": {"bool": "bJump"}}},
                        {"from": "B", "to": "C", "rule": {"and": [{"bool": "Missing"}, {"compare": "Speed", "comparison": ">"}]}}]}
err = expect_fail("AnimStateMachineToolset", "anim_build_state_machine", blueprint_path=BP4, spec=json.dumps(bad5))
want = ["no pin 'Z'", "bind 'X': variable 'bJump'", "NoSuchNode", "NoProp", "needs an 'animation'", "Wobbly", "only used with blend_mode 'Custom'",
        "only 'from' may be '*'", "priority must be a whole number", "exactly one of", "'Missing'", "'compare' needs"]
missing = [w for w in want if w not in err]
check(not missing, "bad spec lists every problem", missing or err.count("\n- "))
check(len(state_machines(BP4)) == before, "nothing created on bad spec", len(state_machines(BP4)))
err = expect_fail("AnimStateMachineToolset", "anim_build_state_machine", blueprint_path=BP4, spec=json.dumps(
    {"name": "Wild", "states": [{"name": "A"}, {"name": "B"}], "transitions": [{"from": "A", "to": "B"}, {"from": "*", "to": "B"}]}))
check("expands to no transitions" in err, "wildcard that expands to nothing is rejected", err[:120])
check(len(state_machines(BP4)) == before, "still nothing created", len(state_machines(BP4)))

# --- v0.3 step 2: tools that replace or break links report every link they disconnected
def links(lst):
    return sorted((l["from_node_guid"], l["from_pin"], l["to_node_guid"], l["to_pin"]) for l in lst)

r = ok("AnimInspectToolset", "anim_list_nodes", blueprint_path=BP4, graph="AnimGraph", include_pins=True)
root4 = [n for n in r["nodes"] if n["class"] == "AnimGraphNode_Root"][0]
loco4 = [n for n in r["nodes"] if n["class"] == "AnimGraphNode_StateMachine"][0]
old_out = [(l["node_guid"], l["pin_name"]) for p in root4["pins"] if p["direction"] == "input" for l in p["linked_to"]]
check(old_out and old_out[0][0] == loco4["node_guid"], "Loco drives the Output Pose before the rebuild", old_out)
r = ok("AnimStateMachineToolset", "anim_build_state_machine", blueprint_path=BP4, spec=json.dumps({"name": "Loco2", "connect_to_output": True, "states": [{"name": "Idle", "animation": IDLE}]}))
r and check(links(r["disconnected"]) == [(loco4["node_guid"], "Pose", root4["node_guid"], "Result")], "connect_to_output reports the replaced Output Pose link", r["disconnected"])

p1 = ok("AnimGraphEditToolset", "anim_add_node", blueprint_path=BP4, graph="AnimGraph", node_class="AnimGraphNode_SequencePlayer", x=-600, y=400)
p2 = ok("AnimGraphEditToolset", "anim_add_node", blueprint_path=BP4, graph="AnimGraph", node_class="AnimGraphNode_SequencePlayer", x=-600, y=600)
bl = ok("AnimGraphEditToolset", "anim_add_node", blueprint_path=BP4, graph="AnimGraph", node_class="AnimGraphNode_TwoWayBlend", x=-300, y=500)
r = ok("AnimGraphEditToolset", "anim_connect_pins", blueprint_path=BP4, from_node_guid=p1["node_guid"], from_pin="Pose", to_node_guid=bl["node_guid"], to_pin="A")
r and check(r["disconnected"] == [] and not r["replaced_existing_links"], "first connection disconnects nothing", r["disconnected"])
r = ok("AnimGraphEditToolset", "anim_connect_pins", blueprint_path=BP4, from_node_guid=p2["node_guid"], from_pin="Pose", to_node_guid=bl["node_guid"], to_pin="A")
r and check(r["replaced_existing_links"] and links(r["disconnected"]) == [(p1["node_guid"], "Pose", bl["node_guid"], "A")],
            "replacing a pose link names the old link", r["disconnected"])
sm2 = state_machines(BP4)
loco2 = [n for n in sm2 if n["state_machine_graph"] == "Loco2"][0]["node_guid"]
r = ok("AnimGraphEditToolset", "anim_connect_pins", blueprint_path=BP4, from_node_guid=bl["node_guid"], from_pin="Pose", to_node_guid=root4["node_guid"], to_pin="Result")
r and check(links(r["disconnected"]) == [(loco2, "Pose", root4["node_guid"], "Result")] and r["disconnected"][0]["to_node"],
            "Output Pose link replaced by connect_pins is reported", r["disconnected"])
r = ok("AnimGraphEditToolset", "anim_disconnect_pins", blueprint_path=BP4, node_guid=bl["node_guid"], pin="A")
r and check(r["links_broken"] == 1 and links(r["disconnected"]) == [(p2["node_guid"], "Pose", bl["node_guid"], "A")], "disconnect_pins lists the broken link", r["disconnected"])
r = ok("AnimGraphEditToolset", "anim_set_node_property", blueprint_path=BP4, node_guid=bl["node_guid"], property_path="Node.bAlwaysUpdateChildren", value="true")
r and check(r["disconnected"] == [], "property change that keeps pins disconnects nothing", r["disconnected"])
r = ok("AnimGraphEditToolset", "anim_remove_node", blueprint_path=BP4, node_guid=bl["node_guid"])
r and check(links(r["disconnected"]) == [(bl["node_guid"], "Pose", root4["node_guid"], "Result")], "remove_node lists the links it broke", r["disconnected"])

r = ok("AnimInspectToolset", "anim_list_nodes", blueprint_path=BP4, graph="Loco")
loco_nodes = {n.get("state_name") or n.get("title"): n for n in r["nodes"]} if r else {}
entry = [n for n in r["nodes"] if n["class"] == "AnimStateEntryNode"][0]["node_guid"]
r = ok("AnimStateMachineToolset", "anim_add_state", blueprint_path=BP4, state_machine_guid=loco4["node_guid"], name="Start", x=0, y=600, set_as_entry=True)
r and check(len(r["disconnected"]) == 1 and r["disconnected"][0]["from_node_guid"] == entry and r["disconnected"][0]["to_node_guid"] == loco_nodes["Idle"]["node_guid"],
            "set_as_entry reports the previous Entry link", r["disconnected"])
r = ok("AnimStateMachineToolset", "anim_set_state_animation", blueprint_path=BP4, state_guid=loco_nodes["Move"]["node_guid"], asset_path=WALK)
if r:
    removed = [n["class"] for n in r["removed_nodes"]]
    gone = [(l["from_node"], l["from_pin"], l["to_pin"]) for l in r["disconnected"]]
    check(removed == ["AnimGraphNode_BlendSpacePlayer"] and len(r["disconnected"]) == 2 and any(p == "X" for _, _, p in gone),
          "set_state_animation reports the replaced player and the binding it orphaned", (removed, gone))
fall_t = [n for n in loco_nodes.values() if n.get("from_state") == "Idle" and n.get("to_state") == "Fall"][0]["node_guid"]
r = ok("AnimStateMachineToolset", "anim_set_transition_rule", blueprint_path=BP4, transition_guid=fall_t, rule="always")
r and check(len(r["removed_nodes"]) == 4 and len(r["disconnected"]) == 4, "set_transition_rule reports the old rule's nodes and links",
            ([n["title"] for n in r["removed_nodes"]], len(r["disconnected"])))
r = ok("AnimStateMachineToolset", "anim_remove_state", blueprint_path=BP4, state_guid=loco_nodes["Fall"]["node_guid"])
r and check(len(r["removed_transition_guids"]) == 3 and len(r["disconnected"]) == 6, "remove_state reports the transition links it broke",
            (len(r["removed_transition_guids"]), len(r["disconnected"])))

# --- v0.3 step 3: animation data, read-only
def close(a, b, eps=1e-3):
    return all(abs(x - y) <= eps for x, y in zip(a, b)) and len(a) == len(b)

info = ok("AnimDataToolset", "anim_get_animation_info", asset_path=WALK)
if info:
    log("walk info: length=%s fps=%s frames=%s keys=%s root_motion=%s curves=%d notifies=%d measured=%s" % (
        info["length"], info["frame_rate"], info["frame_count"], info["key_count"], info["root_motion"], len(info["curves"]), len(info["notifies"]), info.get("bones_measured")))
    check(info["length"] > 0 and info["frame_rate"] > 0 and abs(info["frame_count"] - info["length"] * info["frame_rate"]) < 1.01 and info["key_count"] == info["frame_count"] + 1,
          "length, frame rate and frame count agree", (info["length"], info["frame_rate"], info["frame_count"], info["key_count"]))
    check(info["class"] == "AnimSequence" and info["bone_track_count"] > 0 and info["skeleton"].endswith("TutorialTPP_Skeleton"), "class, skeleton and bone tracks", (info["class"], info["bone_track_count"]))
    trav = {t["bone"].lower(): t for t in info.get("travel", [])}
    check([b.lower() for b in info.get("bones_measured", [])] == ["root", "pelvis"] and set(trav) == {"root", "pelvis"}, "auto travel bones are root and pelvis", info.get("bones_measured"))
    for bone, t in trav.items():
        d = t["delta"]
        consistent = (close([t["end"][i] - t["start"][i] for i in range(3)], [d["x"], d["y"], d["z"]])
                      and abs(t["distance"] - (d["x"] ** 2 + d["y"] ** 2 + d["z"] ** 2) ** 0.5) < 1e-3
                      and abs(t["horizontal_distance"] - (d["x"] ** 2 + d["y"] ** 2) ** 0.5) < 1e-3 and t["path_length"] >= t["distance"] - 1e-3)
        check(consistent, "%s travel is self-consistent" % bone, (d, t["distance"], t["path_length"]))
    s0 = ok("AnimDataToolset", "anim_sample_bones", asset_path=WALK, time=0, bones="root,pelvis")
    s1 = ok("AnimDataToolset", "anim_sample_bones", asset_path=WALK, time=info["length"], bones="pelvis, root")
    if s0 and s1:
        b0 = {b["name"].lower(): b for b in s0["bones"]}
        b1 = {b["name"].lower(): b for b in s1["bones"]}
        check(close(b0["pelvis"]["translation"], trav["pelvis"]["start"]) and close(b1["pelvis"]["translation"], trav["pelvis"]["end"]),
              "sampled pelvis at 0 and at the end matches the travel report", (b0["pelvis"]["translation"], trav["pelvis"]["start"], b1["pelvis"]["translation"], trav["pelvis"]["end"]))
        check(s0["space"] == "component" and s0["frame"] == 0 and abs(s1["frame"] - info["frame_count"]) < 1e-3 and b0["pelvis"]["parent"].lower() == "root",
              "sample reports space, frame and parent", (s0["space"], s0["frame"], s1["frame"], b0["pelvis"]["parent"]))
        q = b0["pelvis"]["quaternion"]
        check(abs(sum(x * x for x in q) - 1) < 1e-3 and len(b0["pelvis"]["rotation"]) == 3 and len(b0["pelvis"]["scale"]) == 3, "rotation is a unit quaternion plus euler angles", q)
    mid = info["length"] / 2
    pc = ok("AnimDataToolset", "anim_sample_bones", asset_path=WALK, time=mid, bones="root,pelvis", space="component")
    pp = ok("AnimDataToolset", "anim_sample_bones", asset_path=WALK, time=mid, bones="root,pelvis", space="parent")
    if pc and pp:
        c = {b["name"].lower(): b for b in pc["bones"]}
        p = {b["name"].lower(): b for b in pp["bones"]}
        check(close(c["root"]["translation"], p["root"]["translation"]) and close(c["root"]["quaternion"], p["root"]["quaternion"]), "root: component space == parent space")
        if close(c["root"]["translation"], [0, 0, 0]) and close(c["root"]["rotation"], [0, 0, 0]):
            check(close(c["pelvis"]["translation"], p["pelvis"]["translation"]), "pelvis under an identity root: component == parent space")
    al = ok("AnimDataToolset", "anim_sample_bones", asset_path=WALK, time=0)
    bones_info = ok("AnimInspectToolset", "anim_list_skeleton_bones", asset_path=SK)
    al and bones_info and check(len(al["bones"]) == len(bones_info["bones"]), "bones='*' samples every skeleton bone", (len(al["bones"]), len(bones_info["bones"])))
    expect_fail("AnimDataToolset", "anim_sample_bones", asset_path=WALK, time=info["length"] + 1)
    expect_fail("AnimDataToolset", "anim_sample_bones", asset_path=WALK, time=0, bones="root,no_such_bone")
    expect_fail("AnimDataToolset", "anim_sample_bones", asset_path=WALK, time=0, space="world")
    expect_fail("AnimDataToolset", "anim_get_animation_info", asset_path=WALK, travel_bones="no_such_bone")
r = ok("AnimDataToolset", "anim_get_animation_info", asset_path=IDLE, travel_bones="none")
r and check("travel" not in r, "travel_bones='none' skips travel", sorted(r.keys()))
r = ok("AnimDataToolset", "anim_get_animation_info", asset_path=IDLE, travel_bones="hand_r")
r and check(r.get("bones_measured") == ["hand_r"], "explicit travel bone", r.get("bones_measured"))
expect_fail("AnimDataToolset", "anim_get_animation_info", asset_path=BS)
expect_fail("AnimDataToolset", "anim_sample_bones", asset_path=BS, time=0)

fails = [l for l in LOG if l.startswith("FAIL")]
log("SUMMARY: %d checks, %d failures" % (len([l for l in LOG if l.startswith(("OK", "FAIL"))]), len(fails)))
