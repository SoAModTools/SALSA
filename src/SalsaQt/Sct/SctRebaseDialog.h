#pragma once

#include "Application/ExclusiveOperationController.h"
#include "SalsaCore/Persistence/SctWorkspaceRebase.h"

#include <QFutureWatcher>

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

class QLabel;
class QTreeWidget;
class QVBoxLayout;

namespace salsa::qt {

class MainWindow;

class SctRebaseController final : public ExclusiveOperationController {
    Q_OBJECT

public:
    using CommitHandler = std::function<QString(const std::vector<core::AssetLocator>&)>;

    SctRebaseController(core::LocalGameProject project,
        std::shared_ptr<const core::LocalSalsaWorkspace> workspace,
        CommitHandler committed, QObject* parent = nullptr);
    ~SctRebaseController() override;

    [[nodiscard]] QString title() const override;
    [[nodiscard]] core::ExclusiveOperationFlowDefinition flowDefinition() const override;
    [[nodiscard]] QWidget* createPage(std::string_view pageId, QWidget* parent) override;
    [[nodiscard]] std::optional<ExclusiveOperationAction> actionForEdge(
        std::string_view edgeId) const override;
    [[nodiscard]] bool edgeEnabled(std::string_view edgeId) const override;
    void handleEvent(std::string_view event) override;
    void pageEntered(std::string_view pageId) override;
    void requestCancel() override;

private:
    struct PreparedAsset final {
        core::SctStalePatchCandidate candidate;
        std::optional<core::SctWorkspaceRebasePlan> plan{};
        core::SctPatchRebasePreview preview{};
        std::vector<core::SctMergeResolution> resolutions{};
        std::vector<core::Diagnostic> diagnostics{};
        bool selected = false;
    };

    QWidget* createProcessingPage(QWidget* parent);
    QWidget* createReviewPage(QWidget* parent);
    QWidget* createEditorPage(QWidget* parent);
    QWidget* createCommitPage(QWidget* parent);
    QWidget* createSummaryPage(QWidget* parent);
    void startDiscovery();
    void finishDiscovery();
    void finishPreparation();
    void rebuildTree();
    void beginSemanticEdit(std::size_t assetIndex, std::string conflictId);
    bool applySemanticEdit();
    void setResolution(std::size_t assetIndex, const std::string& conflictId,
        core::SctMergeResolutionKind kind,
        std::optional<core::SctSemanticState> edited = std::nullopt);
    void buildEditor();
    void commitSelected();
    void finishCommit();
    void finishWith(QString text, std::string_view event);

    core::LocalGameProject project_;
    std::shared_ptr<const core::LocalSalsaWorkspace> workspace_;
    CommitHandler committed_;
    QTreeWidget* tree_ = nullptr;
    QLabel* reviewStatus_ = nullptr;
    QLabel* processingStatus_ = nullptr;
    QLabel* commitStatus_ = nullptr;
    QLabel* summaryStatus_ = nullptr;
    QWidget* editorPage_ = nullptr;
    QVBoxLayout* editorLayout_ = nullptr;
    MainWindow* editor_ = nullptr;
    std::optional<std::size_t> editingAsset_{};
    std::string editingConflict_{};
    std::stop_source stopSource_{};
    QFutureWatcher<core::SctRebaseDiscovery> discoveryWatcher_{};
    QFutureWatcher<std::vector<PreparedAsset>> preparationWatcher_{};
    QFutureWatcher<core::SctWorkspaceRebaseCommitResult> commitWatcher_{};
    std::vector<PreparedAsset> assets_{};
    QString summary_{};
};

}  // namespace salsa::qt
