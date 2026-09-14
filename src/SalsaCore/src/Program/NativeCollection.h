#pragma once
#include "SalsaCore/Program/ProgramService.h"
namespace salsa::core::program {
Result<std::shared_ptr<const Program>> loadNativeCollection(NativeLoadRequest, std::stop_token,
                                                            std::stop_token shutdown, std::function<bool()> current);
}
