# Localizations and logical messages

Open **Localizations…** from the editor's authoring actions. Language groups are project-wide, user-defined names, not a fixed set of supported locales. The feature is available in Instruction mode and does not require story analysis.

## Discover and organize

Choose **Discover / link strings** to propose assignments and link indexed strings into logical messages. Existing explicit assignments are retained. Recognition follows the legacy string extractor: ordinary F/S/G markers at character 1, corroborated by string-group names or an unmarked counterpart; `palyoko` names use character 7. Unmarked counterparts use **Unmarked**, without assuming English. Strings with no language pattern receive an editable **Source** group. Source is initially excluded from runtime output when other language groups were recognized. Footer/debug strings stay outside this indexed localization abstraction.

Logical messages belong to their original script and retain stable identities. Their translations appear together in the semantic string tree. Use **String groups / assignments…** to rename groups, correct membership, manually link variants, split shared resources, or use the existing detailed text editor. These operations edit the same project and share its undo history.

Choose **Recognize language branches…** to review optional organization. Equivalent flat instruction sequences behind `ByteVar[87]` comparisons or switches can become one canonical editable body. Standalone operations with one source variant can also be organized before adding translations. A lone operation referencing an already multilingual family is left explicit until its dispatch is recognized; the presence of translated strings alone does not identify a safe dispatch boundary. The recognizer compares all other operands, scheduling and refresh settings; it does not discard differing preparation work or guess through unresolved control flow. Unsupported or ambiguous cases stay explicit. The recorded explanation is available when inspecting the localized construct.

The semantic editor shows a single localized operation directly, or a **Localized sequence** containing its editable instructions. Indexed operands display a logical message name and a text preview. The localization selector above the semantic tree changes this preview and the inspected translation; it does not change runtime output. Missing translations are shown rather than silently falling back. Multiple translations assigned to the same message/language need manual correction.

Switches do not require a Default case. Import grouping leaves instructions before the selector outside its case bodies. Export preserves whether a Default exists; localization organization and expansion also retain an explicitly empty Default rather than inventing one for switches without it. A nonempty Default still needs explicit authoring and is not automatically collapsed into a localized body. Import-grouping corrections apply when importing source scripts; reopening a saved project does not reorganize its authored groups.

Use generated-instruction preview to inspect the runtime branches. **Expand localization to instructions…** turns a flat localized body back into explicit language branches using the current mappings and included languages. Undo restores its shared representation. Expand before copying an entire localized construct to a fragment. Nested constructs added to a shared body can be lowered, but explicit expansion currently requires a flat body.

## Runtime languages

**Mappings / output languages…** assigns each group a distinct byte value for `ByteVar[87]` and controls its inclusion in output. Excluding a language retains its authored translations. Blank mappings are allowed in drafts; exporting runtime selection requires mappings and an unambiguous translation for each included language. An originally unguarded operation remains unguarded when it has only one included language.

You may add more or fewer groups than the source scripts used. SALSA warns when assigning new or changed selector values: script-side dispatch does not add language choices to the game's title screen. A matching game modification must supply those values. No retail language inventory is enforced. Language names and preview selection do not choose the export encoding, endianness or platform.

Organization retains the original switch/first-match/independent-guard behavior and refresh settings. Editing the shared body changes every generated language variant. Ordinary explicit guards remain ordinary code: their predicates are not rewritten by changing a localization mapping. If such code still references an excluded string, export reports that reference instead of discarding the operation.

## Translation table and CSV

The table uses logical messages as rows and languages as columns, with 100 messages displayed per page. **Apply text edits** commits the accumulated edits as one undoable command. Pending edits must be applied before other localization operations; closing offers to discard them.

**Export CSV…** selects language columns. CSV has two metadata records followed by the editable table:

```csv
"# SALSA localization","1"
"# Project","project-uuid"
"Message","English","French"
"123","\messageHello\e{}","\messageBonjour\e{}"
```

`Message` contains the stable logical-message ID. Keep it intact; rows may be reordered. The project header prevents applying IDs to another project. Language headers are user-defined group names. There are no repeated script, language, encoding or provenance columns. Importing an unknown language name creates a group; absent columns and absent rows leave existing content unchanged. Repeated message IDs, duplicate language headers and unknown IDs are rejected before applying edits.

A blank cell means a missing translation and removes that language assignment, retaining the underlying resource. `\message` represents an empty message, `\empty` an empty indexed record, and `\plain:` an empty plain-text value. Message text can contain quoted CSV commas and newlines. Formatting is reversible: `\h(Speaker)` is a header; commands include `\e{}`, `\p(12){}` and `\c[1,2,3]{}`. The `{}` terminator separates a command from following text. Literal backslashes are doubled in message text. Exported formatting can be retained while translating the surrounding words.

**Import CSV…** reviews the number of affected cells, missing values, and new groups before applying one undoable edit. The table and CSV share the same text notation. Translations may remain incomplete while editing; executable output still needs encodable text and resolved references.

## Import indexed strings from SCT

**Import indexed SCT strings…** reads donor files, matches their script stems and legacy correspondence keys to the open project's logical messages, and presents candidates for explicit selection into a destination language. Choose only one donor variant per logical message. Unmatched messages can be added to an existing matched script. Ambiguous matches and undecoded text are reported. Run discovery first so destination messages are available for matching.

Only indexed translations are adopted. Donor execution, footer text, asset bindings, and baselines do not replace the project's authored scripts. SPICE supplies parsing, text-convention recommendations and text decoding. The current public API reads a temporary complete SCT document to obtain its indexed strings; a selective SPICE reader would be an optimization, not a separate SALSA parser.

## Persistence

Whole-program binary schema **10** persists language mappings, output inclusion, logical uses and localization recipes alongside authored text. Container framing **2** and Zstandard level **1** are unchanged. The preview language is transient presentation state. All accepted edits participate in project undo/redo and save/reopen. Older development project schemas remain unsupported; Python SALSA v7 import remains supported, followed by optional localization discovery.

The initial private `me002a` checks found that the Dreamcast EU examples still contain imported text rejected by native export; the same errors occur before localization changes. Their dispatch is retained explicitly by this recognizer. Logical-message editing and interchange remain available. This is not a claim that every imported script is immediately exportable.


Fresh dataset and supported v7 imports automatically run logical-message discovery and reduce equivalent localization branches before the project is shown. Ambiguous or non-equivalent sequences remain explicit. Reopening a saved project preserves its mappings, manual assignments and explicitly expanded sequences without repeating discovery. The discovery and recognition actions remain available for deliberate later edits.
