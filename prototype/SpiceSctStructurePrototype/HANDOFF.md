# SpiceSCT structured-control-flow prototype

This project is a SALSA-hosted prototype intended for later review by SpiceSCT. It is
Qt-free and depends only on public frozen-v2 SpiceSCT types. Its namespace deliberately
identifies it as a SALSA prototype; adoption by SPICE should move and rename the API
rather than preserving that namespace as a compatibility contract.

## Ownership and evidence

SpiceSCT remains the owner of the document and canonical control-flow edges. This
library derives disposable basic blocks and structured-region suggestions. Its results
are not validation, editing state, persisted data, layout instructions, or export rules.
Runtime-fact catalogues are not consumed.

Current typed edges are primary. Imported edges are considered only when they cross an
opaque attachment that SpiceSCT identifies as a control-flow gap. A region depending on
that evidence is explicitly `EvidenceLimited` and capped at heuristic confidence.

## Legacy heuristic mapping

| Legacy behavior | Prototype implementation | Maximum claim | Safety counterexample | Future SpiceSCT location |
| --- | --- | --- | --- | --- |
| `_group_jump_commands`: follow an If false target | Typed Branch False and Branch True edges establish a candidate header and arms | Verified with current edges | Missing, unresolved, or cross-section targets stay flat | Structured analysis conditional detector |
| Inspect the instruction immediately before the false target | A Jump at that physical position is attached as Pre Target Jump evidence, but does not establish the region | Evidence only | An unrelated goto immediately before the target cannot override graph boundaries | Structured evidence records |
| Negative goto means a while loop | A backward edge proposes a natural loop only when its target dominates the source and the component has one entry | Verified with current edges | Multi-entry irreducible cycles are reported and left flat | Structured analysis loop detector |
| False target equals terminator target means If without Else | Reachability to the immediate common postdominator must yield one defensible arm | Verified with current edges | Overlapping arms or unexplained exits reject the candidate | Structured analysis conditional verifier |
| Different false and goto targets establish Else and join | False target proposes Else; the nearest common postdominator establishes the join | Verified with current edges | A random forward goto that does not postdominate both arms cannot become the join | Structured analysis conditional verifier |
| `_group_switches`: case links establish case starts | Typed Switch Case edges and repeated-group sites establish starts and labels | Verified with current edges | Missing targets or cross-section case transfers remain flat | Structured analysis switch detector |
| Repeated labels can share one target | Labels with the same target are merged into one arm without duplicating instructions | Verified with current edges | Shared names or values alone never merge targets | Structured switch arm contract |
| A case body occupies the physical range before the next case | Physical range is only a candidate; the header and case entry must dominate every member | Verified with current edges | A cross-entry body or unrelated terminal block rejects the switch | Structured switch verifier |
| Adjacent cases can fall through | Only an actual Fallthrough edge from the final block to the next physical case entry is accepted | Verified with current edges | An explicit jump into another case is not presented as fallthrough | Structured switch verifier |
| Forward case-end gotos suggest the switch end | Agreeing forward exits are evidence; the nearest common postdominator is the actual join | Verified with current edges | Different case exits cannot be resolved by choosing the numerically largest target | Structured switch verifier |
| Backward case-end goto denotes a loop | Natural-loop analysis runs before switch recognition and may nest the loop inside the case | Verified with current edges | A backward edge never automatically becomes the switch exit | Structured loop plus nesting passes |
| Scan after the final case while skipping nested If and Switch groups | Postdominance replaces the mutable scan and nesting counter | Verified with current edges | No scan can accidentally select the first unrelated later goto | Graph and postdominator utility |
| Repeated decode passes find nested groups before switches | Loop and conditional candidates are generated before switch candidates, then laminar nesting is resolved once | Verified with current edges | Overlapping non-nested regions are rejected together | Structured analysis orchestration |
| `_check_for_goto_in_garbage` decodes opaque bytes looking for a hidden goto | No decoding occurs. One bound imported edge may be evaluated when SpiceSCT identifies its crossed attachment as a control-flow gap | Evidence Limited and Heuristic | Missing endpoints, conflicting current edges, combined weak hints, or failed verification produce issues and flat instructions | Imported-evidence adapter around structured analysis |

Logical-section folding and name-based subscript grouping are intentionally outside
this intra-section analysis. They require separate cross-section contracts and must not
be inferred from this region tree.

## Intended SPICE adoption

The public header, implementation, this mapping, and the focused synthetic tests move
together. A future SpiceSCT version can place them beside `SctDocumentAnalysis` as a
revision-scoped `SctStructuredControlFlowAnalysis`, review the evidence policy, and
include the value-owned result in the canonical analysis aggregate. SALSA presentation
should then consume the SPICE type directly and delete this project.

SALSA now consumes the result as the verified base of a Semantic editor. That does not
expand this prototype's ownership. Virtual Else or Case intent, lowering policy,
managed-scaffold identity, operation history, dirty state, and materialization
postconditions remain SALSA application contracts and must not move with the analysis.
The analysis should continue returning only defensible graph-derived regions, arms,
joins, evidence, and issues; editors with different domain goals may layer different
authoring abstractions over the same immutable result.

The receiving implementation must retain the separation between current typed edges
and imported research evidence. Runtime-fact catalogs remain outside this analysis and
must not silently become validation or construction rules.
