# SpiceSCT structured-control-flow handoff package

This directory is the complete preliminary handoff package for the portable
structured-control-flow analysis prototyped in SALSA. It targets the frozen
SpiceSCT v2 contract at commit `a1e9a1a79111efd013aa0ae50857f15d181d32da`.

## Package contents

- `HANDOFF.md` explains ownership, evidence policy, the legacy-heuristic mapping,
  safety counterexamples, and the intended SpiceSCT adoption boundary.
- `include/SpiceSctStructurePrototype/SctStructuredControlFlow.h` is the public,
  value-owned analysis contract.
- `src/SctStructuredControlFlow.cpp` contains the Qt-free implementation.
- `tests/test_sct_structured_control_flow.cpp` contains the focused synthetic
  characterization and counterexample suite.
- `SpiceSctStructurePrototype.vcxproj` is the SALSA-hosted reference build project.

The source and tests depend only on public SpiceSCT v2 APIs. The Visual Studio
project contains SALSA-relative paths and is provided as integration guidance,
not as the final project definition for SPICE.

## Recommended receiving workflow

1. Copy this directory to a SpiceSCT feature branch.
2. Read `HANDOFF.md` before changing the API or evidence policy.
3. Rename the prototype namespace and include path only after deciding its
   experimental SpiceSCT location.
4. Move the focused test source into a SpiceSCT-owned test target and retain the
   characterization cases during refactoring.
5. Run representative real-file corpus comparisons before treating the API as a
   frozen SpiceSCT contract.

Do not transfer SALSA's structured-authoring overlay with this package. Virtual
authoring intent, lowering operations, history, postconditions, and UI behavior
remain editor-owned concerns.
