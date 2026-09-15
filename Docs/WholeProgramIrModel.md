# Production program model (WP1)

WP1 introduces `SalsaCore/Program`. The existing editor continues using its old authority until WP6; no synchronization or duplicate live document is installed. The new core model has no Qt dependency and imports no evaluator implementation.

## Ownership and authored structure

A `Program` is an immutable completed bulk build with project UUID, semantic revision and allocator high-water mark. It owns imported script contexts, baseline descriptors, section order, code sequences, paged actions, pooled typed values, texts and cold authoring metadata. It owns no SctDocument, physical patch, JSON tree or baseline bytes.

`ProgramBuilder` is an unpublished construction transaction. A failed mutator prevents `finish()` from publishing any program. Its `importedScript`, `section`, `textEntity`, value and action inputs also provide the boundary for later decoded binary tables. Every script requires a baseline descriptor; these are not new-script creation UI operations. Existing requested script/action/section/text IDs can be retained. Allocation uses monotonically increasing nonzero 64-bit IDs across entity kinds; source names, physical IDs and page positions do not determine identity. An occupied sparse identity-directory page holds 1,024 locations, so large retained identities do not allocate a giant dense array.

An instruction action owns one pooled instruction payload. A conditional action owns an ordered range of stable arms; each tested arm owns a stable condition region and a distinct body sequence. Each condition region owns an ordered preparation sequence plus a typed SCPT result expression. Preparation runs only when that check is reached, so Else If preparation remains conditional on earlier checks failing. Only the final Else arm omits its condition. The first arm is If; subsequent conditional arms are Else If. Each body directly owns its ordered actions, which can include nested constructs or explicit typed instructions. There is no Then wrapper or generated-jump scaffold in authoritative content.

Native import initially constructs explicit instruction sequences. It does not reinterpret arbitrary branches as structured chains. WP3 adds recognition/promotion and semantic edits, including treatment of incoming edges into proposed branches. WP5 lowers constructs to derived explicit flow and maps generated instructions to their owning actions/arms. The inspectable lowered form is not a second editing authority. Explicit instruction references, including irregular control flow, remain representable throughout.

Root sequences belong to sections. Nested sequences identify an arm, condition region, or loop action as their owner, with an explicit sequence role. Modules own selected sequences within one script; ownership cannot overlap another module or an enclosing owned sequence. Entry references and shared calls do not create new ownership. Metadata includes entry activation/response bindings, ports/connections, user language groups/logical text, aliases and item evidence. Script contexts retain optional MLD associations and legacy capsule origin. No resource freshness check gates bulk construction.

Imported `init` and `loop` code sections produce required, enabled lifecycle entries pointing directly at their bodies. Module extraction and response selection are unnecessary. Missing roots produce draft diagnostics. Export requirements and command protections are implemented at their WP2/WP3/WP5 boundaries; WP1 does not export or manufacture missing root bodies.

## Prepared conditions, loops, switches and transfers

The core builder constructs prepared If/Else If checks, While, DoWhile, For, Switch and explicit Break/Continue actions. Kind-specific tables keep loop/switch/transfer details outside ordinary instruction payloads. These are the shared backing structures for imported and manually created groups. SPICE-assisted import normalization and compiler lowering are described in the editor documentation.

A condition has a stable ID, owner action, optional arm, preparation sequence, and result expression. Preparation may contain multiple instructions or nested constructs. On normal completion, the result expression is evaluated using its stored SCPT semantics. An If/Else If check is reached only if preceding checks were false. An Else has neither a check nor preparation. Preparation is a selectable, annotatable authored sequence; it is not an unconditional preamble attached to the enclosing section.

| Loop | First entry | Normal repetition | Continue from body |
|---|---|---|---|
| While | Preparation, then result | True: body, then check again; false: exit | Check preparation/result |
| DoWhile | Body | Body, then check; true: repeat body; false: exit | Check preparation/result |
| For | Initialization once, then check | True: body, update, check; false: exit | Update, then check |

While and DoWhile have no initialization/update sequences. For owns explicit ordered initialization and update sequences, which may be empty. Each check includes its preparation, even the final failed check. Result evaluation occurs only if preparation completes normally. Waits, scheduling and refresh modifiers remain explicit; constructing a loop does not insert or remove a yield. This is separate from the engine-managed section named `loop`.

Switch owns one prepared selector and ordered case/default arms. Each arm owns one body and an ordered list of numeric labels in a shared pool; several labels can select that same body. A separate Default flag permits Default to share a body with numeric labels. Grouped labels do not create duplicate bodies or artificial fallthrough. The selector runs once per switch entry. Labels are distinct signed 32-bit constants across the whole switch (including within an arm); at most one Default may appear, at any position. Every arm needs a numeric label or Default. Label order is retained, and `caseLabels(ArmId)` returns its immutable label range. Default is selected when no label matches. Normal arm completion exits the switch unless explicitly marked Fallthrough; fallthrough enters the next arm body without checking its label or reevaluating the selector. The last arm cannot fall through. An empty switch still evaluates its selector. Later lowering must honor native comparison conventions without silently introducing numeric clamping/coercion.

Break targets a lexically enclosing conditional, loop or switch, including from its preparation. Continue targets a lexically enclosing loop and must originate inside that loop's body, possibly through nested constructs. It cannot target its own initialization/check/update. Those phases may still contain a transfer to an enclosing outer loop body. Sibling, foreign-script, absent or incompatible targets fail at construction. `transferDestination` exposes an after-construct exit for Break and the appropriate update/check sequence for Continue, using stable authored identities. It describes intent for later lowering, not emitted instructions.

Module ancestry traverses all sequence-owner kinds, preventing overlapping ownership through a condition preparation or loop phase. Direct indexes include condition results and each preparation/body/update action independently. They make no assertion about which iteration or branch executes. No generated jump scaffolding is retained as a second editable authority.

## Storage and lifetime

Action and value records use separately allocated 256-row pages. Growing their page directories does not move existing rows. Variable-size children and UTF-8 bytes use pooled flat tables; each value stores a checked 32-bit range. Pools retain no recursive heap object per SCPT operation. Shared immutable order buffers keep execution order separate from identity. A builder creates these buffers once at publication.

`ValueId` is an internal pool handle, not authored identity. Action IDs, arm IDs, condition IDs and operand addresses identify edit sites; operation ordinals are revision-scoped. Returned pointers, spans and string views are valid while their immutable Program is retained. WP2 will add scoped readers, local replacement ranges, prepared deltas, history and reclamation; it must not implement an edit by reconstructing this whole bulk builder. WP1 has no live mutable service, cache, save serialization or timing promises.

## Typed pool contract

Value children are written before their parent, so value graphs are acyclic. `data` is 64-bit; `detail` is 32-bit. The fields below describe this in-memory model, not a finalized disk ABI. WP4 will version portable tables explicitly.

| Kind | data | detail | Children |
|---|---|---|---|
| Instruction | Full opcode | Bit 0 skip-refresh, bit 1 scheduled | Optional schedule first, fixed Parameters, ordered RepeatedGroups |
| Parameter | Schema index | 0 | One typed value |
| RepeatedGroup | 0 | 0 | Ordered Parameters; ordinal is its place among groups |
| Word | Exact 32-bit word | 0 | None |
| Expression | SPICE termination enum | 0 | Ordered SCPT operations |
| ValueOperation | Exact encoding word | SPICE value-kind enum | Exact payload Words |
| BinaryOperation | Exact encoding word | SPICE binary-kind enum | None |
| StackOperation / InertOperation | Exact encoding word | 0 | None |
| Words / StringGroup | 0 | 0 | Ordered Words, including section preambles |
| ActionReference | Authored action ID | 0 | None |
| IndexedTextReference / SupplementaryTextReference | Authored text ID | 0 | None |
| Unresolved | Expected SPICE target storage | 0 for absent text kind, otherwise enum + 1 | Original encoded Words |
| Unavailable | Source item size/count | 1 expression, 2 parameter, 3 text, 4 section | None; no opaque payload |
| PlainText / TextChunk | Character-pool offset | UTF-8 byte count | None |
| Message | Whether header exists | 0 | Optional PlainText header then ordered chunks/commands |
| TextCommand | SPICE command enum | SPICE argument alternative | Decimal argument (including absent value) or ordered byte arguments as Words |
| EmptyIndexedText | 0 | 0 | None |
| IndexedSection | Authored text ID | 0 | Preamble Words value |

This retains full supported typed content even when gameplay meaning is unknown. Structural input errors fail at bulk construction. Unresolved operands are drafts with separate diagnostics. Source-only bytes are excluded; unavailable content stays visibly unavailable rather than becoming an empty valid instruction or text. The eventual exporter must fail gracefully for unsupported executable content. Baseline-qualified `sourceInstruction`, `sourceSection` and `sourceText` observations permit evidence lookup without treating SPICE IDs as global identities.

## Import and direct queries

`appendImported` consumes an already imported public SPICE document by const reference and retains only normalized content and qualified observations. Its temporary target maps are local to one script. No extra SPICE analysis, parser pass or document clone is required.

`appendNative` checks the supplied source revision against the bytes, calls SPICE parse once and document import once, then normalizes through the same builder. It retains the selected recipe, diagnostics and output byte-order/wrapper/header observations. Native graphs are local and released before return. The caller owns file IO and external evidence storage. A failure before normalization leaves the builder unchanged so a collection loader can explicitly decide whether to omit a failed source; a failed normalization poisons the bulk transaction.

Bulk completion builds pooled contiguous variable-use and incoming-action-target postings. Buckets store ranges; there is no separately allocated vector per referenced instruction. Each site includes action, fixed parameter or repeated-group ordinal, expression operation position, and condition/optional arm IDs for structured checks. Preparation actions contribute their own reads/writes and retain their owned sequence identities. Schedule reads remain distinct from parameter reads. Incoming postings distinguish ordinary operands from authored Break and Continue targets; none is automatically classified as a call. `nextInSequence` reports stored order only, not execution reachability or fallthrough across section boundaries. Section order remains available for later control-flow analysis.

Reads recognize SPICE's Bit/Byte/Int/Float variable operations, including low-16 comparison and float-backed IntVar forms. Secondary-value operations retain their exact type without pretending to be ordinary variables. Fixed destination indices recognize raw words, integral fixed-point decimals and integral float payloads. Computed destinations have a separate posting list rather than being dropped or assigned a guessed index. The destination rules cover assignments 5/6/7 and bit writes 17/18/19; opcode 19 is recorded as modification with unspecified resulting value. Set/clear facts mean the instruction occurs, not that its path executes or that it establishes a final story state. No entry-distance heuristic filters writes.

WP3 now supplies transitive helper observations and scoped module summaries; storyline reconstruction is not implemented. See [semantic contracts](WholeProgramIrSemantics.md). Per-source contribution replacement and reclamation are implemented by the WP2 service. WP1 does not build writer-reader Cartesian products, format whole story views or perform a corpus survey as a side effect of import.

## Delivery

WP1 uses four focused synthetic console checks: typed multi-script imports and direct postings across page boundaries; first-class conditional chains including nested bodies and sparse large identities; graceful malformed input and draft handling; and the native SPICE boundary for both byte orders. No GUI-loading or private-corpus tests are included. zstd level 1 remains selected for WP4; the current editor's persistence has not changed.

The structured extension adds two focused console checks covering prepared Else If/loop checks, all loop forms, switch arms/fallthrough/default, nested transfer destinations, condition indexing and invalid scopes/cases/ownership. Elevated v145 Release build and all six WholeProgramTest checks passed. Existing linked SPICE MLD PDB warnings remain. No GUI or corpus run; the earlier unrelated standalone SpiceTests build failure was not retried for this core-only change.

Shared-label extension: the existing structured tests now exercise several labels per body, Default sharing a body, authored label order, and rejection of duplicate/absent labels. The elevated v145 Release build and all six focused core checks passed; no GUI or corpus tests ran. Other suggested authoring conveniences are optional; see [optional authoring concepts](WholeProgramIrOptionalAuthoring.md).

## WP2 service

The bulk builder remains the import boundary. The [production service](WholeProgramIrService.md) now owns mutable tables, local changes/history, scoped immutable readers and bounded asynchronous native loading. Editing does not rebuild a complete Program or use a native document as the live authority. Full checkpoints and payload reclamation are explicit operations. WP3 provides coordinated structured and metadata commands; WP6 integrates the editor.

## WP3 metadata and derived queries

`VariableMeaning` adds project-wide or script-specific variable/value aliases with stable meaning IDs and explicit evidence. It remains cold authored metadata. Module summaries carry their own immutable payloads and dependency sets, separate from executable authority. Semantic receipts distinguish execution changes from aliases and language membership; no writer-reader Cartesian graph is stored. See [WP3 contracts](WholeProgramIrSemantics.md).

## WP4 binary persistence

The [binary project codec](WholeProgramIrPersistence.md) writes live content directly, with explicit field widths and portable remapped pool indices. Reopen reconstructs the owned paged tables and their derived postings without a native parse or executable JSON tree. Legacy v7 imports reuse the existing typed translation boundary and retain supported aliases and presentation folders.

## Cutscene interface follow-up

[Cutscene routes](CutsceneRoutes.md) describes typed entry/exit ports, alternative requirement paths, scoped connection/effect queries, and undoable commands operating on the same paged instruction storage.


## Authoritative imported and new groups

Imported and user-created groups share the same Action/Arm/ConditionRegion/Sequence storage. SPICE regions are transient normalization input only. Empty owned body sequences are first-class, persistent insertion targets; deleting all their items does not remove the construct. There are no view-local group identities to use in editing commands. `ConditionRegion::skipRefresh` and `Transfer::skipRefresh` retain control-instruction timing during import and lowering (binary schema 4). A scoped Break may exit a conditional, loop or switch.
