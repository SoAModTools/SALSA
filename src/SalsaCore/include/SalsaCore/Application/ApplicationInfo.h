#pragma once

#include <string_view>

namespace salsa::core {

[[nodiscard]] std::string_view applicationName() noexcept;
[[nodiscard]] std::string_view applicationVersion() noexcept;

}  // namespace salsa::core
