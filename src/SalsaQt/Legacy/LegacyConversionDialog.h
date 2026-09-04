#pragma once

#include "Application/ExclusiveOperationController.h"
#include "SalsaCore/Legacy/LegacyConversionService.h"

#include <QFutureWatcher>

#include <optional>
#include <stop_token>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QTableWidget;
class QWidget;

namespace salsa::qt {

class LegacyConversionController final : public ExclusiveOperationController {
    Q_OBJECT

public:
    explicit LegacyConversionController(QObject* parent = nullptr);
    ~LegacyConversionController() override;

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
    QWidget* createSummaryPage(QWidget* parent);
    void chooseSource();
    void chooseDestinationParent();
    void startConversion();
    void updateProgress(const core::LegacyConversionProgress& progress);
    void finishConversion();
    void populateSummary();

    QLineEdit* source_ = nullptr;
    QLineEdit* destination_ = nullptr;
    QCheckBox* trusted_ = nullptr;
    QCheckBox* retainOriginal_ = nullptr;
    QCheckBox* disableLimits_ = nullptr;
    QCheckBox* disableLimitsConfirmed_ = nullptr;
    QComboBox* scriptWorkers_ = nullptr;
    QLabel* configurationStatus_ = nullptr;
    QLabel* processingStatus_ = nullptr;
    QLabel* summaryStatus_ = nullptr;
    QTableWidget* scripts_ = nullptr;
    QFutureWatcher<core::LegacyConversionResult> watcher_{};
    std::stop_source stopSource_{};
    std::optional<core::LegacyConversionResult> result_{};
};

}  // namespace salsa::qt
