# Legacy project capsule v2

The legacy capsule is SALSA's inert, normalized handoff from an official final
legacy version-7 `.prj` file. It is an intermediate migration artifact, not a
workspace or an editable script format. Phase 6 owns construction of
`SctDocument` instances and all import decisions.

## Directory contract

A capsule is a directory whose name conventionally ends in `.salsa-legacy`.
It contains:

- `capsule.json`: the manifest and integrity inventory;
- `project.cbor`: project-level authoring metadata and normalization records;
- `scripts/NNNNNN.cbor.zlib`: one zlib-compressed typed CBOR migration-IR
  record per legacy script, in project order;
- optionally, `evidence/source.prj`: the selected original input.

Every file except `capsule.json` appears exactly once in `entries`. Unlisted,
missing, non-regular, reparse-point, and case-colliding entries are rejected.
Original source evidence is noncanonical, so retaining it does not change the
capsule identity.

## Manifest and identity

`capsule.json` is UTF-8 JSON. Its `formatId` is
`jahorta.salsa.legacy-capsule`, its `schemaVersion` is `2`, and its exact
`converterContractId` is `official-final-v7-native-2`. It records source
filename, size, SHA-256, pickle protocol, project version, optional-evidence
disposition, normalization count, per-script summaries, and file entries.

Each script summary has `ordinal`, `key`, `storedName`, `status`, `sections`,
`instructions`, `parameters`, `strings`, `diagnostics`, and `path`. Status is
`accepted` or `failed`. A failed script remains inventoried and makes the
capsule `action-required`; other scripts remain usable.

The capsule ID is SHA-256 over:

```text
converterContractId NUL
sourceSha256 NUL
for each canonical entry, sorted by path:
  path NUL entrySha256 NUL decimalSize LF
```

## Typed script migration IR

Each script record has format `jahorta.salsa.legacy-script-record`, schema 2,
identity and status fields, structural counts, diagnostics, and an `ir` map.
The IR is emitted in this stable semantic order:

1. `header`
2. `footer`
3. `string_groups`
4. `strings`
5. `sections`
6. `links`
7. `sidecar`

The sidecar contains `folded_sects`, `index`, `sect_tree`, `sect_list`,
`string_garbage`, `unused_sections`, `errors`, `error_sections`, and
`variables`. Sections recursively retain semantic, raw, authoring, and
advisory fields for their instructions and parameters. Only compound values
receive record-local node IDs; immutable scalars are encoded inline.

Typed objects use `[12, nodeId, classCode, fields]`. `fields` is positional so
class and field names are not repeated for every entity:

| Code | Type | Retained field order |
| ---: | --- | --- |
| 1 | `SCTProject` | `global_variables`, `version`, `inst_id_colors` |
| 2 | `SCTScript` | `folded_sects`, `name`, `index`, `header`, `sects`, `sect_tree`, `sect_list`, `links`, `footer`, `strings`, `string_groups`, `string_garbage`, `unused_sections`, `errors`, `error_sections`, `variables` |
| 3 | `SCTSection` | `name`, `insts`, `inst_tree`, `inst_list`, `inst_errors`, `errors`, `strings`, `garbage`, `string`, `is_compound`, `type` |
| 4 | `SCTInstruction` | `ID`, `base_id`, `skip_refresh`, `delay_param`, `errors`, `links_out`, `params`, `l_params`, `my_goto_uuids`, `my_master_uuids`, `label`, `encode_inst` |
| 5 | `SCTParameter` | `ID`, `type`, `link`, `errors`, `analyze_log`, `value`, `raw_bytes`, `linked_string`, `override` |
| 6 | `SCTLink` | `type`, `script`, `origin_trace`, `target_trace`, `ID` |

Derived caches are intentionally absent. These include absolute offsets,
instruction and string location indexes, section lengths and usage counts,
derived link endpoints, formatted and arithmetic parameter caches, generated
conditions and synopses, and internal cursor/loop caches. `project.cbor`
contains the authoritative field-disposition table documenting whether every
recognized v7 field is retained, normalized, recomputed, or discarded.

Strings and bytes are CBOR byte strings so legacy byte sequences do not depend
on UTF-8 validity. Integer values remain decimal text and float values retain
their exact IEEE-754 bits. Containers retain list/tuple/set/dictionary type and
ordering. Only the final-v7 class allowlist can appear in object records.
Each script shard is independently zlib-compressed after its CBOR stream is
complete. Readers enforce the decompressed record-size limit before parsing,
so compression does not weaken capsule resource safeguards or require loading
the entire project.

## Bounded-memory conversion

The converter reads and hashes the source in the same buffered pass. Its
restricted pickle VM writes payloads and container mutations to one temporary
append-only spool; the VM stack, memo table, and node-offset index contain only
integer handles. Parsing and graph normalization are serial. The normalized
store is then sealed against mutation, and each script worker opens an
independent read-only spool cursor with its own field-name cache and sparse
traversal state. Script validation and counting run in parallel, their results
are folded in project order for cumulative limits, and independent script
records are then encoded, compressed, hashed, written once to a `*.part` file,
and atomically renamed in parallel. Inventory construction, capsule identity,
the project record, and the manifest remain serial and deterministic. Worker
count therefore does not affect capsule bytes or identity.

Auto processing uses the smaller of four workers, available logical processors,
and script count. An empty project starts no workers. The broker's normal memory
ceiling is half of installed physical RAM, clamped to 4--16 GiB, with an 8 GiB
fallback when discovery fails. Disabling resource safeguards removes that
ceiling. Each worker still buffers at most one script record at a time; the
ceiling is not an allocation target. The project record and manifest are
published last, and the spool is deleted before finalization.

Readers stream integrity checks for ordinary entries. A compressed script is
read and hashed once, decompressed under its record-size limit, and validated
with a fixed-schema CBOR cursor rather than a general-purpose document tree.
This keeps validation memory proportional to one script shard instead of its
expanded JSON-like object graph.

Rejected versions, unsafe pickle operations, conversion failure, and
cancellation never produce an accepted capsule. The spool is transient and is
not part of the capsule.

## Compatibility policy

Schema version 2 and converter contract `official-final-v7-native-2` are one
exact development contract. Readers reject other versions or converter IDs.
Only protocol-4 official final version-7 projects are accepted. Earlier
projects must be opened and resaved by final legacy SALSA; later or
development-only shapes are rejected.
