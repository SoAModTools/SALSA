# Cutscene interfaces and routes

The whole-program IR represents a cutscene as a module owned by one script. Its entry and exit slots are stable `Port` records. Several incoming routes may use the same entry. Slots and connections can carry aliases and attached explanation records through the existing annotations API.

The core API feeds the existing semantic outline. This does not add a graph canvas or a cutscene widget.

## Stored interface

An entry slot selects an existing action or the start of a sequence. An exit selects an exact instruction operand (including a switch case ordinal), physical fallthrough after an action, a terminal instruction, or a sequence end. Nested arm bodies can have their own sequence-end slots. Slots refer to content in their owning module; they do not copy the program.

A connection may have a `RouteDefinition`. Its method distinguishes arrival, battle return, direct handler, signal, named entry, internal call and unknown/user-unclassified entry. Its outcome distinguishes local transfer, return to caller, explicit loop transfer, script transition and scheduled call. Observation-only records do not change execution.

Observed `RoutePath` records contain conjunctions of existing operand/condition sites; multiple paths are alternatives. They retain polarity, case labels and ordered preparation/consumption references. These are observations of shared source structure, not independently editable cutscene contracts. Promote a selected shared region to route dispatch before independently changing its entry requirements.

Unresolved endpoint bindings, evidence and incomplete observations can be saved. A connect command requires concrete targets. Requirements are user-selected logic, not a proof that a scene is reachable or a flag means “seen.” Bit numbers do not establish storyline order.

## Independent route requirements

A `Dispatch` action owns ordered arms. Each arm owns its complete logical condition and one body sequence. For example, shared `P` around `If Q: A` and `If R: B` becomes route A requiring `P && Q` and route B requiring `P && R`. Removing P from A leaves B unchanged. Stable arm IDs identify routes; a connection can bind to its arm through `RouteDefinition::dispatchArm`. Conditions are executable IR, not a second expression tree stored in story metadata.

`PromoteDispatch` recovers pure shared wrapper guards around independent If routes or one If/Else If/Else chain. It preserves the root action, leaf arm and body identities. Optional explicit body-to-connection bindings are part of the same atomic operation. Recorded requirements outside the selected region remain attached; promotion replaces only the checks it consumes. `SetRouteRequirement` and ordinary semantic `ReplaceCondition` edit the same condition root. Undo restores the prior structure; invalid edits leave the project unchanged. Promotion attaches its rationale to the dispatch annotation while retaining an existing alias and notes.

In the semantic editor, select the structured outer If and choose **Promote shared guards to route dispatch**. Expand **Route dispatch**, then a **Route**, and edit its **Condition**. Edit its ordinary instructions in the same body. Raw instruction flow can first use the existing explicit-flow promotion. The core connection API accepts an explicit route binding; an integrated story-view binding picker remains future work.

Independent routes remain ordered and may all run. First-match routes retain priority and stop at the first matching body. Timing samples record when original pure atomic comparisons were read. They are separate from the route's editable condition. Unchanged comparisons keep their original read phase; newly authored comparisons read at that route. Consequently, A changing P does not cause B to read a shared P later than before. Samples are inspectable through the scoped core reader, but cannot be edited as ordinary conditions.

Lowering builds a bounded decision graph using branches and jumps, without temporary game flags. Multiple native copies of a body may be needed to carry earlier decisions through later routes. There remains one authored body, and instruction realization records include every emitted copy. Ordinary body scheduling and refresh fields are retained. Fresh native export uses this same lowering; no saved generated instructions become authoring authority.

Current recovery accepts at most 32 source guards, 16 routes and 32 atomic samples; lowering is bounded to 2,048 states/decisions. Conditions must be pure typed SCPT expressions with empty instruction preparation. Mixed independent/first-match sibling groups, cross-boundary jumps into route bodies, and identical comparisons observed at different phases within one route require keeping that region explicit. Stack-manipulating, inert or secondary-value SCPT programs are not rearranged. This bounded implementation does not recover arbitrary control flow or move instruction preparation. Route ordering and timing are currently fixed by promotion; body and condition editing are supported.

## Scoped query

`ProgramService::cutscene(revision, module)` returns detached interface records, incoming paths, outgoing control-flow edges and variable effects. It uses action-target postings for incoming references and scoped control-flow analysis for body/helper effects. Traversal stops at another declared module. Returns inside helpers are not described as cutscene exits. Effects retain their original action sites, including late writes, clears and non-story variables; callers can filter presentation using user meanings.

Inferred incoming guards include enclosing structured branches and bounded paths through the caller's local sequence. Promoted first-match routes also report the required failure of earlier routes; these priority exclusions are derived from the dispatch order. Module-summary dispatch edges carry `dispatchMode` and describe logical routes, not a raw instruction CFG. Exact switch operands retain their ordinal. Named activation and direct physical section fallthrough are separate observations. Other transfers remain visible with unresolved destinations where needed. This analysis does not execute calls to compute path state, solve arbitrary cross-section predicates, or establish runtime reachability. Cycles or excessive alternatives mark the result incomplete. Explicit paths can supply information the recognizer cannot recover. Inferred and authored records remain distinguishable and retain item-specific rationale/evidence.

The result owns its selected value buffer. Requirement values follow path/requirement order. It can be kept by an editor without pointers into mutable pages. Queries are cached by module and invalidated by intersecting action, sequence, text and target-bucket changes. New incoming edges invalidate a module even when its previous bucket was empty. Layout changes and cold metadata edits conservatively invalidate cutscene views. This does not rebuild executable postings or serialize a project snapshot.

## Commands and execution

`ConnectSlots` is a semantic operation in the existing edit batch. It changes authoritative instructions and records the connection in one undo step:

- Retarget an exact action operand, preserving other operands, scheduling and refresh modifiers.
- Insert a jump after a selected physical-fallthrough action or at a sequence end.
- Replace a selected terminal with a local transfer, return, or explicit jump to the existing `loop` body.
- Insert a scheduled call with an explicit schedule expression. Its continuation remains in place; scheduling is not scene completion.
- Retarget an existing literal load (opcode 43) to the selected destination script and retarget the explicitly selected arrival response. The arrival's conditions remain authored logic. Change them explicitly in the same edit batch if required. The command does not invent a previous-location test or dispatch block.

Literal-load editing creates a separate footer filename when it changes, preserving any shared original string. Reconnecting the same filename reuses the existing text. Cross-script action references are never created: the source keeps a filename reference and the destination handler keeps a local response reference. Other native transition forms can be recorded as observations; they are not silently rewritten by this command.

A reconnect reuses its recorded implementation action. If a low-level edit moved that action away from the selected boundary, the query reports the detached binding and reconnect asks for explicit boundary reselection. Return-to-caller and resume-loop outcomes do not use the destination-port field. Ordinary low-level edits remain authoritative; the query reports a changed target/binding instead of export replaying old metadata. Removing connection metadata removes the description, not its executable instructions. Undo reverses an actual connect operation. Revision checks reject stale commands before installation. Invalid requests fail atomically through the existing service result contract.

Materialization maps slot and connection identities to their actual instruction locations. It consumes the same authoritative IR as ordinary semantic edits. There is no second editable SCT document, whole-project conversion on connection refresh, or separate verification operation.

## Persistence and boundaries

Program binary schema 3 persists the interface, route paths, unresolved bindings, route dispatch conditions and timing samples, and command observations using the existing paged IR checkpoint and zstd level 1 container. Container framing remains version 2. Old development-schema projects are rejected with a diagnostic; supported Python SALSA v7 import remains available and v7 export is not introduced.

Storyline reconstruction, flag allocation, portable annotation catalogs and the integrated story editor remain future features. MLD evidence is optional. No native parser changes, binary patches, footprint pruning or runtime simulator are part of this work.
