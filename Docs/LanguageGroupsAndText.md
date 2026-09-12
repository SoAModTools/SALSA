# Language groups and text authoring (S7)

Open **Document → Language Groups and Text** for the active imported script.
Languages are project-wide groups with stable IDs and user-defined names. A
string may be unassigned, assigned to one group, or shared by several groups.
There is no prescribed locale list or requirement to fill every group.

## Discovery and correction

Opening the editor discovers indexed and footer text resources. Indexed section
names and native string-group markers supply initial, editable suggestions:

- Ordinary legacy F/S/G markers use character 1 of the string name, corroborated
  by the group suffix (lowercase or uppercase) or an unmarked counterpart.
- The legacy `palyoko` convention uses character 7 of a nine-character name.
- Short, unmatched, conflicting, and duplicate candidates remain unassigned.
- An unmarked counterpart is labelled **Unmarked**, without assuming English.
- Footer text has no indexed-name evidence and starts unassigned.

Create or rename groups, select strings, select their desired groups, and apply
the assignment. Selecting no groups explicitly unassigns the strings. Explicit
assignments survive rescanning. Grouping never rewrites section names, references,
ByteVar selectors, predicates, or output encoding. Source names and the basis for
inference remain separate evidence.

## Logical text and execution uses

A logical text links explicitly selected resources belonging to one script. It
may belong to a module; the module filter also shows text used by that module.
Suggested correspondence keys help find likely translations, but do not create
execution routes or prove correspondence.

Edit selected resources together using the existing message/plain-text editors.
The batch is one project-history command. Every value must encode under the
selected text convention before any edit is accepted. A shared resource is edited
once, affecting all its uses. **Split** creates a separate resource for one of its
groups and retargets only the exact uses selected by the user. Other callers
continue to use the original resource.

Deleting an unreferenced text through the ordinary editor also removes its group
membership and logical-text references in the same history command. The logical
text retains its identity and can remain incomplete; undo restores the resource
and its assignments.

Create new indexed or footer text resources, then insert uses at explicit action
locations. Select an existing operation as the template and an action before
which to insert it within the intended route. The insertion batch retains the
template's other operands, result behavior, scheduling, and refresh settings.
Indexed dialogue, indexed choices, footer choices, and generated-list operations
keep their original opcode; choosing a language group never invents dispatch or
fallback behavior. Physical fallthrough includes the new operation. Optionally
select one incoming reference, including a switch-case operand, to enter the new
operation too; unselected references keep their original targets. A case reached
only by a jump needs that explicit incoming-edge selection. Independent operations
on different arrivals to one shared anchor require separate explicit blocks.
Action promotion and insertion commit together; cancelling selection changes no
project state.

## Persistence and boundaries

Private semantic schema **8** stores group IDs, resource identities and membership,
explicit-assignment state, source evidence, and logical correspondence. Text values
and instruction operands remain in the owned authored program; the catalog is not
a second document authority. Presentation and compressed-container versions are
unchanged. Project history, save/reopen, core materialization, and editor
publication use the same content. Baseline replacement with text bindings requires
explicit reconciliation rather than matching fresh physical IDs accidentally.

Language labels do not choose encoding, regional packaging, runtime selector
values, or supported platforms. Existing Python SALSA v7 import remains supported;
discovery can be run on the imported authored script. Opaque text requires explicit
repair before it can become executable authored text. New-script creation,
automatic language dispatch, library pruning, and graph authoring are outside S7.
