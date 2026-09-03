# Local GameProject Shim

## Status and Purpose

No shared GameProject base or repository currently exists. SALSA should not wait for one before replacing its editing backend, and it should not invent the complete cross-tool contract while only SALSA's requirements are concrete.

The initial solution is a real SALSA-private `LocalGameProject` behind a deliberately narrow `GameProjectContext` capability boundary. This is a compatibility seam, not a mock object and not a claim that SALSA's local persistence schema is the future suite standard.

The design succeeds if a later `SharedGameProjectAdapter` can replace `LocalGameProject` without changing the SCT document, edit commands, undo/redo, patch semantics, or Qt feature widgets.

## Implemented First Phase

The initial framework now implements the smallest useful part of this boundary entirely in memory:

- `DatasetContext` with a canonical local root and `DatasetIdentity`.
- Optional caller-supplied platform and region metadata; neither is inferred.
- `AssetLocator` as a normalized, case-insensitive, dataset-relative identity.
- Recursive SCT discovery without following symbolic links, junctions, or other reparse points.
- Immutable asset descriptors and source snapshots.
- SHA-256 source revisions and a versioned, root-independent catalog fingerprint over every discovered SCT locator, size, and revision.
- Stale-source rejection and explicit rescanning.
- Structured results, diagnostics, cancellation, and warning propagation.

This phase creates no manifest, workspace directory, session file, patch, receipt, or modified SCT. It has no SPICE dependency. Those responsibilities remain behind future capabilities rather than being baked into the dataset scanner.

## Implemented Workspace Shell Phase

The Qt application now consumes the first-phase project boundary as a functional dataset workspace without introducing a temporary SCT document model:

- Dataset inspection and refresh run outside the GUI thread with frontend-neutral progress reporting and explicit cancellation.
- Opening a replacement dataset is transactional; a failed or cancelled inspection leaves the prior workspace intact.
- Refresh compares immutable catalog snapshots, reports added, removed, and changed assets, and preserves selection when the selected locator remains present.
- The Project Explorer presents the dataset-relative SCT hierarchy, while the central details view presents dataset and selected-source identity.
- Structured scan diagnostics are presented through a diagnostics model and dock.
- Recent dataset roots and window layout are machine-local `QSettings` preferences rather than project state.

Asset selection remains details-only. This phase does not load a parsed SCT document, create document tabs, define edit commands, or establish a durable workspace schema. Those capabilities remain deferred until the `SpiceSCT` document contract is ready.

## Position in the Architecture

```text
SalsaQt
    -> SalsaCore
        -> SctEditSession
        -> SalsaScriptPatch
        -> GameProjectContext capabilities
            -> LocalGameProject                 initial
            -> SharedGameProjectAdapter         future
```

The project boundary supplies identified source assets and durable patch storage. It does not own the live `SctDocument` or decide how instructions are edited.

## Longer-Term Scope

Later phases of `LocalGameProject` should add only the capabilities required by the SALSA refactor:

- One active extracted game-data dataset.
- Dataset root, platform, region, and source fingerprint metadata.
- Discovery and loading of SCT assets.
- Structured asset locators relative to the dataset.
- Storage and retrieval of SALSA-owned patch envelopes and `SalsaScriptPatch` payloads.
- Expected-source and semantic conflict checks when materializing patches.
- Direct publication to a user-selected destination using SPICE validation and serialization.
- Publication receipts containing source/output identities, destination, warnings, and results.
- Resumable SALSA session state such as selected asset and open documents.
- SALSA-private bounded history artifacts associated with individual SCT assets, without interpreting their revision semantics as shared project state.
- An optional read-only dependency-provider adapter backed by `SpiceContentGraph`.

The initial implementation may be SCT-only and SALSA-only. It does not need to coordinate SKEWER or SpiceRack patches, publish multiple gameplay domains atomically, or define every future asset type.

## Capability Boundary

Consumers should depend on small capabilities rather than a large speculative `IGameProject` interface. The provisional conceptual split is:

- `DatasetContext`: identifies the active dataset and its platform, region, root, and revision evidence.
- `AssetCatalog`: discovers assets and loads an immutable source snapshot for a structured locator.
- `PatchStore`: loads, checkpoints, archives, and removes SALSA patch envelopes.
- `PublicationService`: resolves a working revision or patch, performs preflight, publishes, and records a receipt.
- `DependencyProvider`: optionally reports read-only relationships and evidence.

These names are provisional. Their important property is dependency direction: editing services request capabilities without reading the local project manifest, constructing storage paths, or depending on a specific future repository.

The boundary should use explicit result types and diagnostics. Missing assets, source conflicts, invalid patches, failed preflight, and publication failures must not be represented as partially populated objects or silent Boolean state.

## Identity Model

The shim must keep three identity categories distinct:

### Dataset identity

`DatasetIdentity` describes the logical source dataset using platform, region, and stable fingerprint evidence. The absolute extracted root is a location associated with that identity, not the identity itself.

### Shared asset locator

`AssetLocator` is a structured dataset-relative locator. It may contain a normalized relative path and, when required, archive-member or semantic-subresource components. It must not be an ad hoc concatenated string whose meaning cannot later be migrated.

Section names and byte offsets may be recorded as source locators or evidence but are insufficient as durable identities when duplicates, renames, insertions, and relocation are possible.

### SALSA editor identity

`DocumentId`, `SectionId`, `InstructionId`, `StringId`, and related IDs belong to the materialized SALSA document and patch. They remain stable through local edits and patch checkpoints. A project adapter maps the containing source asset to an `AssetLocator`; it does not reinterpret the internal editor IDs.

### Source expectations

A `SourceRevision` or expected semantic value records what the patch was authored against. It supports conflict detection and explicit rebasing without pretending that file paths or timestamps establish identity.

Current `SpiceContentGraph` path/name/offset IDs are useful for read-only discovery within a particular source snapshot. They must not be adopted as editor IDs or durable patch anchors without a future shared identity contract.

## Patch Envelope and Payload

The local implementation should use the anticipated shared envelope shape while keeping SALSA's payload private:

```text
PatchEnvelope
├── owner and tool schema version
├── dataset identity
├── affected asset locators
├── expected source revisions or semantic values
├── dependency asset locators
└── SalsaScriptPatch payload or payload reference
```

`SalsaScriptPatch` describes the current semantic delta from the parsed source baseline. It may contain additions, removals, moves, value changes, reference changes, strings, and SALSA annotations using stable editor IDs. It is deterministic, versioned, auditable, and independently migratable.

The patch is not an append-only command history. Checkpointing may regenerate or compact it from the baseline and current document. Reverting an edit to the baseline removes the corresponding delta. SALSA maintains one undo history alongside each open `SctDocument`; the format document itself does not own history. A bounded recent portion may be retained in a separate optional SALSA-private artifact so it can be reconstructed across sessions; that artifact is not the current patch and is not required to open the checkpointed document.

## Deferred Local Storage

The SALSA-private workspace is implemented for patch checkpointing and retains exact
content-addressed patch baselines. It uses project.json, patches, and baselines outside
the extracted dataset. History and
publication directories in the illustrative shape below remain future extensions.

A simple initial workspace may use this shape:

```text
SalsaWorkspace/
├── project.json
├── patches/
│   └── <asset-key>.salsa-patch.json
├── baselines/
│   └── <source-revision>.sct-source
├── history/
│   └── <asset-key>.salsa-history.json
├── receipts/
└── session.json
```

- `project.json` identifies the private schema version, dataset, and local configuration.
- `patches/` stores deterministic semantic patch documents rather than copied mutable SCT files.
- `baselines/` stores exact immutable source bytes required to verify and later rebase active patches.
- `history/` may store bounded, versioned per-document revision lineage for cross-session undo. It is independently discardable and does not replace the current patch.
- `receipts/` stores the results of successful or attempted publication as required by policy.
- `session.json` stores resumable UI and workspace state separately from semantic patches.

The exact filenames and encoding remain implementation-planning decisions. The durable rule is that the schema declares itself SALSA-local and versioned. It should not be named or documented as the shared `GameProject v1` format. Introducing this layout requires a separate design and implementation decision; the current scanner must not create it implicitly.

## Materialization and Checkpointing

Opening an asset follows this flow:

1. Resolve the dataset and `AssetLocator` through `AssetCatalog`.
2. Load immutable source bytes and revision evidence.
3. Parse through `SpiceSCT` and construct the canonical editable baseline.
4. Load the current SALSA patch through `PatchStore`.
5. Validate source expectations and apply the patch semantically.
6. Load any compatible optional history artifact for this asset and reconstruct a fresh per-document linear history whose current revision matches the materialized working state; otherwise start with that state as the history's initial revision.

After an atomic edit transaction, SALSA may derive and checkpoint the current semantic patch. A checkpoint failure must not corrupt the in-memory revision or overwrite the last valid patch. Dirty state distinguishes the current in-memory revision, the last checkpointed patch, and the last published source/output receipt.

## Publication Boundary

The initial `PublicationService` can resolve selected SALSA patches or working revisions against the local dataset, materialize any selected SALSA authoring and localization overlays, apply SALSA policy checks, run SpiceSCT target validation and serialization, and atomically write the returned bytes to a user-selected destination. Source game files must not be overwritten implicitly.

Ordinary publication does not reparse its proposed output as an additional gate. SpiceSCT's exporter performs the required structural and target validation while building the output. Parse/export/reparse semantic comparison remains regression-test evidence or a separate developer diagnostic rather than a required publication step.

A future shared implementation can compose compatible patches from several tools, repeat source-resolution and conflict checks, validate all selected outputs, publish atomically across assets, and create a suite-level receipt. SALSA's SCT serializer and script patch semantics remain unchanged; only orchestration behind the publication boundary changes.

An editor transaction and a publication transaction are deliberately separate. Undo reverses document revisions inside SALSA. It does not attempt to undo previously written external files.

## Migration to a Shared GameProject

The eventual transition should require these bounded changes:

- Add a dependency on the shared GameProject contract or repository.
- Map shared dataset and asset identity types to the capability boundary used by SALSA.
- Implement `SharedGameProjectAdapter` for asset discovery, patch storage, dependencies, publication, and receipts.
- Import or reference existing SALSA-local manifests, patches, and receipts.
- Retire `LocalGameProject` only after equivalent behavior and migration have been verified.

The transition should not require changes to:

- `SpiceSCT` parsing or serialization.
- The canonical `SctDocument`.
- SALSA edit commands or transaction semantics.
- `SalsaScriptPatch` contents and migration rules.
- Undo/redo history behavior.
- Qt feature-widget contracts.
- Dolphin runtime integration.

If any of those layers depends directly on the local workspace directory or manifest shape, the shim boundary has leaked and should be corrected before extraction.

## Non-Goals

- Designing the complete suite-wide GameProject schema inside SALSA.
- Establishing one undo stack across separate applications.
- Moving SALSA instruction-edit policy into `SpiceContentGraph` or the future shared coordinator.
- Treating ContentGraph snapshot IDs as durable editor identities.
- Persisting mutable source-file copies as the authoritative workspace state.
- Requiring optional per-document history artifacts to open or recover the authoritative checkpointed patch state.
- Requiring cross-tool publication before SALSA can open, edit, checkpoint, and export scripts.
