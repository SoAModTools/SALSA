# Whole-program persistence and v7 import (WP4)

WP4 supplies binary persistence for the production paged IR through `ProgramPersistence`, background installation through `ProgramService::openProject`, and supported Python SALSA v7 capsule ingestion through `LegacyProgramImport`. There is no JSON executable tree, native reparse, baseline hash scan or SPICE document reconstruction when reopening a saved program. WP6 uses this authority and file path in the production Qt entry point. WP5 supplies fresh native materialization.

## Binary contract

New project files use the eight-byte `SALSAPRJ` magic followed by three little-endian u32 fields: framing version **2**, codec **1** (Zstandard), and program binary schema **2**. The existing v1 container implies zlib and is not reinterpreted as this format. Unknown versions/codecs/schemas fail with a diagnostic; loading does not write or replace the original file. There is no development-schema converter.

After the header, each independent record has a u32 decoded length, u32 compressed length and one complete zstd frame. A zero/zero record terminates the file; trailing bytes are rejected. Decoded records are at most 1 MiB. Frame checksums are enabled. Compression uses the pinned Zstandard 1.5.7 library at level 1, with one context, no dictionary and no worker threads. Encoder and decoder reuse bounded scratch buffers; they do not retain a worst-case compressed buffer for every record. The encoded file itself is held in memory for the existing transaction API. This framing is an integrity/container contract, not authentication or encryption.

The logical binary stream is independent of record boundaries. Fields use explicit little-endian widths: authored IDs and revisions are u64, pool indices/counts and enum/variant tags are u32, flags are one byte, and coordinates are finite IEEE-754 binary64. Strings are counted UTF-8 bytes; digest bytes retain their exact 32-byte representation. UUIDs retain their textual identity. Optionals have a boolean presence flag; variants have explicit schema-ordered tags. No struct padding, pointer, STL layout or native-endian memory dump is persisted. `BinaryArchive.inl` defines the field order for every cold record; changing that contract requires a schema bump.

Stream order:

1. Project identity, captured revision, allocator high-water mark and source-omission counts.
2. Imported baseline descriptors and ordered scripts, including output profiles, legacy origins and optional MLD associations.
3. Live bottom-up values with inline child-index lists and text bytes.
4. Live section and text records, followed by sequence identities, ownership, roles and action orders.
5. Live actions; structured actions include their arms/shared labels and kind-specific loop/switch/transfer records.
6. Live condition regions and authored metadata: modules, entrypoints/activation, ports, connections/routes, language groups, logical text, annotations and variable meanings.
7. Separate presentation/session content: positions, selection, ordered tabs with view/selection, active tab, opcode colors and presentation folders.

The encoder flushes the semantic record before presentation starts. Presentation changes therefore leave preceding semantic records unchanged. Authored order is retained. Dead actions, obsolete arm ranges and unreachable prior value versions are omitted. Values and range indices may change on reopen; authored identities do not. The allocator includes consumed IDs even when their entities were deleted. Derived postings, module summaries, service generation, saved-history tokens and undo history are not persisted. Baseline descriptors qualify source identities; baseline bytes remain outside executable state and outside this file.

The decoder checks framing and configured bounds, constructs unpublished tables, checks ownership/references and arranges derived postings once. No partial project escapes on failure. Value shape checks are reused when arranging postings. Temporary ownership marks use contiguous arrays rather than one allocated tree node per instruction. Value child indices and text bytes are read directly into their pools without a temporary vector or string per value. Missing lifecycle roots remain drafts; unsupported or unresolved executable content retains its diagnostics. Export eligibility belongs to WP5's normal export boundary.

Default limits are 1 GiB encoded, 4 GiB decoded and 64 million items in one collection. These are admission limits, not a measured process-memory ceiling: owned tables, ID directories, derived indexes, captured checkpoints and encoded input also occupy memory. Saving explicitly captures a complete checkpoint; ordinary view refresh still uses scoped readers.

Schema 2 adds typed cutscene slots, route definitions referencing existing conditions, and connection-command observations. Slot/connection annotations can retain aliases and explanations. Derived cutscene queries are not persisted. Schema 1 development files are rejected without modifying them; Python SALSA v7 import remains supported.

## Save, recovery and background open

The caller captures `ProgramService::checkpoint(expectedRevision)` and prepares `ProgramPersistence::mutation` off the UI thread. That mutation contains one encoded project artifact and the expected prior SHA-256, suitable for the existing `WorkspaceTransactionService`. Destination directories are established by the workspace. Baseline retention and other workspace artifacts can participate in that same transaction. No second file transaction engine is introduced.

Compression can be cancelled before staging/publication. Once a workspace recovery or commit stage starts it finishes through the existing noncancellable transaction protocol. After successful commit, the caller marks only the captured revision/history token saved. Later edits stay dirty; undo back to the saved history state is clean. A save failure does not mark the authority saved.

`openProject` reads, decompresses and prepares the program on a background worker, then asks the owner service to adopt it. Optional workspace recovery runs in that worker before reading. The request captures project/revision/generation; edits, another load, cancellation or shutdown prevent stale installation. Presentation/session data is returned only with successful installation. Failed or unsupported loads leave the old authority intact. Native collection loads and project loads share the generation used to reject superseded work.

Progress reports concrete phases and counts: reading the file, decompressing records, arranging program relationships and readiness. Encoding reports preparation/compression and completed encoding. Recovery progress is marked noncancellable. Low-level `encode`, `decode`, `load` and legacy-import calls are synchronous Qt-free APIs for workers; `openProject` supplies asynchronous reading/installation. Callbacks run away from Qt and must be queued by the future editor adapter. UI tab population and exclusive-operation presentation remain WP6. No GUI-loading tests run in this slice.

## Supported legacy v7 path

The existing trusted Python v7 converter and capsule validation boundary remain operational. `LegacyProgramImport::importCapsule` takes explicit accepted-script selections, target asset locators and publication conventions. It reuses the existing typed capsule translator; it does not reconstruct Python objects as execution authority. The translated native document exists only at this import edge. The importer creates retained native baseline bytes once through the existing exporter, hashes them, normalizes the translated content into the IR and releases the temporary document. It does not export/reparse/reimport the same script to construct the IR.

Supported metadata uses the existing extraction helpers: project and script variable aliases become stable meaning records, opcode colors become presentation settings, and nested section folders become presentation folders referencing stable section IDs. Legacy folders never imply modules. Script contexts retain capsule ID, script key and ordinal. Unsupported metadata remains in the retained capsule, with an attached note identifying that origin. Supported-field extraction failures become warnings and durable pending-evidence notes rather than silently dropping metadata or blocking otherwise supported script content. The caller retains returned baseline bytes with the workspace's existing baseline storage. No new-script authoring is added, and reopening the resulting binary project requires neither the capsule nor a native baseline parse.

## Dependency and delivery

The production library compiles the vendored Zstandard C subset under `third-party/zstd`; it does not depend on the evaluator's optional generated props or machine-local binaries. `SOURCE.json` pins the upstream version/archive and records the exact source-file hashes. No submodule changes are required. SALSA selects Zstandard's BSD 3-Clause option and retains its complete license text. Build output receives the notice and license. Release packaging must run `tools/package_zstd_sources.py --release-directory <directory>` to include the exact dependency sources and build integration; see [third-party notices](../THIRD_PARTY_NOTICES.md).

The implementation checkpoint records the elevated MSVC v145 Release build, focused synthetic console checks, packaging check and any remaining validation limits. Corpus and GUI testing are not expanded for this persistence slice.

Validation: elevated MSVC v145 Release SalsaCore/SalsaTests build passed. All 21 focused console checks passed: the three new persistence/v7 scenarios, existing production IR/service/semantic cases, three existing v7 regressions and two existing workspace transaction cases. The dependency source/notice packager passed its hash checks. Existing LNK4020 PDB debug-symbol warnings remain. Standalone SpiceTests is still unavailable after the previously recorded unrelated SpiceMix/PVM build failure; the production IR fixtures exercise public SCT import in both byte orders. No GUI or private-corpus tests ran, and no full-corpus performance claim is made.

WP5 connects binary-reopened programs to [fresh native export](WholeProgramIrMaterialization.md). Supported v7 imports use that same materializer after explicit lifecycle repair when needed; retained migration evidence is not consulted as execution state.
