#include "SalsaCore/Project/AssetLocator.h"

#include <Windows.h>

#include <system_error>
#include <utility>

namespace salsa::core {
namespace {

[[nodiscard]] Diagnostic invalidLocator(
    std::string message,
    const std::filesystem::path& path) {
    return {
        DiagnosticSeverity::Error,
        DiagnosticCode::InvalidAssetLocator,
        std::move(message),
        path,
    };
}

[[nodiscard]] Result<std::wstring> invariantLowercase(const std::wstring& value) {
    if (value.empty()) {
        return Result<std::wstring>::success({});
    }

    const int required = LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_LOWERCASE,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr,
        0);
    if (required == 0) {
        return Result<std::wstring>::failure(invalidLocator(
            "Windows could not normalize the asset locator for comparison.", value));
    }

    std::wstring lowered(static_cast<std::size_t>(required), L'\0');
    if (LCMapStringEx(
            LOCALE_NAME_INVARIANT,
            LCMAP_LOWERCASE,
            value.data(),
            static_cast<int>(value.size()),
            lowered.data(),
            required,
            nullptr,
            nullptr,
            0) == 0) {
        return Result<std::wstring>::failure(invalidLocator(
            "Windows could not normalize the asset locator for comparison.", value));
    }
    return Result<std::wstring>::success(std::move(lowered));
}

[[nodiscard]] Result<std::string> utf8(const std::wstring& value) {
    if (value.empty()) {
        return Result<std::string>::success({});
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required == 0) {
        return Result<std::string>::failure(invalidLocator(
            "The asset locator is not valid Unicode.", value));
    }

    std::string converted(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            converted.data(),
            required,
            nullptr,
            nullptr) == 0) {
        return Result<std::string>::failure(invalidLocator(
            "The asset locator is not valid Unicode.", value));
    }
    return Result<std::string>::success(std::move(converted));
}

}  // namespace

AssetLocator::AssetLocator(std::filesystem::path path, std::string identityKey)
    : path_(std::move(path)), identityKey_(std::move(identityKey)) {}

Result<AssetLocator> AssetLocator::fromRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()) {
        return Result<AssetLocator>::failure(invalidLocator(
            "An asset locator must be a non-empty path relative to the dataset root.", path));
    }

    for (const auto& component : path) {
        if (component == L"." || component == L".." || component.empty()) {
            return Result<AssetLocator>::failure(invalidLocator(
                "An asset locator cannot contain empty, current-directory, or parent-directory components.",
                path));
        }
    }

    const auto normalized = path.lexically_normal();
    if (normalized.empty() || normalized == L".") {
        return Result<AssetLocator>::failure(invalidLocator(
            "An asset locator must identify a file.", path));
    }

    auto lowered = invariantLowercase(normalized.generic_wstring());
    if (!lowered) {
        return Result<AssetLocator>::failure(lowered.diagnostics());
    }
    auto key = utf8(lowered.value());
    if (!key) {
        return Result<AssetLocator>::failure(key.diagnostics());
    }
    return Result<AssetLocator>::success(AssetLocator(normalized, std::move(key).takeValue()));
}

const std::filesystem::path& AssetLocator::path() const noexcept {
    return path_;
}

const std::string& AssetLocator::identityKey() const noexcept {
    return identityKey_;
}

bool AssetLocator::operator==(const AssetLocator& other) const noexcept {
    return identityKey_ == other.identityKey_;
}

std::strong_ordering AssetLocator::operator<=>(const AssetLocator& other) const noexcept {
    return identityKey_ <=> other.identityKey_;
}

}  // namespace salsa::core
