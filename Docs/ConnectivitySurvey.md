# Standalone SCT connectivity survey

`SalsaConnectivitySurvey` inventories and inspects complete SCT datasets without opening the editor, creating authoring projects, or exporting scripts. It is a standard C++ console executable linked to SalsaCore and public SPICE contracts. It does not depend on Qt or gtest.

The survey and the editor share `SctInterpretation::extract`. Editor naming and user assertions remain separate from source facts. The shared connectivity index stores variable-to-writer and variable-to-reader incidence lists; it does not allocate their Cartesian product. An editor caller can enumerate candidate pairs on demand.

## Run

Build `src/SalsaConnectivitySurvey/SalsaConnectivitySurvey.vcxproj` in **Release x64**, with the elevated MSVC v145 toolchain required by this repository. The executable is placed in `bin/x64/Release`.

Provide an external local manifest:

```json
{
  "format": "jahorta.salsa.connectivity-survey-input",
  "schemaVersion": 1,
  "datasets": [
    {
      "id": "example_disc1",
      "release": "example",
      "platform": "dreamcast",
      "region": "eu",
      "disc": "1",
      "root": "X:/local-input",
      "textConvention": null
    }
  ]
}
```

`platform` is `gamecube` or `dreamcast`; `region` is `jp`, `us`, or `eu`. Dataset IDs are unique alphanumeric names with optional underscores/hyphens. Each configured dataset must exist and contain SCTs. `textConvention` can be null for the existing loader's assessment, or an explicit SPICE text-convention stable name. Keep separate discs and releases in separate datasets.

```text
SalsaConnectivitySurvey scan --manifest input.local.json --output survey-output
SalsaConnectivitySurvey scan --manifest input.local.json --output survey-output --resume --jobs 2 --timeout-seconds 300
```

The input directory is recursively inventoried for every `.sct`, with case-normalized relative locators, sorted input order, and SHA-256 fingerprints. Case-colliding locators and files that resolve outside the input tree are rejected. Output must be outside every input tree. Existing nonempty output requires `--resume`. An exclusive output lock prevents concurrent writers.

Two isolated child processes run by default. `--jobs` accepts 1–64; each file has a configurable timeout (300 seconds by default). Windows job objects terminate children on cancellation or parent shutdown. Ctrl+C stops new work and cancels active children; inventory hashing finishes before cancellation reporting if interruption occurs during inventory. Individual parse failures, exceptions, crashes, and timeouts are recorded while other files continue. Configuration, orchestration, and report-writing failures terminate the run.

Exit codes: **0** complete, **1** incomplete files, **2** fatal configuration/orchestration/report error, **130** cancelled. “Analyzed” means an inspectable document was obtained, not that it is export-ready. Import diagnostics and readiness remain in its facts.

## Stored results and resume

- `survey.json`: versioned inventory, qualified source identities, per-file status, diagnostics, cache references, load/extraction/worker timings and peak working set.
- `summary.md` / `summary.json`: coverage, groups, scopes, unresolved writes, ungrouped writes, frequently accessed variables, incidence components, slow files, and repeated relative paths across datasets.
- `index.json.zlib`: compressed dataset-specific script/variable incidence. The run records its digest so a stale index cannot be queried as current.
- `facts/<digest>.json.zlib`: independently compressed, checksummed per-script facts. Only source relationships and relevant state expressions are included; no source-byte, baseline, or dialogue dump.

Fact schema version 1 is independent of the authoring project schema and compressed project container. IDs are decimal strings. Physical IDs are meaningful only together with the dataset, relative path, input hash, import options, and analyzer/build identity. No random authoring project identity is introduced. Object ordering is deterministic; instruction and section order remain explicit. Timings naturally vary between runs.

Resume hashes the current inventory and reuses only successfully analyzed facts whose qualified identity and checksum still match. Input bytes, platform, region, selected encoding, analyzer version, and linked executable hash participate in cache identity. Failed or corrupt entries are retried. Old cache files are retained but are excluded from the current manifest and queries. Worker job files are removed after a completed/cancelled run. A hard shutdown can leave these temporary files; the next scan replaces its own job records.

Queries require a finalized matching index; a run interrupted during a report write can be repaired by resuming. Source SCTs are not opened during queries.

## Queries

```text
SalsaConnectivitySurvey query --survey survey-output --variable BitVar:1960
SalsaConnectivitySurvey query --survey survey-output --script example_disc1:field/me002a.sct
SalsaConnectivitySurvey query --survey survey-output --cutscene example_disc1:field/me002a.sct#12
SalsaConnectivitySurvey query --survey survey-output --entrypoint example_disc1:field/me002a.sct#19
```

A script query lists sections and candidate scope IDs for the other queries. `init`, `loop`, and direct `M` handlers are labeled as named entrypoints. Cutscene candidates use the same section-name recognition as the editor; unrecognized code remains fully represented as script facts rather than being omitted.

Variable keys are `BitVar:N`, `ByteVar:N`, `IntVar:N`, and `FloatVar:N`. A variable query searches every dataset separately, or a selected `--dataset`. Script/cutscene/entrypoint queries include the same-dataset incidence for the variables they use, making other writers and readers discoverable without constructing every pair.

Access records include script identity, section, instruction ID, opcode, parameter/repeated-group location, source payload offset where available, schedule, expression, and operation. Use `--json` for machine output, `--all` for all access records, or `--offset N --limit N` to page them (default 100). Total and truncation are explicit. Pagination applies to access records; supporting section/scope/control-flow metadata and factored incidence remain available.

## Interpretation limits

- All recognized sets, clears, toggles, assignments, and reads are retained. There is no story-bank filter or early-cutscene cutoff.
- Scopes include entire candidate sections and transitive referenced helpers, both conditional arms, switches, schedules, and modeled physical fallthrough. They can overlap and are not module ownership or runtime execution traces.
- Dynamic bit destinations are retained as unresolved writes. Unknown/native opcode internal effects are not enumerated. A variable with no SCT readers is still reported.
- Components describe the undirected **script/variable incidence graph**, including shared reads and shared writes. They are not a verified story backbone. Writer/reader counts and potential pair factors disclose whether a variable actually has both sides.
- Matching numbers across different datasets never connect scripts. Repeated paths/hash observations are inventory information, not cross-release equivalence or deduplication.
- This does not require MLDs, execute the game, infer story meaning, check retail reachability, modify semantic projects, or prune library code.

## Validation and private use

Synthetic tests cover the shared extraction and compact index, compressed records, queries, parallel/serial equivalence, corrupt-cache retry, partial failure, timeout, cancellation, and exit status. These tests live in SalsaTests; the survey executable itself has no test-framework dependency.

Actual corpus launchers belong in the sister private testing repository. Its separate `tools/salsa-connectivity/Invoke-Survey.ps1` runs the approved full DC EU survey on both available discs under that repository's mutex, using its local corpus-location configuration. It does not call the gtest harness or alter the existing `me002*.sct` connectivity regression scope. Keep input locations and all survey artifacts untracked.
