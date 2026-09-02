# Retired SpiceSCT structured-control-flow handoff record

This directory preserves the design and heuristic handoff record for the portable
structured-control-flow analysis prototyped in SALSA. SpiceSCT adopted and hardened
the analysis in frozen SctDocument v3 at commit
`92cefc8a234d8fc9f99564efc5efac271344a6af`. SALSA now consumes the canonical
`spice::sct::SctStructuredControlFlowAnalysis` in `SctDocumentAnalysis`.

## Package contents

- `HANDOFF.md` explains the original ownership and evidence policy, maps the legacy
  heuristics to the adopted analysis, and records the editor-owned boundary.
- This README records why no duplicate implementation remains in SALSA.

The prototype source, tests, and Visual Studio project were removed after adoption.
Their maintained equivalents now belong to SpiceSCT. This directory must not be used
as a second implementation or compatibility contract.

## Current integration rule

SALSA consumes the frozen v3 aggregate directly and retains only its semantic-authoring
overlay. Any correction to graph construction, region recognition, source evidence,
or structure issues belongs in a future SpiceSCT version.

Do not transfer SALSA's structured-authoring overlay with this package. Virtual
authoring intent, lowering operations, history, postconditions, and UI behavior
remain editor-owned concerns.
