#pragma once

#include "SalsaCore/Legacy/LegacyConversionService.h"

#include <QDialog>
#include <QFutureWatcher>

#include <stop_token>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace salsa::qt {

class LegacyConversionDialog final : public QDialog {
    Q_OBJECT

public:
    explicit LegacyConversionDialog(QWidget* parent = nullptr);
    ~LegacyConversionDialog() override;

private:
    void chooseSource();
    void chooseDestinationParent();
    void startConversion();
    void cancelConversion();
    void updateActions();
    void updateProgress(const core::LegacyConversionProgress& progress);
    void finishConversion();
    void showSummary(const core::LegacyCapsuleSummary& summary);

    QLineEdit* source_ = nullptr;
    QLineEdit* destination_ = nullptr;
    QCheckBox* trusted_ = nullptr;
    QCheckBox* retainOriginal_ = nullptr;
    QCheckBox* disableLimits_ = nullptr;
    QComboBox* scriptWorkers_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QTableWidget* scripts_ = nullptr;
    QPushButton* sourceBrowse_ = nullptr;
    QPushButton* destinationBrowse_ = nullptr;
    QPushButton* convert_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* close_ = nullptr;
    QFutureWatcher<core::LegacyConversionResult> watcher_{};
    std::stop_source stopSource_{};
};

}  // namespace salsa::qt
