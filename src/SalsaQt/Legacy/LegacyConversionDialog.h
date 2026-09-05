#pragma once

#include "Application/ExclusiveOperationController.h"
#include "SalsaCore/Legacy/LegacyConversionService.h"
#include "SalsaCore/Legacy/LegacyFreshImport.h"

#include <QFutureWatcher>

#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QTableWidget;
class QWidget;

namespace salsa::qt {

class LegacyImportController final : public ExclusiveOperationController {
    Q_OBJECT

public:
    using Completion = std::function<void(QString source, QString workspace)>;

    explicit LegacyImportController(Completion completion = {}, QObject* parent = nullptr);
    ~LegacyImportController() override;

    [[nodiscard]] QString title() const override;
    [[nodiscard]] core::ExclusiveOperationFlowDefinition flowDefinition() const override;
    [[nodiscard]] QWidget* createPage(std::string_view pageId, QWidget* parent) override;
    [[nodiscard]] std::optional<ExclusiveOperationAction> actionForEdge(
        std::string_view edgeId) const override;
    [[nodiscard]] bool edgeEnabled(std::string_view edgeId) const override;
    void handleEvent(std::string_view event) override;
    void requestCancel() override;

private:
    QWidget* createConfigurationPage(QWidget* parent);
    QWidget* createProcessingPage(QWidget* parent);
    QWidget* createReviewPage(QWidget* parent);
    QWidget* createCommitPage(QWidget* parent);
    QWidget* createSummaryPage(QWidget* parent);
    void chooseSource();
    void chooseSourceDestination();
    void chooseWorkspaceDestination();
    void startConversion();
    void finishConversion();
    void populateReview();
    void startPreparation();
    void finishPreparation();
    void populateCommitReview();
    void startCommit();
    void finishCommit();
    void cleanupTransientArtifacts();
    [[nodiscard]] core::FreshLegacyImportRequest importRequest() const;
    [[nodiscard]] QString diagnosticText(
        const std::vector<core::Diagnostic>& diagnostics) const;

    QLineEdit* source_ = nullptr;
    QLineEdit* sourceDestination_ = nullptr;
    QLineEdit* workspaceDestination_ = nullptr;
    QLineEdit* customTarget_ = nullptr;
    QCheckBox* trusted_ = nullptr;
    QCheckBox* retainOriginal_ = nullptr;
    QCheckBox* disableLimits_ = nullptr;
    QCheckBox* disableLimitsConfirmed_ = nullptr;
    QComboBox* scriptWorkers_ = nullptr;
    QComboBox* target_ = nullptr;
    QComboBox* platform_ = nullptr;
    QComboBox* region_ = nullptr;
    QComboBox* characterEncoding_ = nullptr;
    QComboBox* messageSpace_ = nullptr;
    QComboBox* byteOrder_ = nullptr;
    QComboBox* wrapper_ = nullptr;
    QLabel* configurationStatus_ = nullptr;
    QLabel* processingStatus_ = nullptr;
    QLabel* reviewStatus_ = nullptr;
    QLabel* commitStatus_ = nullptr;
    QLabel* summaryStatus_ = nullptr;
    QTableWidget* scripts_ = nullptr;
    QTableWidget* metadata_ = nullptr;

    QFutureWatcher<core::LegacyConversionResult> conversionWatcher_{};
    QFutureWatcher<core::Result<core::FreshLegacyImportPreparation>> preparationWatcher_{};
    QFutureWatcher<core::FreshLegacyImportCommitResult> commitWatcher_{};
    std::stop_source stopSource_{};
    std::optional<core::LegacyConversionResult> conversion_{};
    std::optional<core::FreshLegacyImportPreparation> preparation_{};
    std::optional<core::FreshLegacyImportCommitResult> commit_{};
    std::filesystem::path capsuleStage_{};
    std::filesystem::path sourceStage_{};
    std::filesystem::path workspaceStage_{};
    std::filesystem::path recoveryRegistry_{};
    Completion completion_{};
    bool activityReported_ = false;
};

}  // namespace salsa::qt
