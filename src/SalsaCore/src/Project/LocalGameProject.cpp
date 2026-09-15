#include "SalsaCore/Project/LocalGameProject.h"

#include <Windows.h>
#include "SalsaCore/Foundation/BackgroundConcurrency.h"
#include <condition_variable>
#include <thread>

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
    const std::filesystem::path& path, std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max()) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return Result<std::vector<std::byte>>::failure(error(
            DiagnosticCode::AssetReadFailed,
            "The source asset could not be opened for reading.",
            path));
    }

    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > maximum || static_cast<std::uintmax_t>(end) >
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
    const DatasetScanObserver& observer, const DatasetScanPipeline& pipeline) {
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

    if (!pipeline.workingByteBudget || !pipeline.sourceExpansionFactor)
        return Result<ScannedCatalog>::failure(error(DiagnosticCode::AssetReadFailed, "Invalid scan working-memory budget."));
    std::vector<std::uint64_t> reservations;
    for (const auto& [locator, path] : located) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size > pipeline.workingByteBudget / pipeline.sourceExpansionFactor)
            return Result<ScannedCatalog>::failure(error(DiagnosticCode::AssetReadFailed,
                "The source cannot fit the scan working-memory budget.", path));
        reservations.push_back(std::min(pipeline.workingByteBudget,
            std::max<std::uint64_t>(1024 * 1024, size * pipeline.sourceExpansionFactor)));
    }
    std::vector<std::optional<AssetDescriptor>> descriptors(located.size());
    std::stop_source combined;
    std::stop_callback cancel(stopToken, [&] { combined.request_stop(); });
    std::mutex queueMutex, observerMutex;
    std::condition_variable_any changed;
    std::size_t next = 0, completed = 0;
    std::uint64_t reserved = 0;
    std::optional<std::vector<Diagnostic>> failed;
    auto budget = backgroundConcurrency();
    const auto count = std::min(located.size(), pipeline.workers
        ? std::min(pipeline.workers, static_cast<std::size_t>(budget->limit()))
        : static_cast<std::size_t>(budget->limit()));
    {
        std::vector<std::jthread> workers;
        struct StopOnExit { std::stop_source& stop; ~StopOnExit() { stop.request_stop(); } } stopOnExit{combined};
        for (std::size_t worker = 0; worker < count; ++worker) workers.emplace_back([&] {
            try {
                for (;;) {
                    std::size_t ordinal;
                    {
                        std::unique_lock lock(queueMutex);
                        changed.wait(lock, combined.get_token(), [&] {
                            return next == located.size() || reservations[next] <= pipeline.workingByteBudget - reserved;
                        });
                        if (combined.stop_requested() || next == located.size()) return;
                        ordinal = next++;
                        reserved += reservations[ordinal];
                    }
                    auto permit = budget->acquire(combined.get_token());
                    if (!permit) return;
                    auto process = [&]() -> Result<void> {
                        const auto& [locator, path] = located[ordinal];
                        auto safe = rejectReparseComponents(root, locator);
                        if (!safe) return safe;
                        // Check growth before allocation, so a changing input cannot evade the reservation.
                        auto bytes = readFile(path, reservations[ordinal] / pipeline.sourceExpansionFactor);
                        if (!bytes) return Result<void>::failure(bytes.diagnostics());
                        if (combined.stop_requested()) return Result<void>::failure(cancelled());
                        auto source = CapturedSource::capture(locator, std::move(bytes).takeValue());
                        if (!source) return Result<void>::failure(source.diagnostics());
                        if (pipeline.consume) {
                            auto consumed = pipeline.consume(ordinal, source.value(), combined.get_token());
                            if (!consumed) return consumed;
                        }
                        descriptors[ordinal] = source.value().descriptor();
                        return Result<void>::success();
                    }(); // Source bytes and parser graphs are released before the next job.
                    permit.reset();
                    {
                        std::lock_guard lock(queueMutex);
                        reserved -= reservations[ordinal];
                        if (!process && !failed) failed = process.diagnostics();
                    }
                    if (!process) combined.request_stop();
                    changed.notify_all();
                    if (!process) return;
                    std::lock_guard lock(observerMutex);
                    if (observer) observer({DatasetScanPhase::Hashing, ++completed, located.size(), located[ordinal].second});
                }
            } catch (const std::exception& e) {
                {
                    std::lock_guard lock(queueMutex);
                    if (!failed) failed = std::vector<Diagnostic>{error(DiagnosticCode::AssetReadFailed, e.what())};
                }
                combined.request_stop(); changed.notify_all();
            }
        });
        // Join here without requesting stop during a successful scan.
        for (auto& worker : workers) worker.join();
    }
    if (failed) return Result<ScannedCatalog>::failure(std::move(*failed));
    if (stopToken.stop_requested()) return Result<ScannedCatalog>::failure(cancelled());
    std::vector<AssetDescriptor> assets;
    assets.reserve(descriptors.size());
    for (auto& descriptor : descriptors) {
        if (!descriptor) return Result<ScannedCatalog>::failure(cancelled());
        assets.push_back(std::move(*descriptor));
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
    const DatasetScanObserver& observer, const DatasetScanPipeline& pipeline) {
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

    auto scanned = scanDataset(root, stopToken, observer, pipeline);
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

Result<SourceAssetSnapshot> LocalGameProject::loadContextAsset(const AssetLocator& locator) const {
    auto safe = rejectReparseComponents(dataset_.root, locator);
    if (!safe) return Result<SourceAssetSnapshot>::failure(safe.diagnostics());
    std::error_code ec;
    auto path = std::filesystem::canonical(dataset_.root / locator.path(), ec);
    if (ec || !pathIsWithin(dataset_.root, path))
        return Result<SourceAssetSnapshot>::failure(error(DiagnosticCode::AssetOutsideDataset,
            "The selected context asset cannot be resolved inside the dataset.", locator.path()));
    auto bytes = readFile(path);
    if (!bytes) return Result<SourceAssetSnapshot>::failure(bytes.diagnostics());
    auto digest = sha256(bytes.value());
    if (!digest) return Result<SourceAssetSnapshot>::failure(digest.diagnostics());
    AssetDescriptor descriptor{locator, bytes.value().size(), SourceRevision{digest.value()}};
    return Result<SourceAssetSnapshot>::success({std::move(descriptor), std::move(bytes).takeValue()});
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
