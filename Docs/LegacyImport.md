# Importing a legacy SALSA project

The new SALSA imports projects saved by the official final Python SALSA project format version 7. It does not open older project versions, unfinished development formats, forks, or arbitrary pickle files.

## Updating an older project

Download the designated final legacy SALSA build from the [SALSA releases page](https://github.com/jahorta/SALSA/releases). Open the older project in that application and save a new copy. Confirm that the saved copy reports project format version 7, then import that copy in the new SALSA. The new application does not install, launch, or automate the legacy application.

## Starting an import

Choose **File > Import Legacy Project**. Select the version-7 `.prj`, confirm that you trust the file, and choose a new SCT source directory and a separate new SALSA workspace directory. Both destinations must be missing or completely empty and must be on the same filesystem volume.

Choose the target platform, Dreamcast disc when applicable, region or explicit Unknown region, preferred text encoding, message-space convention, byte order, and wrapper. These choices are durable import metadata. SALSA does not infer provenance that the legacy project did not retain. Final legacy SALSA selected between Shift-JIS and Windows-1252 for individual strings and did not retain that choice in version-7 projects, so migration first uses the selected preference and losslessly falls back to the other legacy encoding only when the preferred encoding cannot represent a retained string.

## Reviewing scripts

SALSA inventories every script before import. A script that cannot be converted remains selected until you explicitly exclude it or correct an allowed output-stem remap. SALSA never silently drops, renames, or deduplicates a script. A disagreement between the legacy dictionary key and stored script name requires exclusion; invalid or colliding output stems may be explicitly remapped after review.

The import prepares every selected script as a canonical `SctDocument`, exports it through SpiceSCT, reparses the result, and requires semantic equivalence. Invalid recognized metadata blocks preparation until you explicitly discard the listed record; the decision is recorded and the immutable capsule remains available. Eligible project and script aliases, opcode colors, and section folders are promoted into the new authoring project. Other metadata stays in the migration capsule with its disposition and explanation. Legacy folders remain grouping metadata and do not create script modules.

## Publication and recovery

You may cancel while SALSA reads, reviews, or prepares the project. Cancellation becomes unavailable during the brief final publication step. SALSA publishes the source directory and workspace through a recoverable same-volume transaction; an interrupted transaction is completed or rolled back when SALSA next starts. If either destination contains unrecognized changes, recovery stops instead of deleting or overwriting them.

The completed workspace retains the authoring project, immutable capsule, portable import report, legacy-to-current mappings, generated source baselines, target scope, and metadata disposition history. Reopening restores the same script and content identities. The original `.prj` remains unchanged. Retaining a copy of it inside the capsule is optional and disabled by default.

Creating an SCT file and assigning a target scope does not prove that the game can reach that script. The initial importer also does not merge into an existing source dataset or workspace.
