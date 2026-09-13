# Production whole-program IR implementation plan

Status: proposed production slices, following the user's explicit architecture decision on 2026-09-13. This document authorizes no production implementation by itself. Branch: whole_program_ir. The storage/compression decision is settled; individual implementation details may evolve as work proceeds.

## Locked decisions

- Use paged instruction storage in SalsaCore. Dense, section-local and reference implementations remain evaluation tools, not production options.
- Use zstd level 1 for new production authoring project payloads. Start with independent bounded records, one compression worker and no dictionary. Page size (initially 256 rows), record chunk size (initially up to 64 MiB) and physical table layout remain implementation details, not authored identity or native SCT requirements.
- Keep portable explicit binary tables, shared section-order buffers, pooled variable-length values and indexes, scoped immutable reads, incremental dependency updates and one live authority.
- Use zstd under its BSD 3-Clause option. Preserve copyright/license/disclaimer, package third-party notices and the dependency sources required for GPLv3 corresponding-source distribution. No SALSA license change or exception is needed.
- Full-corpus and compression experiments are complete; they are no longer pre-decision gates. The user selected this combination after reviewing those results. The editor has not yet been migrated.

## What the evidence establishes

The corrected GC US experiment loaded 258 scripts and 443,655 instructions across all three candidates, three repetitions each. All 39,382 opcode 17/18 write targets were indexed. Paged growth moved no surviving instruction rows. The codec experiment captured that checkpoint once, then compared identical bytes across six settings and three repetitions; zstd level 1 compressed about 205 MiB to 41 MiB in about 0.214 s and decompressed in about 0.121 s by local medians. These are observations, not production UI promises or automated timing thresholds.

Native parsing/import remains the dominant initial-load cost. The roughly 528 MiB paged ready-state figure includes common pools, directories, indexes, capacity and metadata; choosing pages alone does not solve all memory costs. Codec process peaks include worst-case output capacities retained by the experiment. Production should avoid keeping a full set of worst-case compressed buffers resident.

Two experimental hosts must be reconciled, not copied blindly: ProgramHost has incremental summary/lifetime mechanisms but a narrow semantic fixture schema; the corpus host preserves broad typed native content but has narrower derived facts and retains dead payload versions. Neither is a complete production authority.

## Boundaries retained from semantic authoring

Every script has an imported baseline; no new-script creation. Script-owned modules may appear independently in presentation. Required init/loop cannot be disabled, renamed or removed; missing-root imports remain drafts and need explicit export repair. Optional entries preserve enabled state and whole activation-block boundaries. MLD associations remain optional/advisory. Language groups are user-defined with editable string membership and import suggestions, not an enforced supported-locale list. Preserve script-specific entrypoint aliases, module/cutscene aliases and item-attached interpretation evidence.

SPICE owns native file parsing and composition. SALSA owns authoring identity, editing, relationships and persistence. Do not add a SALSA native parser or modify submodules. Typed instructions with unknown gameplay meaning remain editable/preservable where their structure is supported. Opaque source-only bytes stay in retained evidence/notes rather than the hot executable model.

No story graph redesign, automatic main-story certainty, full timeline simulation, pruning, new-script creation, SALT integration or MLD relationship expansion belongs to this conversion. Queries must still support all cutscene bit writes, including clears and writes late in a scene. Story presentation uses user-classified bits/relevant byte state; control flags are not automatically called story progress.

## Proposed slices

| Slice | Group | Working outcome |
|---|---|---|
| WP1 | Production IR and native ingestion | A complete imported collection in the Qt-free paged model, with direct connection queries |
| WP2 | Core service, edits, history and lifetimes | Atomic local edits, undo/redo, scoped readers and responsive bounded loading |
| WP3 | Semantic authoring and incremental relationships | Existing semantic operations and connected views supported by changes to affected data |
| WP4 | Binary project persistence and legacy import | Save/reopen complete projects with zstd 1 and supported Python SALSA v7 import |
| WP5 | Fresh native materialization and publication | Export the authored IR through SPICE with activation lowering and stable mappings |
| WP6 | Editor cutover and retirement | The current editor uses the new authority; superseded live state and broad refresh paths are removed |

These are implementation groups, not a second set of experiment milestones. Each authorized slice ends with a scoped commit and updated notes, without pushing. The production model may be built alongside the existing editor temporarily, but do not live-mirror edits between two authorities. Switch the workspace/editor authority as a coherent cutover in WP6.

## WP1 — Production IR and native ingestion

Create a Qt-free production program area under SalsaCore (proposed SalsaCore/Program). Extract the paged storage, direct ID-directory and pool mechanisms without benchmark fixtures, report infrastructure, Windows process supervision or alternative storage templates leaking into the public interface.

Represent full typed opcode values, scheduling and refresh flags, fixed and repeated parameters, ordered SCPT operations, literal payloads, instruction/text targets, section order, text/control content and unresolved bindings. Retain all authored metadata within the same project aggregate: scripts/baselines, modules, entrypoints/activation, language groups, aliases and optional resource evidence. Keep existing external authored IDs where meaningful; qualify imported physical identities and translate them once into stable authored instruction/value identities. Never expose page/slot addresses as IDs.

Use bounded allocations for pages and pools. A value edit can replace an owned immutable range without replacing its enclosing project. Metadata and variable-length values need ownership/range descriptors, not a heap object per expression operation. Define ownership and lifetime contracts now; implement reclamation with the journal/readers in WP2.

Import a script once through public SPICE parse/import APIs, normalize its typed data, then release temporary source/inspection/document graphs when their required immutable evidence is retained. Establish one bulk input interface usable by native import and binary loading. Keep native baseline resolution separate from executable authority.

Build direct incoming-target and variable access postings. Recognize literal numeric forms from typed semantics, including integral float and fixed-point decimal encodings. Keep computed targets explicit. Preserve scheduling/conditions as evidence; a write fact does not assert unconditional execution. Reuse opcode/SCPT contracts and reviewed semantic rules rather than copying the corpus host's limited recognizer as the entire production effect model.

Completion: load multiple scripts into the production model, perform direct lookups and reverse bit/target queries, and inspect full instruction/text content without resident editable SctDocuments or a second production representation. A small fixture covers names shared across scripts and the float-encoded bit case that exposed the experiment bug. Existing editor remains on its old authority in this slice.

## WP2 — Core service, edits, history and lifetimes

Put the mutable program behind one serialized owner executor. Public operations are typed load/query/edit/undo/redo/read-scope requests using project identity and expected revision. Qt invokes this service; workers never mutate live authority. Scoped reader handles expose a coherent selected script/module/action range, with explicit lifetime. A full-project checkpoint is an explicit save operation, never the default view capture.

Replace whole-state return values in the current command contract with prepared change sets. A change set includes affected record/value replacements, order edits, metadata changes and corresponding index updates. Prepare allocations and recoverable work before publication. Publish once as one revision/history item; failure before publication leaves live state unchanged. Undo/redo apply inverse/forward changes through the same path. Use a saved history-state token for dirty tracking so undo back to saved content works; do not serialize the project to detect dirtiness.

Supply primitive parameter/expression/text edits, target retargeting (including repeated/switch operands), insert/delete/reorder, section edits within imported scripts and stable ownership updates. Retain physical fallthrough separately from explicit target operands. Changed receipts identify entities and relevant dependency buckets without building formatted UI models.

History, readers and prepared changes retain the ranges they actually need. Reclaim dead payload ranges after those owners release them, at controlled maintenance points. Storage compaction does not alter authored IDs. Shared order buffers are the initial design; long-section copies remain visible, with chunked order only if measured need warrants it. Do not retain all dead payload versions as the experimental corpus host does.

Route load work off the UI thread. Add bounded per-script parse/import/normalization workers after inspecting the public SPICE paths for independent-call safety. Use memory backpressure, independent per-job context and deterministic manifest-order ID assignment/binding. Do not retain every large SPICE document while waiting to merge. The worker count is a resource policy, not an identity input. If a required dependency path cannot safely run concurrently, document the specific limitation and retain the working single-worker path rather than inventing a duplicate parser.

Completion: semantic-level primitive edits and history execute without whole-project copies; unchanged sources are not reexamined; invalid/stale/cancelled requests fail at their normal operation boundary; reader/history release reclaims their payload ownership. Cancellation/shutdown never install late results. The existing exclusive-operation progress protocol can consume concrete stage/count updates.

## WP3 — Semantic authoring and incremental relationships

Port the existing sequence, structured-condition, module, entrypoint/activation, route, text/language and interpretation commands onto WP2 primitives. Semantic intent is editable metadata in the same authority. Editing an if/else group must not construct, serialize and readopt a whole SctDocument as its implementation.

Build function/module summaries and activation/transition relationships over direct facts and control flow. Preserve predicate structure, polarity, explicit targets, fallthrough, scheduling and source IDs. Whole-module traversal includes calls/shared helper dependencies, with explicit cycle/unknown handling. Never infer unconditional final story state just from the presence of a set or clear.

Track the dependencies of queries and summaries, including empty buckets. Invalidate both old and new buckets for a changed bit/target and propagate only through the affected dependency closure. A previously absent connection must appear when an edit creates it. Alias/language-label changes update presentation dependencies without rebuilding execution facts. Stable relationships keep identity where their source/role survives; storage movement alone causes no semantic invalidation.

Expose scoped query results for the existing story/interaction views. Format labels and item-attached abstraction explanations lazily. Preserve user assertions separately from inferred evidence, and surface what instructions underpin an abstraction on request. Do not precompute every possible writer-reader pair or render all interpretations while opening a tab.

Completion: changing a cutscene guard, a late bit write, a shared helper or an outgoing target updates the expected affected relationships and leaves unrelated script results reusable. Existing authored activation controls, language membership, aliases and notes remain editable. This completes backend semantics, not a new story canvas.

## WP4 — Binary project persistence and legacy import

Define a versioned production binary table schema for executable content and authored metadata, independent of page layout and C++ ABI. Keep 64-bit authored identities, checked fixed-width internal indices and explicit meaningful order. Separate presentation/session data and retained source evidence from hot execution tables; no intermediate JSON tree or SPICE document is needed to reopen executable content.

Extend container framing with an explicit codec identifier/version and bounded decoded record sizes; the current v1 framing implies zlib and must not silently reinterpret old bytes as zstd. New project saves use zstd level 1. Chunk tables as needed and process records with reusable bounded output buffers; do not retain one worst-case compressed allocation per record. Start single-threaded/no dictionary. Rebuild derived indexes once while loading the owned model; they are not a second persisted authority.

Use an explicit consistent checkpoint capture and the existing workspace transaction/recovery mechanisms. A save marks only its captured history state saved; edits made while it writes remain dirty. Keep saved tabs/recent associations, explicit startup, cancellation and progress behavior. Recovery/commit finishes safely where cancellation cannot interrupt it. Avoid repeated baseline parsing/hash passes; reuse retained qualification and inspection evidence within the load operation.

Supported Python SALSA v7 import remains mandatory. Translate its supported program content and promote supported metadata into the new aggregate, retaining the migration capsule/script origin where needed. Do not reconstruct Python objects as execution state or infer modules from legacy folders. Existing interpreter/converter boundaries can remain at the import edge.

Development-schema backward compatibility is not required. Default handling of unsupported old authoring schemas is a visible diagnostic with the file left untouched; do not silently resave it or silently discard metadata. An optional one-time converter is a separate decision, not a reason to keep two live models. Existing native files and supported Python v7 projects remain import paths.

Integrate the pinned zstd dependency for the actual production target with BSD selection, notices, license texts and release corresponding-source packaging. The experiment's optional generated props are not the production dependency/release mechanism. Persist current authored state; cross-session undo history is not added by this migration.

Completion: save/reopen a complete authored project with stable IDs, metadata and selected session state through binary/zstd loading; no native reparse of unchanged authored content or UI-thread project decode. Supported v7 conversion remains operational. No codec benchmark matrix is needed again.

## WP5 — Fresh native materialization and publication

Materialize a selected captured script revision into a fresh SPICE SctDocument, including texts and control references, then compose native output through public SPICE APIs. Keep a lossless typed mapping for supported executable content without importing opaque source bytes into the IR. Generate realization locations qualified by project, revision, script and realization; derived wrappers map back to their authored entrypoint owners.

Port the shared activation lowering so optional named wrappers and whole-block bypasses preserve original bodies, internal callers, unaffected scheduling and physical fallthrough. Required init/loop checks occur as part of export; missing-root drafts remain saveable. Optional MLD absence/staleness remains advisory. Apply release endianness/text conventions/compression at the export boundary; project zstd compression is independent of native SCT compression.

Route both core export and editor publication through this one materializer, using the captured revision and ordinary operation diagnostics. Unsupported executable structures or dangling targets fail gracefully there. Do not add a separate project verification workflow or reparse export after every edit.

Completion: representative authored edits can save, reopen and export from the production model through the same core path with stable mappings and correct activation behavior. Retain supported v7-to-native export. No UI cutover is required to exercise this end-to-end path.

## WP6 — Editor cutover and retirement

Switch WorkspaceController/SctDocumentController and authoring widgets to the new core service as one workspace authority. Document tabs and semantic/story/interaction/text views hold scoped immutable data and stable selection IDs, not mutable SctDocuments or their own instruction maps. Retarget semantic UI commands and undo/redo to the production journal. Compatibility presentation adapters may be temporary and disposable; no permanent bidirectional synchronization layer.

Replace SctWorkingState/SctDocumentSnapshot-based refresh and SctEditSession document mutation where superseded. Remove the old full-state history and snapshot/validation paths once their callers are migrated. Retain SPICE document building only at import/export boundaries and legacy conversion only at its import boundary. Alternative storage candidates remain solely in SalsaIrEvaluation.

Use existing exclusive-operation UI from explicit dataset/workspace opening through actual presentation readiness. Stage datasets without decoding scripts as an incidental catalog-inspection side effect; restore the authored project once, then populate saved tabs from its resident IR. Opening another view of a resident script must not parse it again. Populate large views incrementally, defer inactive views, and use receipts to update affected rows/labels. Never rebuild the whole story graph as an incidental script-open operation. Preserve tab order/selection, missing-item diagnostics and saved sessions on cancellation.

Completion: the current semantic editor, saved workspace sessions, project save/load, supported v7 import and fresh native publication all use the new authority. No old executable authority remains live. Loading and edits have stage/work measurements showing where time and retained memory go; do not claim the earlier evaluator's timings as measured GUI readiness.

## Practical validation and delivery

Keep the user's lightweight approach: no separate verification mode, exhaustive oracle suite, large new test matrix or hard timing assertions. Fail gracefully during load/edit/save/export, with enough diagnostics to identify the affected item and operation. Build compiled changes with elevated MSVC v145 Release and keep only a few focused synthetic checks for invariants that can silently corrupt ownership, undo or output. Do not run GUI-loading tests. Record manual GUI validation as unavailable until explicitly authorized.

Use the established standalone evaluator for focused production-backed loading/editing/persistence measurements after the relevant slice, not a complete corpus/compression rerun after every change. Reuse captured evidence when it remains applicable. The opt-in full GC US architecture exception remains scoped to its standalone SALSA experiment. New/changed native corpus acceptance follows SALSA's AGENTS.md policy; the me002 DC EU story-connectivity exception remains separate. Follow private-repository mutex/location rules if that repository is actually used for native acceptance. No corpus or planning evidence is committed.

For each slice, record what became usable, remaining representation/coverage limits and scoped checks, then commit. No pushes or submodule changes. Start with WP1 after approval of its implementation scope; do not implement all six based only on this planning request.
