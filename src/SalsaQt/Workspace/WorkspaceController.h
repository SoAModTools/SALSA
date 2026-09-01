#pragma once

#include "SalsaCore/Foundation/Diagnostic.h"
#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/Project/LocalGameProject.h"

#include <QFutureWatcher>
#include <QObject>
#include <QString>

#include <cstdint>
#include <optional>
#include <stop_token>
#include <vector>

namespace salsa::qt {

class WorkspaceController final : public QObject {
    Q_OBJECT

public:
    enum class Operation {
        None,
        Opening,
        Refreshing,
    };
    Q_ENUM(Operation)

    explicit WorkspaceController(QObject* parent = nullptr);
    ~WorkspaceController() override;

    [[nodiscard]] bool openDataset(const QString& rootPath);
    [[nodiscard]] bool refresh();
    void closeWorkspace();
    void cancel();
    void selectAsset(std::optional<core::AssetLocator> locator);

    [[nodiscard]] bool hasWorkspace() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] Operation operation() const noexcept;
    [[nodiscard]] const core::DatasetContext* dataset() const noexcept;
    [[nodiscard]] const core::AssetCatalogSnapshot* catalog() const noexcept;
    [[nodiscard]] std::optional<core::AssetDescriptor> selectedAsset() const;
    [[nodiscard]] const std::optional<core::AssetLocator>& selectedLocator() const noexcept;
    [[nodiscard]] const std::vector<core::Diagnostic>& diagnostics() const noexcept;

signals:
    void workspaceChanged();
    void selectionChanged();
    void diagnosticsChanged();
    void operationStateChanged();
    void progressChanged(
        bool determinate,
        int completed,
        int total,
        const QString& currentPath);
    void operationCompleted(
        salsa::qt::WorkspaceController::Operation operation,
        bool success,
        bool cancelled,
        const QString& message);
    void datasetOpened(const QString& canonicalRoot);

private:
    void beginOperation(Operation operation);
    void handleProgress(std::uint64_t generation, const core::DatasetScanProgress& progress);
    void onOperationFinished();
    void clearWorkspaceState();

    QFutureWatcher<core::Result<core::LocalGameProject>> watcher_{};
    std::optional<core::LocalGameProject> project_{};
    std::optional<core::AssetLocator> selectedLocator_{};
    std::vector<core::Diagnostic> diagnostics_{};
    std::stop_source stopSource_{};
    Operation operation_ = Operation::None;
    std::uint64_t generation_ = 0;
    std::uint64_t runningGeneration_ = 0;
};

}  // namespace salsa::qt
