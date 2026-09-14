# Production editor cutover (WP6)

The executable opens `ProgramWindow`, backed by one `ProgramService` per workspace. Tabs contain script IDs, a paged outline model, and a stable selected subject. They do not contain an `SctEditSession`, mutable native document, full authoring history, or a script-wide instruction map.

## Loading and persistence

Startup restores geometry and recent-item settings without opening a dataset. Explicit dataset opening restores its associated workspace. `EditorWorkspaceIO` runs dataset inventory/fingerprinting, workspace assessment/recovery, and checkpoint preparation in a worker. The first import reads the native collection using the bounded native loader; a saved workspace reads the binary `project.salsaproject` directly. Catalog inspection itself does not parse SCTs. Resident script tabs never parse their source again.

The existing exclusive-operation coordinator covers preparation, installation, catalog population, and sequential tab restoration. Progress reports actual phases and counts; unknown-duration phases remain indeterminate. Worker progress is buffered and delivered on the Qt thread at a throttled rate. Reassociation is a review inside that operation. Cancellation waits for a noninterruptible stage to return; filesystem commits finish safely. A failed or cancelled workspace preparation leaves the installed workspace unchanged. Cancellation during tab restoration keeps the new workspace and completed tabs and retains the unopened saved-tab records.

Saving captures one checkpoint for binary/zstd-1 encoding and commits it through the workspace transaction service. Views, positions, language presentation, and saved tabs remain separate from execution content. Script IDs and selection IDs survive reopening. Save is project-wide because the journal and checkpoint belong to the workspace, rather than to individual document tabs.

Earlier development authoring containers are intentionally unsupported. Opening one reports its decoder diagnostic instead of replacing it with newly imported source content. Use a fresh workspace for a new native import. Supported Python SALSA v7 projects still use the trusted converter and review/commit workflow. Commit now creates the binary program directly through `LegacyProgramImport`, respecting reviewed metadata exclusions. The migration capsule remains in the workspace's import evidence. SALSA does not export v7 projects.

## Scoped editing and presentation

`outline` returns one page of direct children, with no instruction payload or descendant-body copy. The Qt tree requests at most 128 rows at a time. Its nodes occupy a flat slot vector; only requested containers and rows are represented. `read` can request an individual section, sequence, condition, action, or text. `descend=false` excludes nested instruction bodies, and `referencedText=false` excludes automatically following text references. Returned pool handles belong only to that immutable read result.

`locate` returns the ancestry of a stable subject for navigation and saved selection restoration. Script/section order, action ownership and nested construct ownership determine that path. It does not lower or analyze the script. Parameter changes refresh the affected label in place; sequence order changes invalidate only the fetched owning container. Script-layout receipts are distinct from edits inside a section. Aliases have a service-owned derived lookup that is invalidated by metadata changes, rather than a per-view copy of program identity.

The editor routes instruction and SCPT editing, prepared conditions, structured branch/loop insertion, moves/deletion, section edits, branch/case ordering, repeated parameter groups, explicit-flow promotion, text editing/creation, aliases, variable meanings, entrypoint activation, and language membership through the production journal. Existing SCPT and formatted-message widgets are reused as detached typed-value editors. Their display bindings are translated by a captured authored-ID closure; they do not look up or mutate a native document. Generated instructions never become editable authoritative content.

Modules, entrypoints, language groups, item explanations, variable uses, and module effects are opened explicitly from Authoring tools. Opening a tab does not build story connectivity. Variable-use and generated-instruction lists use lazy models. Module story-effect inspection filters its default report to BitVar and ByteVar; the explicit variable query still permits all banks. Observations and user assertions remain distinct, and a variable write is not presented as proof of runtime reachability or a guaranteed final state.

Native publication and explicit generated-instruction inspection call the same revision-qualified materializer. Publication settings choose native byte order, text convention and wrapper independently of project compression. Replacing files in the source dataset requires the publication control to be selected. Missing MLD evidence does not gate publication. A stale or unsupported executable draft fails there with diagnostics.

## Retirement and remaining presentation work

The old MainWindow, WorkspaceController/SctDocumentController, snapshot outline/parameter models, full-state authoring dialogs, and patch/rebase UI are excluded from the production Qt target. Historical sources remain available to their isolated regression targets and import-boundary helpers. There is no runtime switch back to the old authority, and no synchronization layer between it and the production service.

This cutover supplies a compact service-backed editor surface. It is not a claim that the earlier graph layouts, snippet/clipboard presentation, or every legacy convenience dialog has been redesigned and ported. Those old snapshot-dependent surfaces are not exposed by the new entry point. Imported folder/opcode-color presentation is retained in the checkpoint; its visual redesign remains separate. Optional MLD association inspection is an explicit worker operation: it reads and qualifies only the selected asset, outside the SCT catalog fingerprint, and preserves authored targets. The earlier per-entry provider picker and tile-preview UI still require a scoped presentation port. These remaining presentation gaps should be considered when assessing WP6 against the full original editor-parity boundary.

## Measurements and validation

Activity records workspace preparation time, resident action/value page counts, pooled child-reference/character counts, and local edit work counts. Outline requests report page row counts and preparation time. These are current stage/work measurements, not previous evaluator timing claims, and page counts are not a measurement of total process memory.

Validation uses focused synthetic console checks for scoped pages and value decoding, local history and persistence, native lowering, and the v7-to-binary workspace save/reopen/export path. The GUI is built with elevated v145 Release but is not launched or tested through a QApplication, in accordance with the user's instruction. No manual GUI readiness or usability measurement is claimed. No private corpus run is added by this cutover.
