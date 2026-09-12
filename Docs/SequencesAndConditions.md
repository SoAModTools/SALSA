# Sequences and conditions

Open an imported script and choose **Document > Sequences and Conditions**. Changes use the same project Undo/Redo and Save Authoring Project commands as the physical editor.

## Sequences

**Name sequence** gives an existing section a name and stable action identities. Naming does not change the SCT output. A sequence is an editing unit; it does not assert that the section is a cutscene or module.

The action list shows calls, returns, branches, switches, waits, schedules, refresh settings, and explicit destinations. **Edit timing** changes a schedule expression or skip-refresh setting. An empty schedule means immediate execution. Leaving the schedule text untouched preserves its exact imported encoding.

Use **Go to action** to edit parameters, insert or move instructions, or work with the existing structured branch/switch editor. Action identities follow instructions moved between named sequences in the same script. New instructions receive new identities. Ambiguous control flow remains available in the physical editor.

## Named conditions

**Name condition** binds explicitly selected conventional branch conditions. Multiple uses must have exactly matching expressions. **Edit expression** changes every bound use as one project command. **Go to use** navigates to a selected occurrence. Renaming changes only the authored name.

For example, a condition called `Ready to leave town` can represent `BitVar[12] != 0 && ByteVar[87] == 1`. Names and variable aliases refer to existing storage; they do not allocate flags. Access views such as `Low16IntVar[15]` retain their exact meaning over the integer variable bank.

A physical edit that makes shared uses disagree is rejected. Remove the definition to edit those uses independently. Removing a definition retains the script instructions.

## Previous location

**Arriving from** replaces the location literal in a simple existing `Low16IntVar[15] == ...` condition. Compound conditions use the explicit expression editor. **Arrival switch case** changes an existing non-default case of a switch whose selector is exactly `Low16IntVar[15]`. It preserves case order and destinations and rejects duplicate locations.

The supplied convention maps `me033b` to `331`. The picker supports `me000a` through `me999j`; later letters collide with another script number and are unqualified. `10000` and `40000` retain tentative battle-return interpretations, while `20000` remains unresolved. The picker does not synthesize special returns or write to the previous-location staging field.

## Semantic output

Project export constructs a fresh SCT document from the understood instructions and text. SPICE generates its layout and references. Source padding, gaps, opaque sections, and unreferenced opaque text remain available in the immutable imported baseline; they are omitted from semantic output. Adding schedules or growing conditions is not constrained by their old offsets.

If an instruction refers to undecoded text, repair that text or select its encoding before exporting. Unknown executable operands and unresolved references likewise require interpretation. They are not silently removed or emitted as raw bytes. The current text editor supplies explicit repairs; automatic mixed-encoding interpretation is not provided by this slice.

A structurally valid semantic edit enters project history even when the script needs further work before export. Export verifies the generated program by reimporting it, including instruction order, parameters, targets, scheduling, refresh settings, and text. A failed export does not install output.

Named definitions are qualified by their imported baseline. Remove definitions before rebasing that script onto another source baseline, then promote them again. Automatic rebinding across source changes and extracting multi-section modules belong to later work.

## Implementation boundary

The project still records edits against an imported baseline and uses SPICE instructions as its current action representation. Fresh semantic lowering removes source-layout constraints; it does not yet replace that bridge with a complete independent module compiler. The original document workflow retains its separate preservation behavior.
