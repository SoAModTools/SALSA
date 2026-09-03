# SALSA

SALSA is being rewritten as a C++20 and Qt 6 desktop editor for Skies of Arcadia Legends script files.

The new implementation is organized as a Qt-free `SalsaCore` static library, a Qt Widgets `SalsaQt` application, and a GoogleTest-based `SalsaTests` executable. It consumes the frozen SpiceSCT document API through a pinned SPICE submodule.

SALSA can inspect extracted datasets, decode and edit SCT files through document-local history, checkpoint deterministic semantic patches in a separate workspace, and export the active script to an explicit publication target. The editable model remains platform- and region-agnostic; target platform and encoding choices are supplied only when exporting.

The original Python application is preserved under `legacy/python` as a behavioral and domain-knowledge reference. It is not a dependency of the new application.

User-facing documentation starts at [Docs/README.md](Docs/README.md).

## Building

Clone with submodules, or initialize the pinned dependencies after cloning:

```powershell
git submodule update --init --recursive
```

Open `SALSA.sln` in Visual Studio 18 and build the `Debug|x64` or `Release|x64` configuration with the MSVC v145 toolchain. The Qt application expects the `6.10.3_msvc2022_64` Qt kit. GoogleTest and SPICE are pinned under `third-party`.

Build outputs are written to `bin/<platform>/<configuration>`.

## Testing

Run `bin/x64/Debug/SalsaTests.exe` after building the Debug configuration. Automated coverage is restricted to the Qt-free core. GUI behavior is tested manually; automated GUI-driving tests are not part of this project.
