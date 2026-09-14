# Third-party notices

SALSA remains licensed under GPL version 3. Existing submodules retain their own license and source-distribution requirements.

## Zstandard 1.5.7

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

SALSA uses Zstandard under its BSD 3-Clause option. The complete notice, redistribution conditions and warranty disclaimer are in `third-party/zstd/LICENSE`. The upstream alternative GPL license is retained in `third-party/zstd/COPYING`; selecting BSD does not change SALSA's GPL license.

The vendored C implementation and public headers are in `third-party/zstd/lib`. The pinned upstream version, source archive digest and per-file digests are in `third-party/zstd/SOURCE.json`. No assembly, CLI, dictionary builder or multithreading is enabled. Compression level 1 is a SALSA persistence setting, not a change to the library.

Binary releases must include this notice and the Zstandard BSD license. Run `python tools/package_zstd_sources.py --release-directory <staging-directory>` during release packaging to include the exact vendored Zstandard source subset and build-integration files with their notices. This supplies the Zstandard portion of corresponding source; it does not replace the obligation to provide SALSA and its other dependencies' corresponding source.
