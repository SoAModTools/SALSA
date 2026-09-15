#pragma once
#include "SalsaCore/Program/Program.h"
#include "SalsaCore/Project/CapturedSource.h"
#include "SpiceSCT/SctDocumentImporter.h"
#include "SalsaCore/Authoring/SctAuthoringStore.h"

namespace salsa::core::program {
// Temporary import evidence: keep only undecided indexed text bytes, never a
// retained parsed document. Collection loading resolves them before installation.
struct NativePendingText {
    std::uint64_t sourceText = 0;
    spice::sct::SctTextKind kind{};
    std::vector<std::uint8_t> bytes;
};
struct NativeTextEvidence {
    SctScriptId script;
    bool automatic = false;
    std::optional<spice::sct::SctKnownTextConvention> recommendation;
    std::vector<spice::sct::SctKnownTextConvention> candidates;
    std::array<std::uint64_t, spice::sct::kSctKnownTextConventions.size()> support{};
    std::vector<NativePendingText> pending;
};
spice::sct::SctKnownTextConvention selectNativeConvention(std::span<const NativeTextEvidence>);
// Parse once, import once, normalize, and release native graphs before returning.
// Caller owns baseline bytes/evidence storage. No filesystem IO or global cache.
Result<SctScriptId> appendNative(ProgramBuilder&, std::span<const std::uint8_t>,
    SctAuthoringBaseline, SctScriptContext, const spice::sct::SctDocumentImportOptions& = {},
    std::stop_token = {}, SctLoadingObserver = {}, NativeTextEvidence* = nullptr);
// The immutable capture already binds its bytes to their source digest.
Result<SctScriptId> appendNative(ProgramBuilder&, const CapturedSource&,
    SctAuthoringBaseline, SctScriptContext, const spice::sct::SctDocumentImportOptions& = {},
    std::stop_token = {}, SctLoadingObserver = {}, NativeTextEvidence* = nullptr);
}
