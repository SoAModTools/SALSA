# SALSA

SALSA is being rewritten as a C++20 and Qt 6 desktop editor for Skies of Arcadia Legends script files.

The new implementation is organized as a Qt-free `SalsaCore` static library, a Qt Widgets `SalsaQt` application, and a GoogleTest-based `SalsaTests` executable. SPICE integration will be added after its editable SCT document API is ready to be consumed as a pinned dependency.

The first core framework can inspect an extracted dataset without modifying it. It discovers SCT files recursively, represents them with normalized dataset-relative locators, records SHA-256 source revisions, computes a deterministic dataset fingerprint, and loads immutable source snapshots only while their bytes still match the inspected catalog. Platform and region metadata are optional and are never guessed. Project manifests, patch persistence, SCT parsing, editing, and publication are intentionally later phases.

The original Python application is preserved under `legacy/python` as a behavioral and domain-knowledge reference. It is not a dependency of the new application.

## Building

Clone with submodules, or initialize the pinned GoogleTest dependency after cloning:

```powershell
git submodule update --init --recursive
```

Open `SALSA.sln` in Visual Studio 18 and build the `Debug|x64` or `Release|x64` configuration with the MSVC v145 toolchain. The Qt application expects the `6.10.3_msvc2022_64` Qt kit. GoogleTest is owned directly by SALSA and pinned under `third-party/googletest`; SPICE is not currently a dependency.

Build outputs are written to `bin/<platform>/<configuration>`.

## Testing

Run `bin/x64/Debug/SalsaTests.exe` after building the Debug configuration. Automated coverage is restricted to the Qt-free core. GUI behavior is tested manually; automated GUI-driving tests are not part of this project.
