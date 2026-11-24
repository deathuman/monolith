---
name: unreal-animation
description: Use when inspecting or editing Unreal animation assets via Monolith MCP — sequences, montages, blend spaces, animation blueprints, notifies, curves, sync markers, skeletons, IKRig, IK Retargeter, Control Rig. Triggers on animation, montage, ABP, blend space, notify, anim sequence, skeleton, IKRig, retargeter, control rig.
---

# Unreal Animation Workflows

**130+ animation actions** via `animation_query()`. Discover with `monolith_discover({ namespace: "animation" })` for the live figure (this table covers the high-traffic subset).

## Key Parameters

- `asset_path` -- animation asset path (e.g. `/Game/Animations/ABP_Player`)
- `machine_name` -- state machine name (from `get_state_machines`)
- `state_name` -- state within a machine
- `graph_name` -- optional graph filter for `get_nodes`

## Action Reference

| Action | Key Params | Purpose |
|--------|-----------|---------|
| **Montage** | | |
| `add_montage_section` | `asset_path`, `name`, `time` | Add named section |
| `delete_montage_section` | `asset_path`, `name` | Remove section |
| `set_section_next` | `asset_path`, `section`, `next` | Set section playback order |
| `set_section_time` | `asset_path`, `section`, `time` | Move section to specific time |
| **Blend Space** | | |
| `add_blendspace_sample` | `asset_path`, `animation`, `x`, `y` | Add animation at X/Y (auto-bakes triangulation) |
| `edit_blendspace_sample` | `asset_path`, `index`, `x`, `y` | Move existing sample (auto-bakes) |
| `delete_blendspace_sample` | `asset_path`, `index` | Remove sample point (auto-bakes) |
| `bake_blend_space` | `asset_path` | Rebuild `FBlendSpaceData` triangulation (`ResampleData`) — repair externally-authored or pre-fix blend spaces. Returns `has_blendspace_data`, `sample_count`, `baked`, `warning` if a 2D blend space has <3 samples |
| `set_blend_space_interpolation` | `asset_path`, `use_grid`?, `preferred_triangulation_direction`? | Set `bInterpolateUsingGrid` + edge direction (`None`/`Tangential`/`Radial`), resample. Grid mode → `has_blendspace_data:false` is correct |
| **ABP Reading** | | |
| `get_state_machines` | `asset_path` | List all state machines |
| `get_state_info` | `asset_path`, `machine_name`, `state_name` | State details |
| `get_transitions` | `asset_path`, `machine_name` | Transition rules |
| `get_blend_nodes` | `asset_path` | Blend node trees |
| `get_linked_layers` | `asset_path` | Linked anim layers. An ABP-native (self) layer shows the node title `"<Layer>\nAnim Layer (self)"` — there is no separate `layer_kind` field |
| `get_graphs` | `asset_path` | All graphs in ABP |
| `get_nodes` | `asset_path`, `graph_name`? | Nodes in graph(s) |
| `get_abp_variables` | `asset_path` | Variables with types and defaults |
| `get_abp_linked_assets` | `asset_path` | Referenced anim assets via Asset Registry |
| **ABP Writing (EXPERIMENTAL)** | | |
| `add_state_to_machine` | `asset_path`, `machine_name`, `state_name`, `position_x/y`? | Add state |
| `add_transition` | `asset_path`, `machine_name`, `from_state`, `to_state` | Add transition |
| `set_transition_rule` | `asset_path`, `machine_name`, `from_state`, `to_state`, `variable_name` OR structured `rule` | Wire a transition condition. The `rule` object's `kind` is `bool` / `auto` / `compare` / `expression`; `expression` takes compound multi-term `terms: [{ lhs, op, rhs, abs?, negate? }]` folded through Boolean AND/OR via `combine: "and"|"or"` |
| `remove_anim_state` | `asset_path`, `machine_name`, `state_name`, `remove_dependent_transitions`? | Remove state + tear down inner graph. Refuses to remove the current entry state — re-point first. `remove_dependent_transitions` defaults `true` |
| `set_anim_entry_state` | `asset_path`, `machine_name`, `state_name` | Re-point Entry node at an existing state. Returns previous target; `unchanged` when already the entry |
| `remove_anim_transition` | `asset_path`, `machine_name`, `from_state`, `to_state` | Remove a from→to transition. Reports `matched_transition_count` |
| **ABP Graph Authoring** | | |
| `add_apply_additive` | `asset_path` | Add an Apply Additive node (Base/Additive/Alpha pins) |
| `add_apply_mesh_space_additive` | `asset_path` | Add an Apply Mesh-Space Additive node |
| `add_slot_node` | `asset_path`, `slot_name` | Add a Slot node. `slot_name` validated against the skeleton's slot groups |
| `add_save_cached_pose` | `asset_path`, `cache_name` | Add a Save Cached Pose node |
| `add_use_cached_pose` | `asset_path`, `cache_name` | Add a Use Cached Pose node. Pairs by `cache_name` with the matching save |
| `set_output_pose_source` | `asset_path`, `node` | Wire a node's pose output into the anim graph's Output/Root result pin |
| `set_state_result_source` | `asset_path`, `machine_name`, `state_name`, `node` | Wire a node's pose output into a state's result pin |
| `add_blend_by_int` | `asset_path`, `num_poses` | Add a Blend Poses by Int node grown to `num_poses` pins |
| `add_blend_by_enum` | `asset_path`, `enum_path`, `enumerators`?, `graph_name`?, `state_name`? | Add a Blend Poses by Enum node bound to a `UEnum`: one pose pin per exposed enumerator + a Default/else pin. Skips the auto `_MAX` sentinel and `Hidden` enumerators. `enumerators` exposes a subset. Companion to `add_blend_by_int` |
| `set_sync_group` | `asset_path`, `node`, `name`, `role`?, `method`? | Set a player node's sync group |
| `set_layered_blend_bones` | `asset_path`, `node`, `bones` | Set per-bone branch filters (bone + blend depth) on a Layered Blend Per Bone node |
| `add_anim_control_rig_node` | `asset_path`, `control_rig_class` | Add a Control Rig anim-graph node. IO pins regenerate from the class |
| `add_anim_layer_graph` | `asset_path`, `layer_name`, `input_poses`?, `compile`? | Create an ABP-native animation layer — a `UAnimationGraph` on the anim schema in the ABP's own function graphs (the My Blueprint → **+** → Animation Layer button). No `UAnimLayerInterface` asset needed, so ABP variants need not share a layer signature. Output Pose root node created automatically. `input_poses` takes pose NAMES only (`["InPose"]` or `[{"name":"InPose"}]`, max 16). Refuses duplicate graph names, `AnimGraph`, child ABPs, macro libraries, interface Blueprints |
| `add_linked_anim_layer` | `asset_path`, `layer_name`, `interface_class`?, `instance_class`? | Add a Linked Anim Layer node. Resolves interface-declared layers first, then ABP-native layers as SELF layers (payload: `interface_class: "<self>"`, `guid_resolved: false`). `interface_class` disables the native fallback; `instance_class` is rejected for a self layer. **Order matters:** the layer must be compiled into the ABP's skeleton class first, or the node comes up with no pose pins — keep `add_anim_layer_graph`'s default `compile: true` |
| `add_conduit` | `asset_path`, `machine_name`, `name` | Add a conduit node to a state machine. Its bound graph is a transition-logic graph, not an anim graph |
| **Notifies** | | |
| `set_notify_time` | `asset_path`, `notify`, `time` | Move notify |
| `set_notify_duration` | `asset_path`, `notify`, `duration` | Set notify state duration |
| **Bone Tracks** | | |
| `set_bone_track_keys` | `asset_path`, `bone`, `keys` | Set keyframes |
| `add_bone_track` | `asset_path`, `bone` | Add track |
| `remove_bone_track` | `asset_path`, `bone` | Remove track |
| **Skeleton** | | |
| `add_virtual_bone` | `asset_path`, `source`, `target` | Create virtual bone |
| `remove_virtual_bones` | `asset_path`, `bones` | Remove virtual bones |
| `get_skeleton_info` | `asset_path` | Bone hierarchy, sockets, virtual bones |
| `get_skeletal_mesh_info` | `asset_path` | Mesh details, LODs, materials |
| **Physics Assets** | | |
| `get_physics_asset_info` | `asset_path` | Bodies, constraints, profiles, solver settings |
| `set_constraint_properties` | `asset_path`, `collision_pair_only`?, `bone_1`, `bone_2`, `disable_collision` | With `collision_pair_only: true`, toggle collision for the exact body pair, including nonadjacent bodies such as `thigh` and `pelvis`; the action does not require a joint between them |
| **IKRig** | | |
| `get_ikrig_info` | `asset_path` | Solvers, goals, chains, skeleton |
| `add_ik_solver` | `asset_path`, `solver_type`, `root_bone`?, `goals`? | Add solver + goals. `solver_type` accepts a friendly alias (`fullbodyik`/`fbik`, `limb`, `pole`, …), exact struct name, or unique substring — Full Body IK now resolves (`root_bone` meaningful for FBIK) |
| `remove_ik_solver` | `asset_path`, `solver_index` | Remove a solver by 0-based index (validated). Returns `removed_index`, `solver_count_after` |
| `get_retargeter_info` | `asset_path` | Source/target rigs, chain mappings |
| `set_retarget_chain_mapping` | `asset_path`, `auto_map`? OR `source_chain`+`target_chain` | Map chains |
| **Control Rig** | | |
| `get_control_rig_info` | `asset_path`, `element_type`? | Hierarchy -- bones, controls, nulls |
| `get_control_rig_variables` | `asset_path` | Animatable controls and BP variables |
| `add_control_rig_element` | `asset_path`, `element_type`, `name`, `parent`?, `control_type`?, `animatable`?, `transform`? | Add bone/control/null |

## Common Workflows

### Inspect ABP state machines
```
animation_query({ action: "get_state_machines", params: { asset_path: "/Game/Animations/ABP_Player" } })
animation_query({ action: "get_transitions", params: { asset_path: "/Game/Animations/ABP_Player", machine_name: "Locomotion" } })
```

### Set up montage section flow (intro -> loop -> outro)
```
animation_query({ action: "add_montage_section", params: { asset_path: "/Game/Animations/AM_Attack", name: "Intro", time: 0.0 } })
animation_query({ action: "add_montage_section", params: { asset_path: "/Game/Animations/AM_Attack", name: "Loop", time: 0.5 } })
animation_query({ action: "set_section_next", params: { asset_path: "/Game/Animations/AM_Attack", section: "Intro", next: "Loop" } })
```

### Toggle collision for a nonadjacent physics-body pair
```
animation_query({ action: "set_constraint_properties", params: {
  asset_path: "<WolfPA asset path>",
  collision_pair_only: true,
  bone_1: "thigh",
  bone_2: "pelvis",
  disable_collision: true
} })
```
`<WolfPA asset path>` is a placeholder for the mounted asset path, and bone names must match the asset exactly. `collision_pair_only` defaults to `false`. When true, both bone names must resolve to distinct bodies and `disable_collision` must be supplied explicitly. Do not include `constraint_index` or angular/linear constraint-profile parameters. The action refuses edits while PIE or Simulate in Editor is active or queued, and returns the body indices plus collision-table membership read back through `UPhysicsAsset::IsCollisionEnabled`.

## Posed Skeletal-Mesh Capture (editor:: action)

`editor_query("capture_scene_preview", { asset_path: "/Game/.../SK_Char", asset_type: "skeletal_mesh", animation_path: "/Game/.../A_Idle", seek_time: 0.5 })` renders the skeletal mesh at the requested pose (omit `animation_path` for rest pose). Useful for visually verifying retarget output, montage section poses, or sequence keyframes. See `monolith_guide(section="recipes")` entry "Visual introspection — going beyond thumbnails".

## Rules

- Editing tools modify assets **live in the editor** -- changes are immediate
- Primary param is `asset_path` (not `asset`)
- `get_nodes` accepts optional `graph_name` filter
- Use `project_query("search", { query: "AM_*" })` to find animation assets first
- ABP write actions are **EXPERIMENTAL** -- always compile after and check for errors
- `set_retarget_chain_mapping`: `auto_map: true` for automatic OR explicit `source_chain`+`target_chain`
- `add_control_rig_element`: `animatable` uses `IsAnimatable()` internally -- not a raw bool field
- `set_constraint_properties`: omit `collision_pair_only` or set it to `false` for the existing constraint-profile behavior; pair-only mode requires an explicit boolean `disable_collision` and exact distinct body bone names
- Blend space mutators (`add/edit/delete_blendspace_sample`, `set_blend_space_axis`) now auto-bake the triangulation (`ResampleData`) after each edit, so MCP-authored blend spaces no longer ship empty and A-pose at runtime. For blend spaces authored externally or before this fix, call `bake_blend_space` once to repair. In grid mode (`use_grid: true`) an empty triangulation / `has_blendspace_data: false` is correct, not a failure
- `add_ik_solver`: `solver_type` resolves against the live solver-struct table (alias -> exact struct name -> unique substring); an ambiguous value returns the candidate list. Full Body IK now adds correctly
- `auto_layout`: use `formatter: "builtin"` (also the `"auto"` fallback) for a dependency-aware anim-graph layout that works WITHOUT Blueprint Assist, so layout works in release builds where Blueprint Assist is compiled out
- `set_anim_node_pin_binding` now bootstraps the binding object on previously-unbound nodes, so it works on a node that has no binding yet instead of refusing it
- `set_transition_rule`: pass a structured `rule` object instead of authoring rule-graph nodes by hand — `kind` is `bool` / `auto` / `compare` / `expression`. The `expression` kind builds compound multi-term conditions (`terms: [{ lhs, op, rhs, abs?, negate? }]` folded through Boolean AND/OR via `combine`); `get_transition_rule` decodes any rule back

## Read-only locomotion/contact analysis (I04)

`animation.analyze_locomotion` is implemented for project UE5.8.1 in pending CL1518; cold build, focused fixtures and representative read-only reports passed. Owner acceptance/submission remain separate. Inspect its live schema before use; registration in source alone does not prove availability in the running editor.

1. Read `get_sequence_info` and `get_skeleton_info`; select one non-additive sequence and explicit left/right foot bones. Do not assume common names or interpret skeleton membership as successful pose evaluation.
2. Request a summary at a fixed sample rate (default 30Hz). Optionally bound `start_time`/`end_time`; the full request must fit the 2048-sample limit. Leave `include_samples=false` until detailed trajectories are useful.
3. Inspect root path/net displacement, foot-height/speed summaries, authored contact evidence and geometric candidates separately. The height baseline is the lowest sampled foot position, not a traced gameplay floor.
4. Inspect sliding availability before interpreting suspect intervals. Root-locked/in-place/disabled-motion cases have no actor-motion basis and cannot establish gameplay sliding.
5. For useful findings, request detailed samples and inspect height/speed/contact plots. Native animation-editor playback supplies qualitative clip inspection. The CL1518 `editor.capture_anim_frames` repair has pixel-verified readable changing mannequin/wolf poses and reports live single-node asset/time. Material/texture readiness barriers now make first-after-relaunch and repeated mannequin walk/wolf run captures byte-identical at the six verified times. This supports repeatable pose comparisons for that tested scope; other assets/materials/graphs need their own pixel check. Inspect actual output for each new asset/camera. Raw analysis and preview pixels do not replace runtime/IK acceptance.

This action preserves source clips: no marker creation, retargeting, saves or animation graph edits. `derive_foot_sync_markers` remains a separate authoring action with `dry_run=false` by default; never substitute it for this read-only report without checking its mutation contract. Existing animation-preview and NPC acceptance tasks remain separate.
