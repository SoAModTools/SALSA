# Whole-program IR architecture decision

Decision date: 2026-09-13. IR7 selects **paged instruction storage, shared section-order buffers, scoped immutable readers, incremental dependency indexes, and compressed binary project payloads** as the direction for production integration. The editor has not been migrated. The evaluator remains available for focused comparisons; production will use one storage implementation.

## Why this combination

The objective is to load an imported script collection quickly, find connections across scripts, and update the affected connections when someone edits a condition, effect or destination. Stable identities and incremental indexes provide that behavior. Physical record storage should keep edits inexpensive without tying modules or graph nodes to physical SCT sections.

| Choice | Reason | Remaining cost |
| --- | --- | --- |
| Paged instruction slots | Growth and deletion reuse slots without relocating surviving instructions; close to dense loading speed | Page/directory overhead, page allocations and retained empty capacity |
| Shared immutable section-order buffers | Identity and execution order remain separate; preparation/history/undo share unchanged order | Structural edits still copy the affected section and repair ranks |
| Pooled operands, expressions and direct facts | Avoid an individual heap object for each program element; permit indexed cross-script queries | Index capacity and metadata can cost much more than records |
| Incremental module summaries and query dependencies | A changed source updates its contributions and dependent results; unchanged results remain reusable | A heavily shared helper can legitimately affect many modules |
| Explicit scoped readers | Worker/view data remains coherent without refreshing a whole-project snapshot | Capturing a large scope still copies records, facts and referenced values |
| Compressed binary tables | Direct loading removes JSON-tree and temporary-model construction | The first layout compresses slowly and is slightly larger; saving still captures a checkpoint |

The current Release executable completed 78 matched 100k synthetic runs across six workloads and all four candidates, including two additional lifetime-policy comparisons. Its SHA-256 matches the executable used for the preceding 144 native/persistence runs, which supply that part of the decision. Earlier slice timings explain design evolution but are not pooled into current timing statistics.

Representative current medians, in milliseconds, including synthetic input construction in readiness:

| 100k workload | Reference | Dense | Paged | Section-local |
| --- | ---: | ---: | ---: | ---: |
| Few long sections | 47.685 | 31.231 | 30.811 | 30.460 |
| Many short sections | 54.379 | 36.541 | 37.974 | 38.352 |
| Churn | 47.698 | 31.034 | 32.069 | 31.635 |
| Story connectivity | 69.663 | 44.736 | 46.735 | 47.754 |

Paged storage moved no live records during growth or deletion. Dense growth moved 100,000 rows in the insertion workloads. With many short sections, record-domain allocation counts were 392 for paged, 3 for dense, and 6,255 for section-local. These are component counts, not all application allocations. Paged is selected for predictable edit growth and independence from section distribution, rather than winning every timing.

At 10k instructions, dense binary reload medians were 14.1–14.7 ms versus JSON's 56.7–71.2 ms across the native/story workloads. All candidates showed the same broad benefit. Binary compressed files were about 2–3.3% larger, and compression was slower. Retain a versioned portable binary payload inside the existing container; do not freeze the experimental row layout or compression tuning as a production ABI. The [evaluator documentation](WholeProgramIrEvaluation.md) records measurement boundaries and save-side costs.

## One authored authority, several derived views

SALSA owns a Qt-free authoring project containing all imported scripts. Scripts qualify assets and ownership; modules remain script-owned and may be displayed independently. Entrypoints, activation descriptors, text/language groups, aliases and resource associations refer to stable authored IDs. They are editable metadata within the same authority, not another executable document tree synchronized with the IR.

Instruction IDs are project-local and monotonic, independent of names, order, memory addresses and exported offsets. Private page slots and ID directories provide lookup. Start with the evaluated 256-record page size as an implementation parameter, not a serialized or public identity contract. Preserve full instruction/value semantics when adapting this design: the evaluator's small opcode enum and three-operand bundle are experiments, not the production instruction schema.

SPICE remains the native parse/compose/decompose boundary. Import produces SALSA-owned authoring content; export constructs fresh SPICE documents and revision-qualified output mappings. Imported baselines/source evidence remain outside hot execution data. Opaque source bytes are not promoted into executable semantic content. No SPICE submodule changes are part of this decision.

Direct indexes connect variables to reads/writes and targets to their incoming edges. Derived summaries connect module effects, guards and transitions. Relationships retain supporting instruction IDs; explanations and aliases are materialized for the requested view. Stream query results instead of storing every possible writer-to-reader pair. Story presentation emphasizes user-classified story bits and relevant byte state; Int/Float remain available in the program and detailed inspection. A dependency does not establish a guaranteed runtime path or an unconditional final state.

## Editing and ownership

Commands prepare the affected records, order fragments and index deltas, then publish one revision and one history entry. Undo/redo use the same changes. Naming edits notify labels; expression edits reconsider their read/write contributions; retargeting updates the selected target edge and its dependents. Structural edits repair affected order/control flow. New connections must be discoverable through dependencies on previously empty buckets as well as existing edges.

Update scope is the affected dependency closure, not always a single instruction. The 100k story trace changed one source/contribution and one summary for a guard/effect edit; a shared-helper edit changed one source and prepared three summaries. No whole-project semantic snapshot or reanalysis is needed for ordinary refresh. The current long-section order operation remains broader: some edits copied roughly 50k order IDs and took about 4–5 ms across the optimized candidates. Keep this visible; chunked order is a possible later focused improvement, not an implemented benefit of paged records.

For production, place the mutable core behind one serialized owner executor so expensive loading, capturing and preparation do not execute on the UI thread. UI commands and results cross that boundary as IDs, scoped data and change notifications. Worker jobs consume immutable selected scopes; they never mutate the live authority. Project identity/revision/generation checks prevent stale installation. The evaluator proves detached-reader ownership and sequential preparation, not a completed concurrent core service; that service is a production implementation obligation. Make read scope explicit in the production API, avoiding accidental whole-project capture.

Keep history and change-receipt lifetimes explicit. Reclaim unused payloads at deliberate maintenance points; advance or release subscribers and expire abandoned preparations. History and active readers can legitimately retain data. Do not perform a full root scan or rebuild just because a widget refreshes. Record pages, directory capacity and some index pools still retain high-water capacity in the experiment.

Errors are handled at the operation that encounters them: malformed input fails its load, an invalid edit is rejected without changing state, incomplete bindings remain inspectable drafts where supported, and export reports unsupported executable content. There is no separate project-verification workflow on normal edits or view refreshes.

## Production handoff

1. **Production IR and native authoring.** Extract the selected storage/identity/change/index mechanisms into SalsaCore, finish the full parsed instruction/value and semantic command coverage, and provide the serialized core service. Preserve script-owned modules, optional activation, required `init`/`loop`, text organization and optional MLD context. Finish section/module creation and boundary edits within imported scripts. Native parsing can later use a bounded pool per script, followed by deterministic ID assignment/binding and global relation construction; that concurrency is not implemented by this decision.
2. **Project persistence and legacy import.** Adapt binary tables to complete authored content, integrate loading/history/session behavior, and keep supported Python SALSA v7 import and metadata promotion operational. Do not infer modules from legacy folders. Decide development-project migration explicitly; backward compatibility for experimental payloads is not required. Keep expensive save capture/serialization off the UI thread. Compression tuning and incremental save capture can follow measured need.
3. **Editor and view cutover.** Route semantic editing and selective presentation through the single authority. Build labels/explanations and inactive views on demand; update affected items from receipts. Retire the superseded live representation only as its operations are replaced. The user-facing loading widget and cancellation must cover actual work through presentation readiness.

Reuse mechanisms from `Program.h`, `PagedStorage.h`, `Lifetime.h`, `Connectivity.h`, `ProgramInput.h` and `BinaryPersistence.cpp`; separate them from fixture, benchmark, Windows-process and report concerns. `Native.cpp` is a bounded integration example, not a complete SCT adapter. Keep reference/dense/section-local and JSON codecs in the evaluator; do not introduce production representation toggles. No further experiment is required before starting the production foundation.

Provisional performance reference points on these fixtures are roughly 30–50 ms for 100k compact-program readiness, under 0.1 ms for the small story edits, and 15–20 ms for 10k binary reload. These guide investigation, not automated pass/fail thresholds or promises for complete real projects. High-use queries remain proportional to the output visited; index allocation and long-section order copies deserve attention before shaving small differences from record lookup.

The evidence is bounded to synthetic inputs up to 100k, three current repetitions per variant and the supported native subset. No million-record, full-corpus, dataset-inspection or actual GUI-readiness claim follows. Full storyline reconstruction, pruning and new-script creation remain outside this integration decision. No GUI tests, corpus runs, production code changes or submodule changes were made during IR7.
