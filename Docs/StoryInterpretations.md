# Story interpretations and interaction names

SALSA stores names and interpretations as project metadata. They do not rename executable sections, retarget MLD handlers, change predicates, or change exported instructions.

## Interaction view

Open **Document > Interactions and Interpretations**. Choose an imported script. The outline reuses the semantic editor's structured branches and ordered instructions. By default it shows lifecycle roots, M handlers, and declared entrypoints; **All script sections** exposes other content. **Friendly conditions** switches between aliases and exact expressions. **Edit in semantic editor** navigates to the existing editor.

Use **Name selected item** for an entrypoint, section, or condition. **Name variable / value** defines a script-specific or project-wide variable/value name. Script definitions take precedence over project definitions. Existing variable aliases remain available. Naming a whole condition records its exact instruction evidence; changing that evidence invalidates its friendly interpretation.

**Entrypoints and Resources > Name selected handler or resource entry** distinguishes a handler alias from an individual resource entry alias. Several resource entries may share a handler. Names remain attached to qualified identities and do not silently move to newly inspected resource entries.

## Explanations

**Analyze project** records bounded cutscene-entry suggestions from parsed instructions. An entry SetBit may occur immediately or after a label prefix and leading condition. Matching section names identify candidates, not recovered authorial module boundaries. Explicit module ownership also supplies context.

Select an item to see its attached explanations. A condition also exposes interpretations of variables it reads. Each explanation includes its rule/version, captured revision, evidence, reasoning, qualification, and decision. Double-click a physical evidence item to navigate to it. **Include interpretation history / debug observations** shows superseded records and their captured observations.

Accepting a suggestion creates a value label only when there is no explicit user name. Rejected suggestions are not repeatedly recreated by unchanged analysis. Changed evidence requires a new analysis record; outdated inferred names fall back to exact expressions. User decisions and names participate in project undo/redo and compressed persistence. Historical targets may refer to deleted content and never rebind by a reused name or offset.

Entry flags mean **entered**, not **completed**. Other writers and dynamic bit writes are disclosed. State dependencies do not establish direct calls, exclusive story phases, runtime reachability, or object visibility. The analysis retains instruction-level accesses throughout the project, including ordinary helpers. Candidate story dependencies are limited to flags associated with scene entries to avoid an unreadable graph of incidental state.

## Storage and boundaries

Semantic schema 9 stores names and attached explanation records. The existing container and presentation schemas are unchanged at this checkpoint. Derived access indexes are rebuilt; they are not a second editable program. SPICE continues to own physical SCT semantics. Python SALSA v7 import remains supported.
