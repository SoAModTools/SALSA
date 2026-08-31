# SALSA

SALSA is being rewritten as a C++20 and Qt 6 desktop editor for Skies of Arcadia Legends script files.

The new implementation is organized as a Qt-free `SalsaCore` static library, a Qt Widgets `SalsaQt` application, and a `SalsaTests` executable. SPICE integration will be added after its editable SCT document API is ready to be consumed as a pinned dependency.

The original Python application is preserved under `legacy/python` as a behavioral and domain-knowledge reference. It is not a dependency of the new application.

## Building

Open `SALSA.sln` in Visual Studio 18 and build the `Debug|x64` or `Release|x64` configuration with the MSVC v145 toolchain. The Qt application expects the `6.10.3_msvc2022_64` Qt kit.

Build outputs are written to `bin/<platform>/<configuration>`.

## Testing

Run `bin/x64/Debug/SalsaTests.exe` after building the Debug configuration. The current runner is a dependency-free scaffold check and will move to GoogleTest when the test dependency is introduced.
