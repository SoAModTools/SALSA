# Whole-program IR evaluation tool

`SalsaIrEvaluation` is an experimental, Qt-free console tool for comparing SALSA's future internal representations. IR6 adds bounded SPICE SCT import/fresh export and compressed experimental persistence to the dense, paged, section-local, and reference storage candidates behind the shared program/editing host. It does not change the editor. IR7 selects paged storage and shared policies for production integration; see [the architecture decision](WholeProgramIrArchitecture.md). Native experiments use synthetic SCT bytes; corpus loading is not enabled.

## Run an experiment

Build `src/SalsaIrEvaluation/SalsaIrEvaluation.vcxproj` with MSVC v145 Release. The executable is placed in `bin/x64/Release`.

```powershell
.\bin\x64\Release\SalsaIrEvaluation.exe run --candidate reference --count 64 --seed 1 --report "planning/Whole program IR/results/run-1.json"
.\bin\x64\Release\SalsaIrEvaluation.exe run --candidate reference --count 64 --seed 1 --repetition 1 --report "planning/Whole program IR/results/run-2.json"
```

Each command runs once and writes measurements or a failure report. There is no preliminary verification run, receipt, or mode to enable. Output paths must be fresh so an earlier observation is not overwritten.

The trace is `semantic-edits`, using a declared synthetic instruction language across two source-qualified scripts. Select a fixture with `--fixture`:

| Fixture | Distribution or workload |
| --- | --- |
| `small-story` / `few-long` | Two populated long sections and an initially empty helper section. These names select the same baseline shape. |
| `many-short` | The first eight records retain the seed story; remaining records occupy sections of up to 16 instructions. |
| `high-use` | The long-section distribution, with remaining reads/writes concentrated on BitVar[17]. |
| `churn` | The long-section distribution, followed by a 300-instruction insertion, repeated batch deletion/undo, and section moves/undo/redo. |
| `lifetime` | The churn workload plus 512 value edits with periodic undo/redo, a held reader, history release, and explicit reclamation. |
| `native-gc` / `native-dc` | Two synthetic SCTs with required lifecycle sections and bit/byte/control/wait edits. GC uses big-endian raw output; DC uses little-endian AKLZ. Requires at least 32 instructions. |
| `story-connectivity` | Six declared modules with bit/byte guards, shared helpers, cutscene/field transfers, scheduled calls, unknown effects, cycles, and edits with undo/redo. Requires at least 16 instructions. |

The requested count is the initial instruction count; churn adds 300 records beyond the baseline edits. All candidates receive identical section catalogs, including empty sections, and identical inputs/traces. The trace exercises reads, literal/expression writes, direct calls, batch replacement/insertion/deletion, same-script section moves, reordering, renaming, unresolved targets, and undo/redo. The original fixtures use a declared synthetic language. IR6 adds a bounded mapping to actual SCT opcodes, described below. General control-flow interpretation, cross-script owner moves, complete native SCT conversion, and concurrent live-host access remain unsupported. Explicit detached execution readers are described below. The bounded declared-guard subset is described below.

Choose `--candidate reference`, `dense`, `paged`, or `section-local`:

| Candidate | Private record layout and edit behavior |
| --- | --- |
| `reference` | A PMR map with individual nodes, retained as a simple comparison baseline. |
| `dense` | One contiguous table; checked 1.5x capacity growth and swap removal with ID-directory repair. |
| `paged` | Fixed 256-record pages, occupancy bitmaps, and a non-full-page list. Deletion reuses holes without moving live rows. |
| `section-local` | Independently growing contiguous tables for each imported section; growth and swap removal affect that section. |

Dense and section-local share the packed-table implementation with different grouping policies. All three compact candidates use paged ID-to-location directories. Physical slots never determine authored order or public identity. Fixed section catalogs are supplied at load; adding/removing sections is not yet part of this experiment.

## Results and comparison

Reports contain the fixture/trace fingerprints, complete executable SHA-256, compiler/build identity, configuration, declared coverage, phase/operation timings, allocation counters, record work, final content checksum, and completion/failure status. IDs, nanoseconds, and 64-bit counters are decimal strings. Fingerprints describe reproducibility, not runtime validity of a game script.

Query visiting and cold-buffer materialization are separate measurements. Allocation counters cover candidate storage and shared value, order, index, directory, preparation, history, connectivity, and result-buffer PMR resources. Input generation, cold metadata strings, temporary associative work lists, harness/JSON objects, and allocator overhead are outside those counters. The process-memory sample is a coarse supplemental observation. Sample formatting and final content summarization happen after measured work.

Create a local comparison manifest, for example beside the reports:

```json
{
  "format": "jahorta.salsa.ir-evaluation-manifest",
  "version": 1,
  "reports": ["run-1.json", "run-2.json"]
}
```

```powershell
.\bin\x64\Release\SalsaIrEvaluation.exe compare --manifest "planning/Whole program IR/results/runs.json" --report "planning/Whole program IR/results/comparison.json"
```

Comparison accepts completed runs of the same build, fixture, trace, common implementation, and operation schedule. Different candidates can be compared under those same shared conditions. Bounded order/reader alternatives are labeled separately in each timing group; their policies are not collapsed into candidate-only averages. Intermediate query counts/digests and final content must agree. Failed or mismatched inputs produce a diagnostic instead of a misleading combined table. Comparison reports retain each run's load, storage, memory, and work measurements, and aggregate load phases alongside operation timings. Individual observations and descriptive median/p95/maximum values remain visible; small samples do not establish latency guarantees. The tool does not automatically pick a winner.

## Failures and process lifetime

A parent console process supervises each workload in a hidden worker with a time limit and Windows job memory limit. Defaults are 30 seconds and 512 MiB; `--timeout-ms` and `--memory-mb` change them. Count is bounded to 8 through 1,000,000 synthetic records. Use `--help` for options.

Ctrl+C requests cancellation at the worker's next operation boundary. If it cannot finish safely within five seconds, the parent terminates the disposable worker and records cancellation. Timeouts also discard partial results. No editor document or workspace is opened or saved. A worker report is retained beside the final report as `.worker`; an interrupted write may leave `.partial` evidence. Missing or malformed worker output is reported as incomplete.

Exit codes: 0 completed, 2 invalid request/report I/O error, 3 unsupported operation/fixture/candidate, 4 detected result inconsistency, 5 operation failure, 6 cancelled, 7 timed out, 8 allocation/resource limit, 9 crashed/incomplete. A process killed externally may leave no final report; that is not a completed observation.

## Development scope

Three console smoke groups are available through `SalsaIrEvaluation self-test`: indexed load/edit/undo and bounded storage churn across all adapters/order policies; lifetime, detached readers, story dependencies and rejection/cancellation; and run/report/cross-candidate comparison. Private storage inspection runs only in these smoke checks, not as a prerequisite or extra pass in normal experiments. They are development checks, not a prerequisite to running experiments or a frozen specification for the future IR. Keep validation lightweight while these designs evolve.

The public target and fixtures contain no corpus data. Keep routine reports local and ignored. Future game-input launchers and evidence belong in the private testing repository under SALSA's approved coverage policy and mutex. This target has no Qt dependency. IR6 links the existing public SpiceSCT/Compression/SpiceRoot libraries and reuses SALSA's container framing and SHA-256 implementation. It requires no submodule changes.


## Shared program foundation

`ProgramHost<Storage>` owns identities, script/section metadata, immutable operand/expression pools, authored order, direct facts, and edit history. `StoredRecord` holds stable instruction/owner IDs and a slice into shared operands. The reference adapter stores those records and applies prepared batches; it does not interpret variables, calls, expressions, or order.

Project-local IDs use one monotonic allocator. Source metadata is qualified by the synthetic baseline key. New instructions require a reserved ID; deletion does not make an ID available to another entity. Undo restores original instructions, operand IDs, and surviving fact roles. Project identity, revision, generation, and host ownership qualify preparations; stale or cancelled preparations cannot publish.

Variable readers/writers and incoming targets are indexed. Fact nodes use pooled slots with separate bucket/source links. Call and physical-fallthrough target buckets use the paged instruction directory; variable buckets are shared by bank/index. Literal expressions, variable leaves, binary addition, and explicit unknown expressions are represented without per-node heap objects. Expression reads are discovered structurally; no VM execution or constant propagation is claimed. Explicit synthetic guards have the bounded interpretation described below. Writes carry either a literal or an expression reference. Scheduled-expression reads are retained separately from ordinary operand roles.

Bindings with unresolved evidence remain draft content and appear through the unknown-fact query. They do not become resolved calls to missing IDs. Physical fallthrough is a distinct fact, and reordering repairs only affected neighboring edges. Deleting a resolved call target requires an explicit repair of its incoming callers in the same batch. Different incoming edges can still target a shared body.

Preparation retains changed records, affected order lists, names, and fact deltas. Publication applies storage, direct indexes, order, and revision together. Undo/redo use those same update paths with retained before/after values. Unchanged fact roles preserve IDs and postings. A name change does not rebuild execution indexes; owner/flag-only changes reuse unchanged operand ranges.

The report breaks initial load into common input/value/order preparation, candidate record storage, and direct index construction. `operation_work` reports instructions examined, facts extracted/changed, postings changed, order IDs copied, and ranks repaired. These are measured work counts, not estimates based solely on command size.

Remaining starter costs are explicit: the reference map allocates per record, while order mutation still detaches a complete affected section and repairs its ranks. `retired_operand_bytes` counts retained non-live bundles, including history/preparation roots and garbage awaiting maintenance; it is not an estimate of immediately reclaimable bytes. IR5 adds explicit reclamation and detached-reader policies below. These changes operate inside the experiment, not the live editor or existing project format.


## Storage measurements and current limits

The interface is version 4: `load(records, sectionLayouts)` (section IDs and row counts), typed lookup, physical enumeration, preparation, allocation-free publication, maintained metrics, and optional developer inspection. The host checks batch identities and ownership and rejects stale preparations. Storage lookup pointers are borrowed until the next load/preparation/publication; consumers retain IDs rather than pointers.

All allocations needed for publication happen during preparation. Packed-table preparation can retain enlarged capacity even when abandoned; paged preparation owns unpublished new pages and releases them when discarded. Shared host generation checks prevent stale plans from installing. The record-storage candidates retain spare capacity and empty pages. IR5 reclaims shared operand payload pages, not candidate record-storage capacity.

`storage` records live/capacity slack, group and empty-group counts, ID-directory bytes, growth events, relocated rows, swap-removal moves, and directory writes. Groups mean pages or tables; they are not story modules. Growth and directory-work counters exclude initial loading. Directory bytes cover the candidate's ID-location directory only; section maps, page ownership, and free-list overhead remain visible in the PMR domain totals. Reference-map capacity equals live records; node/allocator overhead is not represented as spare record slots. Row size is reported so relocation bytes can be computed without assuming a compiler layout.

`operation_work` also reports storage growth/movement/directory deltas and retained capacity after each action. Authored order copies and rank repairs remain separate shared-host costs. Dense growth relocates rows without rewriting numeric slot locations. Paged page additions move owner handles when necessary, not instruction records. The allocation resource captures those container allocations.

Initial synthetic comparisons cover 10,000 and 100,000 initial instructions, four workload shapes, all four adapters, and five repetitions each, in separate workers with alternating candidate order. Local observations show the expected tradeoffs: dense uses few allocations but incurs global growth; paged avoids live-row relocation with bounded page slack; section-local narrows growth but allocates more when there are many sections. Shared direct indexes and long-section order/history work are substantial costs independent of storage choice. These observations do not select a production winner or measure editor startup. These IR3 observations predate the IR4 derived index. Million-record scaling remains later evaluation work. IR5 measures shared payload reclamation and IR6 adds native/persistence observations separately below.


## Incremental story connectivity (IR4)

```powershell
.\bin\x64\Release\SalsaIrEvaluation.exe run --candidate paged --fixture story-connectivity --count 10000 --report "planning/Whole program IR/results/story-run.json"
```

`Module` declares a stable identity, owning script, fixed section boundaries, an original entry instruction, and a role (cutscene, interaction, helper, or field entry). Boundaries are supplied by the fixture; they are not inferred from naming, instruction position, or patterns. Sections cannot belong to two modules. Module/boundary editing is outside this slice.

The synthetic `Guard` operation declares literal equality against a bit or byte for its owning module. Each guard is considered independently. The derived view relates every bit/byte write in the source module and its immediate helper closure to matching guard variables. A literal can support or oppose that individual guard; an expression-valued write is explicitly unknown. Multiple writes, including repeated set and clear, remain separate contributions. Int/Float writes remain in the host but are not included as story effects. This does not combine guards into a complete Boolean entry condition or infer temporal ordering.

Immediate calls to declared module entries contribute the callee's known effects. Calls and transitions also produce direct relationships with the exact target instruction. A transition does not imply executing the destination within the source module. Scheduled calls retain their schedule-expression identity and do not contribute immediate callee effects. Immediate calls into undeclared/interior boundaries, unresolved calls, and unknown instructions qualify the summary as incomplete. Cycles retain known effects once per module and carry a cycle qualification; no runtime execution or loop count is inferred.

`StoryConnectivity` is a discardable derived index. It maintains pooled per-instruction contributions and reverse helper dependencies, using the same host facts for every storage backend. The host publishes compact revision-indexed receipts containing changed instruction IDs and binding-change flags. These no longer retain the immutable history payloads. Each derived consumer keeps a cursor until it acknowledges consumed revisions; explicit maintenance retires receipts past all active consumers. Synchronization coalesces changed instructions across batches, undo, redo, or several edits. Incoming callers are revisited only when a target's presence or section ownership changes. Value edits change their own contributions and invalidate affected helper callers without re-extracting their instructions.

Dirty summaries become pending. Preparation walks only affected module dependencies; installation rejects another host/project, stale revision/generation, or a consumed preparation. Reads used during preparation currently require a quiescent host. These capture checks support delayed installation. IR5 also provides detached execution readers; the existing `StoryConnectivity` preparation still reads the quiescent host and is not automatically a concurrent snapshot consumer. A failed derived operation leaves authored content intact, and its derived index can be discarded/reloaded. A partially updated derived index is not available for queries.

Relationships stream through a sink instead of allocating a permanent writer-by-reader graph. Their identity is the source module plus supporting effect/guard fact IDs. They include supporting instructions, required/written values, exact control target, and scheduling evidence. Fact identities survive value/target edits and undo. Query tickets watch the source-summary version and relevant guard-bucket/binding versions, including empty buckets. A newly added or retargeted guard therefore invalidates an earlier query result even without a previous edge. Unrelated changes can retain the result; a flag-only edit does not rebuild an unchanged summary.

A module summary exposes `pending` and issue bits: cycle `1`, unresolved instruction/boundary `2`, traversal budget `4`, missing declared entry `8`, and expression-valued effect `16`. Known connections may still be inspected for an incomplete summary. Current bounds are 64 modules and 4,096 call visits per summary, 32 relevant contributions per instruction, and 100,000 emitted relationships per query. Summary budget exhaustion and output truncation are explicit. This is a bounded candidate-connection view, not a proven route through the game, a symbolic VM, or a reconstructed story timeline.

Reports add connectivity loading time, per-operation source/contribution/summary/call work, streamed relationship counts/digests, module qualifications, final connectivity checksums, and derived pool metrics. PMR memory includes the connectivity pool, directories, dependency metadata, and scratch/ticket buffers; cold module strings and boundary vectors remain outside PMR accounting. Comparison checks intermediate relationship output, final connections, and module qualifications as well as authored content. Report formatting and final result summarization remain outside measured operations.

The initial IR4 comparison uses only `story-connectivity`, at 10,000 and 100,000 instructions, all four adapters, and three repetitions: 24 final-build supervised runs. The three console smoke groups include hand-checked relationships, empty-bucket invalidation, stale completion, coalesced helper updates, moved targets, and a small fixture-only fresh-derived comparison through edits/undo/redo. These checks do not run as a verification phase before normal experiments. No GUI or corpus is loaded.

Derived pools retain spare slots/capacity, and the initial contribution reservation is conservative. IR5 addresses receipt/history payload retention and compares order-copy policies, while leaving derived pool sizing unchanged. This evaluator does not change SALSA's live editor, native SCT handling, or project persistence.

## Memory lifetime and consistent readers (IR5)

```powershell
.\bin\x64\Release\SalsaIrEvaluation.exe run --candidate dense --fixture lifetime --count 100000 --order-policy shared --reader-policy selected --report "planning/Whole program IR/results/lifetime-run.json"
```

IR5 introduced run specification version 2, adding `order_policy` (`shared` or `copy`) and `reader_policy` (`selected` or `whole`). Defaults are `shared` and `selected`; report and comparison-manifest versions remain 1. These policies appear in comparison group names. The common variant identifies the new payload, history, order, and reader implementation. Earlier binaries are not comparable with this one.

`OperandPool` holds immutable three-value bundles in 256-bundle pages. Live records, undo/redo history, and outstanding preparations keep bundles rooted. Private handles survive reclamation; authored IDs are never recycled. `clearHistory()` releases the journal and invalidates outstanding installations, while a still-held preparation protects its payload until cancelled or destroyed. Consumed/cancelled preparations release their roots promptly. Preparations remain tied to their host and allocator lifetime.

`reclaim()` explicitly scans roots and pages, frees unrooted bundle slots, and returns completely empty operand pages. It also retires acknowledged change receipts. An active lagging subscriber retains needed receipts; asking for already-retired receipts fails with a rebuild diagnostic. Receipts contain IDs rather than old payloads. Ordinary edits do not run this maintenance or make a complete snapshot.

Expression IDs remain available by default, even if currently unused. `reclaim(true)` explicitly opts into retiring expressions unreachable from live/history/preparation operands, including their expression dependencies. It compacts expression storage and repairs private locations without reusing authored IDs. Callers must not continue using an unreferenced expression ID after opting into retirement. This scan/copy and its temporary allocation are maintenance costs, not hidden work on view refresh.

`captureRead(sections, whole)` returns immutable, independently owned execution data at one captured project/revision/generation. It copies the requested records, direct facts, section orders, referenced expression closure, owning script metadata, and evidence. `selected` captures section 10 in the lifetime fixture; `whole` captures every instruction section. External targets remain IDs: failure to find one in a selected scope means outside that scope, not necessarily dangling. This is not a complete authoring-project persistence snapshot or a precomputed story graph.

Capture runs against a quiescent host on its owner thread. The resulting reader can be used by a worker during subsequent edits, history release, or reclamation, and can outlive both host and allocator context. It owns ordinary C++ values instead of borrowing live pool memory. `accepts(reader.capture())` rejects stale deferred output. The mutable host and existing story-preparation API are not thread-safe; this slice does not make arbitrary live-host calls concurrent.

Reader bytes are a separate capacity-based estimate of owned data, including vector/string capacity. They exclude allocator/control-block overhead and may count inline string storage twice; they are not exact heap accounting. PMR domain counters continue to measure host allocations separately. Reader capture is a separate timed phase after initial readiness, with no automatic reader per edit.

The `shared` order policy shares immutable section-order buffers between live state, history, and preparations; mutation detaches the affected section. Undo/redo can reuse saved buffers. The `copy` alternative copies each order-list value. Both retain identical semantics and candidate-independent instrumentation. Sharing reduces repeated copying but does not eliminate whole-section detachment or rank repair on insertion/reorder. A chunked order structure would be another experiment.

The lifetime fixture holds a reader across churn, 512 value edits and periodic undo/redo, maintenance with history retained, history release and reclamation, and finally reader release. Reports include these checkpoints, rooted/freed bundle counts, returned page bytes, retained order/history bytes, reader estimates, and maintenance timings. Console smoke coverage additionally exercises pending preparations, expression retirement, a worker reading after reclamation, stale results, lagging receipt subscribers, and a reader that outlives its host.

The bounded IR5 comparison comprises 36 supervised runs: 10,000/100,000 initial instructions, three repetitions, all four candidates under shared/selected, plus dense-only copy/selected and shared/whole alternatives. All intermediate query and final content comparisons agree. At 100k, sharing reduces copied order IDs over this trace from 960,665 to 252,717; selected readers hold 50k records and about 15.6 MB of estimated data versus 100k records and 31.2 MB for whole readers. These are local synthetic observations, not editor performance or a production storage decision.

Remaining retention is explicit: partially occupied operand pages stay allocated with reusable holes; page-owner vectors, ID directories, receipt/root metadata and derived pools retain high-water capacity. History can grow until explicitly released; stale preparations or lagging consumers intentionally retain their roots/receipts. Explicit reader scopes can still be large copies. Maintenance scans the current pool and roots, and no million-record latency guarantee is claimed. IR6 adds native SCT and compressed persistence evaluation below.


## Native SCT and compressed persistence (IR6)

```powershell
.\bin\x64\Release\SalsaIrEvaluation.exe run --candidate paged --fixture native-dc --count 10000 --persistence compressed --report "planning/Whole program IR/results/native-container.json"
.\bin\x64\Release\SalsaIrEvaluation.exe run --candidate dense --fixture story-connectivity --count 10000 --persistence compressed --report "planning/Whole program IR/results/story-container.json"
```

Run specifications now use version 3. `--persistence none|compressed|binary` defaults to `none` and can be used with any fixture. The native fixtures always exercise fresh SCT output; adding `compressed` also measures saving/reloading the edited model. The common implementation variant changes to reflect native control kinds, persisted identity roles, and the container adapter. Report and comparison-manifest versions remain 1. Development-format compatibility with earlier experiment builds is not provided.

### Native coverage

`importNative` uses SPICE's parser and document importer once per input. It copies the supported executable meaning into the shared IR, qualifies each script with a dataset key, normalized asset name, source SHA-256 and SPICE import lineage, then releases the parsed/document objects. Source-only opaque attachments are counted in the report and excluded from execution state. Non-code sections, supplementary text, unsupported opcodes or expressions, and unresolved execution targets fail explicitly instead of vanishing into a successful result.

| Native instruction | IR meaning |
| --- | --- |
| 5 | Byte-variable assignment |
| 17 / 18 | Set / clear a fixed bit index |
| 10 / 11 / 12 | Jump / call / return |
| 15 / 16 | Next frame / sleep |

Expression coverage includes nonnegative integral encoded-decimal literals through 32767, fixed inline constants, bit/byte variable leaves, and conventional addition. Float literals, negative/fractional decimals, integer/float banks, comparisons, arbitrary stack programs, and opaque expressions remain outside this bounded adapter. Unknown/unresolved synthetic draft values remain supported by the program container; they are not silently converted into native output.

Scheduling and skip-refresh modifiers retain their identities/values. Scheduled jumps/returns are explicitly unsupported. Jumps and returns have no ordinary fallthrough fact; calls retain their continuation. Physical fallthrough across section boundaries is explicitly unsupported in this adapter, so nonempty imported sections must end in an unscheduled jump or return. Synthetic story guards and abstract field transitions are not presented as implemented native branch or script-loading opcodes. Native module recovery and general story-condition extraction remain future work.

Imported `init` and `loop` names are protected against rename commands; their bodies remain editable. A document missing a root can remain a saved draft. Fresh output requires exactly one nonempty `init` and `loop`, with explicit repair when missing. The adapter does not synthesize startup calls or replacement bodies. Section creation/removal remains outside the current host operation set.

`exportNative` builds fresh SPICE documents from a captured checkpoint, then uses SPICE's exporter for validation, layout, relocation, encoding and optional AKLZ. Generated mappings connect authored script/section/instruction IDs to revision-qualified output instruction IDs, ordinals and decoded payload spans. No source-byte reuse or opaque preservation is required. The native fixture edits a bit, retargets a call, and inserts a write before a terminal, with undo/redo. Final output is reparsed and compared by supported values/order/target locations; checkpoint reload must also reproduce identical fresh output bytes.

### Experimental save contract

`ProgramHost::checkpoint()` is an explicit save capture. It retains project identity, semantic revision, allocator high-water state, scripts/source qualification, section order, modules, expressions, evidence, reserved instruction IDs, and live instruction/operand/fact identities. Deleted IDs remain retired through the high-water state. Fact identity roles are saved so supporting references survive reload; derived values/postings are rebuilt from authored content. No storage slots, pointers, compiler struct images, baseline bytes, undo journal, or module-summary cache are serialized.

The semantic format is `jahorta.salsa.ir-program`, version 1, encoded as deterministic JSON with decimal-string identities and explicit ordered tables. It is stored as `ir/program.json` inside the existing SALSAPRJ v1 zlib/SHA-256 framing. This is a private experimental `.salsair` container, distinct from the editor's authoring project. The only accepted derived-cache policy is `rebuild`; history is explicitly `not-saved`. All candidates use the same codec.

Decoding checks format, row shapes, numeric ranges and policy fields. Normal host construction then checks ownership, target references, expression dependencies, identity roles and allocator state while building storage and direct indexes. Derived construction checks module ownership/boundaries. A failed load is discarded; there is no extra whole-project verification step on edit or refresh. Reload preserves semantic revision and gives the host a fresh instance identity, so old prepared work cannot install into it.

### Measurement scope

Reports separate synthetic native-source generation, SPICE parse/decompression, document import, IR normalization, model/index construction, and readiness excluding source generation. Native input bytes are already in memory; these are not native-file disk-read timings. The generated SCTs use two scripts each, without claiming coverage of actual game inventories or releases.

Persistence reports separate checkpoint capture, JSON construction/dump, compression/framing/checksum, temporary-file write/close and read, decompression/unpacking/checksum, JSON parsing, model decode, normal validation/storage/index load, and derived rebuild. File reads immediately follow writes; OS caching is uncontrolled, and no transaction/durability claim is made. Final identity/content/output comparisons are observations after measured loading/export work, not a prerequisite run before the workload.

Windows process-wide peak working set/pagefile counters and current private bytes include native documents, input/checkpoint copies, JSON and compression buffers. They are cumulative process observations, not exact temporary-object accounting. Reload PMR domain totals are reported separately. Original host/save data may coexist with reloaded state, and the reports do not hide that residency. JSON/native conversion uses ordinary temporary containers outside PMR allocation counters.

The focused IR6 comparison uses 1,000 and 10,000 initial instructions, all four candidates, three repetitions, and native-gc/native-dc/story-connectivity with compressed persistence. It is a bounded integration experiment; larger scaling and consolidated architecture selection belong to IR7. The retained suite remains three console smoke groups, plus six filtered existing SPICE SCT checks covering relocation, AKLZ, expression construction and scheduling. No GUI or corpus is loaded.

IR6 does not migrate the live semantic editor, its existing compressed projects, or Python SALSA v7 import. Those production paths remain unchanged and supported; completing their operation coverage and metadata promotion is a later integration requirement. The binary-table follow-up below is complete. IR7 has consolidated the evidence and selected the production integration direction; see [the architecture decision](WholeProgramIrArchitecture.md).


## Binary-table persistence follow-up (before IR7)

```powershell
.\bin\x64\Release\SalsaIrEvaluation.exe run --candidate dense --fixture native-gc --count 10000 --persistence binary --report "planning/Whole program IR/results/binary-container.json"
```

`compressed` retains the compressed JSON experiment; `binary` selects explicit binary tables in the **same** SALSAPRJ v1 zlib/SHA-256 container. Both use identical checkpoints and the same host/storage/index construction. Persistence is now a comparison variant: JSON and binary runs with matching workloads can be compared, while persistence-disabled runs remain a different workload. Spec version 3 and report/manifest version 1 are unchanged; the storage interface is now 4 and the common implementation variant is updated. Compare fresh runs from the same build.

The `ir/program.bin` payload starts with `SALSAIRB`, binary version 1, an endian marker, total size, project identity, revision, allocator high-water state and a table directory. Integers are explicitly little-endian regardless of native SCT platform. Twelve ordered tables hold scripts, sections, modules, expressions, instructions, instruction identity roles, fact identity roles, authored instruction order, module membership, evidence text references, reserved IDs and a string byte pool. References use IDs or table-relative ranges. Text preserves its bytes, including embedded NULs. The version implies rebuilt derived state and unsaved history, matching JSON's explicit policy fields.

Each table has a fixed encoded stride: scripts 112 bytes, sections 48, modules 64, expressions 40, instructions 56, instruction identity rows 48, fact identity rows 32, order/membership/reserved IDs 8, text references 16, and string bytes 1. The 64-byte header is followed by twelve 24-byte directory entries containing offset/count/stride. These are explicit encodings, independent of compiler padding, addresses, allocator state and candidate-specific slot layouts. Numeric instruction values retain their full 64 bits. This first layout favors simple fixed records; it is not the smallest possible binary encoding.

`viewBinaryProgram()` checks the header and bounded, contiguous, nonoverlapping table directory, then prepares cold script/module/text metadata. Instructions, expressions, section orders, reserved IDs and identity/fact roles remain borrowed table views. `ProgramHost::load()` consumes either these views or the existing fixture input through one implementation. It decodes small rows directly into owned pools and order lists, stages one contiguous candidate-record array, and constructs the usual directories/indexes. Candidates receive section IDs/counts rather than temporary section order vectors. There is no JSON tree, full temporary `Fixture`, or per-instruction heap object in the binary reader. The reference candidate still allocates its map nodes by design.

This is direct table loading, not a C++ object dump or memory-mapped editable state. Header/range/tag errors fail during view construction or row consumption; ownership, references and identities are checked during normal host construction. Failed new hosts are discarded. No borrowed input remains in the live host, and the input buffer is released before derived queries or editing. Explicit save capture still constructs the existing checkpoint, and the writer stages output tables before assembling the payload. Those save-side costs remain in scope and visible.

Reports add `binary_table_encode_ns`, `binary_table_view_ns`, the payload format/input policy, and `reload_to_ready_ns`. The latter includes decompression/checksum, input decoding, ordinary host construction, temporary-input cleanup and derived rebuilding; it excludes disk reading. Individual phase timers do not include every intervening destructor, so their medians need not sum to total readiness. Final canonical JSON comparison happens **after** measured loading and the `after_reload_memory` snapshot for both formats. This prevents the binary readiness memory measurement from including the comparison's JSON tree. Process peaks still include initial fixture/native construction, the original host, save capture and compression; PMR totals only count instrumented domains.

The bounded follow-up ran 144 supervised synthetic cases: 1k/10k initial instructions, native-gc/native-dc/story-connectivity, all four candidates, both compressed payloads, and three repetitions. Candidate/format order rotates. All six workload/size comparisons agree on edited content, stable identities, connections and native output. No corpus, GUI, native parser parallelization or production persistence changes are included.

Dense-candidate medians at 10,000 initial instructions (milliseconds; three observations each):

| Workload | JSON reload to ready | Binary reload to ready | JSON compressed bytes | Binary compressed bytes |
| --- | ---: | ---: | ---: | ---: |
| Synthetic GC native | 68.826 | 14.120 | 278,919 | 284,917 |
| Synthetic DC native | 71.214 | 14.288 | 278,901 | 284,915 |
| Synthetic story connectivity | 56.700 | 14.745 | 234,648 | 242,317 |

All storage candidates show the same broad reload improvement: their 10k binary medians range from 14.1 to 16.3 ms. The lower memory observation is also material: dense GC native peak working set through reload was 42.20 MiB for JSON versus 26.57 MiB for binary; story was 33.80 versus 18.74 MiB. These are cumulative process observations, not exact allocation savings attributable solely to parsing.

There are tradeoffs. This fixed-record binary payload is larger before compression (GC native: 2,160,886 versus 1,777,876 bytes) and slightly larger after compression. Its compression/framing/checksum cost is higher (GC native medians: 122.704 versus 71.384 ms), enough to outweigh faster binary encoding in this save path. Binary row decoding also makes ordinary host construction slower than loading already-materialized JSON-decoded records (8.851 versus 4.650 ms for dense GC native). It removes the much larger JSON parse/model construction and cleanup costs; it does not eliminate all conversion or index work. Compression/layout tuning and architecture selection remain later decisions.

Validation: elevated MSVC v145 Release build and the existing three console smoke groups passed. The integration group now includes deterministic binary encoding, cross-candidate identity preservation, editing/undo/redo after input release, malformed header/range/allocator handling and JSON/binary report agreement. Existing native smoke cases exercise both payloads. No additional suite, GUI test or separate verification prerequisite was introduced. No SPICE implementation changed, so unrelated or additional SPICE suites were not run.
