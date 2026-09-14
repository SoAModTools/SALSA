#pragma once
#include "SalsaCore/Program/Program.h"
#include "SpiceSCT/SctDocumentImporter.h"
#include "SalsaCore/Authoring/SctAuthoringStore.h"

namespace salsa::core::program {
// Parse once, import once, normalize, and release native graphs before returning.
// Caller owns baseline bytes/evidence storage. No filesystem IO or global cache.
Result<SctScriptId> appendNative(ProgramBuilder&, std::span<const std::uint8_t>,
    SctAuthoringBaseline, SctScriptContext, const spice::sct::SctDocumentImportOptions& = {},
    std::stop_token = {}, SctLoadingObserver = {});
}
