# Whole-program service (WP2)

`SalsaCore/Program/ProgramService.h` is the Qt-free owner of the production IR. WP2 adds local primitive edits, history, immutable scoped readers and asynchronous collection ingestion. The production editor now uses this service; see [editor cutover](WholeProgramIrEditor.md). There is no live mirroring between representations.

## Requests and ownership

One executor serializes service requests. Each request returns a future containing the existing `Result`/diagnostic contract. A `Revision` carries project identity, semantic revision and service generation. Replacing the project changes the generation, including when the replacement uses the same project identity and revision. Stale, cancelled and malformed requests fail at their ordinary operation boundary; callers do not run a separate verification pass.

`adopt` consumes an exclusively owned completed `Program`, moving its tables instead of copying the project. Pass the builder result with move semantics and release other handles first. `load` prepares a fresh native collection away from the owner executor, then requests adoption against the original revision. An edit, replacement, newer load or shutdown prevents an older load from replacing the authority.

`catalog` returns script identities/names/counts and module descriptions without capturing executable content. `read` requires an explicit script, module or action selection. Its immutable handle owns copied selected records and reachable payloads, with shared immutable sequence-order buffers. Selecting a construct includes its condition preparation and nested bodies. Explicit action selections keep enclosing sequence IDs/orders for navigation without copying all sibling instructions. Incoming execution targets remain stable IDs rather than recursively expanding the whole call graph. Value handles in a scope or query belong to that result only.

`uses` and `incoming` read direct dependency buckets. Variable query results include the expressions needed to inspect those facts, using result-local handles. Readers can outlive edits, undo, maintenance and service shutdown. No caller receives a pointer into the mutable authority.

`checkpoint` is an explicit full-project copy for save/export work. It is never the normal view refresh path. It carries its captured revision and history-state token; `markSaved` accepts an issued checkpoint token from the same generation. Saving a captured state while later edits exist leaves those later edits dirty. Undoing to saved content restores clean status without serializing or comparing project bytes. WP4 now supplies the [binary persistence consumer](WholeProgramIrPersistence.md).

## Primitive edits and publication

An `EditRequest` contains an ordered batch of typed operations. WP2 supports:

- Parameter replacement addressed by action identity, schema index and optional repeated-group ordinal. Native switch-case operands therefore remain independently addressable.
- Instruction replacement, including schedule expression, refresh flag and full fixed/repeated parameter content.
- Structured condition-result replacement and text payload replacement.
- Instruction insertion, deletion and movement, including moves between sequences of the same imported script.
- Ordinary section rename, layout reorder, empty section creation and empty section removal within an imported script.
- Alias/evidence replacement on an existing annotation, retaining its subject identity.

`ValueBuffer` supplies detached bottom-up typed values, children and UTF-8 characters; it is not a recursive object tree or a native document. Its expression helper accepts the existing public SPICE expression type. Service preparation checks payload shape and final resolved references. Unsupported/incomplete payloads retain their explicit draft representation and diagnostics.

`prepare` stages only touched rows, replacement values, sequence orders and old/new dependency buckets. The serialized owner temporarily applies reversible row edits inside its private tables while preparing their final contributions, then restores the published rows before returning. Concurrent requests cannot observe that staging, and previously issued readers own immutable data. Preparation failure restores the original records; reserved authored IDs are consumed rather than reused. Abandoned payload allocations are reclaimed at maintenance.

The opaque prepared handle exposes a receipt preview, including newly allocated action/section/sequence IDs. `install` checks the service identity/revision and cancellation again. All potentially allocating receipt/history work precedes the publication swaps. One batch publishes one semantic revision and one history entry. Undo and redo swap the same forward/inverse row, order and index ranges. They do not rerun the importer or rebuild a project snapshot.

Direct fact extraction examines affected source actions using the same rules as bulk ingestion. It replaces only buckets reached by old/new variable uses and action targets, including formerly empty buckets. Receipts contain affected action/sequence/section/text/condition identities, annotation indices, old/new variable and target keys, diagnostics and work counts. They contain no formatted UI models. Physical sequence and section order remains separate from explicit target operands.

Deletion fails if unchanged execution references or authored metadata would dangle. Required `init`/`loop` names cannot be renamed or removed. Primitive operations cannot silently turn an ordinary section into a lifecycle root. Editing instruction content within required bodies remains possible. A coordinated edit is needed when changing metadata-bound operand layouts or entry/module section identities. Whole-construct insertion/removal/movement, arm/case-label changes, lifecycle repair and metadata commands are now available through [WP3 semantic operations](WholeProgramIrSemantics.md). Existing structured constructs and their identities survive WP2 leaf/condition edits.

## Lifetime and maintenance

Initial indexes remain pooled contiguous postings. Only edited buckets acquire shared immutable replacement buffers; there is no separate vector allocation for every referenced instruction in a freshly imported collection. History owns the inactive sides of changed records/buckets and shared order buffers, rather than complete past projects.

`maintain` is an explicit off-UI maintenance request. It traces current payload roots and the roots retained by history and usable prepared changes, copies live payloads into compact pages/pools, remaps internal handles and consolidates index ranges. It does not reinterpret instructions, re-extract facts or change authored IDs/revision. Its receipt reports before/after value counts. Temporary copying means maintenance itself has a memory peak and a collection-size cost; it is deliberately not part of every edit or view refresh.

Released readers/checkpoints free their own storage. Dropped or stale prepared changes do not pin live service values. Passing `discardHistory=true` intentionally releases undo/redo ownership before reclamation. Empty action/section/sequence slots become reusable only after no retained delta needs them; authored identity allocation remains monotonic. Page capacity for record slots can remain resident for reuse. WP6 can schedule maintenance at an appropriate idle or explicit history-release point. WP2 does not impose an automatic history limit.

## Bounded native loading

`NativeLoadRequest` carries an ordered manifest, worker limit and working-memory reservation budget. Each item supplies its qualified baseline, import options, a cancellable source reader and a working-byte reservation. Readers are called only after admission. A bounded wave admits no more than the worker limit or summed reservation budget. Out-of-order completion is merged in manifest order; only that bounded wave can retain completed per-script IR awaiting merge.

The reservation is a caller-supplied estimate for source bytes, parsing/import, normalization and merge overhead, not a hard allocator limit inside SPICE. It must be positive, at least the declared source byte count, and fit the request budget. Source size and revision are checked against the actual reader result. The eventual dataset adapter must supply conservative estimates; this API does not claim a measured whole-corpus peak. Invalid limits or an oversized declared job return a diagnostic, with the previous program intact.

Each job has its own parser, import context, builder and scratch data. SALSA parses once, imports once, normalizes, then releases the bytes/native graphs. The coordinator merges normalized tables and existing facts, remapping pool offsets rather than parsing again. Final collection work arranges postings once. A failed item fails the collection request; no silent omission occurs.

Fresh import reserves a 32-bit allocation window per manifest ordinal within the 64-bit authored-ID space. IDs within that window come from the normal allocator. Worker count and completion order cannot affect identities, and a job exceeding its window fails. These are fresh-import allocations, not identity inferred from section names or offsets. Persisted authored projects will retain their stored IDs through WP4 rather than reimporting to recover identity.

The inspected SPICE paths use per-call parser/import state, immutable constexpr opcode metadata, per-call text conversion/hash state and local AKLZ decompression storage. No mutable shared parser/import cache was found in these paths. The synthetic native fixture exercises both byte orders through WP1 and concurrent independent imports through WP2. This is source inspection and bounded synthetic evidence, not a claim about every SPICE API's thread safety.

Progress reuses `SctLoadingObserver`: reading SCT, checking source baseline, parsing SCT, importing typed SCT, normalizing program, script ready, arranging collection postings and collection prepared. Counts represent completed scripts; no parser percentage is invented. Delivery is serialized across jobs and occurs away from the UI thread. A UI adapter must queue it into the exclusive-operation widget. Observer closures must remain valid until the request's future completes and must not block waiting for the same load.

Cancellation is checked around source reading, parsing, import, normalization, per-script completion and installation. Noninterruptible SPICE calls finish before their result is discarded. Shutdown requests stop, rejects queued/new operations and returns without joining loader threads on the caller. The shutdown future acknowledges owner-queue drainage; outstanding load futures report their eventual cancelled exit. No worker can install into a closed service. GUI presentation/close integration remains WP6.

## Delivery checks

Four focused synthetic console scenarios cover local batch publication and history, retained readers and maintenance, repeated target ordinals/order/atomic failure, bounded loading with a controllable source barrier, deterministic fresh identities, cancellation, stale completion and shutdown. The six existing IR fixtures continue to cover typed content and structured ownership, including both native byte orders. No GUI or private-corpus run is required for this slice.

Validation results are recorded with the implementation checkpoint. The existing editor's persistence, supported Python SALSA v7 importer and output path remain operational and unchanged; production zstd 1 integration is now implemented in WP4; the old editor path remains until WP6.

Delivery: elevated MSVC v145 Release SalsaCore/SalsaTests builds passed. All 12 focused console checks passed: four ProgramServiceTest cases, six WholeProgramTest cases, and two existing synthetic legacy byte-order/metadata-promotion regressions. Existing linked SpiceMLD LNK4020 PDB warnings remain. No GUI or private-corpus tests ran. No standalone SpiceTests executable is available in this checkout; the prior WP1 build failed in unrelated SpiceMix/PVM includes (`SpicePvm/Model/PvmTextureModel.h`). That unrelated submodule build was not repeated or modified. The focused native fixtures exercise the public SPICE SCT boundary directly.

## WP3 extension

Semantic commands share this service and journal. Scoped module summaries, interactions and item explanations are lazy derived queries. See [WP3 contracts](WholeProgramIrSemantics.md) for metadata table costs, execution versus presentation receipts, dependency invalidation and bounded native recognition. WP4 persistence and WP6 editor integration remain separate.

## WP4 project loading

`openProject` prepares binary tables and presentation on a worker, optionally recovers workspace transactions, and adopts only against its captured revision/generation. It shares load supersession and shutdown handling with native collection loads. See [persistence contracts](WholeProgramIrPersistence.md) for bounded record IO, save tokens and the legacy import boundary.

WP6 extends `read` with explicit text/section/condition/sequence selection and a shallow mode. `outline` pages direct children; `locate` supplies stable navigation ancestry. `CreateText`/`RemoveText` use the same local journal; removal rejects metadata and execution references. `ProgramValues` shares typed expression/text decoding with the materializer for detached selected-item editors.
