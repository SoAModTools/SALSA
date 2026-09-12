# Standalone SCT and MLD connectivity survey

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
      "textConvention": null,
      "mlds": [
        { "path": "field/a099a.mld", "role": "base", "scripts": ["field/me099a.sct"] }
      ]
    }
  ]
}
```

`platform` is `gamecube` or `dreamcast`; `region` is `jp`, `us`, or `eu`. Dataset IDs are unique alphanumeric names with optional underscores/hyphens. Each configured dataset must exist and contain SCTs. `textConvention` can be null for the existing loader's assessment, or an explicit SPICE text-convention stable name. Keep separate discs and releases in separate datasets.

`mlds` is optional. Each entry explicitly associates one dataset-relative MLD with zero or more script locators in that dataset. Roles are `base`, `auxiliary`, `streamed-tile`, or `unclassified`. Combine associations for a repeated resource path; retain separate resources when several candidates exist. The executable never binds resources solely by filename. Missing or malformed selected MLDs are advisory gaps; SCT analysis continues. No geometry or source bytes are stored.

```text
SalsaConnectivitySurvey scan --manifest input.local.json --output survey-output
SalsaConnectivitySurvey scan --manifest input.local.json --output survey-output --resume --jobs 2 --timeout-seconds 300
```

The input directory is recursively inventoried for every `.sct`, with case-normalized relative locators, sorted input order, and SHA-256 fingerprints. Case-colliding locators and files that resolve outside the input tree are rejected. Output must be outside every input tree. Existing nonempty output requires `--resume`. An exclusive output lock prevents concurrent writers.

Two isolated child processes run by default. `--jobs` accepts 1–64; each file has a configurable timeout (300 seconds by default). Windows job objects terminate children on cancellation or parent shutdown. Ctrl+C stops new work and cancels active children; inventory hashing finishes before cancellation reporting if interruption occurs during inventory. Individual parse failures, exceptions, crashes, and timeouts are recorded while other files continue. Configuration, orchestration, and report-writing failures terminate the run.

Exit codes: **0** SCT analysis complete, **1** incomplete SCT files, **2** fatal configuration/orchestration/report error, **130** cancelled. Optional MLD failures are counted separately and do not change a successful SCT result. “Analyzed” means an inspectable document was obtained, not that it is export-ready. Import diagnostics and readiness remain in its facts.

## Stored results and resume

- `survey.json`: versioned inventory, qualified source identities, per-file status, diagnostics, cache references, load/extraction/worker timings and peak working set.
- `summary.md` / `summary.json`: coverage, groups, scopes, unresolved writes, ungrouped writes, frequently accessed variables, incidence components, slow files, and repeated relative paths across datasets.
- `index.json.zlib`: compressed dataset-specific script/variable incidence and MLD entry associations. The run records its digest so a stale index cannot be queried as current.
- `facts/<digest>.json.zlib`: independently compressed, checksummed per-script or per-MLD facts. Only source relationships, entry parameters, script targets, and relevant state expressions are included; no source-byte, baseline, geometry, or dialogue dump.

Output/fact/index/query schema version **2** is independent of the authoring project schema and compressed project container. Input manifests remain version **1**. IDs are decimal strings. Physical IDs are meaningful only together with the dataset, relative path, input hash, import options, and analyzer/build identity. No random authoring project identity is introduced. Object ordering is deterministic; instruction and section order remain explicit. Timings naturally vary between runs.

Resume hashes the current inventory and reuses only successfully analyzed facts whose qualified identity and checksum still match. Input bytes, platform, region, selected encoding, analyzer version, linked executable hash, and selected MLD associations participate in cache identity. Changing an MLD invalidates its facts without invalidating unchanged SCT facts. Failed or corrupt entries are retried. Old cache files are retained but are excluded from the current manifest and queries. Worker job files are removed after a completed/cancelled run. A hard shutdown can leave these temporary files; the next scan replaces its own job records.

Queries require a finalized matching index; a run interrupted during a report write can be repaired by resuming. Source SCTs are not opened during queries.

## Queries

```text
SalsaConnectivitySurvey query --survey survey-output --variable BitVar:1960
SalsaConnectivitySurvey query --survey survey-output --script example_disc1:field/me002a.sct
SalsaConnectivitySurvey query --survey survey-output --cutscene example_disc1:field/me002a.sct#12
SalsaConnectivitySurvey query --survey survey-output --entrypoint example_disc1:field/me002a.sct#19
SalsaConnectivitySurvey query --survey survey-output --resource example_disc1:field/a099a.mld
```

A script query lists sections and candidate scope IDs for the other queries. `init`, `loop`, and direct `M` handlers are labeled as named entrypoints. Cutscene candidates use the same section-name recognition as the editor; unrecognized code remains fully represented as script facts rather than being omitted.

Variable keys are `BitVar:N`, `ByteVar:N`, `IntVar:N`, and `FloatVar:N`. A variable query searches every dataset separately, or a selected `--dataset`. Script/cutscene/entrypoint queries include the same-dataset incidence for the variables they use, making other writers and readers discoverable without constructing every pair.

Access records include script identity, section, instruction ID, opcode, parameter/repeated-group location, source payload offset where available, schedule, expression, and operation. Use `--json` for machine output, `--all` for all access records, or `--offset N --limit N` to page them (default 100). Total and truncation are explicit. Pagination applies to access records; supporting section/scope/control-flow metadata and factored incidence remain available.

## Optional resource connections

SPICE imports MLD entries; `SmoresScript` supplies the existing relationship conventions. Entry table index, local ID, entry ID, `tblId`, function, parameters, and imported-document qualification remain visible. Entries with repeated IDs or `tblId` values remain distinct.

- **Direct field handlers:** a non-eventhook entry's `tblId` suggests `Mxxxxx` in an explicitly associated script. Matching handlers expose conditions, writes, calls, jumps, and script transitions across their structural response scope, including referenced helpers. All script stems below **200** are fields; stem **99** carries additional overworld context. These categories do not restrict which relationship mechanisms can be reported.
- **Eventhook signals:** parameter 8 supplies the existing inferred BitVar signal candidate. Its reads in the associated SCT are listed, including compound predicates and readers outside `loop`. Unrepresentable signals and signals without readers remain visible. Eventhooks do not also acquire a direct handler solely from `tblId`.
- **Conditional response:** SPICE's structured control-flow regions supply controller, arms, case ordinals, and continuation. Each arm exposes its own calls, jumps, writes, and transitions without absorbing the shared continuation. Callees are identified but their instructions are not folded into the arm. Unrecognized boundaries stay explicit; matching a bit does not establish which branch executes or prove an activation.
- **Script transitions:** SPICE's load-script effects retain the target operand and semantic confidence. Detailed resource responses list same-dataset filename candidates, or mark the target missing, ambiguous, or unresolved. They never choose between duplicate filenames or infer a runtime transition from a numeric coincidence.

Resource queries return detailed associations. Variable queries add matching eventhook providers; script queries add the associated resource context. Entrypoint queries limit handlers/readers to the selected section. Cutscene queries show handlers calling/jumping into the cutscene root and eventhook predicates whose structured arms directly call/jump there (or whose reads lie in that scope). These are discoverable structural connections, not a complete reachability graph. Missing-resource diagnostics remain available through resource and associated script queries. Resource context is not paginated with access records.

## Interpretation limits

- All recognized state writes and reads are retained. Repeated setting and clearing can be inspected separately; there is no story-bank filter or early-cutscene cutoff.
- Scopes include entire candidate sections and transitive referenced helpers, both conditional arms, switches, schedules, and modeled physical fallthrough. They can overlap and are not module ownership or runtime execution traces.
- Dynamic bit destinations are retained as unresolved writes. Unknown/native opcode internal effects are not enumerated. A variable with no SCT readers is still reported.
- Components describe the undirected **script/variable incidence graph**, including shared reads and shared writes. They are not a verified story backbone. Writer/reader counts and potential pair factors disclose whether a variable actually has both sides.
- Matching numbers across different datasets never connect scripts. Repeated paths/hash observations are inventory information, not cross-release equivalence or deduplication.
- This does not require MLDs, execute the game, infer story meaning, check retail reachability, modify semantic projects, or prune library code.

## Validation and private use

Synthetic tests cover the shared extraction and compact index, compressed records, queries, parallel/serial equivalence, corrupt-cache retry, partial failure, timeout, cancellation, and exit status. MLD cases cover duplicate entry identities, direct handlers, signal consumers, structured responses, qualified transition candidates, missing resources, and changed-resource cache invalidation. These tests live in SalsaTests; the survey executable itself has no test-framework dependency.

Actual corpus launchers belong in the sister private testing repository. Its separate `tools/salsa-connectivity/Invoke-Survey.ps1` runs the approved full DC EU survey on both available discs under that repository's mutex, using its local corpus-location configuration. By default it adds paired base-MLD candidates for field stems below 200 from each disc's actual inventory, retaining all filename matches and reporting missing pairs. `-SctOnly` omits that optional context. It does not call the gtest harness or alter the existing `me002*.sct` connectivity regression scope. Keep input locations and all survey artifacts untracked.

## Candidate path queries

```text
SalsaConnectivitySurvey query --survey survey-output --paths example_disc1 --json
SalsaConnectivitySurvey query --survey survey-output --paths example_disc1 --start example_disc1:field/me002a.sct#12 --max-paths 32 --max-steps 64 --json
```

Path queries use the same Qt-free analysis as the story viewer, operating entirely on finalized cached facts. They return qualified nodes, typed/evidenced edges, unresolved gaps, strongly connected components, topological layers, and bounded candidate paths through those components. Cycles are unordered groups; the component member order does not claim a story sequence. Every path references component IDs. `truncated` reports edge, path, or length limits (defaults: 20,000 edges, 64 paths, 128 components per path). Path response format is `jahorta.salsa.connectivity-path-query`, version 1; existing survey formats remain version 2.

Sets and clears affecting predicates that guard actual transfers can suggest enables/inhibits relationships. Compound conditions retain their other requirements, and writes remain possible effects rather than guaranteed final state. Single-observed-writer candidates receive a stronger ranking hint. Ordinary literal loads can match an explicit supported previous-location comparison or switch case. Unqualified special return producers remain ordinary source observations. Field navigation cycles and repeated use of one script are expected; no total main-story order is imposed.
