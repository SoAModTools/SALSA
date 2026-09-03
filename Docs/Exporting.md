# Exporting SCT Documents

SALSA keeps editing and publication separate. Editing changes the active document and its patch checkpoint; exporting creates a complete SCT file for one explicit target. Export does not save or clear the document's patch state.

## Export the active document

1. Activate the SCT document tab you want to export.
2. Choose **File > Export Active SCT...** or press **Ctrl+Shift+E**.
3. Choose a destination file and every required target setting.
4. Select **Export**.

SALSA suggests a new filename next to the source file and remembers the last export directory on this computer. When reliable import evidence is available, the dialog prefills matching settings. Review those settings before exporting; the editable document itself does not store a target platform or region.

The target settings are:

- **Target platform**: GameCube or Dreamcast.
- **Character encoding**: Windows-1252 or Shift-JIS.
- **Message spaces**: byte `0x7F` or Shift-JIS `0x8140`.
- **Byte order**: big endian or little endian.
- **Output wrapper**: raw SCT or AKLZ compressed.

Opaque source material must remain exactly preservable for ordinary export. If strict preservation or target validation fails, SALSA leaves an existing destination file unchanged and reports the current failure in Diagnostics.

## Editing while export runs

Export captures the exact active revision when it begins. You may continue editing while that revision is materialized, validated, and written in the background. If newer edits exist when the operation finishes, SALSA reports that they remain unpublished. Run export again when you want those edits included.

Cancelling stops the operation at the next safe boundary. It does not alter the source document or an existing destination file.

## Replacing the loaded source file

Choosing the loaded SCT itself as the destination requires an additional confirmation. SALSA refuses the replacement if the source bytes have changed since the document was opened. A successful replacement makes the open document's source baseline stale, so reload the document before creating another patch checkpoint or replacing the source again.

Exporting somewhere else remains available when the original source has changed. In that case SALSA warns that the output represents the captured in-memory revision rather than the current file on disk.

## Results and diagnostics

The status bar and Activity log record whether publication completed, failed, or was cancelled. Diagnostics contains only current actionable warnings and errors. A successful publication receipt is retained for the current application session; it is not embedded in the SCT file or stored as project state.
