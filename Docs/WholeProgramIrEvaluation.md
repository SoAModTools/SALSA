# Whole-program IR evaluation tool

`SalsaIrEvaluation` is an experimental, Qt-free console tool for comparing SALSA's future internal representations. IR2 provides the shared program/editing foundation behind the runner and reference storage adapter. It does not change the editor, load game files, or select a production representation.

## Run an experiment

Build `src/SalsaIrEvaluation/SalsaIrEvaluation.vcxproj` with MSVC v145 Release. The executable is placed in `bin/x64/Release`.

```powershell
.\bin\x64\Release\SalsaIrEvaluation.exe run --candidate reference --count 64 --seed 1 --report "planning/Whole program IR/results/run-1.json"
.\bin\x64\Release\SalsaIrEvaluation.exe run --candidate reference --count 64 --seed 1 --repetition 1 --report "planning/Whole program IR/results/run-2.json"
```

Each command runs once and writes measurements or a failure report. There is no preliminary verification run, receipt, or mode to enable. Output paths must be fresh so an earlier observation is not overwritten.

The available fixture is `small-story`, and its trace is `semantic-edits`. They use a declared synthetic instruction language across two source-qualified scripts and three sections. The trace exercises reads, literal/expression writes, direct calls, batch replacement/insertion/deletion, same-script section moves, reordering, renaming, unresolved targets, and undo/redo. These are not SCT opcode definitions. Guard summaries, cross-script owner moves, native SCT conversion, snapshots, and persistence remain later work and are reported as unsupported.

The `reference` adapter uses a standard map. Its allocation behavior is intentionally simple, not a proposed compact production representation. Dense, paged, and section-local candidates arrive in IR3 through the same compile-time storage interface and shared workload driver.

## Results and comparison

Reports contain the fixture/trace fingerprints, complete executable SHA-256, compiler/build identity, configuration, declared coverage, phase/operation timings, allocation counters, record work, final content checksum, and completion/failure status. IDs, nanoseconds, and 64-bit counters are decimal strings. Fingerprints describe reproducibility, not runtime validity of a game script.

Query visiting and cold-buffer materialization are separate measurements. Allocation counters cover the reference storage and shared value, order, index, directory, preparation, history, and result-buffer PMR resources. Input generation, cold metadata strings, temporary associative work lists, harness/JSON objects, and allocator overhead are outside those counters. The process-memory sample is a coarse supplemental observation. Sample formatting and final content summarization happen after measured work.

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

Comparison accepts completed runs of the same build, fixture, trace, common policy, and operation schedule. Different candidates may eventually be compared under those same shared conditions. Failed or mismatched inputs produce a diagnostic instead of a misleading combined table. Individual observations and descriptive median/p95/maximum values remain visible; small samples do not establish latency guarantees. The tool does not automatically pick a winner.

## Failures and process lifetime

A parent console process supervises each workload in a hidden worker with a time limit and Windows job memory limit. Defaults are 30 seconds and 512 MiB; `--timeout-ms` and `--memory-mb` change them. Count is bounded to 8 through 1,000,000 synthetic records. Use `--help` for options.

Ctrl+C requests cancellation at the worker's next operation boundary. If it cannot finish safely within five seconds, the parent terminates the disposable worker and records cancellation. Timeouts also discard partial results. No editor document or workspace is opened or saved. A worker report is retained beside the final report as `.worker`; an interrupted write may leave `.partial` evidence. Missing or malformed worker output is reported as incomplete.

Exit codes: 0 completed, 2 invalid request/report I/O error, 3 unsupported operation/fixture/candidate, 4 detected result inconsistency, 5 operation failure, 6 cancelled, 7 timed out, 8 allocation/resource limit, 9 crashed/incomplete. A process killed externally may leave no final report; that is not a completed observation.

## Development scope

Three small console smoke checks are available through `SalsaIrEvaluation self-test`: indexed load/edit/undo with stable fact identity, rejection/cancellation, and run/report/comparison. They are development checks, not a prerequisite to running experiments or a frozen specification for the future IR. Keep validation lightweight while these designs evolve.

The public target and fixtures contain no corpus data. Keep routine reports local and ignored. Future game-input launchers and evidence belong in the private testing repository under SALSA's approved coverage policy and mutex. This target has no Qt or SPICE runtime dependency and requires no submodule changes.


## Shared program foundation

`ProgramHost<Storage>` owns identities, script/section metadata, immutable operand/expression pools, authored order, direct facts, and edit history. `StoredRecord` holds stable instruction/owner IDs and a slice into shared operands. The reference adapter stores those records and applies prepared batches; it does not interpret variables, calls, expressions, or order.

Project-local IDs use one monotonic allocator. Source metadata is qualified by the synthetic baseline key. New instructions require a reserved ID; deletion does not make an ID available to another entity. Undo restores original instructions, operand IDs, and surviving fact roles. Project identity, revision, generation, and host ownership qualify preparations; stale or cancelled preparations cannot publish.

Variable readers/writers and incoming targets are indexed. Fact nodes use pooled slots with separate bucket/source links. Call and physical-fallthrough target buckets use the paged instruction directory; variable buckets are shared by bank/index. Literal expressions, variable leaves, binary addition, and explicit unknown expressions are represented without per-node heap objects. Expression reads are discovered structurally; no VM execution, constant propagation, or story-guard interpretation is claimed. Writes carry either a literal or an expression reference. Scheduled-expression reads are retained separately from ordinary operand roles.

Bindings with unresolved evidence remain draft content and appear through the unknown-fact query. They do not become resolved calls to missing IDs. Physical fallthrough is a distinct fact, and reordering repairs only affected neighboring edges. Deleting a resolved call target requires an explicit repair of its incoming callers in the same batch. Different incoming edges can still target a shared body.

Preparation retains changed records, affected order lists, names, and fact deltas. Publication applies storage, direct indexes, order, and revision together. Undo/redo use those same update paths with retained before/after values. Unchanged fact roles preserve IDs and postings. A name change does not rebuild execution indexes; owner/flag-only changes reuse unchanged operand ranges.

The report breaks initial load into common input/value/order preparation, candidate record storage, and direct index construction. `operation_work` reports instructions examined, facts extracted/changed, postings changed, order IDs copied, and ranks repaired. These are measured work counts, not estimates based solely on command size.

Remaining starter costs are explicit: the reference map allocates per record; affected section order lists are copied for preparation/history; retired operand ranges remain retained. `retired_operand_bytes` includes both history-referenced ranges and discarded preparation ranges. Reclamation and retained-reader policies belong to IR5. These changes operate inside the experiment, not the live editor or existing project format.
