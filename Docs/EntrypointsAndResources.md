# Entrypoints and resources

Open an imported script and choose **Document > Entrypoints and Resources**. The inspector works without MLD files. Optional base, auxiliary, and streamed-tile MLDs add entry discovery and source context.

## Required lifecycle

Every exported script requires nonempty `init` and `loop` sections. `init` runs once before the VM uses `loop`; `loop` remains the permanent bottom stack entry. Both are always enabled. Their bodies remain editable, including optional activation routes inside them. Commands cannot remove or rename existing lifecycle section identities.

An imported script missing a root remains an inspectable, editable, saveable draft. Repair the missing root explicitly before export. SALSA does not synthesize startup calls or infer a replacement body.

## Declaring and connecting entries

Use **Declare named entrypoint** to expose an existing section or create an editable named body, including an `M` handler without an MLD. A new body has a leading true condition and a return. Its declaration does not claim the engine invokes it.

Create or extract a module using **Script Modules**, then use **Connect response to module**. Select a module action port in the same script and the existing response target operand. The picker retains opcode, parameter, and repeated-group ordinal. A false-branch operand retains its actual polarity. A named or lifecycle body can be declared without extracting it into a module.

Arrival and signal candidates retain their existing physical predicates. Arrival predicates read `Low16IntVar[15]`; signal consumers read a selected bit. Provider inference is separate from execution targets and user assertions. SALSA makes no runtime-reachability assurance.

## Enabling and disabling

Optional entries have durable enabled state. **Activation boundary** defines what disabling bypasses; **Enable / disable entrypoint** changes the state through project history. Required lifecycle entries have no disable control.

For a named entry, disabling emits an inert external wrapper and retains its original body under a generated private label. Other instruction references continue to target the original body. The wrapper retains the leading-condition convention used by direct section dispatch.

For an arrival or signal consumer, select the first action of the whole activation block, its bypass continuation, and the incoming edge to redirect. Structured analysis suggests boundaries where available; manual selection is supported. Review preparation, response, and consumption together. Only the selected incoming edge bypasses the block; other callers and routes remain intact. A block at the beginning of `init` or `loop` can be bypassed while its required lifecycle entry stays enabled.

Disabled bodies remain authored, editable, and emitted. This is not footprint reduction. Unknown activation boundaries can be saved as drafts, but a disabled entry requires a concrete boundary before export. Structural execution-reference errors still require repair. Reenabling restores the route without reconstructing deleted content.

Core materialization and editor publication use the same captured-revision lowering. Generated wrappers and bypasses are output artifacts, with realization mappings back to the entrypoint; they do not become a second editable document authority.

## Optional resource context

Select an MLD and role, then use **Inspect / refresh MLD**. The script association persists independently of entrypoint bindings. Entries show table index, physical entryId, tblId, function, and source revision. A numeric handler-name match is an inferred association. Consider the entry's function when declaring a response.

Selected evidence records dataset fingerprint, normalized resource key, exact revision, imported-document identity, table index, SPICE local ID, physical entryId, tblId, function, and parameters. A changed or reordered entry requires explicit reselection to adopt its new evidence; SALSA never silently rebinds by index or name. Inspecting a new resource snapshot does not rewrite the script's immutable baseline.

Missing, stale, ambiguous, and unresolved MLD context is advisory. It does not block editing, saving, or export. These states do not establish deletion, residency, or an orphaned handler. Script execution targets remain independent of MLD evidence. Modded resource combinations are permitted regardless of retail inventories.

SPICE parses MLDs. Catalogs remain derived views; MLD bytes are not stored as authored execution content. The deterministic inspection discriminator remains `spice-mld-s6-v1:<source digest>`.

## Overworld selectors

**Overworld selectors** displays each tile family, its ByteVar selector, computed variant, and optional project availability. **Select tile or overworld variant** chooses a family and letter without requiring knowledge of its physical variable index. Insert the assignment before the selected action or replace a selected byte assignment.

The advanced assignment editor accepts ordered `index=expression` lines, including copies such as `6=ByteVar[63]`. Insertions are one undoable batch. Replacements preserve existing scheduling and refresh modifiers. Unspecified variables remain unchanged.

Preview an inclusive range in one sequence using optional known byte values. The preview propagates literal assignments and known copies; unsupported expressions and scheduled assignments yield unknown values. It does not execute branches, calls, or other effects: narrow the range to byte assignments and waits. Preview inputs are transient; authored assignments save normally.

The filename plan is selected for **subsequent overworld initialization**. Selector writes do not immediately reload tiles. Ordinary streaming reuses captured filenames. Tile selectors are independent of ByteVar[6], which selects the overworld SCT and base MLD. There is no global story-stage enum or implicit filename fallback.

The portable rules describe 42 tile keys, 27 coordinate-specific selectors, and fixed `a` variants for unmapped keys. Keep filename keys separate from spatial coordinates: the GC US observation maps digits to `(5 - gridRowZ, gridColumnX)`. Rule evidence must not be confused with current resource residency. Retail regions/discs are not availability whitelists; the actual project catalog supplies availability.

## Ownership and output

[SmoresScript](../SmoresScript/README.md) holds portable relationships intended for donation to SMORES. SALSA owns authored decisions, history, schema, and UI. SPICE owns file formats. Future SALT coordination supplies workspace services; MLD context remains optional authoring assistance.

Activation was introduced in semantic schema 7; [language groups and text](LanguageGroupsAndText.md) advance the current semantic schema to 8. Presentation and compressed container encoding are unchanged. Development-schema compatibility is not provided; Python SALSA v7 import remains supported.

Release output settings concern endianness, supported text conventions/localizations, and compression. Game-binary packaging is outside SALSA. Graph editing, spatial viewing, and unused-library removal are separate slices; footprint reduction is on hold pending the engine-entrypoint inventory.
