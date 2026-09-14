# Whole-program IR native materialization (WP5)

`program::ProgramMaterializer` lowers one selected script from an immutable `Checkpoint`. It is a synchronous, Qt-free worker API, shared by core clients and the future WP6 publication adapter. It does not install a document in the service or mutate history. The editor still uses its existing authority until WP6.

## Capture and output

Pass the captured `Revision`, selected script, and a fresh realization UUID. A mismatched project, revision, or operation generation fails before lowering. `materialize` returns an immutable fresh SPICE `SctDocument`, revision-qualified locations, diagnostics, and local template work counts. `exportScript` uses the same path and calls the normal SPICE exporter with explicit platform, text encoding, byte order, wrapper and optional header choices. It returns output bytes, their SHA-256 digest, and SPICE's layout. Native Raw/AKLZ output is independent of the zstd authoring container.

The worker cannot establish whether its capture remains the live revision. Before displaying mappings as current or committing prepared output, the caller must compare the service's current revision, script and realization with `Realization::fresh`. Capture equality includes operation generation. A later edit never mutates the old output or makes its mappings current again.

There is no unchanged-source byte reuse or baseline reparse. Baselines remain import evidence, not executable authority. Typed instructions, exact SCPT operations, schedule/refresh settings, repeated operands, message commands, plain/empty indexed text and text-group preambles are reconstructed from the pools. Action and text references are resolved in the selected script. Missing/cross-script executable targets and unavailable executable operands fail with diagnostics. Unavailable source sections and unreferenced unavailable text are excluded with diagnostics; a reference to excluded text fails. Source opaque bytes are never copied into output.

## Structured lowering

- Conditional arms evaluate their preparation followed by their condition. False reaches the next arm's preparation or Else body. Normal exits join the chain's continuation. Existing Return, Jump and scoped transfers keep their exits; no redundant normal-exit jump is emitted after a terminal body.
- While checks before the body. DoWhile enters the body before checking. For initializes once, checks, enters its body, updates and checks again. Preparation runs on every check, including the final failed one. Continue targets For's update or the next While/DoWhile check; Break targets the selected enclosing construct's continuation.
- Switch evaluates its prepared selector once using a native switch instruction. Shared numeric/default labels reference one body. The native Default row follows all numeric rows, independently of that body's authored position. Case exit and fallthrough remain explicit. An absent Default routes to the continuation. SCT reserves numeric case -1 for Default, so an authored numeric -1 fails with a diagnostic instead of silently changing its meaning.
- Explicit native target operands remain symbolic action references until assembly, including targets inside structured bodies. Calls, scheduling and refresh modifiers on instruction leaves are retained.
- Section boundaries remain physical boundaries. A normal continuation may alias the following code section; no call or Return is synthesized at a boundary. A needed continuation beyond the emitted code or through a data section fails rather than inventing a body.

Generated condition, switch and transfer instructions map to their authored actions and condition/arm owners. Sequence and module entry locations may be shared. These are locations within a realization, not new authored IDs. Fresh SPICE IDs are allocated deterministically in emitted order, so they may change when a preceding construct's layout changes.

## Incremental work

One worker owns a materializer instance and its disposable local-template cache. Each template contains instruction payloads, symbolic labels, fixups, and references to child sequences. The cache compares exact local dependency signatures rather than hash-only matches or pool addresses; reload, undo and pool compaction cannot accidentally reuse an unrelated payload.

A body edit changes its action templates, not the templates of all enclosing constructs. Reordering child actions changes assembly order without recompiling unchanged leaves. Changing a condition, case list or loop kind recompiles that local construct and any transfer template whose destination depends on the change. Normal-exit decisions consult the current child tail during assembly. Removed actions in the selected script are evicted. Changing project or script clears the cache; it retains templates for only the most recently materialized script, avoiding a second native representation of the entire project. Destroy the worker-owned materializer to release its templates.

This is selective lowering, not incremental binary layout. Each explicit materialization scans the selected script's local dependencies and assembles its complete derived document. SPICE still lays out the full selected script at export, and the explicit checkpoint is still a complete capture. Ordinary edits and connection/view queries do not call this path. No claim about GUI timings or whole-corpus performance is made by these synthetic checks.

## Activation and lifecycle

Both backends share the detached `SctActivationEntry` and resolver interface in `SctEntrypointAuthoring`. It validates unique ownership of named entries or physical activation edges and applies the existing wrapper/bypass transformation. Optional named entries keep their external name as an inert leading-condition/Return wrapper; original bodies receive collision-free private names. Internal callers and physical predecessors still reach those bodies. Optional local routes redirect only the selected operand (including repeated switch-case ordinals), or insert a jump on the selected fallthrough edge. Preparation, response and consumption remain emitted, so reenabling restores their original path without reconstruction. Generated locations map back to the owning entrypoint.

Export requires one nonempty `init` and `loop`. Required lifecycle entries remain enabled and need neither a module nor response port. They enter their current section body; inserting an action before the originally imported first action does not stale the engine entry. Missing roots remain saveable drafts and require explicit `BindLifecycle` repair. Disabled unresolved activations fail export. MLD availability and provider evidence do not gate this materializer or rewrite authored names/targets. Structural SPICE export checks still cover disabled bodies.

Activation boundaries currently identify explicit action operands/fallthrough, as in the WP3 authoring contract. A boundary made stale by restructuring fails instead of guessing an equivalent generated edge; it must be reselected. General arm/condition-specific activation editing belongs with the editor's boundary selection support.

## Checks

Three focused console scenarios cover structured targets and local template reuse through edit/save/reopen/undo, activation bypasses and wrappers preserving schedules/internal calls/physical fallthrough, and draft/stale/unsupported failures. The existing synthetic v7 case additionally repairs missing lifecycle roots explicitly and exports its binary-reopened program to native SCT. Legacy Python SALSA v7 remains import-only; no v7 project exporter is introduced. Existing activation regressions exercise the shared lowerer. No GUI launch or private corpus expansion is part of WP5's synthetic validation.
