# Legacy import artifacts v1

A committed fresh legacy import retains three SALSA-owned JSON artifacts in addition to the immutable version-2 capsule.

- `authoring/dataset-target.json` uses format `jahorta.salsa.legacy-target-scope`, schema 1, and records the confirmed target scope, region, and custom name.
- `import-state/<capsule-id>.json` uses format `jahorta.salsa.legacy-import-state`, schema 1, and records the capsule and plan identities, importing application version, asset and entity mappings, stable metadata record IDs, current dispositions, and versioned decision history.
- `imports/<capsule-id>/report.json` uses format `jahorta.salsa.legacy-import-report`, schema 1, and is the portable audit report. It records the importing application version and contains no absolute machine paths.

The immutable capsule is copied to `imports/<capsule-id>/capsule`. Generated SCT bytes are retained separately in the content-addressed baseline store, and no initial patch file is created.

Interrupted publication uses a machine-local `jahorta.salsa.legacy-import-transaction` schema-1 journal plus temporary markers in the staged or published directories. The journal may contain absolute local paths because it is recovery state rather than a portable workspace artifact. Recovery only removes a destination when its marker matches the recorded plan identity; otherwise recovery blocks.

Metadata disposition values are `applied`, `pending`, `recomputed`, `contract`, `dropped-by-user`, `blocked`, `unsupported`, and `invalid`. Invalid recognized metadata blocks import until its stable record is explicitly discarded. Promotion adapters receive read-only access to the retained capsule root, are keyed by capsule hash plus stable record identity, and commit through the ordinary workspace transaction service without modifying the capsule.
