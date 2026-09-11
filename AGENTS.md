# Repository validation guidance

- Keep test execution scoped to the code and data formats changed by the task.
- For SALSA work that only changes SCT behavior or its SPICE SCT integration, run the relevant `SalsaTests` suites and filtered `SpiceTests` SCT suites. Do not run the complete `SpiceTests` executable without a filter.
- Do not run SPICE MLD, PVM, GVR, ECT, or other unrelated file-type or whole-corpus tests for SCT-only SALSA changes.
- Expand validation to another SPICE file type only when the changed code crosses that boundary or the user explicitly requests it.

# SALSA private corpus coverage

- For new or changed SALSA corpus-backed behavior, test representative scripts across GameCube and Dreamcast and every available JP, US, and EU release. Include available disc variants where relevant.
- Keep the matrix focused on the affected SCT behavior and script groupings; this does not require unrelated suites or whole-corpus runs.
- Required acceptance cases must fail when configured inputs are missing rather than silently skip. Report unavailable platform/region/disc coverage explicitly.
- Run private corpus tests only in Release, in the sister private testing repository, following its mutex and corpus-boundary rules. This coverage policy is SALSA-specific, not a global SoAModTools rule.
