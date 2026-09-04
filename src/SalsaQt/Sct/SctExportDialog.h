#pragma once

#include "Application/ExclusiveOperationController.h"
#include "SalsaCore/Project/LocalGameProject.h"
#include "SalsaCore/Sct/SctPublication.h"

#include <filesystem>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

namespace salsa::qt {

class SctDocumentController;

class SctPublicationController final : public ExclusiveOperationController {
    Q_OBJECT
public:
    SctPublicationController(SctDocumentController* documents,
        core::LocalGameProject project, core::AssetLocator locator,
        core::RevisionId revision, core::SctPublicationDefaults defaults,
        std::filesystem::path sourcePath, QString initialDirectory,
        QObject* parent = nullptr);
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
    QWidget* createReviewPage(QWidget* parent);
    QWidget* createProcessingPage(QWidget* parent);
    QWidget* createSummaryPage(QWidget* parent);
    void chooseDestination();
    [[nodiscard]] bool settingsComplete() const;
    [[nodiscard]] bool replacingSource() const;
    [[nodiscard]] core::SctPublicationOptions options() const;
    [[nodiscard]] std::filesystem::path destination() const;
    void startPublication();
    SctDocumentController* documents_ = nullptr;
    core::LocalGameProject project_;
    core::AssetLocator locator_;
    core::RevisionId revision_{};
    core::SctPublicationDefaults defaults_{};
    std::filesystem::path sourcePath_{};
    QString initialDirectory_{};
    QLineEdit* destinationEdit_ = nullptr;
    QComboBox* platformCombo_ = nullptr;
    QComboBox* characterEncodingCombo_ = nullptr;
    QComboBox* messageSpaceCombo_ = nullptr;
    QComboBox* byteOrderCombo_ = nullptr;
    QComboBox* wrapperCombo_ = nullptr;
    QLabel* configurationStatus_ = nullptr;
    QLabel* reviewStatus_ = nullptr;
    QLabel* processingStatus_ = nullptr;
    QLabel* summaryStatus_ = nullptr;
    QString summary_{};
    bool started_ = false;
};
}  // namespace salsa::qt
