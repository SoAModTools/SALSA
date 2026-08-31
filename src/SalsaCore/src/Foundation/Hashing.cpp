#include "SalsaCore/Foundation/Hashing.h"

#include <Windows.h>
#include <bcrypt.h>

#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace salsa::core {
namespace {

[[nodiscard]] Diagnostic hashDiagnostic(DiagnosticCode code, std::string message) {
    return { DiagnosticSeverity::Error, code, std::move(message), std::nullopt };
}

[[nodiscard]] bool failed(const NTSTATUS status) noexcept {
    return status < 0;
}

}  // namespace

Sha256Digest::Sha256Digest(std::array<std::byte, Size> bytes) noexcept
    : bytes_(bytes) {}

std::span<const std::byte, Sha256Digest::Size> Sha256Digest::bytes() const noexcept {
    return bytes_;
}

std::string Sha256Digest::toHex() const {
    std::ostringstream output{};
    output << std::hex << std::setfill('0');
    for (const auto value : bytes_) {
        output << std::setw(2) << std::to_integer<unsigned int>(value);
    }
    return output.str();
}

struct Sha256Hasher::Impl {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<std::byte> hashObject{};
    bool finished = false;

    ~Impl() {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    }
};

Sha256Hasher::Sha256Hasher(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

Sha256Hasher::Sha256Hasher(Sha256Hasher&&) noexcept = default;
Sha256Hasher& Sha256Hasher::operator=(Sha256Hasher&&) noexcept = default;
Sha256Hasher::~Sha256Hasher() = default;

Result<Sha256Hasher> Sha256Hasher::create() {
    auto impl = std::make_unique<Impl>();
    if (failed(BCryptOpenAlgorithmProvider(
            &impl->algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return Result<Sha256Hasher>::failure(hashDiagnostic(
            DiagnosticCode::HashInitializationFailed,
            "Windows could not initialize the SHA-256 provider."));
    }

    ULONG objectSize = 0;
    ULONG bytesWritten = 0;
    if (failed(BCryptGetProperty(
            impl->algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize),
            sizeof(objectSize),
            &bytesWritten,
            0)) ||
        bytesWritten != sizeof(objectSize)) {
        return Result<Sha256Hasher>::failure(hashDiagnostic(
            DiagnosticCode::HashInitializationFailed,
            "Windows could not report the SHA-256 state size."));
    }

    impl->hashObject.resize(objectSize);
    if (failed(BCryptCreateHash(
            impl->algorithm,
            &impl->hash,
            reinterpret_cast<PUCHAR>(impl->hashObject.data()),
            objectSize,
            nullptr,
            0,
            0))) {
        return Result<Sha256Hasher>::failure(hashDiagnostic(
            DiagnosticCode::HashInitializationFailed,
            "Windows could not create a SHA-256 operation."));
    }

    return Result<Sha256Hasher>::success(Sha256Hasher(std::move(impl)));
}

Result<void> Sha256Hasher::update(const std::span<const std::byte> bytes) {
    if (impl_ == nullptr || impl_->finished) {
        return Result<void>::failure(hashDiagnostic(
            DiagnosticCode::HashStateInvalid,
            "The SHA-256 operation is no longer writable."));
    }
    if (bytes.empty()) {
        return Result<void>::success();
    }
    if (bytes.size() > std::numeric_limits<ULONG>::max()) {
        return Result<void>::failure(hashDiagnostic(
            DiagnosticCode::HashUpdateFailed,
            "The SHA-256 input chunk is too large."));
    }
    if (failed(BCryptHashData(
            impl_->hash,
            reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
            static_cast<ULONG>(bytes.size()),
            0))) {
        return Result<void>::failure(hashDiagnostic(
            DiagnosticCode::HashUpdateFailed,
            "Windows could not update the SHA-256 operation."));
    }
    return Result<void>::success();
}

Result<Sha256Digest> Sha256Hasher::finish() {
    if (impl_ == nullptr || impl_->finished) {
        return Result<Sha256Digest>::failure(hashDiagnostic(
            DiagnosticCode::HashStateInvalid,
            "The SHA-256 operation has already finished."));
    }

    std::array<std::byte, Sha256Digest::Size> digest{};
    if (failed(BCryptFinishHash(
            impl_->hash,
            reinterpret_cast<PUCHAR>(digest.data()),
            static_cast<ULONG>(digest.size()),
            0))) {
        return Result<Sha256Digest>::failure(hashDiagnostic(
            DiagnosticCode::HashFinalizationFailed,
            "Windows could not finish the SHA-256 operation."));
    }
    impl_->finished = true;
    return Result<Sha256Digest>::success(Sha256Digest(digest));
}

Result<Sha256Digest> sha256(const std::span<const std::byte> bytes) {
    auto created = Sha256Hasher::create();
    if (!created) {
        return Result<Sha256Digest>::failure(created.diagnostics());
    }
    auto hasher = std::move(created).takeValue();
    auto updated = hasher.update(bytes);
    if (!updated) {
        return Result<Sha256Digest>::failure(updated.diagnostics());
    }
    return hasher.finish();
}

}  // namespace salsa::core
