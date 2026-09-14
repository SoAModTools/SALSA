# Whole-program semantic editing and relationships (WP3)

WP3 adds semantic commands and scoped derived queries to the Qt-free `ProgramService`. Edits use the WP2 journal and paged authority. They do not construct, serialize or readopt an SCT document. The existing editor still uses its previous authority until WP6; persistence and fresh lowering remain WP4 and WP5.

## Commands

An `EditRequest` may mix ordinary primitives with `SemanticOperation` values. One batch prepares and publishes once and produces one undo item. Recoverable command failures leave published content unchanged. Existing project, revision, generation and cancellation checks still apply.

| Operation | Authored effect |
|---|---|
| `InsertConstruct` | Create a conditional chain, switch or While/DoWhile/For with owned bodies, condition preparation and loop phases |
| `ReplaceArms` | Insert, remove or reorder conditional/switch arms, edit conditions, shared labels, Default membership and case exit |
| `MoveAction`, `EraseAction` | Move a complete action/construct within its script, or recursively remove its owned content |
| `InsertTransfer`, `RetargetTransfer` | Author Break/Continue using stable enclosing construct identities |
| `PromoteFlow` | Explicitly request bounded recognition of native instruction flow |
| `BindLifecycle` | Explicitly select an existing code section as a missing required init/loop root |
| `PutMetadata`, `EraseMetadata` | Edit modules, entrypoints/activation/responses, ports, connections, logical text, language groups and variable meanings |
| `PutAnnotation` | Set an item's alias and attached direct, inferred or user-provided evidence |
| `AssignLanguages` | Explicitly assign a text resource to user-defined language groups |
| `ScriptSettings` | Edit script name, output profile and optional resource associations while retaining the imported baseline |

Zero IDs request allocation; updating existing arms or metadata retains their IDs. An existing arm keeps its body sequence when reordered or relabeled. Preparation, body, initializer and update sequences accept ordinary instruction and semantic commands. `ReplaceCondition` edits the result expression independently. Transfers remain attached to their target constructs when moved; a move that breaks lexical scope is rejected. Continue targets loops and must occur in a loop body rather than its check, initializer or update. Switch labels must be unique, only one arm can include Default, and the final arm cannot fall through.

Required lifecycle identities cannot be disabled, removed, renamed or reassigned to another body through entrypoint updates. Their contents remain editable. Missing-root drafts can explicitly bind a chosen existing body; SALSA does not invent one or synthesize startup calls. Optional entry activation descriptors and unresolved bindings remain authored metadata. MLD associations are advisory and can be absent. Activation lowering is WP5.

Script/module entry ownership and resolved references are checked at the normal command boundary. A coordinated batch can repair metadata and execution references together. Deleting content still referenced outside that batch fails. A metadata edit copies only the affected cold table once per batch, not every metadata table or the executable project. Metadata checks may walk the cold tables; instruction fact extraction stays scoped to affected sources. `PutAnnotation` cannot be mixed with the older index-addressed `EditAnnotation` in the same batch; use subject-addressed updates consistently.

History retains inverse structured rows, owned orders, changed table sides and protection sets. Maintenance additionally compacts retained arm-descriptor and case-label ranges without changing public IDs, including ranges needed by undo/redo. Fixed construct-record capacity may remain allocated; this is not a claim that every table shrinks. Payload maintenance retains its WP2 explicit-operation cost and lifetime contract.

## Bounded native recognition

`PromoteFlow` receives a header and explicit continuation in one sequence. It recognizes plain If[/Else] diamonds and While back-jumps using typed condition and target operands. It requires normal refresh, no scheduling on consumed control instructions, supported exits and no foreign incoming edges into the promoted interior. Irregular or nested explicit branch regions remain instructions and return an operation diagnostic when promotion is requested.

The header and surviving body action IDs and source-instruction observations remain stable. Consumed glue jumps are retained by undo history. An inference record is attached to the promoted construct explaining the rule. General native Else If, Switch and complex loop recognition is not implemented by this bounded recognizer; those constructs can be authored directly. Recognition does not equate a native section boundary with a module boundary.

## Scoped derived data

`summarize(revision, module)` returns an immutable, cached `ModuleSummary`. Its value handles belong to its own payload; it owns no live pointers into the authority. It covers every declared module action, including late writes and clears, and follows explicit call/jump/branch targets into shared helpers. Structured bodies and preparation/loop phases participate. Physical fallthrough follows ordered code sections without inventing calls or returns. Missing destinations, unsupported control and computed targets are explicit unknowns. Directed cycles are flagged; this is not a claim about infinite execution.

Effects retain variable-access sites and expressions. Flow records retain source identity, role/polarity, target or target condition, operand address, arm identity, predicate, scheduling, refresh and source-instruction evidence. Loop descriptors preserve the distinct phase order. Break/Continue retain their lexical target as well as available continuation locations. These are conservative relationships and observations, not a simulator or a claim that a write unconditionally establishes final story state. Empty sequence and condition endpoints remain meaningful and must be interpreted through the structured descriptors rather than flattened into invented instructions.

Native transition opcodes produce observations with unresolved destination semantics; authored connection targets are returned independently by `interactions`. No runtime storyline ordering or final bit-state reconstruction is inferred. Story presentation can select user-classified bits and relevant byte state; detecting an IntVar access does not make it story progress.

Relationship keys use source script/action, role and stable arm ID. Native repeated operands retain their ordinal; authored arm reordering does not change identity just because its position changed. The summary records sorted action/sequence/section/text dependencies, including shared helpers and empty owned sequences. Changes invalidate intersecting summaries and changed module membership. Unrelated cached summaries remain reusable through edit, undo and redo. The invalidation pass inspects cache entries and searches their sorted dependency lists using the changed IDs; it does not reanalyze every module.

Direct `uses` and `incoming` buckets continue to replace old and new source contributions, including previously empty buckets. Consumers can query a bit's readers and writers without building their Cartesian product. Receipts separate execution changes from presentation text membership, aliases and metadata changes. Labels and resource evidence do not require re-extracting instruction facts.

`interactions(revision, script)` returns the selected script's entrypoints, authored connections and item annotations, plus applicable variable meanings and language definitions. `explain(revision, subject)` retrieves an item's alias and attached evidence on request. Neither query renders a whole story view. Meaning assertions and inferred evidence remain distinct; an observed flag write is not automatically named “cutscene seen.”

## Delivery

Three focused semantic console scenarios cover structured ownership/transfers and history, scoped summaries with shared helpers and separate presentation changes, and native promotion with source identity. They run alongside four service fixtures, six production IR fixtures and two existing synthetic v7 regressions. No GUI or private-corpus tests are part of this slice.

The elevated MSVC v145 Release SalsaCore/SalsaTests build passed, and all 15 focused console checks passed. Existing LNK4020 debug-symbol PDB warnings remain; details are recorded in the ignored WP3 checkpoint. The standalone SpiceTests executable remains unavailable after the earlier unrelated SpiceMix/PVM include failure; the existing production IR native fixtures exercise public SPICE SCT parsing/import in both byte orders. No submodule changes are required.
