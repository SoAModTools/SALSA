#pragma once

#include "SalsaCore/Foundation/Result.h"

#include <array>
#include <compare>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace salsa::core {

class Sha256Digest final {
public:
    static constexpr std::size_t Size = 32;

    explicit Sha256Digest(std::array<std::byte, Size> bytes) noexcept;

    [[nodiscard]] std::span<const std::byte, Size> bytes() const noexcept;
    [[nodiscard]] std::string toHex() const;

    [[nodiscard]] auto operator<=>(const Sha256Digest&) const = default;

private:
    std::array<std::byte, Size> bytes_{};
};

class Sha256Hasher final {
public:
    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;
    Sha256Hasher(Sha256Hasher&&) noexcept;
    Sha256Hasher& operator=(Sha256Hasher&&) noexcept;
    ~Sha256Hasher();

    [[nodiscard]] static Result<Sha256Hasher> create();
    [[nodiscard]] Result<void> update(std::span<const std::byte> bytes);
    [[nodiscard]] Result<Sha256Digest> finish();

private:
    struct Impl;

    explicit Sha256Hasher(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_{};
};

[[nodiscard]] Result<Sha256Digest> sha256(std::span<const std::byte> bytes);

}  // namespace salsa::core
