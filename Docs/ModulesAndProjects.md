# Modules and authoring projects

Open an imported script and choose **Document > Script Modules**. Use **Extract sections** to select one or more unowned script sections, or **New empty module** to start an empty sequence in that script.

Each module retains one parent script. Extraction preserves action order, targets, shared text, and helper references. It does not infer a cutscene boundary, move caller preparation, or change emitted behavior. Modules in different scripts can have the same display name and still have independent identities.

## Editing a module

**Open module** shows its sequences, actions, named conditions, and callers/dependencies. Edit fixed expression parameters and timing in this view. **Append action** inserts instructions whose required parameters have a complete default draft; other instruction forms use the existing document editor. An empty sequence retains its entry port when its first action is added.

Double-click a caller or dependency to navigate to the corresponding instruction. Shared conditions explicitly update every bound use. Section adjacency is shown as a possible continuation, not a proven execution edge. Resource residency and ambient caller state still require interpretation; extraction does not establish them automatically.

Module and document edits use the same project revision, Undo/Redo, and save operation. Opening a module does not create another copy to edit. Reparenting modules is unsupported. [Entrypoints and Resources](EntrypointsAndResources.md) connects existing entrypoints and selected resource entries to module responses; [cross-script route authoring](CrossScriptRoutes.md) connects supported script loads and arrival responses separately from narrative proposals. Arbitrary graph execution remains deferred.

## Saving and reopening

**Save Authoring Project** writes a compressed `project.salsaproject` container in the workspace's authoring component. It contains complete current actions, expressions, text, references, order, metadata, module identities, and presentation state. Reopening restores those records directly; it does not reconstruct current content by importing SCT bytes and replaying patches.

The container also retains immutable imported SCT baselines and available legacy migration capsules as evidence. Source inspection is loaded separately when requested. Python SALSA v7 project import remains supported, including supported metadata promotion and origin references. Legacy folders are not automatically interpreted as modules.

Incomplete bindings and missing-meaning markers can be saved. Opaque source bytes remain evidence, outside authored content. Referenced undecoded text, unresolved executable references, and unknown executable operands require repair before fresh export. Unreferenced opaque source artifacts are omitted from fresh output.

Saving uses the existing workspace transaction and recovery mechanism. Conflicting writes or failed validation do not replace the last accepted save. This container is the authoring project within a workspace; it is not a general workspace archive or a standalone import/export format for SPICE.

See [the container contract](ProjectContainerFormat.md) for diagnostic inspection APIs and encoding details.
