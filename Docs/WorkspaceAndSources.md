# Workspace and source files

Open **View > Workspace and sources** for the source hierarchy, current project path and save status. Activate an imported script row to open it. Filter by script, path or status; select a row for byte sizes and SHA-256 fingerprints.

**Review source changes** (F5) explicitly scans and hashes the dataset on a worker under the exclusive-operation widget. It compares current files against each script's immutable imported source, not against generated output. It does not decode SCTs, refresh the IR, merge changes or modify the source dataset. A source exported over the original file therefore appears changed. Review results describe the files when inspected; refresh again after external changes.

Changed and missing files retain their authored scripts and original baselines. Files marked **Not imported** have no authored script to navigate to. Optional MLD associations remain in **Authoring > Optional MLD resources…**, with explicit reselection; this SCT source review does not scan MLD contents.

## Editing multiple discs

SALSA currently does not support multiple discs in a single project. Dreamcast Disc 1 and Disc 2 can contain different versions of the same-named script, so keep each disc's extraction, authored project and workspace separate. Do not combine the two extraction folders into one imported dataset.

To edit both discs at the same time, open two separate instances of SALSA:

1. In the first instance, open the Disc 1 extraction with its own workspace.
2. In the second instance, open the Disc 2 extraction with a different workspace.
3. Save each project independently and export to the corresponding disc's output directory.

Use separate workspaces rather than opening the same workspace for editing in both instances. Each project has its own edits and Undo/Redo history; changes are not synchronized between discs.

The existing clipboard can copy instructions, complete constructs and code sections between projects. Paste remaps internal identities and requires you to resolve references outside the copied content, including referenced text. This is not a complete module or cutscene transfer: dialogue, module definitions, entrypoints, connections and MLD assets are not automatically bundled. Prefer Copy followed by Paste and review; the current Cut command removes source content immediately after copying and does not wait for a successful paste in the other instance.

A dedicated module/cutscene transfer workflow is planned, not yet implemented. It will retain separate project ownership and make destination dependencies explicit. See the [planned transfer direction](WholeProgramIrProductionPlan.md#separate-disc-projects-and-future-module-transfer).

## Recovery and external edits

- **Save project** saves the authoritative binary project and view state using the workspace transaction. It also detects external checkpoint changes rather than silently overwriting them.
- **Reopen saved project** uses the unified open flow, including pending transaction recovery, reassociation decisions, and unsupported-schema diagnostics. Unsaved changes still receive the normal Save / Discard / Cancel decision. A failed open leaves the current workspace installed. Save first if you want to retain your current edits before reopening.
- **Import into another workspace…** opens the unified import flow. Choose the source dataset and a fresh destination to inspect externally changed scripts independently. This does not merge them into the current project.
- Source refresh cannot discard edits. Save and native export continue to use authored content. Missing original files do not authorize synthesizing a new script or rebinding a baseline.

The old editor's byte-patch rebase, patch conflict decisions and patch-evidence maintenance are superseded by complete authored projects and fresh native lowering. Their underlying tasks are now source-change review, immutable import provenance, transactional save/recovery and explicit import. Automatic three-way semantic merging of an externally modified SCT into an edited script is not implemented. No old patch authority or older IR project conversion is introduced. Supported Python SALSA v7 import remains available.

## Layout and sessions

**View > Reset layout** restores the default dock arrangement and visibility. **Help > Current editor guide** (F1), **Keyboard shortcuts**, and **Workspace and source recovery** are available offline.

Saving retains tab order, active tab, view mode, and independent instruction/semantic selections and expanded items. An inactive view restores its state only when selected. Startup still opens no dataset or workspace; explicit open restores the saved session. Cancelling tab restoration retains the saved unopened tabs.
