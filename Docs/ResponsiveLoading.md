# Opening datasets, workspaces, and scripts

SALSA starts without opening a project. Window layout, saved tabs and workspace associations remain remembered. **Open Project** selects an existing authoring workspace; **Recent projects** and **Reopen saved project** start loading that workspace immediately. Missing saved checkpoints are errors, never a reason to silently import source files.

Saved opening uses one exclusive operation with a compact phase/progress/Cancel presentation through transaction recovery, binary project decompression, IR/index restoration and saved-tab presentation. It closes automatically on success. No extraction scan, fingerprinting, SCT decoding, localization rediscovery or story analysis runs on this path. Sources may be unavailable; **Review source changes** performs the explicit inventory/fingerprint scan when requested. Optional resource inspection reads only the selected resource.

**Import Project** retains the full exclusive-operation widget: configuration, source loading, decisions, editor preparation and the optional story-analysis choice afterward. Phases without meaningful internal counts use an indeterminate indicator. Recovery finishes safely before cancellation is available. Cancellation waits for noninterruptible work and discards its result; it never terminates a worker thread. Once cleanup finishes the widget closes without a cancelled-summary page.

A cancelled document load leaves the existing document unchanged. Cancelling tab restoration retains the opened workspace and completed tabs, and protects the saved session list from being overwritten by the partial restoration. A later successful workspace restore releases that protection. Missing optional saved tabs are reported as restoration gaps. Invalid or unsupported project files are reported without replacing the current authoring authority or overwriting the file.

## Implementation boundary

Workers prepare immutable authoring changes and editor data. The live authoring session installs a prepared change only against its exact captured state; the controller additionally checks its operation generation and cancellation state. Import history is published once. QObject and widget updates remain on the UI thread. Physical and semantic outline data are constructed without QObjects in the worker, then installed by model swaps; no eager per-node widget creation is needed.

Imported programs retain their source parse/inspection evidence locally. Baseline and authored snapshots share that evidence rather than reparsing the same bytes. Project persistence and executable output formats are unchanged, and Python SALSA v7 import remains supported.

## Validation

Regression tests use synthetic scripts, core services, QCoreApplication event loops, and controllers. They do not instantiate QApplication, load the GUI, or create windows. Worker barriers verify event-loop responsiveness and cancellation before installation, including previously authored script reopening. Startup dispatch and main-window wiring are inspected statically; no GUI walkthrough or automated GUI validation is claimed.
