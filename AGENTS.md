# Repository validation guidance

- Keep test execution scoped to the code and data formats changed by the task.
- For SALSA work that only changes SCT behavior or its SPICE SCT integration, run the relevant `SalsaTests` suites and filtered `SpiceTests` SCT suites. Do not run the complete `SpiceTests` executable without a filter.
- Do not run SPICE MLD, PVM, GVR, ECT, or other unrelated file-type or whole-corpus tests for SCT-only SALSA changes.
- Expand validation to another SPICE file type only when the changed code crosses that boundary or the user explicitly requests it.

# SALSA private corpus coverage

- For new or changed SALSA corpus-backed behavior, test representative scripts across GameCube and Dreamcast and every available JP, US, and EU release. Include available disc variants where relevant.
- Keep the matrix focused on the affected SCT behavior and script groupings; this does not require unrelated suites or whole-corpus runs.
- Required acceptance cases must fail when configured inputs are missing rather than silently skip. Report unavailable platform/region/disc coverage explicitly.
- Dreamcast discs have expected differences in their asset inventories. Define cases from each disc's catalog and report absent assets as coverage differences; they are not defects. Missing inputs for a declared acceptance case still fail.
- Run private corpus tests only in Release, in the sister private testing repository, following its mutex and corpus-boundary rules. This coverage policy is SALSA-specific, not a global SoAModTools rule.

- User-approved S8 story-connectivity exception: private acceptance uses only the `me002*.sct` family in Dreamcast EU, on available relevant discs. Do not expand this connectivity matrix to other platforms or regions. Other SALSA corpus behavior retains the general policy above.

- The standalone connectivity survey is a separate, opt-in analysis executable, not a gtest suite. Its approved initial full-corpus run covers every SCT in DC EU on both available discs. This does not expand the `me002*.sct` regression exception above. Keep its corpus launcher and all local manifests, caches, and reports in the private testing repository; use Release and its mutex.
- The approved MLD survey extension adds optional paired base-MLD context for field stems below 200, including overworld stem 99, from each DC EU disc's actual inventory. Report unavailable pairs as context gaps; do not require runtime reachability or infer cross-disc equivalence.

- User-approved whole-program IR experiment exception: the GameCube US full-SCT corpus loader, seeded-edit workload, and launcher live in SALSA, not the private testing repository. The launcher may read the private repository's local location configuration. Keep all machine paths, inventories, traces and reports ignored in SALSA; keep raw inputs and temporary binary products outside repositories. Use Release console workers, no GUI or gtest corpus suite. This exception is limited to this opt-in GC US architecture experiment; do not expand its platform/region scope automatically.

- The approved GC US IR experiment also includes the SALSA-owned checkpoint compression comparison. Capture the checkpoint once and reuse its immutable chunks across codecs; keep binary captures temporary and outside repositories, with manifests/reports ignored in SALSA. This does not change production compression or broaden platform/region coverage.
