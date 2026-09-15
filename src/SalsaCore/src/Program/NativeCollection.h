#pragma once
#include "SalsaCore/Program/ProgramService.h"
#include <map>
#include <mutex>
namespace salsa::core::program {
struct ProgramData;
// Detached per-script IR; dataset identity is installed only when the complete
// scan has finished. No provisional fingerprint escapes into live state.
class NativeDatasetPreparation {
public:
    explicit NativeDatasetPreparation(SctAuthoringProjectId project) : project_(std::move(project)) {}
    Result<void> append(std::size_t, const CapturedSource &, SctScriptContext, SctImportedDocumentId,
                        std::optional<spice::sct::SctKnownTextConvention>,
                        const spice::sct::SctDocumentImportOptions &, std::stop_token, SctLoadingObserver);
    Result<std::shared_ptr<const Program>> finish(const DatasetFingerprint &, std::stop_token,
                                                  SctLoadingObserver);

private:
    struct Part {
        std::shared_ptr<const Program> program;
        NativeTextEvidence text;
        std::vector<Diagnostic> diagnostics;
    };
    SctAuthoringProjectId project_;
    std::mutex mutex_;
    std::map<std::size_t, Part> parts_;
};
void resolveNativeText(ProgramData &, std::span<const NativeTextEvidence>, std::stop_token);
Result<std::shared_ptr<const Program>> loadNativeCollection(NativeLoadRequest, std::stop_token,
                                                            std::stop_token shutdown,
                                                            std::function<bool()> current);
} // namespace salsa::core::program
