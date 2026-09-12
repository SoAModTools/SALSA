# SALSA - Skies of Arcadia Logic and Script Assistant

SALSA is being rewritten as a C++20 and Qt 6 desktop editor for Skies of Arcadia Legends script files.

The new implementation is organized as a Qt-free `SalsaCore` static library, a Qt Widgets `SalsaQt` application, and a GoogleTest-based `SalsaTests` executable. It consumes the frozen SpiceSCT document API through a pinned SPICE submodule.

SALSA can inspect extracted datasets and edit imported SCT scripts through one authoring project and transaction history. Save checkpoints the entire project, its stable identities, and immutable source baselines in a separate workspace. Closing a document view retains its edits in the project. Export publishes the selected script to an explicit target. Official final-version-7 legacy projects import into a fresh dataset and authoring workspace, including supported metadata and retained migration evidence. Target platform and encoding choices are supplied when publishing or migrating data.

The original Python application is preserved under `legacy/python` as a behavioral and domain-knowledge reference. It is not a dependency of the new application.

User-facing documentation starts at [Docs/README.md](Docs/README.md).

Imported scripts also support [named sequences, shared conditions, and previous-location selection](Docs/SequencesAndConditions.md) through **Document > Sequences and Conditions**. Project export generates fresh SCT layout from understood content; valid drafts can be saved before unresolved text or references are repaired.

## Building

Clone with submodules, or initialize the pinned dependencies after cloning:

```powershell
git submodule update --init --recursive
```

Open `SALSA.sln` in Visual Studio 18 and build the `Debug|x64` or `Release|x64` configuration with the MSVC v145 toolchain. The Qt application expects the `6.10.3_msvc2022_64` Qt kit. GoogleTest and SPICE are pinned under `third-party`.

Build outputs are written to `bin/<platform>/<configuration>`.

## Testing

Run the relevant filtered suites in `bin/x64/Debug/SalsaTests.exe` after building. `SalsaQtTests.exe` exercises the document controller with a Qt event loop and synthetic assets; it does not drive widgets or require game data. Manual testing covers the rendered GUI. Private corpus tests live in the sister testing repository and run only in Release.
