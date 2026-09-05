# Repository validation guidance

- Keep test execution scoped to the code and data formats changed by the task.
- For SALSA work that only changes SCT behavior or its SPICE SCT integration, run the relevant `SalsaTests` suites and filtered `SpiceTests` SCT suites. Do not run the complete `SpiceTests` executable without a filter.
- Do not run SPICE MLD, PVM, GVR, ECT, or other unrelated file-type or whole-corpus tests for SCT-only SALSA changes.
- Expand validation to another SPICE file type only when the changed code crosses that boundary or the user explicitly requests it.
