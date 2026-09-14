# Optional authoring concepts and section boundaries

These are design notes, not a commitment to implement additional constructs. The user selected shared switch labels for immediate implementation; the remaining ideas are optional. Guard clauses are of particular interest, but no implementation is authorized by this document alone.

## Shared switch labels: implemented core support

One switch arm owns an ordered list of numeric labels, optional Default membership, and one body sequence. Labels select that shared body directly. Normal arm exit/fallthrough remains a separate property. Labels live in one pooled table rather than an allocated vector per published arm. Duplicates within/across arms, multiple Defaults, unlabelled non-default arms and final-arm fallthrough are rejected during construction. Stable arm/body identity is independent of the number of labels.

Recognition and lowering remain in their existing slices. SPICE already supplies shared-case-target evidence and multiple case labels per structured arm. WP3 should use that input when promoting supported switch shapes, retaining original operand ordinals as source evidence. WP5 emits the corresponding case operands to one body target; grouping must not duplicate execution or introduce synthetic fallthrough. No new private format is introduced in this extension; WP4 persists label ranges, Default membership and arm identity in its future binary tables.

## Optional conveniences

1. **Named calls/returns:** the opcodes already express these operations. Prefer existing typed instruction editing and optional name/navigation improvements. There is no current need for a separate Call/Return authoring action or an inferred function signature. This is not an active implementation item.
2. **Guard clauses / early exits:** a favored optional presentation. A conditional whose selected body immediately returns, breaks or continues could appear as a compact guard, reducing nesting. Reuse conditional regions and explicit exits; retain branch polarity, check preparation and evaluation order. Guard recognition must distinguish an immediate exit from a larger conditional body. No dedicated guard action is required merely to change presentation.
3. **Combined conditions:** an individual SCPT parameter program already supports compound conditions. Use that existing expression representation and editor; do not duplicate it with a new All/Any construct. A future optional presentation could combine checks spread across separate SCT instructions/condition regions when useful, retaining their original evaluation order, preparation and branch behavior. Do not assume SCPT compound evaluation is interchangeable with short-circuit control flow between instructions.
4. **Wait / wait-until presentation:** explicit wait/delay opcodes already exist. Optional labels or a compact view of a polling loop may improve readability, retaining its check preparation, wait actions, scheduling and polling cadence. Do not add or remove a yield as part of renaming or grouping.
5. **Named jump destinations:** optional editable labels/navigation for explicit jumps that remain unstructured. This does not imply that every section name is an independently used jump target.

These conveniences should normally reuse existing authored instructions/structures. Their priority can be reassessed after the semantic editor is operating on the new IR. They do not expand WP1 or the mandatory completion criteria of WP2-WP6.

## Sections that fall through in cutscenes

User observation: some cutscene sections have no Return at their end and appear to continue into the next section. The user has not noticed direct jumps to some of those boundaries. One plausible explanation is organization in the original source; this is not a claim about the original compiler or authoring process.

Keep physical section boundaries, control transfers, callable entrypoints and semantic module boundaries as separate concepts. A cutscene module may span several sections connected by fallthrough. An organizational section name can remain as a source marker or optional displayed subheading without becoming a separate routine or an invented call/return boundary. Existing modules can own multiple section-root sequences; recognition must preserve their assembly order and continuation relationships.

The lack of a Return alone does not establish fallthrough: other transfers and the actual adjacent physical content matter. When import recognition becomes active, use available SPICE control-flow/layout observations to identify continuation across section boundaries, and distinguish these edges from calls and explicit jumps. Lack of observed incoming jumps does not establish that the engine can never enter by name. Keep external entry evidence independent from organizational presentation.

WP3 should permit grouping a cutscene across such boundaries while retaining source markers, stable instruction identity and any known alternate entries. WP5 must preserve the section layout/continuation or realize the same flow explicitly when rearranging output; it must not insert a Return merely because a section ended. This is a preservation/design consideration for those slices, not a new region-merging implementation now. No automatic routine splitting, section deletion, entrypoint disabling or library pruning follows from these observations. Footprint reduction remains on hold.
