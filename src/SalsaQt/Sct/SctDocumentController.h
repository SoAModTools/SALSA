#pragma once

#include "SalsaCore/Project/LocalGameProject.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"

#include <QFutureWatcher>
#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace salsa::qt {

class SctDocumentController final : public QObject {
    Q_OBJECT

public:
    enum class SourceStatus { Current, Changed, Missing };
    Q_ENUM(SourceStatus)

    explicit SctDocumentController(QObject* parent = nullptr);
    ~SctDocumentController() override;

    [[nodiscard]] bool openDocument(
        core::LocalGameProject project, const core::AssetLocator& locator);
    [[nodiscard]] bool reloadDocument(
        core::LocalGameProject project, const core::AssetLocator& locator);
    [[nodiscard]] bool selectTextConvention(
        const core::AssetLocator& locator,
        spice::sct::SctKnownTextConvention convention);
    void synchronizeCatalog(const core::AssetCatalogSnapshot& catalog);
    void closeDocument(const core::AssetLocator& locator);
    void closeAll();
    void cancel();

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool contains(const core::AssetLocator& locator) const;
    [[nodiscard]] std::shared_ptr<const core::SctDocumentSnapshot> snapshot(
        const core::AssetLocator& locator) const;
    [[nodiscard]] SourceStatus sourceStatus(const core::AssetLocator& locator) const;
    [[nodiscard]] std::vector<core::AssetLocator> openLocators() const;
    [[nodiscard]] const std::vector<core::Diagnostic>& failureDiagnostics() const noexcept;
    [[nodiscard]] const std::vector<core::SctPipelineDiagnostic>& failurePipelineDiagnostics() const noexcept;

signals:
    void busyChanged();
    void documentChanged(const QString& identityKey);
    void documentClosed(const QString& identityKey);
    void focusRequested(const QString& identityKey);
    void operationCompleted(
        const QString& identityKey, bool success, bool cancelled, const QString& message);

private:
    enum class Operation { None, Opening, Reloading, Reimporting };
    struct DocumentState {
        core::AssetLocator locator;
        std::shared_ptr<const core::SctDocumentSnapshot> snapshot;
        SourceStatus status = SourceStatus::Current;
    };

    void begin(Operation operation, const core::AssetLocator& locator);
    void onFinished();

    QFutureWatcher<core::SctLoadResult> watcher_{};
    std::unordered_map<std::string, DocumentState> documents_{};
    std::optional<core::AssetLocator> runningLocator_{};
    std::stop_source stopSource_{};
    Operation operation_ = Operation::None;
    std::uint64_t generation_ = 0;
    std::uint64_t runningGeneration_ = 0;
    std::vector<core::Diagnostic> failureDiagnostics_{};
    std::vector<core::SctPipelineDiagnostic> failurePipelineDiagnostics_{};
};

}  // namespace salsa::qt
