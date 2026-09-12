# SmoresScript

Independent C++20 script/resource relationship library, incubated here for transfer to SMORES. No Qt, SalsaCore, filesystem access, JSON, editor history, or authored project IDs. SPICE supplies immutable typed documents; consumers supply qualified resource snapshots. This library does not parse game bytes.

Resource names use ASCII case-insensitive relative keys. Entries retain table index, SPICE document-local ID, physical entryId and tblId separately. Source revision and imported-document identity are mandatory. Resolution never repairs stale bindings automatically. Missing catalog inputs never prove orphaning; availability never proves residency.

Handler name matches and eventhook parameter-8 candidates are inference, not engine execution proof. Resource operations retain section/instruction order and scheduling indicators; no global load timeline is inferred. Consumers retain original predicates and explicit consumption actions.

Build independently with CMake and `-DSPICE_ROOT=<SPICE checkout>`, or the standalone v145 MSBuild projects. The default SPICE path points to SALSA's existing checkout. Synthetic contract tests require no game files.
