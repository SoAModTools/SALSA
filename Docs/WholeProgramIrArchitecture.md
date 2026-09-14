# Whole-program IR architecture decision

Decision accepted by the user on 2026-09-13: **paged instruction storage and zstd level 1 project compression**, together with shared section-order buffers, scoped immutable readers, incremental dependency indexes and portable binary project tables. The full GC US corpus and compression experiments are complete; these choices are no longer provisional. The editor has not been migrated. Alternative storage/codecs remain evaluation tools, not production representation toggles. See the [production implementation plan](WholeProgramIrProductionPlan.md).

## Authored structure and lowering

SALSA owns a structured authoring program: If/Else If/Else chains own ordered conditions and body sequences, alongside explicit typed instruction/control-flow leaves. The editor can therefore be flatter without requiring generated jumps to be the source of truth. A compiler-style lowering stage produces derived instruction flow for inspection and SPICE composition. Imported irregular control flow remains explicit until it can be safely promoted. Paged storage and zstd level 1 apply to this authored model; they do not require one record per native opcode. Condition regions own preparation actions plus a result expression. While/DoWhile/For, Switch cases/default and scoped Break/Continue share this model, with explicit evaluation phases. WP1 establishes construct records and input; WP3 adds semantic transformation commands; WP5 implements shared lowering.

## Why this combination

The objective is to load an imported script collection quickly, find connections across scripts, and update the affected connections when someone edits a condition, effect or destination. Stable identities and incremental indexes provide that behavior. Physical record storage should keep edits inexpensive without tying modules or graph nodes to physical SCT sections.

| Choice | Reason | Remaining cost |
| --- | --- | --- |
| Paged instruction slots | Growth and deletion reuse slots without relocating surviving instructions; close to dense loading speed | Page/directory overhead, page allocations and retained empty capacity |
| Shared immutable section-order buffers | Identity and execution order remain separate; preparation/history/undo share unchanged order | Structural edits still copy the affected section and repair ranks |
| Pooled operands, expressions and direct facts | Avoid an individual heap object for each program element; permit indexed cross-script queries | Index capacity and metadata can cost much more than records |
| Incremental module summaries and query dependencies | A changed source updates its contributions and dependent results; unchanged results remain reusable | A heavily shared helper can legitimately affect many modules |
| Explicit scoped readers | Worker/view data remains coherent without refreshing a whole-project snapshot | Capturing a large scope still copies records, facts and referenced values |
| Binary tables with zstd level 1 | Direct loading avoids JSON-tree construction; the codec experiment shows fast compression at near-current size | Save capture, framing and I/O remain separate costs; use bounded reusable buffers |

At the earlier IR7 synthetic checkpoint, its Release executable completed 78 matched 100k synthetic runs across six workloads and all four candidates, including two additional lifetime-policy comparisons. Its SHA-256 matches the executable used for the preceding 144 native/persistence runs, which supply that part of the decision. Earlier slice timings explain design evolution but are not pooled into current timing statistics.

Representative IR7 synthetic medians, in milliseconds, including synthetic input construction in readiness:

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

The implementation is organized into six slices: production IR/native ingestion; core service/edits/history/lifetimes; semantic authoring/incremental relationships; binary/zstd persistence and v7 import; fresh native materialization/publication; and editor cutover/retirement. [The production plan](WholeProgramIrProductionPlan.md) defines their boundaries, completion points and the source areas to replace.

Reuse selected mechanisms from Program.h, PagedStorage.h, Lifetime.h, Connectivity.h, ProgramInput.h and the binary/corpus adapters. The synthetic host's rich dependency mechanisms and the corpus host's broader native payload coverage need to be reconciled. Neither is a drop-in complete production model. Remove fixture/report/process-supervisor concerns from the production core.

The corrected full GC US comparison passed nine runs over 258 scripts and 443,655 original instructions, including all 39,382 set/clear targets. Paged growth moved no surviving rows. The separate codec comparison passed 18 runs over one immutable checkpoint. Native parsing/import remains the dominant cold-import stage; record storage is a small part of readiness. These are local experimental observations, not GUI timings, full-story analysis or production memory guarantees. Corpus/codec evidence remains ignored locally.

Use zstd under its BSD 3-Clause option, preserving its notices and license and including required dependency source with GPLv3 corresponding-source distribution. SALSA remains GPLv3. Production dependency and release packaging are part of the persistence slice.

No production implementation is authorized by the decision/planning update alone. Full storyline reconstruction, pruning and new-script creation remain outside this handoff. Keep failure handling in ordinary operations and focused lightweight checks; no separate project-verification workflow or automatic GUI test runs.
