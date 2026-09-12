# Cross-script routes and coordinated output

Open **Document > Story State and Cutscene Effects**, then **Executable routes / export set**. Suggested story paths and adopted storyline steps remain narrative metadata. Creating an executable route is a separate, explicit operation.

## Select and review a route

Select a source action port, a destination arrival entrypoint, and a module response port. Existing module and entrypoint editors provide the organization and arrival bindings. The initial supported producer is opcode 43 with a literal filename. Selecting the destination explicitly supplies its game filename even if that selected imported footer text was undecoded. Other users of shared text retain their original content. The destination must use an existing simple `Low16IntVar[15] == location` comparison or an explicitly selected switch-case ordinal. The selected response must belong to that arrival arm; unsupported boundaries need explicit restructuring in sequence authoring.

A source port can also identify an insertion anchor. **Insert a load after the selected source action** uses the normal instruction and text insertion operations. Review and apply the insertion and destination edit together as one undoable command. Inserting after a return or another non-fallthrough action does not make the new action reachable; the author selects the desired boundaries.

Source prerequisites, destination requirements, existing effects, and continuation can be selected by stable action IDs from the sequence/module inspector. They are retained as observations. A requirement does not cause SALSA to write a flag. No route operation writes the previous-location staging variable. Arrival values use the existing qualified `me000a`–`me999j` encoding (number * 10 + letter index); later-letter collisions and special returns remain unresolved.

Two source actions from the same script may deliberately converge on one arrival. Different responses need an explicit discriminator. The first supported discriminator is a bit with a selected set/clear immediately before each load and a matching selected destination equality enclosing its response. Opposite values distinguish routes. These instructions must already be authored: SALSA never allocates a hidden flag or inserts its producer. Other discrimination structures remain ordinary sequence authoring and cannot yet establish disjoint route intents through this picker.

**Review both endpoint edits** shows the exact selected instructions, expressions, and retained physical operands. **Apply reviewed route** commits both endpoints against that revision. Undo restores both. Reconnection retains connection identity and endpoint action identities, except an explicitly requested insertion creates a new action. Detaching a route removes coordination intent while retaining its executable instructions.

Warp, saved-area/overworld return, battle, and ship-battle exit producers are listed by the Qt-free transition support contract as unqualified. Choose **Save unresolved intent** and provide an explanation to record unqualified transitions without changing existing instructions. Explicit unresolved route descriptors can be saved, but do not publish as resolved routes; tentative labels such as 10000/40000 do not qualify a producer.

Later instruction edits never trigger route replay. A changed observed operand, timing, response binding, or selected structure requires explicit reconnection. Low-level content changes and disabling a destination retain the route as an unresolved draft. Conditions and resource evidence are independent: missing or stale MLDs do not block these operations.

## Prepare an output set

In **Coordinated output**, select a seed script and load its route dependency closure. Choose one output directory, confined relative paths, and per-script platform, byte order, text convention, and compression. Connected outputs require compatible platform and byte order and retain the imported game-asset filename. Module aliases and host filenames are not script-load targets.

Preparation captures the current project revision and completes fresh materialization, activation lowering, encoding, round-trip validation, hashes, and realization mappings for every output before installing any file. The review lists selected endpoint/generated instructions. Editing may continue while preparation or installation runs; the result describes only the captured revision. Changing output controls invalidates their prior review.

**Install reviewed set** uses a journal under `.salsa-publication/transactions` and writes a receipt under `.salsa-publication/receipts`. File replacements are sequential and recoverable, not native multi-file atomicity. An output-directory mutex prevents concurrent SALSA publishers from recovering each other's active transaction. Expected destination hashes and confinement are checked again before installation; external modifications prevent overwrite/recovery.

Cancellation rolls back recognized states when possible. Interrupted commits can roll forward on restart from staged, hashed artifacts, or roll back if staging is unavailable. Unrecognized external state leaves a blocked journal for inspection. Recovery runs before another set is prepared or installed. A receipt is successful only after the complete transaction is verified; its output hashes must still match before it describes current files. Receipt mappings are qualified by project, captured revision, script, and realization identity.

Single-script export remains available for scripts outside a route closure. A script participating in an executable route requires coordinated output of its closure; historical companion receipts do not bypass this initial rule. No half-route publication is silently accepted.

The original imported baseline is immutable. Exporting to a separate directory does not require the original SCT or optional MLDs to remain current on disk. Replacing an original SCT requires explicit authorization and the exact imported source hash. Game-binary packaging and runtime compatibility certification remain outside SALSA.

## Persistence and APIs

Semantic schema 11 includes route descriptors, unresolved evidence, and stable narrative occurrences. Presentation schema 3 includes occurrence placements. Container encoding remains version 1. Supported Python SALSA v7 import continues through the existing importer and does not need route descriptors.

`SctRouteAuthoring` provides support information, validation, create/reconnect/detach, low-level reconciliation, and dependency closure. `SctPublicationSet` separates reviewable preparation from installation and receipt verification. Both use the same core authoring materializer; the existing editor publication service refuses unresolved or companion-dependent route output. The publication journal and receipts are derived host output artifacts, separate from authored project identity.
