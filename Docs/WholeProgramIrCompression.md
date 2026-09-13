# IR checkpoint compression experiment

The opt-in compression experiment lives in SalsaIrEvaluation. It captures one paged GC US checkpoint after the same seed/edit workload as the corpus experiment, releases the native import process, and reuses the exact captured bytes across codecs. It does not change the production container codec or editor persistence.

## Build and run

From an elevated PowerShell in SALSA:

```powershell
python.exe tools/build_ir_codecs.py
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/amd64/MSBuild.exe' src/SalsaIrEvaluation/SalsaIrEvaluation.vcxproj /m /p:Configuration=Release /p:Platform=x64
```

The provisioner downloads hash-pinned official source archives for zstd 1.5.7, LZ4 1.10.0 and libdeflate 1.24. It builds static x64 Release libraries with v145 and the dynamic MSVC runtime. Source/build directories are ignored under `private/ir-compression-deps`; installed libraries, headers, receipt and generated MSBuild props are ignored under `bin/ir-codecs`. No submodule is added or changed. The normal evaluator still builds without these optional dependencies; requesting an unavailable codec fails explicitly. Provisioning does not automatically rebuild the evaluator.

Prepare a local inventory with `python.exe tools/run_ir_corpus.py --prepare-only`, or reuse the unchanged manifest from an earlier corpus run. Then:

```powershell
python.exe tools/run_ir_compression.py --manifest <local-manifest.json> --repetitions 3 --seed 1 --edits 1000
```

This captures once, then runs LodePNG defaults, zstd levels 1/3, LZ4 default and libdeflate levels 1/6 in separate supervised processes. Order rotates across repetitions. All codecs use one thread, no dictionary and identical table/chunk boundaries. LodePNG and libdeflate emit zlib streams, zstd emits frames, and LZ4 uses raw blocks with original sizes supplied by the capture metadata. Compressed-byte totals include each codec's own framing but not the common SALSA container headers or SHA-256 overhead.

The capture uses the existing v2 metadata/node/child/text tables, partitioned into at most 64 MiB chunks. Its binary files exist only inside the launcher's temporary directory outside repositories and are removed on success or failure. The capture manifest, build/dependency identity, run order, reports and logs remain ignored in `private/ir-compression`. Original game inputs remain read-only. This is the SALSA-owned GC US experiment exception, not an expansion of other corpus policies.

Lower-level commands are `corpus ... --capture-directory PATH` and `compression --capture PATH/capture.json --codec NAME --report PATH`. The capture directory must be new and outside a repository. Normal users should use the launcher so temporary binary data is cleaned up even after a failed worker. The worker limits remain 4 GiB and ten minutes. Cancellation is checked at chunk boundaries; a noninterruptible codec call may finish before cancellation is observed, with the existing supervisor providing the outer bound.

## Measurements

Each worker first reads and hashes every captured chunk. This phase is separate from codec timings. The same decoded capture identity, input byte count, source identity and Release executable must agree across all accepted runs.

Compression and decompression measurements sum per-chunk calls, including output-buffer allocation/initialization and the codec's internal work. Codec-context creation is reported separately. Hashing, input reads, decoded-byte comparison, report writing and SALSA container packaging are outside those timers. Decompressed bytes are compared exactly with the original capture during each run; a mismatch or codec failure produces no accepted comparison. This is not native SCT export or runtime verification.

All raw chunks remain resident, followed by all compressed outputs; decompression retains only one decoded chunk at a time. Reports include input, compressed-stage and final process memory, cumulative process peaks and compressed output capacities. Those capacities can exceed logical compressed size because several APIs allocate their worst-case bound. Memory differences therefore include adapter buffers and allocator retention, not just codec scratch space. No exact scratch-allocation accounting or isolated production save peak is claimed.

Three repetitions provide observed medians and ranges, not a tail guarantee. OS scheduling, power state and cache behavior may vary. The comparison is independent of storage candidate selection and does not reload the native corpus for each codec. Complete save latency still includes checkpoint capture, hashing, framing and I/O; complete project opening includes model/index reconstruction after decompression.

## Scoped checks

The existing console smoke group covers every compiled codec with a small repetitive buffer and an empty buffer. There is no GUI or new gtest suite. Production save behavior and the earlier corpus comparison remain separate from this experiment.

The approved full checkpoint experiment completed all 18 codec/repetition runs, and every codec reproduced the same captured bytes. Exact local timing/size/memory observations and dependency receipts remain ignored with the reports. The user selected zstd level 1 after this comparison. Production adoption, notices and codec-aware container framing are planned in [the production persistence slice](WholeProgramIrProductionPlan.md).
