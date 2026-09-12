# Entrypoints and resources

Open an imported script and choose **Document > Entrypoints and Resources**. Select a base or auxiliary MLD and use **Inspect / refresh MLD**. Selection is explicit; a matching filename does not establish a resource relationship. Streamed tile relationships are deferred pending research.

The list exposes lifecycle sections, arrival predicates, bit consumers, saved bindings, and entries in the selected MLDs. Resource entries show table index, physical entryId, tblId, function, and source revision. A numeric handler-name match is a candidate association, not proof of runtime dispatch. All entries remain available for inspection; their function must be considered before asserting a handler relationship. Same-name handlers in different scripts remain independent.

## Connecting a response

Create or extract a module using **Script Modules**, then select an entrypoint/resource candidate and choose **Connect response to module**. Select an action port within a module owned by the same script. For an existing entrypoint, choose its existing call/branch target operand, or identify a section that already starts at that module port. The operand picker shows opcode, instruction, parameter, and repeated-group ordinal. Selecting a false-branch target retains that branch's actual polarity; it does not turn it into a positive event.

For an absent direct handler, SALSA creates a section named from the selected tblId containing an explicit call and return. It rejects duplicate handler names and appending after an unterminated trailing sequence. Existing handlers require explicit selection of their body or response operand. A response can enter a module at an interior action. Empty modules need an action before they can receive execution.

Lifecycle bindings use existing `init` or `loop` sections. Arrival bindings retain a predicate or switch selector reading `Low16IntVar[15]`; use **Sequences and Conditions** to edit the previous-location comparison or select an existing switch case. S6 does not invent new arrival dispatch tables or alter the meaning of tentative special return values.

Signal consumers retain their physical bit predicate and ordered response. The eventhook picker offers matching parameter-8 candidates as **inference**; selecting one records a user assertion separately from source observations. Unknown providers remain explicit and saveable. The inspector lists the complete section, including explicit clears, caller preparation, waits, and continuation. Retargeting changes only the selected existing operand. Predicates remain level checks, and consumption instructions keep their original order. Source order alone does not prove control-flow dominance or engine signaling behavior.

## Resource qualification

A selected entry records dataset fingerprint, normalized relative resource key, exact source revision, imported-document identity, table index, SPICE document-local ID, physical entryId, tblId, function, and function parameters. Table index and tblId are never interchangeable. Reloaded or reordered entries require explicit reselection; selecting a saved binding opens the same response editor and preserves the entrypoint ID. An incomplete catalog reports missing input without deleting bindings or claiming that handlers are orphaned.

The adapter verifies source hashes and delegates MLD parsing to SPICE. Its deterministic imported-document discriminator is `spice-mld-s6-v1:<source digest>`; changing that import contract requires changing the discriminator. Binding checks also compare all captured entry fields, including the document-local ID. Resource catalogs are transient read-only views, while selected binding descriptors persist in the compressed project. MLD bytes are not copied into authoring content. A newly selected resource snapshot may have a newer dataset fingerprint than the script's immutable imported baseline; explicit reselection updates only the resource binding.

Load, wait, and release observations retain section/instruction order, target text when understood, and schedule/refresh indicators. They are not a computed global residency timeline. Available entries do not establish currently loaded resources, simultaneous tblId scope, or runtime dispatch precedence. Resource asset editing remains outside SALSA.

Publication through the editor checks execution bindings and reloads each selected MLD before publishing. A changed or unverifiable selected resource blocks publication with a resolution diagnostic. An explicitly unknown signal provider does not prevent exporting its already-authored physical predicate and response. The lower-level in-memory materializer validates the authored program; callers supplying external resource context should use `SctEntrypointAuthoring::verifyResources` before publication.

## Ownership and persistence

Entrypoints, module ports, and selected responses use stable authoring identities and the existing session's revision checks, Undo/Redo, save, and reopen. Reparenting is unsupported. This slice uses entrypoint response bindings; arbitrary port-to-port graph execution, new event scheduling, streamed tile relationships, and a 3D viewer remain outside S6.

[SmoresScript](../SmoresScript/README.md) contains the portable relationship definitions and static queries. It depends only on standard C++ and public immutable SPICE types. SALSA supplies resource context and owns authored decisions, serialization, history, and UI. The library is intended for transfer to SMORES without taking a SalsaCore or Qt dependency.
