# Durable SCT Patch Checkpointing

Status: implemented, updated for the Phase 2 patch and baseline contract.

## Outcome

SALSA now stores one deterministic semantic patch checkpoint per open SCT asset. The
checkpoint is a squashed difference between the immutable source baseline and a captured
working revision. It is not the gesture-level undo journal and it is not an exported SCT
file.

An extracted dataset remains read-only. The user explicitly associates a separate SALSA
workspace through the Project menu. A new workspace must be an empty directory; an
existing workspace must contain a supported manifest associated with the same canonical
dataset root. The association is machine-local and is restored when that dataset is
opened again.

## Workspace and patch files

The workspace contains a versioned project manifest, a patches directory, and a
content-addressed baselines directory.
Patch filenames are the SHA-256 of the case-insensitive asset locator identity, followed by
the salsa-patch.json suffix. The manifest records only its format, schema version, and
canonical dataset root.

Each patch uses the existing platform-agnostic patch envelope. It affects exactly one
asset and records the source dataset fingerprint plus the exact source asset revision.
Platform, region, disc, media, publication target, timestamps, and absolute source-asset
paths are absent.

The schema-three SCT payload is a clean pre-release break from schema two. It records the
selected source-text convention and typed before/after deltas for allocator high-water
marks, section and instruction order and content, indexed and footer text, the semantic
authoring overlay, and opaque-text repair provenance. It intentionally does
not serialize the undo journal, analysis caches, validation results, UI state, or exported
bytes.

Before a patch envelope is replaced, the exact immutable source bytes are verified
against their source revision and retained as `baselines/<sha256>.sct-source`. Identical
source revisions share one artifact. Baseline retention failure prevents patch
publication.

## Save and restore behavior

Save Document captures the current revision and performs materialization, validation,
canonical diffing, encode/decode, reapplication, and deterministic reproduction checking
off the GUI thread. Only a successfully verified payload replaces the current patch file,
using the existing atomic file-store boundary.

Editing may continue during a save. Success marks the captured revision as the patch
checkpoint. If the user has advanced or branched since capture, the current document
remains dirty. Dirty close prompts can save all applicable documents and continue their
requested close only when every captured revision is still current.

Opening a document first parses its current source baseline, then lazily applies its patch.
A dataset-fingerprint mismatch is a warning when the exact asset source revision still
matches. A source-revision mismatch, missing or corrupt retained baseline, corrupt
payload, unsupported schema, or failed patch application preserves the patch file and
opens the source baseline read-only with a
conflict diagnostic. This slice does not guess at rebasing or discard the conflicting
artifact.

## Boundaries

Checkpointing is per document; there is no cross-document atomic transaction. Workspace
association requires no documents to be open, avoiding a mixture of storage identities in
one editing session. Recovery history, patch migration, conflict resolution, patch removal,
workspace browsing, publication, and shared GameProject storage remain future work.

Manual rendered-GUI verification remains nonblocking. Unchecked behavior may evolve as
later slices add richer workspace and conflict-management surfaces.
