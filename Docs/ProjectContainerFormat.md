# SALSA authoring container contract

SALSA owns this private development format. SPICE owns SCT composition, decomposition, validation, and game-file output. No SPICE persistence API is used.

## Container version 1

`SctProjectContainer::encode` and `decode` operate on a map of named byte records. Decoding exposes the uncompressed JSON and evidence records for diagnostic inspection without writing files. The container is custom framing with zlib records, not ZIP.

All framing integers are unsigned little-endian. The file starts with eight ASCII bytes `SALSAPRJ`, a 32-bit container version (`1`), and a 32-bit record count. Each record follows in lexical name order:

| Field | Size |
| --- | --- |
| Name byte length | 32 bits |
| Uncompressed payload byte length | 64 bits |
| Compressed payload byte length | 64 bits |
| SHA-256 of uncompressed payload | 32 bytes |
| ASCII record name | Declared name length |
| Zlib payload | Declared compressed length |

Limits: 4,096 records, 240 bytes per name, 256 MiB per uncompressed record, 1 GiB for the complete container and aggregate uncompressed payload. Names use ASCII letters, digits, slash, dot, hyphen, and underscore; absolute names, empty components, `.` and `..` components, and duplicate names are rejected. Decode checks version, framing, bounded expansion, lengths, checksums, and trailing container bytes before returning records.

## Records and logical schemas

- `project.json`: `jahorta.salsa.authoring-checkpoint`, schema 2. Includes workspace identity, semantic project, separate presentation, and baseline record bindings.
- Semantic project: `jahorta.salsa.authoring-project`, schema 5. Includes project/entity identities, allocation state, ownership, module section membership, stable action/sequence ports, evidence, unresolved bindings, and authored output profiles.
- Complete program content: `jahorta.salsa.program-state`, schema 1, nested in semantic content. Includes ordered sections/instructions/text, expressions, references, allocation state, control-arm metadata, text repair provenance, aliases, annotations, and folders.
- `baselines/<baseline-id>.sct`: exact imported source bytes, verified against the baseline descriptor on load.
- `legacy/<capsule-id>/...`: available migration capsule evidence, retained across subsequent saves. These records are not execution state.

Container, checkpoint, semantic, presentation, and program versions are independent. Unsupported schemas fail closed. Baseline names are validated against the manifest; unrecognized checkpoint records are rejected. JSON duplicate keys and excessive nesting are rejected. No partially accepted project is returned.

Authoring IDs use decimal strings. Reused program value codecs encode integer scalars as single-field objects with `$unsigned` or `$signed` decimal-string values, preserving full-width identity and signedness without guessing whether user text is numeric. Ordered content remains ordered; metadata collections are canonicalized. Presentation changes leave semantic serialization unchanged.

Program-local SPICE IDs are scoped to the script's persisted program, not globally stable identities. Durable authoring entities retain their own project IDs. The current public SPICE allocator restoration API requires advancing retired IDs individually; decoding bounds that gap to 1,000,000 per allocator. Sparse live IDs are reconstituted directly. Increasing this limit should use an explicit allocator restoration contract rather than unbounded loops.

## Content and evidence boundary

Current program records contain understood content and explicit missing-meaning markers. `uninterpreted-text` and `uninterpreted-operand` do not store source bytes. The in-memory SPICE value adapter uses non-source sentinel values inside opaque variants to satisfy structural requirements; fresh lowering rejects referenced/ executable markers before output. They must never be treated as recovered meaning.

Baselines remain mandatory import provenance. Container load verifies their hashes but defers SCT inspection. Fresh output uses the direct authored program and saved output profile, then verifies generated SCT output through SPICE. Explicit unchanged-source preservation may inspect and reuse the retained baseline.

The existing workspace rebase service still receives generated patch artifacts outside the container. Its derived snapshot retains source attachments because that older patch contract cannot express their removal. These artifacts do not supply live content on reopen and never restore padding to semantic output. Automatic module/definition rebinding across a changed baseline remains unsupported.

Container saves currently rewrite the complete file and use workspace transaction conflict checks, atomic replacement, and recovery. Baseline byte storage, source inspection, and retained capsules are evidence services; the authoring session remains the sole mutable authority.
