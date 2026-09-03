# Same-Lineage SCT Diff and Changelist Foundation

Status: implemented Phase 2 core foundation.

## Outcome

`SalsaCore` can compare multiple same-lineage SCT semantic states and produce one
deterministic, Qt-free change plan. The plan is transient review state. The durable
result remains a schema-three semantic patch authored against exact retained source
bytes.

This foundation is intended for editor workflows and for later import, rebase, and
merge orchestration. It does not perform cross-lineage entity matching.

## Durable patch and baseline contract

Schema three is a clean pre-release break from development schema two patches. It
records typed before and after values for allocator state, orders, sections,
instructions, text, footer entries, authored structured-control-flow arms, and text
repair provenance. Patch application checks every expected-before value before
returning a new semantic state.

Every checkpoint retains the exact immutable source bytes under their SHA-256 source
revision before publishing the patch envelope. Retained bytes are verified when they
are written and read. A missing or corrupt retained baseline leaves the source document
available but places its patch in conflict; SALSA neither guesses a replacement nor
silently discards the patch.

## Changelist model

A root plan contains scripts in stable asset-locator order. Each script contains stable
change-unit IDs grouped by semantic category: structure, instruction, text, footer,
SALSA authoring metadata, or preservation. Instruction opcode, expressions, fixed and
repeated parameters, and instruction-local flags form one atomic selectable entity.
Other represented semantic entities are individually selectable. Whole-script
acceptance is the selection of all selectable units for that script.

Required preservation state is carried with accepted semantic work. Stable-ID allocator
high-water marks never regress when accepted changes are projected over unrelated
current work. Text repair provenance and its semantic text change are coupled so one
cannot be accepted without the other. Authored arms remain visible as a distinct
authoring-metadata category.

Opaque attachment differences are not currently editable patch operations. The plan
keeps the baseline opaque form, emits a nonselectable preservation warning, and requires
an explicit acknowledgement before applying another selected change from that script.

## Preview and application rules

Selections name both the plan ID and stable change-unit IDs. A stale plan, unknown ID,
or attempt to select a diagnostic-only unit is rejected. Before applying a selection,
SALSA compares the planned baseline with the current semantic state. An
expected-before mismatch conflicts only the affected entity; clean selected entities in
the same or other scripts can still be projected.

The projected state is validated. An impossible or unrepresentable operation is blocked.
A representable selection that leaves dangling references or another invalid document
state is allowed only after the caller acknowledges each contributing selected unit. The
result is marked `RepairRequired`; checkpointing and publication continue to reject it
until subsequent edits restore validity.

`preview` and `apply` are intentionally pure at this layer: both return proposed semantic
states, applied IDs, and deterministic diagnostics without mutating a document session or
workspace. A future consuming workflow owns confirmation, Undo integration, and atomic
workspace transactions.

## Current boundary

This slice includes the semantic patch schema, exact baseline store, deterministic
multi-script plan, selective projection, conflict isolation, warning acknowledgement,
repair-required results, diagnostic formatting, and automated tests. It deliberately
does not include a changelist GUI, cross-lineage reconciliation, patch rebase, three-way
integration, legacy decoding, or migration transactions.
