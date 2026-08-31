#include "SalsaCore/Application/ApplicationInfo.h"

namespace salsa::core {

std::string_view applicationName() noexcept {
    return "SALSA";
}

std::string_view applicationVersion() noexcept {
    return "0.1.0-dev";
}

}  // namespace salsa::core
