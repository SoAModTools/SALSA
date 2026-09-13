# Whole-program IR evaluation tool

`SalsaIrEvaluation` is an experimental, Qt-free console tool for comparing SALSA's future internal representations. IR1 provides the runner and a simple reference adapter. It does not change the editor, load game files, or select a production representation.

## Run an experiment

Build `src/SalsaIrEvaluation/SalsaIrEvaluation.vcxproj` with MSVC v145 Release. The executable is placed in `bin/x64/Release`.

```powershell
.\bin\x64\Release\SalsaIrEvaluation.exe run --candidate reference --count 64 --seed 1 --report "planning/Whole program IR/results/run-1.json"
.\bin\x64\Release\SalsaIrEvaluation.exe run --candidate reference --count 64 --seed 1 --repetition 1 --report "planning/Whole program IR/results/run-2.json"
```

Each command runs once and writes measurements or a failure report. There is no preliminary verification run, receipt, or mode to enable. Output paths must be fresh so an earlier observation is not overwritten.

The available fixture is `small-story`, and its trace is `semantic-edits`. They use a declared synthetic instruction language with reads, literal writes, direct calls, replacement edits, and undo/redo. These are not SCT opcode definitions. Insert/delete, owner moves, guard summaries, native SCT conversion, and compressed persistence remain later work and are reported as unsupported.

The `reference` adapter uses a standard map. Its allocation behavior is intentionally simple, not a proposed compact production representation. Dense, paged, and section-local candidates arrive in IR3 through the same compile-time storage interface and shared workload driver.

## Results and comparison

Reports contain the fixture/trace fingerprints, complete executable SHA-256, compiler/build identity, configuration, declared coverage, phase/operation timings, allocation counters, record work, final content checksum, and completion/failure status. IDs, nanoseconds, and 64-bit counters are decimal strings. Fingerprints describe reproducibility, not runtime validity of a game script.

Query visiting and cold-buffer materialization are separate measurements. Allocation counters cover the reference storage, preparation, history, and query-result PMR resources. Input generation, harness/JSON objects, and allocator overhead are outside those counters. The process-memory sample is a coarse supplemental observation. Sample formatting and final content summarization happen after measured work.

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

Three small console smoke checks are available through `SalsaIrEvaluation self-test`: reference load/edit/undo, rejection/cancellation, and run/report/comparison. They are development checks, not a prerequisite to running experiments or a frozen specification for the future IR. Keep validation lightweight while these designs evolve.

The public target and fixtures contain no corpus data. Keep routine reports local and ignored. Future game-input launchers and evidence belong in the private testing repository under SALSA's approved coverage policy and mutex. This target has no Qt or SPICE runtime dependency and requires no submodule changes.
