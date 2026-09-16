# Story interpretations and interaction names

The current paged-IR editor uses **Authoring > Story workspace** with explicit, whole-project opt-in. See [Whole-program IR editor](WholeProgramIrEditor.md#g5-story-interaction-and-route-tools) for the current workflow. The older document-oriented menus and schemas described below are historical implementation notes, not the current project format.

SALSA stores names and interpretations as project metadata. They do not rename executable sections, retarget MLD handlers, change predicates, or change exported instructions.

## Interaction view

Open **Document > Interactions and Interpretations**. Choose an imported script. The outline reuses the semantic editor's structured branches and ordered instructions. By default it shows lifecycle roots, M handlers, and declared entrypoints; **All script sections** exposes other content. **Friendly conditions** switches between aliases and exact expressions. **Edit in semantic editor** navigates to the existing editor.

Use **Name selected item** for an entrypoint, section, or condition. **Name variable / value** defines a script-specific or project-wide variable/value name. Script definitions take precedence over project definitions. Existing variable aliases remain available. Naming a whole condition records its exact instruction evidence; changing that evidence invalidates its friendly interpretation.

**Entrypoints and Resources > Name selected handler or resource entry** distinguishes a handler alias from an individual resource entry alias. Several resource entries may share a handler. Names remain attached to qualified identities and do not silently move to newly inspected resource entries.

## Explanations

**Analyze project** records bounded cutscene-entry suggestions from parsed instructions. An entry SetBit may occur immediately or after a label prefix and leading condition. Matching section names identify candidates, not recovered authorial module boundaries. Explicit module ownership also supplies context.

Select an item to see its attached explanations. A condition also exposes interpretations of variables it reads. Each explanation includes its rule/version, captured revision, evidence, reasoning, qualification, and decision. Double-click a physical evidence item to navigate to it. **Include interpretation history / debug observations** shows superseded records and their captured observations.

Accepting a suggestion creates a value label only when there is no explicit user name. Rejected suggestions are not repeatedly recreated by unchanged analysis. Changed evidence requires a new analysis record; outdated inferred names fall back to exact expressions. User decisions and names participate in project undo/redo and compressed persistence. Historical targets may refer to deleted content and never rebind by a reused name or offset.

Entry flags mean **entered**, not **completed**. Other writers and dynamic bit writes are disclosed. State dependencies do not establish direct calls, exclusive story phases, runtime reachability, or object visibility. The analysis retains instruction-level accesses throughout the project, including ordinary helpers. Connectivity includes all recognized writes and reads, independently of entry-bit suggestions. Naming rules never filter the effect inventory.

## Storage and boundaries

Semantic schema 11 stores names, attached explanation records, and user-ordered storylines. Presentation schema 3 stores story-node positions and zoom independently; container encoding is unchanged. Derived access indexes are rebuilt; they are not a second editable program. SPICE continues to own physical SCT semantics. Python SALSA v7 import remains supported.

## Story state and whole-cutscene effects

Open **Document > Story State and Cutscene Effects**. Each authored module, or named cutscene candidate not already owned by a module, supplies an analysis root. Candidate recognition does not create or move modules. Module names and script ownership remain independent.

The analysis scans every instruction in the root's sections and follows parsed branches, calls, jumps, and physical section fallthrough into helper sections. It includes both branch arms, switch destinations, scheduled actions, shared helpers, and cycles. A section label is not an execution boundary. This closure is an inventory of possible effects, not an execution trace; writes are displayed in document order within the included sections. Explicit module ownership supplies every owned section even if no route currently references it.

**Every write** retains separate set, clear, toggle, and assignment occurrences. A set followed by a clear is not reduced to a net-zero change. Flags remain visible without any SCT reader, allowing names for engine-consumed state such as input acceptance. Readers include conditions and ordinary expressions. Numeric bank ranges and proposed meanings never exclude an effect. Known direct bit destinations are identified from literal operands; expression-addressed destinations remain visible as expressions. Effects internal to native opcodes are not reconstructed by this SCT access index.

The graph presents a cutscene, its affected variables, and sections or entrypoints reading them. Selecting a flag lists all known reader sites across the imported project. Selecting a write shows its opcode, operands, schedule and refresh modifier. **Open in semantic editor** opens the stable source target. Naming an item does not require accepting a suggested interpretation. Value aliases from the interaction editor appear on bit nodes.

Create a named storyline, add cutscenes, and move steps up or down. **Overview** presents this user-defined backbone as a graph; repeated steps and cycles are allowed. This is narrative organization and does not synthesize calls or change execution. Drag nodes and choose **Save layout** to retain positions in the project presentation state, then save the project normally. A layout edit made during an asynchronous save remains dirty until saved in a later capture.

**Inspect generated instructions** materializes the selected script through the normal exporter for one immutable project revision. It lists the mapped instructions, concrete operands and payload offsets. Entrypoint mappings include generated activation wrappers as well as retained bodies. Project, revision, script and realization must match before a mapping is used. An incomplete script reports its normal export diagnostics; the inspector does not substitute or repair executable content.

## Suggested paths and repeated visits

**Suggest all paths** analyzes the current imported scripts; **Suggest from selected** anchors the analysis at the selected cutscene/module. The Suggested paths tab lists bounded candidate paths through a partial order. Cycles remain labeled unordered groups; choose individual steps explicitly. Branch conditions, transfers, arrival matches, possible enables/inhibits relationships, and missing targets are inspectable with each selected item. A suggestion does not certify execution or establish a flag's story meaning.

Choose a storyline and select proposal steps, then use **Adopt selected steps**. This adds narrative occurrences without changing executable instructions. **Reject selected suggestion** records a user decision attached to its source items. Refreshing analysis does not change accepted storylines or names. Proposals captured before another edit must be refreshed before adoption.

Each occurrence has a stable identity, optional name, and independent position, even if the same cutscene appears several times. Use **Name step**, Up/Down, Remove, and Overview to organize these visits. Names attached to occurrences do not rename their underlying modules. Save layout and then save the project as usual.

Path facts are shared with the standalone survey. Resource context comes from explicitly retained project entrypoint bindings, or the survey's explicit MLD associations. Missing MLD context does not block proposals. Survey physical IDs are external evidence and never silently become authored project identities.

Executable routing is explicit and independent of narrative adoption. Open **Executable routes / export set** for supported load/arrival editing and coordinated output. See [the route workflow](CrossScriptRoutes.md).
