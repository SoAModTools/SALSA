# Cutscene interfaces and routes

The whole-program IR represents a cutscene as a module owned by one script. Its entry and exit slots are stable `Port` records. Several incoming routes may use the same entry. Slots and connections can carry aliases and attached explanation records through the existing annotations API.

This is a core authoring API. It does not add a graph canvas or a cutscene widget to the current Qt editor.

## Stored interface

An entry slot selects an existing action or the start of a sequence. An exit selects an exact instruction operand (including a switch case ordinal), physical fallthrough after an action, a terminal instruction, or a sequence end. Nested arm bodies can have their own sequence-end slots. Slots refer to content in their owning module; they do not copy the program.

A connection may have a `RouteDefinition`. Its method distinguishes arrival, battle return, direct handler, signal, named entry, internal call and unknown/user-unclassified entry. Its outcome distinguishes local transfer, return to caller, explicit loop transfer, script transition and scheduled call. Observation-only records do not change execution.

Each `RoutePath` contains a conjunction of requirements; multiple paths are alternatives. Requirements refer to existing instruction operands or structured condition IDs and retain true/false polarity or case labels. Preparation and consumption are ordered action references. Expressions continue to belong to executable IR. The current values can be inspected and edited with ordinary selected-value/condition commands. Editing a requirement does not require rebuilding a separate condition tree in the connection.

Unresolved endpoint bindings, evidence and incomplete observations can be saved. A connect command requires concrete targets. Requirements are user-selected logic, not a proof that a scene is reachable or a flag means “seen.” Bit numbers do not establish storyline order.

## Scoped query

`ProgramService::cutscene(revision, module)` returns detached interface records, incoming paths, outgoing control-flow edges and variable effects. It uses action-target postings for incoming references and scoped control-flow analysis for body/helper effects. Traversal stops at another declared module. Returns inside helpers are not described as cutscene exits. Effects retain their original action sites, including late writes, clears and non-story variables; callers can filter presentation using user meanings.

Inferred incoming guards include enclosing structured branches and bounded paths through the caller's local sequence. Exact switch operands retain their ordinal. Named activation and direct physical section fallthrough are separate observations. Other transfers remain visible with unresolved destinations where needed. This analysis does not execute calls to compute path state, solve arbitrary cross-section predicates, or establish runtime reachability. Cycles or excessive alternatives mark the result incomplete. Explicit paths can supply information the recognizer cannot recover. Inferred and authored records remain distinguishable and retain item-specific rationale/evidence.

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

Program binary schema 2 persists the interface, route paths, unresolved bindings and command observations using the existing paged IR checkpoint and zstd level 1 container. Container framing remains version 2. Old development-schema projects are rejected with a diagnostic; supported Python SALSA v7 import remains available and v7 export is not introduced.

Storyline reconstruction, flag allocation, portable annotation catalogs and the integrated story editor remain future features. MLD evidence is optional. No native parser changes, binary patches, footprint pruning or runtime simulator are part of this work.
