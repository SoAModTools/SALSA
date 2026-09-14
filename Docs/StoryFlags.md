# Story flags and platform differences

A story flag is a meaning you give to game state, such as “the escape cutscene has finished.” A `BitVar` index is the storage location used to represent that state. The same number does not necessarily have the same meaning on Dreamcast and GameCube.

This guide summarizes static executable and script research reviewed in September 2026. Capacity findings cover the retail GameCube and Dreamcast JP, US and EU executables. Range labels describe known uses and working interpretations; they are not exhaustive reservations or a guarantee that a mod will run.

## Bit-bank capacity

All six reviewed executables use the same bit-bank capacity:

| Property | GameCube and Dreamcast |
|---|---:|
| Storage | `0xD58` bytes (3,416 bytes) |
| Organization | 854 words, with 32 flags per word |
| Capacity | 27,328 bits |
| Physical indices | `0–27327`, or `0x0000–0x6ABF`, inclusive |
| Save/restore | The whole bank is copied |

The capacity is established by native reset and save/restore code. It is not estimated from the largest bit number found in scripts. GameCube's additional discoveries and wanted encounters fit within this existing bank.

The native bit accessors do not check the whole-bank boundary. A script operand that can encode a larger index does not allocate additional storage: it may access other game memory. Expanding the bank would require executable work, including its access, initialization and save/restore paths. This is outside SALSA's current authoring/export functionality.

## Known platform differences

All ranges below are hexadecimal and inclusive. Counts describe flag-family slots; they do not claim that every slot is referenced directly by an SCT.

| Flag family | Dreamcast | GameCube |
|---|---|---|
| Discovery found | `0x480–0x4BF` (64 slots) | `0xB54–0xBAD` (90 slots) |
| Secondary discovery state | `0x4C0–0x4FF` (64 slots) | `0xBAE–0xC07` (90 slots) |
| Wanted seen/acknowledged | No corresponding native wanted family identified | `0xAC8–0xACF` (8 slots) |
| Discovery purchase/reward workflow | No corresponding family established by this research | `0xC08–0xC61` (90 slots) |
| Additional discovery completion/reward workflow | No corresponding family established by this research | `0xC62–0xCBB` (90 slots) |

The precise player-facing distinction between the secondary and reward-related discovery states remains partly unresolved. GameCube also uses `ByteVar[100–107]` for an eight-entry wanted availability/status vector. Wanted encounters therefore involve more than the eight acknowledged-state bits.

The discovery families were relocated and expanded. The found-family base moved by `0x6D4`; the secondary-family base moved by `0x6EE`, because the first family grew from 64 to 90 slots. There is no single offset that translates every Dreamcast flag into a GameCube flag. The region between wanted state and discovery state, `0xAD0–0xB53`, contains mixed Legends story, overworld and menu controls; it is not all wanted state.

Flags are physically packed into words, and several subsystem families are allocated consecutively. This does not establish whether the original definitions were handwritten, generated or compiled from a particular declaration format.

## Story state and lifetime

The low range `0x000–0x25E` contains a sparse, approximately ordered collection of story, cutscene and field-progression milestones. It is useful as an initial story-view grouping, not as a chronological rule. GameCube adds story controls elsewhere too. Follow actual writes, conditions and transfers to connect cutscenes, and give flags names appropriate to your project.

Other useful distinctions include:

- `0x600–0x73F` is a heavily used field-object/event-control neighborhood. It contains several kinds of state, not one uniform table.
- **`0x740–0x7FF` (1856–2047) is cleared by the reviewed partial-teardown routines on both platforms.** These bits should not be assumed to survive a field/script teardown merely because the bank is saved.
- A cutscene can change operational state, such as input acceptance, as well as story progress. Record those effects without automatically calling every changed bit a story milestone.

ByteVars also have the same physical capacity across the six executables: 288 bytes. Their reviewed partial-clear behavior differs: GameCube clears `128–255`, while Dreamcast clears `128–286`. Capacity, meaning and lifetime are separate properties.

## Choosing bits for a mod

Distinguish these three descriptions:

1. **Within capacity:** the index has storage in the native bank.
2. **Known use:** a script, native function or resource-related path uses it.
3. **Candidate unused:** the inspected evidence has not found a use. This is not a reservation of that slot.

For example, the research found no exact SCT BitVar references in `0x25F–0x3F1`. That makes it a candidate investigation range, not a list of certified free flags. Computed indices, native code, MLD handlers and other data can use bits absent from an SCT literal census. Known native/MLD-related uses around decimal 24,000 also rule out treating everything above the ordinary story flags as free.

Use **Authoring > Find variable uses** to inspect the current project's indexed uses. Treat this as script evidence, not a complete inventory of executable or MLD consumers. When choosing a slot, consider known uses, teardown behavior and assignments made by other parts of your mod. An available slot inside the existing bank needs no bank expansion, but this research does not designate a universally free allocation range.

## Moving content between platforms

Keep the intended meaning alongside the numerical binding. For example, “discovery ID N found” maps to `0x480 + N` on Dreamcast and `0xB54 + N` on GameCube, for IDs supported by the corresponding native family. Existing scripts that directly check those bits need deliberate review when moved between platforms. A native discovery opcode may already perform the platform-specific calculation internally; changing every matching number indiscriminately would be incorrect.

Changing endianness, text encoding or compression at export does not translate gameplay state or add Legends-only functionality to Dreamcast. The absence of a Dreamcast wanted consumer does not make the same numeric slots inaccessible or automatically free.

Symbolic story-flag allocation, project reservations and explicit platform-binding assistance are planned directions. They are not an automatic allocator or remapping feature in the current editor. Names and interpretations do not themselves rewrite numerical references. The planned cutscene view will associate entry routes, conditions, effects and exits with those meanings while retaining the underlying instructions for inspection.

See [the current editor workflow](WholeProgramIrEditor.md) and [SCPT expressions](Parameters.md) for editing context.
