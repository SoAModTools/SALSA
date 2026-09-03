# Legacy Project Migration and Workspace Reconciliation

Status: **Living implementation strategy; provisional and intentionally revisable.** This document records the current direction for importing archived Python SALSA `.prj` projects into either a newly generated SCT source dataset or an existing SCT source dataset. It should be revised whenever implementation evidence, representative projects, reconciliation behavior, or a better design changes the preferred approach.

The plan guides development; it is not a contract that prevents an alternative implementation from replacing an earlier decision. Preserve the intended safety and user outcomes, record meaningful changes in direction, and prefer current code and evidence over stale plan text. The phases express the present dependency strategy rather than an immutable sequence.

All phases must be completed before the legacy-migration feature itself is called complete. Completion then triggers an assessment of the application as a whole; it neither requires nor implies an immediate public release. Conversely, this plan is not a mechanical public-release gate for SALSA. Product-release scope and timing are separate decisions based on the state of the complete application.

## Purpose

Legacy SALSA serialized an entire mutable project object graph into one Python pickle. The project may contain decoded script state, raw source fragments, derived caches, stable legacy instruction UUIDs, presentation grouping, variable aliases, opcode colors, suppressed instructions, and other authoring state. It does not provide the new SALSA source-baseline, semantic-patch, workspace, and publication boundaries directly. The new importer supports only final legacy project schema version 7; users with older projects must open and save a new copy in the final legacy SALSA before importing it.

Migration must preserve every recognized input or account for it explicitly. It must never imply that a successfully generated SCT dataset contains legacy authoring metadata that was actually discarded. Unsupported but safely decoded legacy information should remain available for later promotion when the corresponding SALSA feature is implemented.

The migration goal is not to make the new application depend on the archived Python object graph. Migration is a one-way conversion into current SCT documents, SALSA-owned workspace state, and an immutable versioned migration capsule. The original `.prj` remains untouched.

## Target Outcomes

SALSA should support two entry points:

1. **Import into a new source dataset:** Convert every accepted legacy script into a validated SCT file in a new destination dataset, create an associated SALSA workspace, retain unsupported authoring metadata in a migration capsule, and open the resulting workspace.
2. **Import against an existing source dataset:** Pair legacy scripts with existing SCT assets, compare their semantic representations, present a reviewable changelist, and store accepted changes as SALSA semantic patches without overwriting matched source files.

Both modes use the same safe legacy decoder, normalized migration package, canonical document conversion, reconciliation, changelist, validation, and transaction services. They differ only in how the canonical source baseline is established and how accepted semantic state is persisted.

## Current Direction

The following points reflect the present design direction. They remain subject to revision rather than becoming permanently locked constraints:

- `SpiceSCT` remains the authority for SCT parsing, canonical format-valid document types, schema factories, validation, layout, relocation, preservation, serialization, compression, and format-level semantic equivalence.
- `SalsaCore` owns source baselines, authoring identity, semantic patches, document reconciliation, changelists, diffing, rebasing, merging, conflict resolution, legacy migration, workspace transactions, and SALSA-only metadata.
- The importer guarantees only version-7 projects created or resaved by the official final legacy SALSA and validates their complete expected shape. It does not contain or invoke the legacy version 1 through 6 upgrade chain and does not promise compatibility with forks or development builds.
- The importer reproduces only documented final-version load normalization, reports every normalization, and does not perform broader repair while decoding the project.
- A user may explicitly exclude a malformed or unconvertible script and atomically import the accepted remainder. The report retains every excluded script and reason.
- Recognized but inconsistent metadata may be explicitly discarded by the user after a precise warning; it is never discarded automatically.
- The shared game-project boundary may report source-revision and affected-asset conflicts, but SALSA patch bodies and SCT conflict semantics remain opaque to it.
- A migration capsule is immutable archival input and pending authoring data. It is not a second live SCT representation and is not consulted as document truth during ordinary editing.
- Supported metadata is promoted from the capsule into normal versioned SALSA workspace state. The capsule remains unchanged so conversion is auditable and newly supported metadata can be reconsidered safely.
- Existing matched source SCT files remain read-only during import. Replacing them remains a separate explicit publication operation.
- All import modes begin with a non-mutating preflight and produce no durable changes until the complete accepted transaction has validated.
- Ambiguous identity, unsupported semantic content, or unproven preservation is surfaced explicitly and is never resolved by silently choosing the nearest name, ordinal, or byte offset.

## State and Identity Model

Migration must distinguish these states:

- **Physical source baseline:** The immutable SCT bytes and parsed `SctDocument` identified by a dataset-relative `AssetLocator` and exact `SourceRevision`.
- **Effective workspace state:** The physical baseline plus the current durable SALSA patch and supported SALSA authoring metadata, or the live captured revision when the document is open.
- **Incoming legacy state:** The normalized semantic and authoring content decoded from one legacy project script.
- **Migration capsule:** The immutable normalized legacy package, source `.prj` provenance, conversion diagnostics, and legacy-to-current identity evidence.
- **Migration plan:** A transient immutable proposal containing asset mappings, entity mappings, semantic changes, metadata disposition, conflicts, warnings, and exact expected source and workspace revisions.

Three identity domains remain separate:

- Dataset and asset identity locates an SCT across the workspace.
- `SpiceSCT` entity IDs identify sections, instructions, strings, footer entries, and opaque attachments within a materialized source lineage.
- Legacy identities identify objects in the migration capsule and map to current asset/entity identities only through an explicit reconciliation result.

Legacy script names, section names, UUIDs, original offsets, and list positions are evidence. They are not automatically current `AssetLocator` or `SpiceSCT` entity identity.

## Target Scope and Export Projection

Filename-stem uniqueness belongs to a concrete game/export target, not necessarily to every physical asset visible to a SALSA workspace. Two assets such as `Disc1/A001.SCT` and `Disc2/A001.SCT` retain distinct path-based `AssetLocator` identities while sharing the logical script stem `A001` in different target scopes. Other asset types may intentionally share that stem to express a game association. For any one export target, however, the projected output must contain at most one SCT for a case-insensitively normalized stem.

Legacy version-7 projects retain script names but not dependable disc-relative paths or target provenance. New-dataset import therefore assigns the imported script set to one explicit target scope selected from known targets such as Dreamcast Disc 1, Dreamcast Disc 2, and GameCube, or an honest unknown/custom scope, and records that scope with mappings and baselines. SALSA may suggest a scope from evidence but requires confirmation. The scope is durable authoring metadata that may later be corrected without rewriting unchanged SCT files. The initial importer creates one concrete target scope per destination dataset directory rather than attempting to reconstruct variants that the legacy project never retained.

A future authoring model may let one workspace contain multiple physical candidates or SALSA-authored variants for the same logical stem, with applicability tags on files and sections. Export would project those candidates for a selected target and require the user to resolve any duplicate applicable script stem. Section-level projection must include the reference, string, footer, and other dependency closure required to produce a valid `SctDocument`. Tags and selection remain SALSA authoring metadata and are not inserted into the SCT format. This future tagging and sub-file projection system is not a prerequisite for legacy import; the target-scope boundary is established now so it can be added without redefining imported asset identity.

## Workspace Persistence Prerequisite

Migration should be built on a fuller SALSA-private workspace rather than adding special migration state to `QSettings` or individual patch files. The current schema-one workspace contains only `project.json` and `patches/`; migration needs a versioned component layout with independently replaceable and recoverable artifacts.

The source dataset and SALSA workspace remain separately selected locations, and more than one workspace may be associated with the same dataset. This permits independent mods, experiments, and migrations without duplicating the extracted source files. New-dataset migration may suggest sibling `dataset/` and `workspace/` locations but does not require that arrangement.

A provisional expanded layout is:

```text
SalsaWorkspace/
|-- project.json
|-- patches/
|   `-- <asset-key>.salsa-patch.json
|-- baselines/
|   `-- <asset-key>/<source-revision>.sct
|-- authoring/
|   `-- <asset-key-or-project-key>.json
|-- imports/
|   `-- <migration-id>/
|       |-- manifest.json
|       |-- legacy-model.json
|       |-- blobs/
|       |-- identity-map.json
|       `-- report.json
|-- import-state/
|   `-- <migration-id>.json
|-- history/
|-- receipts/
|-- transactions/
`-- session.json
```

The exact filenames and legacy-model encoding remain provisional. Each independently interpreted artifact needs its own format identifier and schema version so that optional session or history data can be discarded without invalidating durable patches, authoring metadata, or migration evidence.

`project.json` should identify the SALSA-private workspace schema, a stable workspace ID, the associated dataset identity, and component locations. Dataset binding should prefer a portable relative path when the dataset and workspace are arranged together and retain a last-known absolute location as convenience. SALSA automatically accepts a stored relative location only when it resolves and the dataset fingerprint matches exactly. A mismatch or search outside that expected relative location requires explicit user selection and fingerprint verification.

An existing schema-one workspace is upgraded automatically only after a successful preflight. The schema-two manifest and required components are installed atomically, and the original manifest remains available as rollback evidence until the upgraded workspace opens successfully. A workspace created by an unsupported newer schema is never downgraded; it is rejected or exposed read-only where that can be done without misinterpreting its contents.

## Baseline Retention for Rebase

A source hash alone is insufficient for a robust future rebase after the physical source file changes. SALSA must be able to reconstruct the exact old semantic baseline against which a patch was authored.

Before exposing rebase, the workspace should retain an immutable baseline artifact for every active patch. The preferred initial artifact is the exact original source byte sequence keyed by `AssetLocator` and `SourceRevision`; reparsing it through the pinned or compatible `SpiceSCT` contract reconstructs format evidence as well as semantic state. A canonical semantic snapshot may supplement this artifact but should not replace source bytes unless it is proven to preserve every fact required by reconciliation and opaque-data policy.

Because the C++ SALSA has not yet shipped to users, the baseline-bearing patch contract may make a clean pre-release break. Existing development patch artifacts do not require a recovery or compatibility path and may be discarded and regenerated. Every patch created under the revised contract must capture or reuse its verified immutable baseline at creation time; SALSA must not create a patch first and attempt to recover its baseline later.

Baseline artifacts are rebase evidence, not mutable copies of the dataset and not publication outputs. They may be garbage-collected only when no patch, migration transaction, history artifact, or receipt refers to them.

The patch schema should also gain typed expected-before values for changed semantic sites where those values improve conflict precision. Expected-before values do not eliminate the need for the old baseline when entities move, split, disappear, or require cross-lineage matching.

## Fuller Workspace Restoration

Workspace restoration should be implemented before the legacy importer so an imported project opens as a durable working environment rather than a one-time collection of files.

`session.json` should be machine-tolerant, versioned, atomically replaced, and separate from semantic state. It is updated through debounced atomic replacement after meaningful session changes and receives a final update during orderly shutdown. It records only restorable checkpoint-based workspace state, not uncheckpointed semantic edits. It may record:

- Open asset locators and active document.
- Per-document active view and typed inspection selection.
- Document-local Back and Forward navigation entries that still resolve.
- Selected project-explorer asset and expanded logical paths where useful.
- Pending migration records that have newly supported promotion handlers.

The initial restoration boundary excludes open dialogs, partially edited parameter cells, unapplied Advanced SCPT drafts, transient search sessions, in-progress background operations, uncheckpointed semantic edits, and cross-session Undo/Redo history. Crash recovery and bounded history remain separate future features and do not block the importer.

Window geometry, dock arrangement, theme, recent datasets, and other machine-wide preferences remain in `QSettings`. Workspace state identifies content and navigation, not how a particular machine arranges the application. A corrupt or incompatible session must never prevent the dataset, patches, or authoring metadata from opening.

Restoration proceeds in this order:

1. Resolve and verify the dataset.
2. Open and validate the workspace manifest.
3. Load durable patches and supported authoring metadata.
4. Inspect migration capsules for pending records supported by the current application.
5. Restore open documents and typed selections.
6. Report missing, stale, ambiguous, or discarded optional state without guessing a replacement target.

Opening a dataset automatically restores its last successfully associated workspace when that workspace remains available and compatible. Restoration reports visible nonmodal status and provides an easy way to close or switch workspaces. A missing or invalid associated workspace leaves the source dataset open rather than blocking it.

## Migration Capsule

The main application must never directly unpickle a `.prj`. A self-contained legacy conversion helper bundled with SALSA should load trusted projects using the archived official final-version class definitions and an allowlisted unpickler, require project schema version 7, validate the complete expected version-7 shape and invariants, apply only documented final-version load normalization, and emit an implementation-neutral package without requiring a user-installed Python environment. Every normalization is recorded. The helper must reject missing, older, newer, malformed, development-only, fork-specific, or structurally unexpected schemas instead of attempting an upgrade, broader repair, or guessed defaults. It runs in a separate process with no network access and access limited to a controlled staged input copy and output directory.

Default resource limits are fixed and documented. An advanced per-import control may disable those resource-limit safeguards after an explicit warning, for unusually large trusted projects. This control does not disable process isolation, the no-network boundary, schema-version enforcement, structural validation, or `SalsaCore` package validation. These measures reduce risk but do not make arbitrary third-party pickle files safe; the UI must present a trusted-input confirmation for the selected `.prj` before every conversion.

Development converges on one finalized converter contract and one finalized capsule schema before the importer is offered to users. Development artifacts may be discarded and regenerated as that contract changes. The user-facing importer supports that single pinned version-7 conversion contract rather than carrying a compatibility matrix for intermediate converter or capsule versions.

An older project rejection should identify the detected version when possible and instruct the user to open the project in the final legacy SALSA, save it as a new version-7 `.prj`, and import that updated copy. The final legacy application and concise update instructions therefore remain required migration support artifacts for the new SALSA release.

The normalized capsule should preserve:

- Required project schema version 7, the finalized converter and capsule contract identifiers, original file size, original filename, and SHA-256.
- Project, script, section, instruction, parameter, expression, string, footer, and link records.
- Complete semantic content for instructions marked not to encode.
- Legacy UUIDs and every identity relationship required to interpret links and grouping.
- Project and script variable aliases, opcode colors, section and instruction groups, folded presentation state where retained, and other known authoring metadata.
- Raw headers, parameter bytes, garbage spans, string garbage, and other binary fragments with their legacy provenance and confidence.
- Derived fields and caches classified as recomputable, advisory, obsolete, or unresolved rather than copied into current truth.
- Unknown safely decoded fields as typed archival records when their values can be represented without executable Python objects.
- Every normalization, omission, failure, and ambiguity encountered by the converter.

The exact original `.prj` is not copied into the workspace by default but may optionally be retained in the capsule as inert evidence. Its hash and filename are always recorded. The absolute source path and other machine-specific run details remain in a local receipt rather than the portable capsule. Routine workspace open and metadata promotion never execute or deserialize the retained pickle.

Deterministic normalized records remain separate from run-specific receipt data. The helper inventories and attempts every script before selection. Unexpected project-level structure rejects the whole project; a script-local conversion failure produces a typed failed-script inventory entry that the user may exclude. Cancellation or helper failure produces no accepted capsule, while a successfully atomically finalized capsule may contain complete diagnostics for failed scripts.

Unknown values are retained only when they can be represented as inert typed data without executable Python objects. `SalsaCore` independently validates every capsule before it becomes workspace state. Updating an older project remains a separate manual workflow in the final legacy SALSA; the C++ application neither launches nor automates that application.

## Metadata Promotion

Known unsupported concepts should use explicit versioned capsule record types such as `VariableAlias`, `OpcodeColor`, `PresentationGroup`, and `SuppressedInstruction` rather than an unstructured miscellaneous-property bag.

Metadata already supported by the importing SALSA version is applied during the original import. When a later SALSA version implements a previously pending concept, opening the workspace may display a nonmodal availability notice, but promotion begins only through an explicit user preview. The changelist groups eligible records by owning feature, asset, entity, and dependency group. A selected promotion set is atomic, repeatable without duplication, and bound to compatible asset/entity mappings. Stale or ambiguous mappings and conflicting current authoring metadata block only affected groups.

The immutable capsule is never modified. A separate import-state ledger records the capsule hash, record IDs, disposition, resulting workspace component version, application version, and decision history. Multiple capsules use capsule hash plus record identity for idempotency; semantically competing records are conflicts rather than automatic duplicates. Each SALSA feature owns its typed promotion adapter, with no generic metadata-property bag.

Possible dispositions are:

- `Applied`: represented in current canonical or authoring state.
- `Pending`: safely retained but the current application has no owning feature.
- `Blocked`: the owning feature exists but identity, source, or semantic conflict prevents application.
- `Unsupported`: recognized but no safe current or planned representation exists.
- `DroppedByUser`: a recognized but inconsistent or unsupported metadata record explicitly excluded after a precise loss warning.
- `Invalid`: malformed or internally inconsistent legacy data.

Pending records remain visible through nonmodal workspace migration status without repeated prompts. Because the immutable capsule retains the record, a user may explicitly reconsider a prior `DroppedByUser` disposition; the audit history retains both the original discard and the later decision.

Suppressed instructions remain `Pending` until SALSA has an explicit disabled-entity or reusable-fragment model. They must not be inserted into a generated SCT because doing so could change runtime behavior, and they must not be omitted without appearing in the migration report.

## Semantic Change Model

The existing `SalsaScriptPatchService::diff` is a same-lineage canonical baseline-to-working differ. It should remain the durable patch generator after imported entities have been reconciled to baseline IDs. A separate frontend-neutral changelist model should describe proposed authoring meaning rather than serialized patch details.

A changelist item should identify:

- Owning asset and source/working/incoming revision keys.
- Change kind: added, removed, moved, renamed, modified, unchanged, metadata-only, or conflict.
- Typed current and incoming entity locations.
- Before and after semantic presentations suitable for review.
- Identity-match status: `Exact`, `Strong`, `Ambiguous`, `Unmatched`, or `Contradictory`, plus provenance distinguishing an automatic result from a user-confirmed decision.
- Preservation, validation, and incomplete-evidence diagnostics.
- Whether the item is selectable independently or belongs to an atomic dependency group.

Textual and tree views are projections over this model. The model must remain usable by tests and future non-Qt workflows.

## Cross-Lineage Document Reconciliation

An imported legacy document and an independently parsed source document do not share entity IDs. `SalsaCore` therefore needs a general-purpose `SctDocumentReconciler` shared by existing-source import, patch rebase, and future cross-lineage comparison workflows.

Reconciliation should proceed conservatively:

1. Pair scripts with assets by an explicit mapping proposal based on normalized filename/stem, legacy name, semantic evidence, and user-supplied mapping. An exact name is strong evidence but never overrides contradictory semantics.
2. Match sections by physical kind, exact name bytes, occurrence, order, and structural signature.
3. Align instructions within matched script sections using sequence alignment over opcode, modifiers, scheduled-expression shape, parameter schema shape, and non-reference semantic values.
4. Treat legacy and imported source offsets as supporting evidence only.
5. Resolve instruction, indexed-string, and footer references after a preliminary entity map exists, then refine matches using reference-graph consistency.
6. Match text entities using physical storage, owning section or reference relationships, order, kind, and semantic content.
7. Preserve baseline IDs for matched entities and allocate fresh IDs only for confirmed additions.
8. Validate the reconciled candidate, all references, and every identity invariant before producing a patch or changelist.

Unique `Exact` and `Strong` matches may be used automatically, including structurally supported renames and moves. Repeated identical instructions, duplicate text, renamed sections without sufficient anchors, large moves, missing anchors, opaque attachments, and divergent reference graphs can make a unique mapping impossible. `Ambiguous`, `Unmatched`, and `Contradictory` results require an explicit decision; they do not fall back to matching by ordinal. Users may explicitly identify a genuinely new incoming entity or an intentionally removed current entity rather than pretending it has a correspondence.

Reconciliation continues across unaffected assets and dependency groups when another portion is blocked, producing useful partial results without permitting a blocked group to apply. Reconciliation results should be deterministic for the same inputs and mapping decisions. They should retain evidence explaining why each identity match was accepted so the UI and diagnostic report can distinguish exact, inferred, and user-confirmed correspondence.

Confirmed script- and entity-level decisions are persisted with the migration capsule identity, relevant source asset revision, and reconciliation-contract version. A source change invalidates and reevaluates only affected decisions. The domain supports both levels, while a future UI emphasizes script mapping and exposes entity mapping only when a blocking ambiguity requires it.

Semantic equivalence produces an empty patch even if physical serialization differs. Offsets, layout, and byte-level churn remain diagnostic unless `SpiceSCT` determines that they affect validity or preservation. Opaque or uninterpreted content requires exact preservation or an authoritative `SpiceSCT` equivalence result; insufficient evidence is a blocking conflict for the affected dependency group.

## Patch Rebase

Patch rebase is a SALSA operation over three semantic states:

- `O`: the exact old source baseline expected by the patch.
- `L`: the local authored state obtained by applying the patch to `O`.
- `N`: the newly discovered physical source baseline.

SALSA reconstructs `O` from retained baseline evidence, verifies that the durable patch produces `L`, reconciles `O` with `N`, and attempts to transplant the authored `O -> L` intent onto `N`. Successful transplantation produces a candidate `R` and a new deterministic `N -> R` patch. The old patch and baseline remain intact until the replacement transaction commits.

A changed source marks the affected patch stale but never silently rewrites it. Rebase starts explicitly, requires uncheckpointed document edits to be checkpointed or discarded, and presents a semantic preview even when no conflicts are found. Any relevant source or workspace revision change while the preview is open invalidates the proposal and requires a regenerated changelist.

At minimum, rebase must detect:

- Both sides changing the same parameter, expression, text value, or section name differently.
- Local modification of an entity deleted or physically retyped by the new source.
- Local deletion of an entity changed by the new source.
- Incompatible moves or missing insertion anchors.
- Reference targets that disappear, split, or become ambiguous.
- Footer sharing or text-storage changes that invalidate the previous ownership assumption.
- Opaque source changes that cannot satisfy strict preservation.
- SALSA authoring metadata whose mapped entity no longer exists.

Identical semantic outcomes reached independently combine automatically. Other conflict resolution produces explicit SALSA commands or mapping decisions such as retaining local intent, accepting the new source, manually editing the result, remapping an entity, or explicitly dropping a local change where that choice is valid. Independently safe dependency groups may be resolved separately, but an affected asset commits only as one fully materialized and validated result. `SpiceSCT` validates the resolved candidate but does not classify or resolve authoring conflicts.

Each successfully committed asset rebase appears as one Undo operation in its active document session. Resolution decisions are retained in the transaction receipt and may be reused automatically only for identical relevant revisions. The prior patch and baseline remain recoverable until the replacement workspace state reopens successfully, after which unreferenced artifacts become eligible for reference-based cleanup.

## Merge Model

Existing-source legacy import may involve three current states:

- `A`: the selected physical source baseline.
- `B`: the effective current SALSA workspace state for that asset.
- `C`: the incoming legacy project state.

When `B` equals `A`, import is a two-way `A -> C` proposal after cross-lineage reconciliation.

When `B` differs from `A`, SALSA may compare the existing `A -> B` patch with the proposed `A -> C` legacy changes and produce a merge candidate. However, a legacy `.prj` ordinarily does not prove that `A` was its historical base. Unless provenance establishes that relationship, the operation is described as an integration or comparison merge against a user-confirmed baseline, never as a true three-way merge. Low-confidence or contradictory lineage blocks automatic merge and may require importing into a separate new dataset/workspace.

Before integration, uncheckpointed edits must be checkpointed or discarded and any stale existing patch must be rebased onto the current physical source. The user explicitly confirms the physical source as the comparison baseline. For each dependency group, SALSA retains a change made only by the existing workspace, proposes a change made only by the legacy state, combines identical outcomes, and requires semantic resolution when both sides changed differently. Unrelated existing SALSA edits remain intact.

The result for each accepted asset is one consolidated deterministic patch from the physical baseline to the integrated state, not a stack of workspace and import patches. Safe groups may be selected independently, but each asset materializes and validates atomically; selected resolved assets commit together as one recoverable transaction while blocked or excluded assets retain their original state. The previous patch and integration evidence remain recoverable through successful reopen, and each integrated asset appears as one Undo operation in its active document session. Confirmed target scope and stem mappings remain binding throughout the operation.

Phase 7 requires affected assets to have no current patch or uncheckpointed live edits. Phase 8A handles integration only after patch rebase, typed conflict resolution, multi-document transaction recovery, and representative fixtures are complete.

## Import Into a New Source Dataset

The new-dataset workflow is:

1. Select a trusted final-version legacy `.prj` and run the safe decoder and strict version-7 validator.
2. Review project inventory, invalid records, pending metadata, and any script that cannot be converted.
3. Select a new or completely empty destination dataset and a separately selected new or compatible SALSA workspace; suggest sibling locations without requiring them.
4. Confirm one explicit known or unknown/custom target scope for the imported set.
5. Treat each legacy project dictionary key as the authoritative proposed stem and require it to agree with the stored script name. Preserve its spelling and capitalization while enforcing case-insensitive uniqueness within the selected target.
6. Map each accepted stem to `<stem>.sct` directly in the selected source directory. For an invalid or colliding stem, let the user exclude the script or explicitly remap it after a game-association warning, and retain the original-to-new mapping. Never rename or deduplicate automatically.
7. Select explicit publication settings for platform, text encoding, message-space convention, byte order, and wrapper. Evidence may suggest values but ambiguous legacy state requires user confirmation. Project-wide defaults may be overridden per script.
8. Convert each accepted legacy script into a canonical `SctDocument`, preserving complete representable semantic state and recording unsupported authoring state in the capsule. Semantically identical scripts with different valid stems remain distinct.
9. Analyze dependencies introduced by exclusions, then validate and canonically export every accepted candidate into staging through `SpiceSCT`; retained legacy byte evidence never bypasses current publication.
10. Reparse every generated SCT and semantically compare it with the intended migrated document as a migration acceptance gate.
11. Establish the reparsed documents as the immutable physical source baselines with empty initial patches and finalize legacy-to-current entity mappings and target scope against those final IDs.
12. Create the workspace manifest, supported authoring artifacts, migration capsule, import-state ledger, report, and initial session. Record every exclusion, explicit remapping, pending record, and user-directed discard.
13. Recheck the inputs and both destination locations immediately before committing the selected set as one transaction. Any selected-script failure or changed/nonempty destination aborts without overwrite.
14. Open the completed workspace and show an import summary that distinguishes imported, excluded, pending, discarded, and remapped content and states that file creation or target assignment does not prove game reachability.

The migrated current script state becomes the new source baseline because the `.prj` is a snapshot and does not retain a trustworthy pre-edit source baseline. SALSA must not manufacture a large patch against an unknown historical source.

## Import Against an Existing Source Dataset

The existing-dataset workflow is:

1. Select or use the active source dataset, associate a compatible SALSA workspace, and confirm the concrete target scope, particularly when it is unknown/custom.
2. Select a trusted legacy `.prj` and decode it without changing the dataset or workspace.
3. Assign the incoming project to that target scope. Pair an incoming script to the unique SCT with the same case-insensitive stem in that scope; semantic differences are candidate edits rather than grounds to reject that asset identity.
4. Propose structurally strong cross-stem mappings but require explicit confirmation because they change the game-facing association. Never search other target scopes implicitly.
5. Detect a prior import of the same capsule into the same target and offer to view or resume its migration state rather than duplicating metadata.
6. Capture exact physical source and workspace revisions. In the Phase 7 two-way workflow, require every affected asset to have no current patch or uncheckpointed edit; defer other assets to Phase 8.
7. Convert each incoming legacy script into a canonical candidate and reconcile it with the selected source baseline. Include `Exact` and unique `Strong` entity matches by default while blocking ambiguous or contradictory dependency groups.
8. Produce a semantic changelist covering document changes, metadata disposition, identity confidence, validation, preservation, and conflicts. Collapse unchanged scripts by default and highlight every cross-stem mapping.
9. Let the user include or exclude whole scripts and independently safe dependency groups while preserving required dependencies. Incoming scripts with no source counterpart remain excluded in this phase, and source scripts absent from the legacy project remain untouched.
10. Materialize every accepted result, validate it through `SpiceSCT`, generate deterministic patches against the physical source baselines, serialize and reapply those patches, and verify exact reproduction. A semantically unchanged script creates no patch.
11. Stage the selected clean set's patch, baseline-evidence, authoring, capsule, import-state, report, and session changes as one recoverable workspace transaction. Retain blocked and excluded scripts in the report.
12. Recheck target scope, source revisions, workspace revisions, and migration-plan identity immediately before commit. If any input advanced, discard the staged transaction and refresh the changelist.
13. Commit without changing any matched physical SCT bytes, then open or refresh the affected documents.

Incoming scripts with no matching source asset appear as `New source asset`, not as patches against an invented empty document. Phase 7 requires users to exclude them or choose the new-dataset workflow. A later explicit add-asset operation may create new SCT files after target selection and validation, but it must never overwrite another asset or imply that adding a file makes it reachable by the game.

Source assets absent from the legacy project remain untouched by default. Removing a source file is not an implicit consequence of importing a project that lacks it.

## Workspace Transactions

New-dataset migration can stage the complete destination tree and publish it by a same-volume directory move when the destination does not yet exist. Existing-workspace import changes several independent artifacts and therefore needs a recoverable transaction protocol rather than a sequence of unrelated atomic file replacements.

A transaction record should contain the migration-plan hash, expected dataset fingerprint, expected source revisions, expected prior workspace component revisions, staged artifact hashes, intended replacements, and commit state. Recovery after interruption must be able to determine whether no changes, all changes, or a recognized partial commit occurred and either finish or restore the previous coherent state without consulting the legacy pickle again.

The transaction boundary applies to durable workspace artifacts. Open document histories remain independent; affected live sessions should either be closed before commit or replaced through an explicit coordinated operation after the durable transaction succeeds.

## User-Facing Changelist

The initial changelist should project the hierarchical domain model by script, semantic category, dependency group, and individual change. Unchanged scripts are collapsed by default. Each row should show a concise semantic before/after value, asset and entity breadcrumb, match status, and conflict or incomplete-evidence badge; cross-stem asset mappings receive a prominent association warning. Activating a row should navigate to a side-by-side read-only source/incoming inspection surface where both targets are addressable.

The summary should report:

- Scripts mapped, unchanged, changed, unmatched, excluded, and blocked.
- Section, instruction, parameter, expression, text, footer, and metadata change counts.
- Exact, inferred, manually confirmed, ambiguous, and unmatched identity counts.
- Pending legacy metadata by feature category.
- Required target-profile decisions and preservation restrictions.
- Existing patches or live edits that require merge or block the initial mode.
- Whether accepting the current selection can commit atomically.

The changelist is a transient proposal keyed to exact source and workspace revisions. It becomes stale rather than silently refreshing underneath user selections.

## Implementation Sequence

Phase 0 is a dependency only for phases that decode or import legacy projects. It does not block Phase 1 workspace work, Phase 2 diff and changelist work, Phase 3 cross-lineage reconciliation, or Phase 4 patch rebase and merge. Those prerequisite phases may proceed before the private real version-7 fixture is available to the implementation environment. Phase 0 must satisfy its exit criterion before Phase 5 begins decoding supported user projects and before either user-facing import mode can be accepted.

### Phase 0: Version-7 Contract and Characterization

- Keep the available real official version-7 `.prj` as a private, external organic compatibility fixture and record its original hash and expected project/entity inventory without committing its contents to the repository.
- Define the accepted official final legacy version-7 object shape and post-load invariants in the executable validator and automated tests using the archived classes, final-version loader behavior, encoder, and private real fixture. Do not create a separate field-level planning specification.
- Reproduce only documented final-version load normalization and require the validator to report each normalization; broader repair remains outside decoding.
- Construct focused synthetic version-7 fixtures with the archived classes for empty and multi-script projects, aliases, colors, grouping, folded sections, suppressed instructions, linked and shared strings, opaque parameters, garbage preservation, encoding errors, unusual section names, duplicate content, broken references, and large-project cancellation.
- Add rejected fixtures for missing version fields, versions 1 through 6, newer versions, incomplete, fork-specific, or development-only version-7 shapes, unexpected globals, malformed pickle data, and resource-limit behavior without treating the converter as a general untrusted-pickle sandbox.
- Cover project-level partial import by allowing users to exclude malformed or unconvertible scripts while retaining their inventory and diagnostics in the report.
- Cover explicit user discard of recognized inconsistent metadata without allowing silent discard or automatic repair.
- Specify the exact rejection message and final-legacy-SALSA update workflow for an older project.

Exit criterion: the private real project and focused synthetic fixtures define the supported official version-7 contract in executable validation and tests, every accepted field has an expected disposition, all non-version-7 or unsupported shapes fail closed with actionable diagnostics, and implementation is not blocked on obtaining another real project.

### Phase 1: Workspace Schema and Restoration

- Introduce the expanded versioned workspace manifest with schema-one upgrade support.
- Keep workspaces separately selected from datasets and permit multiple independent workspaces for one dataset.
- Add portable dataset rebinding with automatic acceptance only for an exact fingerprint match at the stored relative location; other rebinding requires explicit selection.
- Add component stores for authoring metadata, imports, import state, baselines, transactions, session, optional history, and receipts.
- Implement debounced atomic `session.json` persistence and tolerant restoration of checkpoint-based open documents, active document, typed selections, active document views, and navigation.
- Automatically restore the last successfully associated compatible workspace while leaving the dataset open after a missing or invalid workspace.
- Keep window geometry, dock arrangement, theme, and other machine-wide preferences in `QSettings` rather than the workspace.
- Exclude unsaved semantic edits, transient editor drafts, searches, background operations, crash recovery, and cross-session Undo/Redo from the initial restoration boundary.
- Upgrade schema-one workspaces automatically after preflight, retain rollback evidence until the upgraded workspace reopens successfully, and never downgrade an unsupported future schema.
- Keep optional-state failure isolated from patches and canonical authoring state.

Exit criterion: multiple independent workspaces can bind to one dataset, a workspace can move or reopen with exact fingerprint verification, schema one upgrades recoverably, and the supported checkpoint-based editing context restores without migration-specific shortcuts or cross-session history.

### Phase 2: Robust Same-Lineage Diff and Changelist Foundation

Status: **Implemented core foundation.** See `SctSameLineageChangelist.md` for the current contract and boundaries.

- Treat the changelist as a transient `SalsaCore` domain model shared by import, rebase, merge, and other editor workflows rather than as a GUI display type.
- Audit and extend `SalsaScriptPatchService::diff` coverage for every currently editable semantic entity and SALSA authoring overlay.
- Organize changes by script, semantic category, and stable semantic entity. Treat each instruction as one atomic change across its opcode, expressions, parameters, repeated groups, and local flags.
- Permit individual entity selection and whole-script selection. If a selection is representable but temporarily invalid, require explicit per-change acknowledgement, mark the resulting state repair-required, and keep checkpoint and export safeguards in force until repaired.
- Show semantic changes and preservation diagnostics by default while keeping offsets, layout churn, and other physical evidence in optional diagnostics.
- Present SALSA-only authoring metadata in the same changelist under a distinct category rather than mixing it into binary patch operations.
- Add typed expected-before information and classify outcomes as informational, warning, conflict, or invalid result. Warnings require acknowledgement where data-preservation policy demands it; conflicts block only their affected changes, while invalid or unrepresentable operations block application.
- Capture or reuse the exact verified immutable SCT baseline whenever a patch is created. Make a clean pre-release break rather than migrating development patches that lack baselines.
- Keep changelists revision-derived and transient. Durable inputs and outcomes are baselines, patches, import reports, and any required correspondence decisions.
- Implement the Qt-free model, deterministic diagnostic formatting, and automated tests in this phase; defer the full user-facing changelist interface until import or rebase consumes it.
- Prove that accepting a complete changelist recreates the existing deterministic patch.

Exit criterion: all supported semantic and authoring changes can be represented in a deterministic hierarchical changelist, every new patch has a verified retained baseline, typed outcomes isolate conflicts and identify repair-required states, and applying a complete accepted changelist reproduces the deterministic patch without depending on Qt.

### Phase 3: Cross-Lineage Reconciliation

- Implement reconciliation as a general Qt-free `SalsaCore` capability shared by import, rebase, and future comparison workflows.
- Implement deterministic asset, section, instruction, string, footer, reference, and opaque-attachment correspondence.
- Classify results as `Exact`, `Strong`, `Ambiguous`, `Unmatched`, or `Contradictory`, retaining the evidence and automatic or user-confirmed provenance behind each result.
- Automatically use unique `Exact` and `Strong` matches, including supported renames and moves, but never let names override contradictory semantics or resolve a tie by ordinal.
- Continue reconciling unaffected scripts and dependency groups when other groups are blocked.
- Support explicit script- and entity-level mapping, addition, and removal decisions. Persist them against the capsule, affected source revision, and reconciliation-contract version, invalidating only affected decisions after source changes.
- Require exact preservation or authoritative `SpiceSCT` equivalence for opaque and uninterpreted content; otherwise block the affected dependency group.
- Re-ID an incoming candidate onto a compatible baseline lineage and feed it through the existing patch differ. Semantically equivalent documents produce an empty patch regardless of irrelevant physical serialization differences.
- Stress repeated and moved content, renames, reference cycles, opaque attachments, incomplete text interpretation, partial reconciliation, and stale persisted decisions.
- Provide deterministic reports and automated tests in this phase; defer the interactive mapping interface until the existing-source importer consumes it.

Exit criterion: independently parsed or constructed equivalent documents reconcile to an empty patch, representative edits produce the expected changelist, no ambiguous or contradictory identity is selected implicitly, unaffected groups remain usable, and persisted decisions are reused or invalidated deterministically without depending on Qt.

### Phase 4: Patch Rebase and Merge

- Mark patches stale after source changes and require an explicit rebase; never silently rewrite a patch.
- Require uncheckpointed document edits to be checkpointed or discarded, then implement old-baseline reconstruction, old-to-new reconciliation, authored-intent transplantation, and deterministic rebased-patch generation.
- Always present a semantic preview before commit. Invalidate and regenerate it if any relevant source or workspace revision changes.
- Combine identical independent outcomes automatically and define typed two-sided conflicts with frontend-neutral commands to keep local intent, accept the new source, edit semantically, remap, or explicitly drop a change where valid.
- Permit resolution by independently safe dependency group, but materialize, validate, and commit each affected asset atomically. A multi-asset operation may commit selected clean assets while leaving conflicted assets and their original patches unchanged.
- Bind saved resolution evidence to the exact relevant revisions and make each successful asset rebase one document-session Undo operation.
- Retain the old patch and baseline through successful reopen, then allow reference-based cleanup of artifacts no longer needed by patches, receipts, transactions, or history.
- Implement general two-way and true three-way change-plan composition in `SalsaCore`. Describe legacy integration without proven ancestry as an integration or comparison merge, and defer its legacy-specific orchestration to Phase 8.
- Add a recoverable multi-artifact workspace transaction boundary.
- Add the first reusable GUI for changelist review, rebase preview, and conflict resolution, backed by non-Qt tests for successful, conflicting, cancelled, stale-input, and interrupted rebases and merges.

Exit criterion: a patch can move between representative source revisions through an explicit preview without silent loss, each selected asset commits atomically, unresolved assets retain their original durable state, stale previews cannot commit, and the resulting rebase is recoverable and undoable in the active session.

### Phase 5: Legacy Capsule and Converter

- Finalize one normalized legacy interchange/capsule schema and one converter contract from the characterized corpus before the importer is offered to users; do not retain compatibility for intermediate development artifacts.
- Bundle a self-contained allowlisted out-of-process loader and strict version-7 validator using the archived final-version Python classes, with no user-installed Python dependency.
- Require a per-file trusted-input confirmation, copy the input into controlled staging, and restrict the helper to that input, its output directory, and no network access.
- Apply fixed documented resource limits by default and provide an explicitly warned advanced option to disable resource limits without disabling isolation or validation boundaries.
- Reject every non-version-7 project and direct the user to update and resave it with the final legacy SALSA; do not implement or invoke the historical upgrade chain.
- Separate canonical inputs, authoring metadata, raw evidence, derived caches, pending records, and invalid data.
- Inventory every script before selection. Reject unexpected project-level structures, but represent script-local failures so users may exclude those scripts.
- Generate deterministic normalized records, inventories, hashes, and diagnostics separately from machine- and run-specific receipts.
- Default to retaining only the original filename and hash, offer an opt-in inert copy of the `.prj`, and keep its absolute source path machine-local.
- Finalize the capsule atomically; cancellation or helper failure leaves no accepted partial capsule.
- Retain unknown fields only as inert typed values and reject unexpected executable objects.
- Validate package input in `SalsaCore` without importing Python or archived runtime types.
- Keep legacy project updating as a separate manual final-legacy-SALSA workflow that the C++ application never invokes.

Exit criterion: every fixture is decoded deterministically through the single release contract, every recognized field and script-local failure has a disposition, resource-limit override cannot bypass mandatory isolation or validation, cancelled or failed conversion leaves no accepted capsule, and the main application never unpickles project data.

### Phase 6: New-Dataset Import

- Require a new or completely empty dataset destination and a separately selected workspace, with optional sibling-location suggestions.
- Implement confirmed known or unknown/custom single-target-scope assignment as correctable durable authoring metadata.
- Treat the legacy dictionary key as the proposed game-facing stem, validate agreement with the stored script name, preserve spelling, and enforce case-insensitive uniqueness within the target.
- Place initial outputs directly in the selected source directory. Allow exclusion or an explicitly warned and recorded stem remap for invalid/colliding names, but never automatically rename or deduplicate.
- Implement project-default and per-script publication profiles, canonical document conversion and publication, exclusion dependency analysis, reparse semantic comparison, final identity mapping, and workspace creation.
- Establish generated SCTs as immutable source baselines with empty initial patches. Preserve supported metadata, retain unsupported metadata in the capsule, and report exclusions, remaps, pending records, and discards.
- Recheck inputs and empty destinations immediately before committing all selected scripts as one transaction. Make cancellation and any selected-script failure leave both destinations and the original `.prj` unchanged.
- Open the resulting workspace with a concise summary and an explicit distinction between creating an asset and proving game reachability.

Exit criterion: representative projects produce self-contained validated single-target datasets and reopenable workspaces with no silent loss, every output SCT stem is unique within the selected target, explicit remaps and exclusions are traceable, unknown provenance remains unguessed, and no failed or stale transaction partially populates either destination.

### Phase 7: Existing-Dataset Two-Way Import

- Confirm one existing target scope and pair exact case-insensitive stems within that scope. Require confirmation for cross-stem proposals and never map across scopes implicitly.
- Detect prior import of the same capsule into the same target and resume or display its state rather than duplicating metadata.
- Keep Phase 7 strictly two-way by deferring assets with existing patches or uncheckpointed edits to Phase 8.
- Implement source/incoming reconciliation, default inclusion of `Exact` and unique `Strong` entity matches, hierarchical changelist review with collapsed unchanged scripts, and selection by script or safe dependency group.
- Exclude unmatched incoming scripts from this phase and leave source assets absent from the legacy project untouched.
- Materialize and validate selected results, generate deterministic patches, serialize and reapply them, and create no patch for a semantically unchanged script.
- Commit the selected clean set as one recoverable transaction after rechecking target scope and all source, workspace, and plan revisions. Retain blocked and excluded scripts in the report.
- Leave every matched physical source file byte-for-byte unchanged.

Exit criterion: accepted changes reopen from unchanged source bytes plus checkpointed patches exactly as reviewed, exact-stem identity remains target-scoped, confirmed cross-stem mappings are traceable, repeated import cannot duplicate state, and ambiguous, edited, or stale inputs cannot partially modify the workspace.

### Phase 8A: Existing-Workspace Integration

- Require uncheckpointed edits to be checkpointed or discarded and stale existing patches to be rebased before integration.
- Require explicit confirmation of the physical source as a comparison baseline; never describe it as a historical ancestor without provenance.
- Retain existing-only changes, propose legacy-only changes, combine identical outcomes, preserve unrelated SALSA edits, and require semantic resolution for differing two-sided changes.
- Reuse the Phase 4 conflict commands and Phase 7 target-scope and stem-mapping decisions.
- Permit safe dependency-group selection while materializing, validating, and committing each asset atomically.
- Replace stacked intent with one consolidated deterministic physical-baseline-to-integrated-state patch.
- Commit selected resolved assets as one recoverable transaction while leaving excluded or blocked assets unchanged. Retain prior patches and integration evidence through successful reopen and make each integrated asset one active-session Undo operation.

Exit criterion: existing SALSA edits and incoming legacy changes can be combined into consolidated validated patches without treating an unproven source as a historical ancestor, losing either side, or partially changing a blocked asset.

### Phase 8B: Metadata Promotion

- Apply metadata already supported by the importing SALSA version during original import.
- Display newly eligible pending metadata through nonmodal workspace status and require an explicit preview before promotion.
- Reuse the hierarchical changelist grouped by owning feature, asset, entity, and dependency group.
- Commit selected promotions atomically without modifying the capsule; stale mappings or conflicting current metadata block only affected groups.
- Keep pending records visible without repeated modal prompts and permit explicit reconsideration of `DroppedByUser` while retaining its decision history.
- Deduplicate only by capsule hash plus record identity and expose semantically competing records from multiple capsules as conflicts.
- Require every supported authoring feature to own its typed idempotent promotion adapter; do not introduce a generic metadata bag.

Exit criterion: every metadata feature supported by the running SALSA version can preview and atomically promote eligible capsule records exactly once, pending and blocked records remain auditable, competing metadata cannot overwrite silently, and the capsule remains immutable.

### Phase 9: Feature Hardening and Program Assessment

- Require completion of every preceding phase, including the metadata-promotion framework and adapters for metadata features supported by that application version. Unsupported metadata may remain explicitly `Pending`.
- Run the private real version-7 fixture manually through both applicable import modes without placing it in normal checked-in CI.
- Run the full checked-in synthetic corpus across target scopes, same-stem assets in different scopes, within-target collisions, key/name disagreement, explicit remapping and exclusions, existing patches, merge conflicts, and interrupted transactions.
- Verify deterministic capsules, generated SCTs, patches, mappings, and reports for identical semantic inputs while excluding declared run-specific receipt fields.
- Inject interruption at every durable transaction boundary and verify automatic finish-or-rollback on reopen without manual workspace-file editing.
- Verify strict schema rejection, cancellation, stale-input detection, and helper isolation with malformed and adversarial fixtures. Disabling resource limits must not disable process, network, schema, or capsule-validation boundaries.
- Exercise projects near default resource limits and a larger trusted project with the explicit override, requiring responsive progress and cancellation rather than a fixed wall-clock promise.
- Reparse and semantically compare every generated SCT in automated acceptance tests.
- Verify by hash that the original `.prj` and existing matched source files remain byte-identical.
- Prove that same stems may coexist across target scopes while every individual projected target contains at most one SCT per case-insensitively normalized stem.
- Produce a portable import report without absolute machine paths that records hashes, target scope, publication settings, mappings, remaps, exclusions, dispositions, warnings, and converter contract identity.
- Keep migration local, with no telemetry or automatic upload of projects, capsules, or reports.
- Link user-facing migration documentation to the final legacy SALSA GitHub release maintained by the project owner and explain the version-7 open-and-resave process. The C++ SALSA project does not bundle, publish, or launch that legacy executable.
- Treat a completed and enabled importer as a supported compatibility feature rather than an experimental path, without treating its completion as a decision to release the overall application.
- Perform the nonblocking manual UI checklists for the import wizard, changelist, conflict surface, restoration, and recovery workflows. Compiled builds, executable validation, and automated tests are the feature acceptance gates.
- Perform a complete consistency and dependency audit of this living plan and the implemented system, revise stale directions, and assess the state of the full application before making any separate product-release decision.

Exit criterion: every migration phase is complete, the importer satisfies the current feature acceptance standards, and the resulting application state has been assessed. This conclusion neither schedules nor blocks a public SALSA release, and the plan remains revisable when better evidence or implementation approaches emerge.

## Acceptance Standards

Migration is not successful merely because SCT files were emitted or a pickle loaded without exception. A supported import must satisfy all applicable requirements:

- The original `.prj` remains byte-identical.
- The input is an official-final-SALSA legacy project declaring schema version 7 and satisfies the complete accepted version-7 shape; forks, development builds, all other versions, and missing-version projects are rejected without mutation.
- Documented final-version load normalization is reported, and no broader repair occurs during decoding.
- Existing matched source SCT files remain byte-identical unless a separate explicit publication operation occurs.
- Explicitly excluded scripts remain absent from committed outputs and are retained with their rejection reasons in the migration report; the accepted subset still commits atomically.
- Every legacy field in the supported schemas is classified as applied, pending, blocked, unsupported, explicitly dropped, invalid, or recomputable derived state.
- Recognized inconsistent metadata is discarded only through an explicit user decision recorded in the report.
- Every generated or materialized document passes current SALSA policy and `SpiceSCT` validation.
- Every generated SCT reparses and semantically matches its intended canonical document.
- Every durable patch serializes, reapplies to its exact baseline, and reproduces the reviewed result deterministically.
- Every inferred entity mapping is reproducible and carries reviewable evidence.
- Ambiguous mapping, unresolved conflict, target ambiguity, source drift, workspace drift, cancellation, or write failure cannot leave a partially accepted migration.
- Workspace restoration reproduces the committed documents, authoring metadata, migration status, and supported session state without reading the legacy pickle.
- Unsupported legacy content remains recoverable from the immutable capsule unless the user explicitly authorizes its removal.

## Deferred or Separate Work

- Automatically adding unmatched legacy scripts to an existing physical dataset.
- Deleting physical source assets absent from the legacy project.
- Treating the legacy project as a live editable backend.
- General import of arbitrary Python pickles.
- Direct import or automatic upgrade of legacy project schemas 1 through 6, missing-version projects, or newer unsupported schemas.
- Cross-tool semantic merge or interpretation of the SALSA patch body by a shared project layer.
- Runtime deployment or Dolphin state repair.
- Automatically materializing suppressed instructions into exported scripts.
- Promoting pending metadata before its owning authoring feature and schema exist.

## Implementation Questions to Resolve From Evidence

These questions do not freeze the architecture or block unrelated progress. Resolve them when the relevant phase has concrete fixtures and code, and revise this plan if the evidence favors a different implementation:

- Whether the single finalized normalized legacy model uses canonical JSON plus blob files, CBOR, or another deterministic inert encoding.
- The concrete file-level mechanism that implements the agreed automatic, preflighted, recoverable schema-one-to-schema-two workspace upgrade.
- Which typed expected-before values materially improve conflict precision in representative diff, rebase, and integration fixtures.
- Which SALSA-only legacy metadata features the assessed application version supports and therefore require typed promotion adapters; all others remain visibly pending.

## Decision Record

- **2026-09-03, provisional:** Support both new-source and existing-source import modes through one normalized legacy conversion boundary.
- **2026-09-03, provisional:** Keep patch diffing, reconciliation, rebase, merge, conflict resolution, and legacy authoring policy in `SalsaCore`; retain only format authority and format-level equivalence in `SpiceSCT`.
- **2026-09-03, provisional:** Preserve unsupported but safely decoded metadata in an immutable migration capsule and promote it later through versioned SALSA feature adapters.
- **2026-09-03, provisional:** Implement fuller workspace persistence, baseline retention, semantic changelists, reconciliation, and rebase/merge infrastructure before the user-facing legacy importer.
- **2026-09-03, provisional:** Existing-source import creates SALSA patches for matched assets and does not overwrite their physical SCT files.
- **2026-09-03, provisional:** Import accepts only final legacy project schema version 7. Users must update and resave older projects with the final legacy SALSA, which remains available with migration instructions.
- **2026-09-03, provisional:** Compatibility is guaranteed only for official-final-SALSA version-7 projects, not forks or development builds. The executable validator and tests are the contract; no separate field-level specification is required.
- **2026-09-03, provisional:** The importer reproduces and reports documented final-version load normalization but does not perform broader repair during decoding.
- **2026-09-03, provisional:** Users may exclude failed scripts and import the accepted subset atomically, and may explicitly discard recognized inconsistent metadata after a precise warning.
- **2026-09-03, provisional:** The available real version-7 project remains a private external fixture. Synthetic version-7 fixtures provide checked-in coverage, and implementation does not wait for another real project.
- **2026-09-03, provisional:** Phase 0 blocks only the phases that decode or import legacy projects; workspace, diff, reconciliation, rebase, and merge prerequisites may proceed independently.
- **2026-09-03, provisional:** Datasets and workspaces remain separately selected, multiple workspaces may use one dataset, and new-dataset migration may merely suggest a sibling layout.
- **2026-09-03, provisional:** A stored relative dataset location rebinds automatically only on an exact fingerprint match; other rebinding requires explicit selection.
- **2026-09-03, provisional:** Initial workspace restoration includes checkpoint-based open documents, the active document, active views, typed selections, and valid navigation, but excludes transient drafts, searches, background work, unsaved semantic edits, crash recovery, and cross-session Undo/Redo.
- **2026-09-03, provisional:** Session state is saved through debounced atomic replacement and restored automatically with the last compatible associated workspace; invalid workspace state does not prevent the source dataset from opening.
- **2026-09-03, provisional:** Schema-one workspaces upgrade automatically after preflight with rollback evidence retained until successful reopen, while unsupported future schemas are never downgraded.
- **2026-09-03, provisional:** Window geometry, dock arrangement, theme, and other machine-wide preferences remain in `QSettings` rather than workspace session state.
- **2026-09-03, implemented direction:** The Phase 2 changelist is a transient, hierarchical `SalsaCore` domain model shared by import, rebase, merge, and editor workflows. Instructions are atomic; other represented semantic entities may be selected individually, and selecting all units accepts a script.
- **2026-09-03, implemented direction:** Changelists emphasize semantic changes and separately categorized authoring metadata; physical layout evidence is diagnostic. Preservation warnings can require explicit acknowledgement, conflicts isolate affected changes, and acknowledged representable invalid selections produce a repair-required state that cannot be checkpointed or exported until repaired.
- **2026-09-03, provisional:** Every newly created patch captures or reuses its exact verified immutable baseline. Because the C++ SALSA is still pre-release, development patches without baselines need not be migrated and may be regenerated.
- **2026-09-03, provisional:** Phase 2 delivers the Qt-free changelist model, deterministic diagnostic formatting, and automated tests; the full GUI waits for a consuming import or rebase workflow.
- **2026-09-03, provisional:** Cross-lineage reconciliation is a general `SalsaCore` capability. Unique `Exact` and `Strong` matches may apply automatically, but `Ambiguous`, `Unmatched`, and `Contradictory` results require explicit decisions and are never resolved by ordinal.
- **2026-09-03, provisional:** Reconciliation supports explicit script- and entity-level correspondence, addition, and removal decisions, while continuing across unaffected scripts and dependency groups.
- **2026-09-03, provisional:** Confirmed mappings are persisted against capsule identity, affected source revision, and reconciliation-contract version; source changes invalidate only affected decisions.
- **2026-09-03, provisional:** Semantic equivalence yields an empty patch despite irrelevant serialization differences. Opaque content requires exact preservation or authoritative `SpiceSCT` equivalence, and insufficient evidence blocks its dependency group.
- **2026-09-03, provisional:** Phase 3 supplies the Qt-free reconciler, deterministic reports, and automated tests; the interactive mapping interface waits for the existing-source importer.
- **2026-09-03, provisional:** Source changes mark patches stale; rebase is explicit, requires checkpointed inputs, always presents a preview, and restarts if a relevant revision changes.
- **2026-09-03, provisional:** Rebase combines identical outcomes automatically and otherwise uses typed semantic resolution commands. Safe dependency groups may be resolved separately, but each affected asset materializes, validates, and commits atomically.
- **2026-09-03, provisional:** A multi-asset rebase may commit selected clean assets while leaving conflicted assets and their original patches unchanged. Each successful asset rebase is one active-session Undo operation.
- **2026-09-03, provisional:** Resolution evidence is reusable only for identical revisions. Old patches and baselines remain recoverable through successful reopen and then follow reference-based cleanup.
- **2026-09-03, provisional:** The general three-way merge engine belongs in Phase 4, but legacy input without proven ancestry is labeled an integration or comparison merge and its orchestration waits for Phase 8.
- **2026-09-03, provisional:** Phase 4 includes the first reusable GUI for changelist review, rebase preview, and conflict resolution.
- **2026-09-03, provisional:** Phase 5 converges before the importer is offered to users on one self-contained converter contract and one capsule schema for official version-7 projects. Intermediate development artifacts may be regenerated; the user-facing importer does not support a matrix of converter or capsule versions.
- **2026-09-03, provisional:** Every conversion requires confirmation that the selected `.prj` is trusted and runs against a controlled staged copy in an isolated no-network helper. Default resource limits have an explicitly warned advanced per-import disable control, but isolation and validation cannot be disabled.
- **2026-09-03, provisional:** The portable capsule records deterministic normalized content, source filename, and hash; machine-specific details remain local and copying the original pickle into the capsule is opt-in.
- **2026-09-03, provisional:** Project-level structural incompatibility rejects the import, while script-local failures remain in the complete inventory so users may exclude them. Cancellation or helper failure never yields an accepted partial capsule.
- **2026-09-03, provisional:** `SalsaCore` independently validates inert capsule data, and updating older projects remains a separate manual workflow in the final legacy SALSA.
- **2026-09-03, provisional:** Script-stem uniqueness is scoped to a concrete export target rather than the entire workspace. Physical assets in different disc/target scopes retain distinct path identities, while each projected target contains at most one SCT per case-insensitively normalized stem.
- **2026-09-03, provisional:** Initial new-dataset migration assigns the legacy script set to one explicit target scope, including unknown/custom when provenance is unavailable. Future multi-target file/section tagging and sub-file projection remain deferred but must preserve dependency closure and require a unique script selection per target.
- **2026-09-03, provisional:** Phase 6 requires an empty dataset destination and separate workspace selection. Target scope is confirmed, durable, and later correctable without rewriting unchanged SCT files.
- **2026-09-03, provisional:** A legacy project dictionary key is the authoritative proposed game-facing stem and must agree with the stored script name. Stems preserve spelling but are unique case-insensitively within the target; invalid or colliding names require exclusion or an explicitly warned, permanently recorded remap.
- **2026-09-03, provisional:** Initial imported SCTs are written directly under the selected source directory, are never automatically renamed or deduplicated, and are produced only through canonical publication plus reparse semantic verification.
- **2026-09-03, provisional:** The selected Phase 6 set commits atomically after exclusion dependency analysis and a final input/destination recheck. Generated SCTs become source baselines with empty patches, and the resulting report distinguishes asset creation from proven game reachability.
- **2026-09-03, provisional:** Phase 7 confirms an existing target scope, treats its unique case-insensitive stem as asset identity, requires confirmation for cross-stem mappings, and never searches another target scope implicitly.
- **2026-09-03, provisional:** Phase 7 remains a two-way import and defers assets with current patches or uncheckpointed edits to Phase 8. Unmatched incoming scripts are excluded and source scripts absent from the legacy project remain untouched.
- **2026-09-03, provisional:** Existing-dataset review reuses the hierarchical changelist, collapses unchanged scripts, includes exact and unique strong entity matches by default, and commits the selected clean set as one transaction after complete materialization and verification.
- **2026-09-03, provisional:** Semantically unchanged scripts create no patch, repeated capsule/target import resumes or displays existing migration state, and matched physical SCT bytes are never changed.
- **2026-09-03, provisional:** Phase 8 is split into existing-workspace integration and metadata promotion with separate acceptance criteria.
- **2026-09-03, provisional:** Existing-workspace integration first checkpoints edits and rebases stale patches, then requires the user to confirm the physical source as a comparison baseline. Existing-only and unrelated edits remain, legacy-only changes are proposed, identical outcomes combine, and differing two-sided changes require resolution.
- **2026-09-03, provisional:** Each integrated asset becomes one consolidated deterministic patch and one active-session Undo operation. Safe groups may be selected, but assets commit atomically and selected resolved assets form one recoverable transaction while blocked assets remain unchanged.
- **2026-09-03, provisional:** Supported metadata applies during initial import; newly supported pending records receive a nonmodal notice and explicit hierarchical preview before atomic promotion.
- **2026-09-03, provisional:** Capsules remain immutable. Promotion is feature-owned and typed, uses capsule hash plus record identity for idempotency, blocks stale mappings and competing semantics rather than overwriting, and allows explicit reconsideration of a prior discard while retaining decision history.
- **2026-09-03, provisional:** All phases must complete before the migration feature is called complete, after which the entire application is assessed. Migration-plan completion neither triggers nor blocks a public SALSA release; product-release scope and timing are separate decisions.
- **2026-09-03, provisional:** The project owner will archive final legacy SALSA in a branch and publish its GitHub release. C++ SALSA documentation links that release and explains the version-7 resave workflow but does not bundle, publish, or launch the legacy executable.
- **2026-09-03, provisional:** This plan is intentionally revisable. Current directions and phase ordering may be replaced when implementation evidence supports a better approach; prior planning text must not prevent an alternative that preserves the required safety and user outcomes.
- **2026-09-03, provisional:** Phase 9 requires deterministic synthetic coverage, manual use of the private organic fixture, transaction fault injection and automatic recovery, helper-boundary tests, target-scoped stem tests, reparse semantic verification, and source/input hash preservation.
- **2026-09-03, provisional:** Migration reports remain portable and local with no automatic telemetry or upload. Manual GUI checklists are nonblocking; compiled builds, executable validation, and automated tests determine migration-feature acceptance.
- **2026-09-03, provisional:** After every migration phase completes, the plan and implementation receive a consistency audit and the complete application is assessed. The plan remains living after that audit rather than becoming an immutable implementation constraint.
