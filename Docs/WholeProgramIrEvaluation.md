# Whole-program IR evaluation tool

`SalsaIrEvaluation` is an experimental, Qt-free console tool for comparing SALSA's future internal representations. IR4 adds incremental story connectivity to the dense, paged, section-local, and reference storage candidates behind the shared program/editing host. It does not change the editor, load game files, or select a production representation.

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
| `story-connectivity` | Six declared modules with bit/byte guards, shared helpers, cutscene/field transfers, scheduled calls, unknown effects, cycles, and edits with undo/redo. Requires at least 16 instructions. |

The requested count is the initial instruction count; churn adds 300 records beyond the baseline edits. All candidates receive identical section catalogs, including empty sections, and identical inputs/traces. The trace exercises reads, literal/expression writes, direct calls, batch replacement/insertion/deletion, same-script section moves, reordering, renaming, unresolved targets, and undo/redo. These are not SCT opcode definitions. General control-flow interpretation, cross-script owner moves, native SCT conversion, snapshots, and persistence remain unsupported. The bounded declared-guard subset is described below.

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

Comparison accepts completed runs of the same build, fixture, trace, common policy, and operation schedule. Different candidates can be compared under those same shared conditions. Intermediate query counts/digests and final content must agree. Failed or mismatched inputs produce a diagnostic instead of a misleading combined table. Comparison reports retain each run's load, storage, memory, and work measurements, and aggregate load phases alongside operation timings. Individual observations and descriptive median/p95/maximum values remain visible; small samples do not establish latency guarantees. The tool does not automatically pick a winner.

## Failures and process lifetime

A parent console process supervises each workload in a hidden worker with a time limit and Windows job memory limit. Defaults are 30 seconds and 512 MiB; `--timeout-ms` and `--memory-mb` change them. Count is bounded to 8 through 1,000,000 synthetic records. Use `--help` for options.

Ctrl+C requests cancellation at the worker's next operation boundary. If it cannot finish safely within five seconds, the parent terminates the disposable worker and records cancellation. Timeouts also discard partial results. No editor document or workspace is opened or saved. A worker report is retained beside the final report as `.worker`; an interrupted write may leave `.partial` evidence. Missing or malformed worker output is reported as incomplete.

Exit codes: 0 completed, 2 invalid request/report I/O error, 3 unsupported operation/fixture/candidate, 4 detected result inconsistency, 5 operation failure, 6 cancelled, 7 timed out, 8 allocation/resource limit, 9 crashed/incomplete. A process killed externally may leave no final report; that is not a completed observation.

## Development scope

Three small console smoke checks are available through `SalsaIrEvaluation self-test`: indexed load/edit/undo and bounded storage churn across all adapters, rejection/cancellation, and run/report/cross-candidate comparison. Private storage inspection runs only in these smoke checks, not as a prerequisite or extra pass in normal experiments. They are development checks, not a prerequisite to running experiments or a frozen specification for the future IR. Keep validation lightweight while these designs evolve.

The public target and fixtures contain no corpus data. Keep routine reports local and ignored. Future game-input launchers and evidence belong in the private testing repository under SALSA's approved coverage policy and mutex. This target has no Qt or SPICE runtime dependency and requires no submodule changes.


## Shared program foundation

`ProgramHost<Storage>` owns identities, script/section metadata, immutable operand/expression pools, authored order, direct facts, and edit history. `StoredRecord` holds stable instruction/owner IDs and a slice into shared operands. The reference adapter stores those records and applies prepared batches; it does not interpret variables, calls, expressions, or order.

Project-local IDs use one monotonic allocator. Source metadata is qualified by the synthetic baseline key. New instructions require a reserved ID; deletion does not make an ID available to another entity. Undo restores original instructions, operand IDs, and surviving fact roles. Project identity, revision, generation, and host ownership qualify preparations; stale or cancelled preparations cannot publish.

Variable readers/writers and incoming targets are indexed. Fact nodes use pooled slots with separate bucket/source links. Call and physical-fallthrough target buckets use the paged instruction directory; variable buckets are shared by bank/index. Literal expressions, variable leaves, binary addition, and explicit unknown expressions are represented without per-node heap objects. Expression reads are discovered structurally; no VM execution or constant propagation is claimed. Explicit synthetic guards have the bounded interpretation described below. Writes carry either a literal or an expression reference. Scheduled-expression reads are retained separately from ordinary operand roles.

Bindings with unresolved evidence remain draft content and appear through the unknown-fact query. They do not become resolved calls to missing IDs. Physical fallthrough is a distinct fact, and reordering repairs only affected neighboring edges. Deleting a resolved call target requires an explicit repair of its incoming callers in the same batch. Different incoming edges can still target a shared body.

Preparation retains changed records, affected order lists, names, and fact deltas. Publication applies storage, direct indexes, order, and revision together. Undo/redo use those same update paths with retained before/after values. Unchanged fact roles preserve IDs and postings. A name change does not rebuild execution indexes; owner/flag-only changes reuse unchanged operand ranges.

The report breaks initial load into common input/value/order preparation, candidate record storage, and direct index construction. `operation_work` reports instructions examined, facts extracted/changed, postings changed, order IDs copied, and ranks repaired. These are measured work counts, not estimates based solely on command size.

Remaining starter costs are explicit: the reference map allocates per record; affected section order lists are copied for preparation/history; retired operand ranges remain retained. `retired_operand_bytes` includes both history-referenced ranges and discarded preparation ranges. Reclamation and retained-reader policies belong to IR5. These changes operate inside the experiment, not the live editor or existing project format.


## Storage measurements and current limits

The interface is version 3: `load(records, sections)`, typed lookup, physical enumeration, preparation, allocation-free publication, maintained metrics, and optional developer inspection. The host checks batch identities and ownership and rejects stale preparations. Storage lookup pointers are borrowed until the next load/preparation/publication; consumers retain IDs rather than pointers.

All allocations needed for publication happen during preparation. Packed-table preparation can retain enlarged capacity even when abandoned; paged preparation owns unpublished new pages and releases them when discarded. Shared host generation checks prevent stale plans from installing. The candidates retain spare capacity and empty pages; reclamation is later work.

`storage` records live/capacity slack, group and empty-group counts, ID-directory bytes, growth events, relocated rows, swap-removal moves, and directory writes. Groups mean pages or tables; they are not story modules. Growth and directory-work counters exclude initial loading. Directory bytes cover the candidate's ID-location directory only; section maps, page ownership, and free-list overhead remain visible in the PMR domain totals. Reference-map capacity equals live records; node/allocator overhead is not represented as spare record slots. Row size is reported so relocation bytes can be computed without assuming a compiler layout.

`operation_work` also reports storage growth/movement/directory deltas and retained capacity after each action. Authored order copies and rank repairs remain separate shared-host costs. Dense growth relocates rows without rewriting numeric slot locations. Paged page additions move owner handles when necessary, not instruction records. The allocation resource captures those container allocations.

Initial synthetic comparisons cover 10,000 and 100,000 initial instructions, four workload shapes, all four adapters, and five repetitions each, in separate workers with alternating candidate order. Local observations show the expected tradeoffs: dense uses few allocations but incurs global growth; paged avoids live-row relocation with bounded page slack; section-local narrows growth but allocates more when there are many sections. Shared direct indexes and long-section order/history work are substantial costs independent of storage choice. These observations do not select a production winner or measure editor startup. These IR3 observations predate the IR4 derived index. Million-record scaling, reclamation, native SCT, and persistence remain later evaluation work.


## Incremental story connectivity (IR4)

```powershell
.\bin\x64\Release\SalsaIrEvaluation.exe run --candidate paged --fixture story-connectivity --count 10000 --report "planning/Whole program IR/results/story-run.json"
```

`Module` declares a stable identity, owning script, fixed section boundaries, an original entry instruction, and a role (cutscene, interaction, helper, or field entry). Boundaries are supplied by the fixture; they are not inferred from naming, instruction position, or patterns. Sections cannot belong to two modules. Module/boundary editing is outside this slice.

The synthetic `Guard` operation declares literal equality against a bit or byte for its owning module. Each guard is considered independently. The derived view relates every bit/byte write in the source module and its immediate helper closure to matching guard variables. A literal can support or oppose that individual guard; an expression-valued write is explicitly unknown. Multiple writes, including repeated set and clear, remain separate contributions. Int/Float writes remain in the host but are not included as story effects. This does not combine guards into a complete Boolean entry condition or infer temporal ordering.

Immediate calls to declared module entries contribute the callee's known effects. Calls and transitions also produce direct relationships with the exact target instruction. A transition does not imply executing the destination within the source module. Scheduled calls retain their schedule-expression identity and do not contribute immediate callee effects. Immediate calls into undeclared/interior boundaries, unresolved calls, and unknown instructions qualify the summary as incomplete. Cycles retain known effects once per module and carry a cycle qualification; no runtime execution or loop count is inferred.

`StoryConnectivity` is a discardable derived index. It maintains pooled per-instruction contributions and reverse helper dependencies, using the same host facts for every storage backend. The host publishes revision-indexed change receipts referencing its existing immutable change/history payloads. Synchronization coalesces changed instructions across batches, undo, redo, or several edits. Incoming callers are revisited only when a target's presence or section ownership changes. Value edits change their own contributions and invalidate affected helper callers without re-extracting their instructions.

Dirty summaries become pending. Preparation walks only affected module dependencies; installation rejects another host/project, stale revision/generation, or a consumed preparation. Reads used during preparation currently require a quiescent host. These capture checks support delayed installation, not concurrent mutation-safe readers; consistent readers belong to IR5. A failed derived operation leaves authored content intact, and its derived index can be discarded/reloaded. A partially updated derived index is not available for queries.

Relationships stream through a sink instead of allocating a permanent writer-by-reader graph. Their identity is the source module plus supporting effect/guard fact IDs. They include supporting instructions, required/written values, exact control target, and scheduling evidence. Fact identities survive value/target edits and undo. Query tickets watch the source-summary version and relevant guard-bucket/binding versions, including empty buckets. A newly added or retargeted guard therefore invalidates an earlier query result even without a previous edge. Unrelated changes can retain the result; a flag-only edit does not rebuild an unchanged summary.

A module summary exposes `pending` and issue bits: cycle `1`, unresolved instruction/boundary `2`, traversal budget `4`, missing declared entry `8`, and expression-valued effect `16`. Known connections may still be inspected for an incomplete summary. Current bounds are 64 modules and 4,096 call visits per summary, 32 relevant contributions per instruction, and 100,000 emitted relationships per query. Summary budget exhaustion and output truncation are explicit. This is a bounded candidate-connection view, not a proven route through the game, a symbolic VM, or a reconstructed story timeline.

Reports add connectivity loading time, per-operation source/contribution/summary/call work, streamed relationship counts/digests, module qualifications, final connectivity checksums, and derived pool metrics. PMR memory includes the connectivity pool, directories, dependency metadata, and scratch/ticket buffers; cold module strings and boundary vectors remain outside PMR accounting. Comparison checks intermediate relationship output, final connections, and module qualifications as well as authored content. Report formatting and final result summarization remain outside measured operations.

The initial IR4 comparison uses only `story-connectivity`, at 10,000 and 100,000 instructions, all four adapters, and three repetitions: 24 final-build supervised runs. The three console smoke groups include hand-checked relationships, empty-bucket invalidation, stale completion, coalesced helper updates, moved targets, and a small fixture-only fresh-derived comparison through edits/undo/redo. These checks do not run as a verification phase before normal experiments. No GUI or corpus is loaded.

Change receipts currently retain history payloads even after an undo branch is replaced. Derived pools retain spare slots/capacity, and the initial contribution reservation is conservative. Those lifetimes and the earlier long-section order-copying costs are explicit inputs to IR5. IR4 does not change SALSA's live editor, native SCT handling, or project persistence.
