# Production program model (WP1)

WP1 introduces `SalsaCore/Program`. The existing editor continues using its old authority until WP6; no synchronization or duplicate live document is installed. The new core model has no Qt dependency and imports no evaluator implementation.

## Ownership and authored structure

A `Program` is an immutable completed bulk build with project UUID, semantic revision and allocator high-water mark. It owns imported script contexts, baseline descriptors, section order, code sequences, paged actions, pooled typed values, texts and cold authoring metadata. It owns no SctDocument, physical patch, JSON tree or baseline bytes.

`ProgramBuilder` is an unpublished construction transaction. A failed mutator prevents `finish()` from publishing any program. Its `importedScript`, `section`, `textEntity`, value and action inputs also provide the boundary for later decoded binary tables. Every script requires a baseline descriptor; these are not new-script creation UI operations. Existing requested script/action/section/text IDs can be retained. Allocation uses monotonically increasing nonzero 64-bit IDs across entity kinds; source names, physical IDs and page positions do not determine identity. An occupied sparse identity-directory page holds 1,024 locations, so large retained identities do not allocate a giant dense array.

An instruction action owns one pooled instruction payload. A conditional action owns an ordered range of stable arms; each arm owns a condition and a distinct body sequence. Only the final Else arm omits its condition. The first arm is If; subsequent conditional arms are Else If. Each body directly owns its ordered actions, which can include nested constructs or explicit typed instructions. There is no Then wrapper or generated-jump scaffold in authoritative content.

Native import initially constructs explicit instruction sequences. It does not reinterpret arbitrary branches as structured chains. WP3 adds recognition/promotion and semantic edits, including treatment of incoming edges into proposed branches. WP5 lowers constructs to derived explicit flow and maps generated instructions to their owning actions/arms. The inspectable lowered form is not a second editing authority. Explicit instruction references, including irregular control flow, remain representable throughout.

Root sequences belong to sections. Nested sequences belong to arms. Modules own selected sequences within one script; ownership cannot overlap another module or an enclosing owned sequence. Entry references and shared calls do not create new ownership. Metadata includes entry activation/response bindings, ports/connections, user language groups/logical text, aliases and item evidence. Script contexts retain optional MLD associations and legacy capsule origin. No resource freshness check gates bulk construction.

Imported `init` and `loop` code sections produce required, enabled lifecycle entries pointing directly at their bodies. Module extraction and response selection are unnecessary. Missing roots produce draft diagnostics. Export requirements and command protections are implemented at their WP2/WP3/WP5 boundaries; WP1 does not export or manufacture missing root bodies.

## Storage and lifetime

Action and value records use separately allocated 256-row pages. Growing their page directories does not move existing rows. Variable-size children and UTF-8 bytes use pooled flat tables; each value stores a checked 32-bit range. Pools retain no recursive heap object per SCPT operation. Shared immutable order buffers keep execution order separate from identity. A builder creates these buffers once at publication.

`ValueId` is an internal pool handle, not authored identity. Action IDs, arm IDs and operand addresses identify edit sites; operation ordinals are revision-scoped. Returned pointers, spans and string views are valid while their immutable Program is retained. WP2 will add scoped readers, local replacement ranges, prepared deltas, history and reclamation; it must not implement an edit by reconstructing this whole bulk builder. WP1 has no live mutable service, cache, save serialization or timing promises.

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

Bulk completion builds pooled contiguous variable-use and incoming-action-target postings. Buckets store ranges; there is no separately allocated vector per referenced instruction. Each site includes action, fixed parameter or repeated-group ordinal, expression operation position, and an arm ID for structured conditions. Schedule reads remain distinct from parameter reads. Explicit targets are operand references, not automatically calls. `nextInSequence` reports stored order only, not execution reachability or fallthrough across section boundaries. Section order remains available for later control-flow analysis.

Reads recognize SPICE's Bit/Byte/Int/Float variable operations, including low-16 comparison and float-backed IntVar forms. Secondary-value operations retain their exact type without pretending to be ordinary variables. Fixed destination indices recognize raw words, integral fixed-point decimals and integral float payloads. Computed destinations have a separate posting list rather than being dropped or assigned a guessed index. The destination rules cover assignments 5/6/7 and bit writes 17/18/19; opcode 19 is recorded as modification with unspecified resulting value. Set/clear facts mean the instruction occurs, not that its path executes or that it establishes a final story state. No entry-distance heuristic filters writes.

Transitive calls, module summaries, guards, storyline paths and dirty dependency buckets belong to WP3. Per-source contribution replacement and reclamation belong to WP2. WP1 does not build writer-reader Cartesian products, format whole story views or perform a corpus survey as a side effect of import.

## Delivery

WP1 uses four focused synthetic console checks: typed multi-script imports and direct postings across page boundaries; first-class conditional chains including nested bodies and sparse large identities; graceful malformed input and draft handling; and the native SPICE boundary for both byte orders. No GUI-loading or private-corpus tests are included. zstd level 1 remains selected for WP4; the current editor's persistence has not changed.
