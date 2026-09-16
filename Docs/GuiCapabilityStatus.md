# GUI capability status after G6

This is a source and wiring assessment against the editor before the whole-program IR transition. It is not a claim of live GUI validation. All current feature components use one shared project journal; imported and empty authored control groups use the same underlying IR types.

| Ledger | Reachable capability | Implementation / limit |
| --- | --- | --- |
| R01–R02 | Script tabs: Instructions / Semantic; View > Instruction inspector; Edit condition, Add preparation instruction, Edit branches / cases, Instruction timing | Shared IDs and paged outline; SPICE import grouping. General full-value/repeated-group reconstruction still uses the advanced adapter below. |
| R03–R04 | View > Text editor and preview; Edit > New string / Delete string | Detached drafts, Apply/Discard, typed references and supported text preview; not game rendering. |
| R05–R06 | View > Navigator; Back / Forward; Authoring > Find variable uses | Explicit paged queries and stable navigation. Queries refresh on request. |
| R07–R08 | Edit > Copy / Cut / Paste / Duplicate / Delete / Move selection; snippets; outline drag/drop | Selection fragments, explicit reference review, one-command history. |
| R09–R11 | Edit organization actions; View > Notes and bookmarks; instruction catalog customization | Section/folder order, notes, bookmarks, colors and catalog metadata use the journal. |
| R12 | Edit > Insert instruction | Complete SPICE opcode catalog. Target availability policy remains deferred. |
| R13 | Authoring language-group tools | User-defined groups, suggestions, explicit reassignment, logical links and batch edits; no fixed supported-locale list. |
| R14–R15 | Authoring module and named-condition tools | Imported-script-owned modules, standalone module views, definitions and uses. No new-script creation. |
| R16–R18 | Authoring > Entrypoints and activation / Optional MLD resources / Overworld selectors and initialization preview | Required init/loop, optional routes, resource reselection, ordered assignments and bounded preview. MLD context is advisory. |
| R19–R22 | Authoring story tools, interactions and Route endpoints | Meanings, narrative ordering, scoped effects/conditions, item-attached decisions and atomic route requirements. A candidate narrative is not a proven game timeline. |
| R23 | Authoring > Coordinated output; File > Export active script | Reviewed output closure and target options; dynamic/unresolved destinations require user selection. |
| R24 | View > Workspace and sources; File > Review source changes (F5) | Filterable source hierarchy and navigable source states. Explicit fingerprint comparison against imported baselines, without parsing SCTs or changing authored content. |
| R25 | Help; View > Reset layout; saved tab state | Offline guides/shortcuts, dock reset, tab order and separate per-view selection/expansion. Startup remains explicit. |
| R26 | Workspace source review / Reopen saved project / Import into another workspace; File > Save project | Complete binary projects and transaction recovery replace byte-patch authority. General three-way semantic merging of an external SCT into edited content is not implemented. |

## Remaining separation work and accepted boundaries

The active shell composes separate feature components; it does not own a second document. Most feature widgets consume Qt-free editor drafts/intents. `WorkspaceComponent` consumes a Qt-free overview and emits navigation/operation requests; the coordinator supplies the current workspace adapter and shared exclusive operation.

`ProgramWorkspaceAdvanced.cpp` deliberately retains the reachable general selected-item/full-value dialog, explicit If/While promotion integration (with shared-guard promotion still dispatched by the coordinator) and generated-instruction preview. These paths still use backend-specific program reads or materialization. A future replacement must cover repeated parameter-group reconstruction and native preview before removing them. Quarantine is not counted as completion of their backend isolation. The disconnected duplicates of capabilities already supplied by G3–G5 were removed.

No old IR schemas or retired patch execution authority are restored. Supported Python SALSA v7 import remains supported; there is no v7 export. Missing MLDs, runtime reachability, retail tile inventory and cross-release content equivalence are not export acceptance gates. Target-specific instruction policies, automatic external-source merging and optional library pruning remain outside this implementation.

Manual GUI layout/usability review and GUI-loading tests were not performed, as requested. The build and focused console validation are recorded in the editor guide and ignored planning records.
