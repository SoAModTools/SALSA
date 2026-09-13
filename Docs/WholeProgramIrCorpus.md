# Whole-script corpus infrastructure

This opt-in, Release-only console experiment lives in SALSA. It prepares an inventory of every `.sct` under the configured GameCube US root, loads the captured inventory into one resident project, applies reproducible eligible edits, and measures memory/timing across the three compact storage candidates. It is not a gtest corpus suite and never launches the editor.

The architecture recommendation is provisional until the real corpus experiment has been run and assessed. Infrastructure completion does not claim a successful full-corpus load.

## Running

Build `src/SalsaIrEvaluation/SalsaIrEvaluation.vcxproj` with elevated MSVC v145 x64 Release. From SALSA:

```powershell
# Enumerate inputs and write the ignored local manifest without decoding SCTs.
python.exe tools/run_ir_corpus.py --prepare-only

# Explicitly run the complete captured inventory, three candidates, three repetitions.
python.exe tools/run_ir_corpus.py --seed 1 --edits 1000 --repetitions 3
```

By default the launcher reads `datasets.gc_us` from the sibling private testing repository's ignored `corpus-locations.json`. Use `--locations-manifest` or `--corpus-root` for an explicit override. The private repository is read only; no test code, mutex, results or manifests are written there. This GC US-only exception is recorded in SALSA's AGENTS.md and does not broaden other corpus permissions.

The launcher writes a fresh timestamped directory under ignored `private/ir-corpus/`. It holds the machine-local inventory, per-worker reports/logs, run order and comparison. The inventory includes all discovered SCTs, an expected count, explicit text convention and ordered relative asset keys. Empty inventories, duplicate keys and input symlinks outside the selected root are rejected. Real inputs stay outside repositories and are read only. No input or generated binary payload is copied into SALSA.

Use `--candidates dense paged section-local` to choose the compared variants. Defaults are seed 1, 1,000 edit groups, three repetitions, a 4,096 MiB worker limit and a ten-minute limit per worker. `--edits 0` measures load/save/reload without edits. `--text-convention` defaults to the declared Shift-JIS/byte-7F convention; it is explicit in the report rather than inferred from platform. Candidate order rotates across repetitions. Each run gets a separate supervised process, with no parser parallelism in this baseline.

The lower-level entry point is `SalsaIrEvaluation corpus --manifest PATH --candidate paged --edits 1000 --seed 1 --report PATH`. Normal use should go through the Python launcher: it also owns a temporary directory outside repositories and arranges cleanup of binary save products after worker completion or failure. The supervisor bounds memory/time, forwards cancellation and rejects missing worker results. Reports use fresh paths; a failed run is retained for diagnosis and stops the comparison.

## What is represented

The earlier IR6 adapter only accepts a small synthetic-compatible native opcode subset. This workload therefore adds a generic native-content host using the **same dense/paged/section-local storage implementations and shared FactIndex**, with flat immutable parameter/value tables. It does not silently translate arbitrary opcodes into empty placeholder instructions or claim that the narrow ProgramHost now supports all SCT authoring.

`CorpusModel.cpp` copies the public SPICE document values into tagged nodes, child-index ranges and a text byte pool. Every instruction retains its opcode, scheduling expression, skip-refresh modifier, fixed parameters and ordered repeated groups. Typed SCPT value/binary/stack/inert operations, termination, encoding words and payload words remain explicit. Instruction/string/footer references use assigned project-local IDs. Encoded words, terminated sequences and unresolved-reference evidence remain present. Indexed text, message headers/chunks/commands/arguments, group markers and supplementary text are included. Strings and instructions with identical names in different scripts remain separate entities.

SPICE parses and imports each input once. Source buffers, parsed inspections and SPICE documents are released after that script's normalization. The temporary record array is released after candidate loading. Script IDs, section IDs, instruction IDs and text IDs are allocated in manifest order; source asset/hash qualification is kept with each script. The native tables are common across candidates, so actual data volume does not depend on which storage implementation is being measured.

Source-only opaque attachments/text are counted and excluded from semantic payloads, retaining lightweight gap descriptors where appropriate. Opaque executable parameters/expressions/sections are explicit execution-coverage gaps. Missing files, failed imports or execution gaps prevent a complete-corpus result; the report lists loaded/failed counts and per-file diagnostics. It never silently benchmarks a successful subset as the whole corpus. Explicit unresolved bindings are stored as draft evidence, not resolved to invented targets.

Direct indexes currently cover typed variable reads, bit set/clear instructions with integral decimal or float literal indices, explicit instruction-reference operands and unknown-reference evidence. Integer access variants retain their original SCPT kind in payloads while sharing the underlying bank index; secondary values are not presented as ordinary integer variables. Incoming references include switch-case/repeated-group operands and carry stable source identities. This is **not** complete side-effect interpretation, physical-fallthrough analysis, recovered cutscene boundaries, module-summary rebuilding or a runtime path model. Those are separate coverage dimensions. Opcode counts, flat-pool sizes, recognized index counts and the count of unrecognized bit-write targets are reported. Computed indices and special inline sentinels are not silently converted to fixed targets.

## Seeded changes

The workload uses SplitMix64 and a stable eligible-site ordering. It cycles through six edit kinds: change a fixed bit set to clear or vice versa; change a bit-variable occurrence in an expression; retarget an explicit instruction operand to an existing section entry in the same script; insert a copied eligible bit-write instruction; delete a previously inserted unreferenced instruction; and rename a non-lifecycle section. `init` and `loop` cannot be renamed, and rename collisions are avoided. Parameters are addressed by instruction ID and ordered child path, preserving repeated-group/case distinctions.

Sampling combines broad eligible-site selection, longest-section pressure, frequently referenced targets and frequently read bits. It is intentionally not uniform over all possible instruction changes. Selected iterations group two distinct bit-write edits, and periodic undo/redo reverses/reapplies the entire group. An isolated run aborts on failure and never publishes a partially successful corpus result. This workload is not the production editor's transactional command API.

Payload replacements copy only that instruction's immutable subtree and refresh its direct facts. Surviving source/fact roles retain local index identity during editing. Order-changing commands share unchanged order buffers with history, copying the affected section's order. Derived fact IDs are rebuilt on project reopening; stable externally persisted fact identities and full story-summary invalidation from the earlier ProgramHost are not claimed by this generic native host.

Reports retain the numeric command trace and its digest, per-group edit/undo/redo/query timings, changed-source counts and order IDs copied. Ineligible requested edit classes are counted and make edit coverage incomplete, rather than disappearing into a passed result. The launcher compares inventory content hashes, executable identity, trace, final canonical content and query digests across runs before emitting a comparison. This checks candidate agreement; it does not prove runtime playability or all shared extraction semantics.

## Measurements and limits

- Loading: input reads, source hashing, SPICE parse/decompression, document import, normalization, candidate/index construction and total readiness. Per-file cleanup and progress/report bookkeeping are included in total readiness, not necessarily individual phase timers.
- Memory: current process private bytes and working set, cumulative peaks, plus PMR allocation/capacity totals. Snapshots cover baseline, import-temporary release, ready state, edit-catalog construction, edits, catalog release, history release and binary reopening. Raw normalized node/child/text counts and capacities are available separately.
- Retention: history changes/order handles are released explicitly. The immutable payload pool currently retains dead versions until host destruction; this is visible retained capacity, not a claim of working reclamation. Save capture compacts reachable payload roots. This experiment can expose whether a different pool policy is needed.
- Persistence: an explicit portable little-endian corpus checkpoint preserves native table contents, names, IDs, revision and allocator high-water state inside the existing compressed/checksummed container. Corpus checkpoint v2 stores separate metadata, node, child-index and text tables. Nodes use an explicit 21-byte encoding (one-byte tag, 64-bit value, and three 32-bit fields); child indices use four bytes and authored IDs remain 64-bit. Tables are split into ordered records of at most 64 MiB, keeping the production container limits unchanged. Reopening fills owned tables without reconstructing per-node child vectors. It is separate from both the production project format and IR6's narrower payload schema. Normal save/reopen compares canonical content after measured work; there is no separate pre-run verification phase.
- Reopen timing includes decompression, decoding, storage and direct-index building. Original and reopened hosts coexist for the comparison, so that phase's process memory is not a standalone project-open footprint. File reads immediately follow writes; no cold-cache or durability claim is made.
- Counters cover instrumented pools and candidate storage. Cold metadata, eligibility paths, history/order wrappers, SPICE allocations, report JSON and allocator overhead are not all instrumented; process snapshots include them. Readiness is sampled before the edit catalog/trace is built. Large record-table staging and full save capture remain measured costs.

The corpus load is sequential and read-only. No native SCT export, executable patch, GUI cutover, MLD loading, or game execution is part of this infrastructure. The initial real GC US run loaded all 258 scripts but exposed two harness defects: float-literal bit targets were omitted from the write index, and the wide single-record checkpoint exceeded the container record cap. Both are addressed by the literal recognition and v2 table checkpoint described above. Results from the earlier run are not an accepted candidate comparison.

Validation used an elevated v145 Release build, the existing three console smoke groups extended with small synthetic native-content cases, a supervised missing-input rejection, the launcher's help path, and six filtered existing SPICE SCT contract checks. Constructed fixtures cover text, repeated references, high bit indices, decimal and float bit targets, edits/batches, undo/redo, small chunk-boundary reopening, and binary reopening across all three candidates. Two tiny generated SCT files exercise actual parsing, without any private game input. No tests or changes were made in the private repository or SPICE submodule.

The corrected harness completed the approved GC US comparison: all three candidates passed three repetitions of the full captured inventory and seeded workload, including checkpoint reopening. Float and decimal bit targets were recognized without omissions in that inventory, and traces, content and query digests agreed. Machine-specific metrics and corpus evidence remain in ignored local reports. This completes the experiment, not production editor integration or architecture sign-off.

For a codec-only comparison using one captured checkpoint, see [the compression experiment](WholeProgramIrCompression.md). It avoids repeating native imports or the storage-candidate matrix for each codec.
