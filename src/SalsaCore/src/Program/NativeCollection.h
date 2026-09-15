#pragma once
#include "SalsaCore/Program/ProgramService.h"
namespace salsa::core::program {
struct ProgramData;
void resolveNativeText(ProgramData&, std::span<const NativeTextEvidence>, std::stop_token);
Result<std::shared_ptr<const Program>> loadNativeCollection(NativeLoadRequest, std::stop_token,
                                                            std::stop_token shutdown, std::function<bool()> current);
}
