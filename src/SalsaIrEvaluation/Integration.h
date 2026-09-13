#pragma once
#include "Evaluation.h"
#include "ProgramInput.h"
#include <stdexcept>

namespace salsa::evaluation {
struct UnsupportedNative : std::runtime_error { using std::runtime_error::runtime_error; };
struct NativeFile { std::string asset; bool dreamcast{}; std::vector<uint8_t> bytes; };
struct NativeInput { Fixture fixture; Json measurements; };
struct NativeLocation {
    ScriptId script; SectionId section; InstructionId instruction;
    uint64_t spiceInstruction{}; uint32_t sectionOrdinal{},instructionOrdinal{},offset{},size{};
};
struct NativeOutput { std::vector<NativeFile> files; std::vector<NativeLocation> locations; ProjectId project; uint64_t revision{}; };

// Only public SPICE parsing/document/SCPT/export APIs cross the native boundary.
NativeInput importNative(std::span<const NativeFile> files,ProjectId project);
NativeInput makeNativeFixture(uint64_t count,uint64_t seed,bool dreamcast);
NativeOutput exportNative(const Fixture& checkpoint);
std::string nativeMeaning(const Fixture& fixture);

// Backend-independent semantic schema inside the existing SALSAPRJ compressed framing.
void validateNativeSource(const NativeSource& source);
std::vector<std::byte> encodeBinaryProgram(const Fixture& checkpoint);
BinaryProgramInput viewBinaryProgram(std::span<const std::byte> bytes);
std::vector<std::byte> packBinaryProgram(std::vector<std::byte> bytes);
std::vector<std::byte> unpackBinaryProgram(std::span<const std::byte> container);
Json encodeProgram(const Fixture& checkpoint);
Fixture decodeProgram(const Json& json);
std::vector<std::byte> packProgram(std::string_view json);
std::string unpackProgram(std::span<const std::byte> container);
std::vector<std::byte> fileRoundTrip(std::span<const std::byte> bytes,Json& measurements);
Json processMemory();
void integrationSmoke();
} // namespace salsa::evaluation
