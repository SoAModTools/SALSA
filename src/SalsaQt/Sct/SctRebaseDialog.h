#pragma once

#include "SalsaCore/Persistence/SctWorkspaceRebase.h"

#include <QDialog>
#include <QFutureWatcher>

#include <memory>
#include <stop_token>
#include <vector>

class QLabel;
class QPushButton;
class QTreeWidget;

namespace salsa::qt {

class SctRebaseDialog final : public QDialog {
public:
    SctRebaseDialog(core::LocalGameProject project,
        std::shared_ptr<const core::LocalSalsaWorkspace> workspace,
        QWidget* parent = nullptr);
    ~SctRebaseDialog() override;

    [[nodiscard]] const std::vector<core::AssetLocator>& committedAssets() const noexcept;

protected:
    void reject() override;

private:
    struct PreparedAsset final {
        core::SctStalePatchCandidate candidate;
        std::optional<core::SctWorkspaceRebasePlan> plan{};
        core::SctPatchRebasePreview preview{};
        std::vector<core::SctMergeResolution> resolutions{};
        std::vector<core::Diagnostic> diagnostics{};
        bool selected = false;
    };

    void startDiscovery();
    void finishDiscovery();
    void finishPreparation();
    void rebuildTree();
    void setConflictResolution(std::size_t assetIndex,
        const std::string& conflictId, int choice);
    void commitSelected();
    void finishCommit();
    void syncButtons();

    core::LocalGameProject project_;
    std::shared_ptr<const core::LocalSalsaWorkspace> workspace_;
    QTreeWidget* tree_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* commitButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    std::stop_source stopSource_{};
    QFutureWatcher<core::SctRebaseDiscovery> discoveryWatcher_{};
    QFutureWatcher<std::vector<PreparedAsset>> preparationWatcher_{};
    QFutureWatcher<core::SctWorkspaceRebaseCommitResult> commitWatcher_{};
    std::vector<PreparedAsset> assets_{};
    std::vector<core::AssetLocator> committedAssets_{};
};

}  // namespace salsa::qt
