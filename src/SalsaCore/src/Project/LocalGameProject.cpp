#include "SalsaCore/Project/LocalGameProject.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace salsa::core {
namespace {

[[nodiscard]] Diagnostic error(
    const DiagnosticCode code,
    std::string message,
    std::optional<std::filesystem::path> path = std::nullopt) {
    return { DiagnosticSeverity::Error, code, std::move(message), std::move(path) };
}

[[nodiscard]] Diagnostic warning(
    const DiagnosticCode code,
    std::string message,
    std::optional<std::filesystem::path> path = std::nullopt) {
    return { DiagnosticSeverity::Warning, code, std::move(message), std::move(path) };
}

[[nodiscard]] Diagnostic cancelled() {
    return error(DiagnosticCode::Cancelled, "Dataset inspection was cancelled.");
}

[[nodiscard]] bool isReparsePoint(const std::filesystem::path& path, DWORD& attributes) {
    attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

[[nodiscard]] bool equalOrdinalIgnoreCase(
    const std::wstring& left,
    const std::wstring& right) noexcept {
    return CompareStringOrdinal(
               left.data(),
               static_cast<int>(left.size()),
               right.data(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool isSctPath(const std::filesystem::path& path) {
    return equalOrdinalIgnoreCase(path.extension().wstring(), L".sct");
}

[[nodiscard]] bool pathIsWithin(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) {
    auto rootPart = root.begin();
    auto candidatePart = candidate.begin();
    for (; rootPart != root.end(); ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() ||
            !equalOrdinalIgnoreCase(rootPart->wstring(), candidatePart->wstring())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Result<void> rejectReparseComponents(
    const std::filesystem::path& root,
    const AssetLocator& locator) {
    auto current = root;
    for (const auto& component : locator.path()) {
        current /= component;
        DWORD attributes = INVALID_FILE_ATTRIBUTES;
        if (isReparsePoint(current, attributes)) {
            return Result<void>::failure(error(
                DiagnosticCode::AssetOutsideDataset,
                "The asset path crosses a symbolic link, junction, or other reparse point.",
                current));
        }
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return Result<void>::failure(error(
                DiagnosticCode::AssetNotFound,
                "The source asset no longer exists.",
                current));
        }
    }
    return Result<void>::success();
}

[[nodiscard]] Result<std::vector<std::byte>> readFile(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return Result<std::vector<std::byte>>::failure(error(
            DiagnosticCode::AssetReadFailed,
            "The source asset could not be opened for reading.",
            path));
    }

    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        static_cast<std::uintmax_t>(end) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return Result<std::vector<std::byte>>::failure(error(
            DiagnosticCode::AssetReadFailed,
            "The source asset size cannot be represented by this process.",
            path));
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty() &&
        !input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()))) {
        return Result<std::vector<std::byte>>::failure(error(
            DiagnosticCode::AssetReadFailed,
            "The source asset could not be read completely.",
            path));
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] Result<DatasetFingerprint> fingerprintCatalog(
    const std::vector<AssetDescriptor>& assets) {
    auto created = Sha256Hasher::create();
    if (!created) {
        return Result<DatasetFingerprint>::failure(created.diagnostics());
    }
    auto hasher = std::move(created).takeValue();

    static constexpr char domainText[] = "SALSA-SCT-CATALOG-V1\0";
    constexpr std::string_view domain(domainText, sizeof(domainText) - 1);
    auto update = hasher.update(std::as_bytes(std::span(domain.data(), domain.size())));
    if (!update) {
        return Result<DatasetFingerprint>::failure(update.diagnostics());
    }

    const auto appendU64 = [&hasher](const std::uint64_t value) -> Result<void> {
        std::array<std::byte, 8> encoded{};
        for (std::size_t index = 0; index < encoded.size(); ++index) {
            encoded[index] = static_cast<std::byte>(
                (value >> ((encoded.size() - index - 1) * 8)) & 0xffU);
        }
        return hasher.update(encoded);
    };

    for (const auto& asset : assets) {
        if (asset.locator.identityKey().size() >
            static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
            return Result<DatasetFingerprint>::failure(error(
                DiagnosticCode::HashUpdateFailed,
                "An asset locator is too large to fingerprint."));
        }
        update = appendU64(static_cast<std::uint64_t>(asset.locator.identityKey().size()));
        if (!update) {
            return Result<DatasetFingerprint>::failure(update.diagnostics());
        }
        update = hasher.update(std::as_bytes(std::span(
            asset.locator.identityKey().data(), asset.locator.identityKey().size())));
        if (!update) {
            return Result<DatasetFingerprint>::failure(update.diagnostics());
        }
        update = appendU64(asset.byteSize);
        if (!update) {
            return Result<DatasetFingerprint>::failure(update.diagnostics());
        }
        update = hasher.update(asset.revision.digest.bytes());
        if (!update) {
            return Result<DatasetFingerprint>::failure(update.diagnostics());
        }
    }

    auto digest = hasher.finish();
    if (!digest) {
        return Result<DatasetFingerprint>::failure(digest.diagnostics());
    }
    return Result<DatasetFingerprint>::success(
        DatasetFingerprint{ std::move(digest).takeValue() });
}

struct ScannedCatalog final {
    AssetCatalogSnapshot catalog;
    std::vector<Diagnostic> diagnostics{};
};

[[nodiscard]] Result<ScannedCatalog> scanDataset(
    const std::filesystem::path& root,
    const std::stop_token stopToken,
    const DatasetScanObserver& observer) {
    std::vector<Diagnostic> diagnostics{};
    std::vector<std::filesystem::path> candidates{};
    std::error_code iteratorError{};
    std::filesystem::recursive_directory_iterator iterator(
        root,
        std::filesystem::directory_options::none,
        iteratorError);
    if (iteratorError) {
        return Result<ScannedCatalog>::failure(error(
            DiagnosticCode::DatasetEnumerationFailed,
            "The dataset directory could not be enumerated: " + iteratorError.message(),
            root));
    }

    const std::filesystem::recursive_directory_iterator end{};
    while (iterator != end) {
        if (stopToken.stop_requested()) {
            return Result<ScannedCatalog>::failure(cancelled());
        }

        const auto entryPath = iterator->path();
        DWORD attributes = INVALID_FILE_ATTRIBUTES;
        const bool reparse = isReparsePoint(entryPath, attributes);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return Result<ScannedCatalog>::failure(error(
                DiagnosticCode::DatasetEnumerationFailed,
                "The attributes of a dataset entry could not be read.",
                entryPath));
        }
        if (reparse) {
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                iterator.disable_recursion_pending();
            }
            diagnostics.push_back(warning(
                DiagnosticCode::ReparsePointSkipped,
                "A symbolic link, junction, or other reparse point was skipped.",
                entryPath));
        } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && isSctPath(entryPath)) {
            candidates.push_back(entryPath);
            if (observer) {
                observer(DatasetScanProgress{
                    DatasetScanPhase::Discovering,
                    candidates.size(),
                    std::nullopt,
                    entryPath,
                });
            }
            if (stopToken.stop_requested()) {
                return Result<ScannedCatalog>::failure(cancelled());
            }
        }

        iterator.increment(iteratorError);
        if (iteratorError) {
            return Result<ScannedCatalog>::failure(error(
                DiagnosticCode::DatasetEnumerationFailed,
                "The dataset directory could not be enumerated completely: " +
                    iteratorError.message(),
                entryPath));
        }
    }

    std::vector<std::pair<AssetLocator, std::filesystem::path>> located{};
    located.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        const auto relative = candidate.lexically_relative(root);
        auto locator = AssetLocator::fromRelativePath(relative);
        if (!locator) {
            return Result<ScannedCatalog>::failure(locator.diagnostics());
        }
        located.emplace_back(std::move(locator).takeValue(), candidate);
    }
    std::ranges::sort(located, [](const auto& left, const auto& right) {
        if (left.first.identityKey() != right.first.identityKey()) {
            return left.first.identityKey() < right.first.identityKey();
        }
        return left.first.path().generic_wstring() < right.first.path().generic_wstring();
    });

    for (std::size_t index = 1; index < located.size(); ++index) {
        if (located[index - 1].first == located[index].first) {
            return Result<ScannedCatalog>::failure(error(
                DiagnosticCode::DuplicateAssetLocator,
                "Two SCT assets have the same case-insensitive locator.",
                located[index].second));
        }
    }
    if (located.empty()) {
        return Result<ScannedCatalog>::failure(error(
            DiagnosticCode::NoSctAssets,
            "The dataset does not contain any SCT files.",
            root));
    }

    std::vector<AssetDescriptor> assets{};
    assets.reserve(located.size());
    for (auto& [locator, path] : located) {
        if (stopToken.stop_requested()) {
            return Result<ScannedCatalog>::failure(cancelled());
        }
        auto bytes = readFile(path);
        if (!bytes) {
            return Result<ScannedCatalog>::failure(bytes.diagnostics());
        }
        auto revision = sha256(bytes.value());
        if (!revision) {
            return Result<ScannedCatalog>::failure(revision.diagnostics());
        }
        assets.push_back(AssetDescriptor{
            std::move(locator),
            static_cast<std::uint64_t>(bytes.value().size()),
            SourceRevision{ std::move(revision).takeValue() },
        });
        if (observer) {
            observer(DatasetScanProgress{
                DatasetScanPhase::Hashing,
                assets.size(),
                located.size(),
                path,
            });
        }
        if (stopToken.stop_requested()) {
            return Result<ScannedCatalog>::failure(cancelled());
        }
    }

    auto fingerprint = fingerprintCatalog(assets);
    if (!fingerprint) {
        return Result<ScannedCatalog>::failure(fingerprint.diagnostics());
    }
    return Result<ScannedCatalog>::success(ScannedCatalog{
        AssetCatalogSnapshot{ std::move(assets), std::move(fingerprint).takeValue() },
        std::move(diagnostics),
    });
}

}  // namespace

LocalGameProject::LocalGameProject(
    LocalGameProjectOptions options,
    DatasetContext dataset,
    AssetCatalogSnapshot catalog)
    : options_(std::move(options)),
      dataset_(std::move(dataset)),
      catalog_(std::move(catalog)) {}

Result<LocalGameProject> LocalGameProject::inspect(
    const LocalGameProjectOptions& options,
    const std::stop_token stopToken,
    const DatasetScanObserver& observer) {
    if (stopToken.stop_requested()) {
        return Result<LocalGameProject>::failure(cancelled());
    }
    if (options.datasetRoot.empty()) {
        return Result<LocalGameProject>::failure(error(
            DiagnosticCode::InvalidDatasetRoot,
            "A dataset root is required."));
    }

    DWORD inputAttributes = INVALID_FILE_ATTRIBUTES;
    if (isReparsePoint(options.datasetRoot, inputAttributes)) {
        return Result<LocalGameProject>::failure(error(
            DiagnosticCode::InvalidDatasetRoot,
            "The dataset root cannot be a symbolic link, junction, or other reparse point.",
            options.datasetRoot));
    }
    if (inputAttributes == INVALID_FILE_ATTRIBUTES ||
        (inputAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return Result<LocalGameProject>::failure(error(
            DiagnosticCode::InvalidDatasetRoot,
            "The dataset root must be an existing directory.",
            options.datasetRoot));
    }

    std::error_code canonicalError{};
    auto root = std::filesystem::canonical(options.datasetRoot, canonicalError);
    if (canonicalError) {
        return Result<LocalGameProject>::failure(error(
            DiagnosticCode::InvalidDatasetRoot,
            "The dataset root could not be resolved: " + canonicalError.message(),
            options.datasetRoot));
    }

    auto scanned = scanDataset(root, stopToken, observer);
    if (!scanned) {
        return Result<LocalGameProject>::failure(scanned.diagnostics());
    }
    auto scan = std::move(scanned).takeValue();
    auto catalog = std::move(scan.catalog);
    auto diagnostics = std::move(scan.diagnostics);
    auto projectOptions = options;
    projectOptions.datasetRoot = root;
    DatasetContext dataset{
        root,
        DatasetIdentity{ options.platform, options.region, catalog.fingerprint },
    };
    return Result<LocalGameProject>::success(
        LocalGameProject(std::move(projectOptions), std::move(dataset), std::move(catalog)),
        std::move(diagnostics));
}

Result<LocalGameProject> LocalGameProject::rescan(
    const std::stop_token stopToken,
    const DatasetScanObserver& observer) const {
    return inspect(options_, stopToken, observer);
}

const DatasetContext& LocalGameProject::dataset() const noexcept {
    return dataset_;
}

const AssetCatalog& LocalGameProject::assets() const noexcept {
    return *this;
}

const AssetCatalogSnapshot& LocalGameProject::snapshot() const noexcept {
    return catalog_;
}

Result<SourceAssetSnapshot> LocalGameProject::loadAsset(
    const AssetLocator& locator) const {
    const auto found = std::ranges::lower_bound(
        catalog_.assets,
        locator,
        {},
        &AssetDescriptor::locator);
    if (found == catalog_.assets.end() || found->locator != locator) {
        return Result<SourceAssetSnapshot>::failure(error(
            DiagnosticCode::AssetNotFound,
            "The requested asset is not present in this project snapshot.",
            locator.path()));
    }

    auto safePath = rejectReparseComponents(dataset_.root, locator);
    if (!safePath) {
        return Result<SourceAssetSnapshot>::failure(safePath.diagnostics());
    }

    const auto sourcePath = dataset_.root / locator.path();
    std::error_code canonicalError{};
    const auto canonicalSource = std::filesystem::canonical(sourcePath, canonicalError);
    if (canonicalError) {
        return Result<SourceAssetSnapshot>::failure(error(
            DiagnosticCode::AssetNotFound,
            "The source asset no longer exists or cannot be resolved.",
            sourcePath));
    }
    if (!pathIsWithin(dataset_.root, canonicalSource)) {
        return Result<SourceAssetSnapshot>::failure(error(
            DiagnosticCode::AssetOutsideDataset,
            "The source asset resolves outside the dataset root.",
            sourcePath));
    }

    auto bytes = readFile(canonicalSource);
    if (!bytes) {
        return Result<SourceAssetSnapshot>::failure(bytes.diagnostics());
    }
    auto revision = sha256(bytes.value());
    if (!revision) {
        return Result<SourceAssetSnapshot>::failure(revision.diagnostics());
    }
    if (revision.value() != found->revision.digest ||
        bytes.value().size() != found->byteSize) {
        return Result<SourceAssetSnapshot>::failure(error(
            DiagnosticCode::SourceChanged,
            "The source asset changed after the project snapshot was created; rescan before loading it.",
            canonicalSource));
    }

    return Result<SourceAssetSnapshot>::success(SourceAssetSnapshot{
        *found,
        std::move(bytes).takeValue(),
    });
}

}  // namespace salsa::core
