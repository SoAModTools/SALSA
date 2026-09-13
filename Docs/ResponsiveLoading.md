# Opening datasets, workspaces, and scripts

SALSA starts without opening a dataset or workspace. Recent datasets, workspace associations, saved tabs, and window layout remain remembered. Choose a dataset explicitly from Open or Recent to restore its associated workspace and saved session. A workspace needing reassociation must be opened through the explicit workspace review.

Dataset opening uses one exclusive operation through inspection, associated workspace recovery, authoring project loading, and tab restoration. Individual SCT opening and reloading use the same operation widget. Selecting an already open tab only focuses it.

The widget reports the current phase and item, including project reading/decompression, baseline verification, authoring validation, SCT parsing, editor preparation, and restoration counts. Phases without meaningful internal counts use an indeterminate indicator. Recovery finishes safely before cancellation is available. A cancellation request during a noninterruptible parser/validation call waits for that call and discards the result; it does not terminate a worker thread.

A cancelled document load leaves the existing document unchanged. Cancelling tab restoration retains the opened workspace and completed tabs, and protects the saved session list from being overwritten by the partial restoration. A later successful workspace restore releases that protection. Missing optional saved tabs are reported as restoration gaps. Invalid or unsupported project files are reported without replacing the current authoring authority or overwriting the file.

## Implementation boundary

Workers prepare immutable authoring changes and editor data. The live authoring session installs a prepared change only against its exact captured state; the controller additionally checks its operation generation and cancellation state. Import history is published once. QObject and widget updates remain on the UI thread. Physical and semantic outline data are constructed without QObjects in the worker, then installed by model swaps; no eager per-node widget creation is needed.

Imported programs retain their source parse/inspection evidence locally. Baseline and authored snapshots share that evidence rather than reparsing the same bytes. Project persistence and executable output formats are unchanged, and Python SALSA v7 import remains supported.

## Validation

Regression tests use synthetic scripts, core services, QCoreApplication event loops, and controllers. They do not instantiate QApplication, load the GUI, or create windows. Worker barriers verify event-loop responsiveness and cancellation before installation, including previously authored script reopening. Startup dispatch and main-window wiring are inspected statically; no GUI walkthrough or automated GUI validation is claimed.
