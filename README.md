# SALSA - Skies of Arcadia Logic and Script Assistant

SALSA is being rewritten as a C++20 and Qt 6 desktop editor for Skies of Arcadia Legends script files.

The new implementation is organized as a Qt-free `SalsaCore` static library, a Qt Widgets `SalsaQt` application, and a GoogleTest-based `SalsaTests` executable. It consumes the frozen SpiceSCT document API through a pinned SPICE submodule.

SALSA can inspect extracted datasets and edit imported SCT scripts through one authoring project and transaction history. Save checkpoints the entire project, its stable identities, and immutable source baselines in a separate workspace. Closing a document view retains its edits in the project. Export publishes the selected script to an explicit target. Official final-version-7 legacy projects import into a fresh dataset and authoring workspace, including supported metadata and retained migration evidence. Target platform and encoding choices are supplied when publishing or migrating data.

The original Python application is preserved under `legacy/python` as a behavioral and domain-knowledge reference. It is not a dependency of the new application.

User-facing documentation starts at [Docs/README.md](Docs/README.md).

The production editor uses the [whole-program service](Docs/WholeProgramIrEditor.md): one workspace authority, paged resident-script views, local journal edits, and binary project persistence with zstd level 1. Startup opens no dataset automatically. Native SCTs are imported once; reopening a saved workspace and its tabs reads the binary project. Native publication generates fresh SCT layout from authored content. Python SALSA v7 remains an import format only.

## Building

Clone with submodules, or initialize the pinned dependencies after cloning:

```powershell
git submodule update --init --recursive
```

Open `SALSA.sln` in Visual Studio 18 and build the `Debug|x64` or `Release|x64` configuration with the MSVC v145 toolchain. The Qt application expects the `6.10.3_msvc2022_64` Qt kit. GoogleTest and SPICE are pinned under `third-party`.

Build outputs are written to `bin/<platform>/<configuration>`.

See [Entrypoints and resource context](Docs/EntrypointsAndResources.md) and the [portable SmoresScript library](SmoresScript/README.md) for S6 authoring and relationship contracts.

The **Authoring** menu opens language membership, modules/entrypoints, variable-use queries, item explanations and optional resources on demand. The earlier authoring documents describe domain contracts; their old **Document** menu paths refer to the retired snapshot editor. See [current editor controls and presentation limits](Docs/WholeProgramIrEditor.md).

## Testing

Run relevant filtered console suites in `bin/x64/Release/SalsaTests.exe` after building. The production program suites use synthetic data and do not launch the GUI. Historical Qt controller suites remain separate; GUI-loading tests and manual GUI runs require explicit authorization. Private corpus tests live in the sister testing repository and run only in Release, subject to the exceptions recorded in SALSA's AGENTS.md.
