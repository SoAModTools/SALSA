# Exporting SCT Documents

SALSA keeps authoring and publication separate. **Document > Save Authoring Project** (Ctrl+S) checkpoints every imported script and its edits together. Export creates complete SCT files for explicit targets and does not clear the project's unsaved state. Closing a document tab keeps its edits in the project. Undo and redo follow the shared project history across tabs; the undo stack lasts for the current session.

## Export the active document

1. Activate the SCT document tab you want to export.
2. Choose **Document > Export SCT...** or press **Ctrl+Shift+E**.
3. Choose a destination file and every required target setting.
4. Select **Export**.

SALSA suggests a new filename next to the source file and remembers the last export directory on this computer. When reliable import evidence is available, the dialog prefills matching settings. Review those settings before exporting; the imported script context and authored output profile retain defaults, while each publication selects its explicit release settings.

The target settings are:

- **Target platform**: GameCube or Dreamcast.
- **Character encoding**: Windows-1252 or Shift-JIS.
- **Message spaces**: byte `0x7F` or Shift-JIS `0x8140`.
- **Byte order**: big endian or little endian.
- **Output wrapper**: raw SCT or AKLZ compressed.

Authoring-project export builds a fresh SCT from understood current content. Opaque imported bytes remain baseline evidence; referenced undecoded text and unsupported executable content require repair. A separate standalone-document preservation workflow retains its stricter source-preservation contract. Validation or encoding failure leaves the destination unchanged.

## Editing while export runs

Export captures the exact active revision when it begins. You may continue editing while that revision is materialized, validated, and written in the background. If newer edits exist when the operation finishes, SALSA reports that they remain unpublished. Run export again when you want those edits included.

Cancelling stops the operation at the next safe boundary. It does not alter the source document or an existing destination file.

## Replacing the loaded source file

Choosing the loaded SCT itself as the destination requires an additional confirmation. SALSA refuses the replacement if the source bytes have changed since import. A successful replacement makes the imported baseline stale. Save the authoring project and explicitly reconcile or reimport that source before replacing it again; current authored content is not reconstructed by replaying stale patches. Project saving retains the immutable imported baseline and does not overwrite the source file.

Exporting somewhere else remains available when the original source has changed. In that case SALSA warns that the output represents the captured in-memory revision rather than the current file on disk.

## Results and diagnostics

The status bar and Activity log record whether publication completed, failed, or was cancelled. Diagnostics contains only current actionable warnings and errors. A single-file publication receipt is retained for the current application session. Coordinated-set receipts persist under the chosen output root and require matching current output hashes. Receipts are not embedded in SCTs or used as authoring execution state.

## Cross-script route outputs

Open **Story State and Cutscene Effects > Executable routes / export set > Coordinated output** to review and publish a complete route dependency closure. Every script is encoded before recoverable installation begins. Single-file export reports companion dependencies instead of publishing half a route. See [cross-script routes and coordinated output](CrossScriptRoutes.md) for revision capture, output review, cancellation, and recovery.
