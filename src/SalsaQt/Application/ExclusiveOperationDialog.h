#pragma once

#include "Application/ExclusiveOperationController.h"
#include "SalsaCore/Application/ExclusiveOperationFlow.h"

#include <QDialog>

#include <memory>
#include <string>
#include <unordered_map>

class QCloseEvent;
class QDialogButtonBox;
class QLabel;
class QProgressBar;
class QStackedWidget;

namespace salsa::qt {

class ExclusiveOperationDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ExclusiveOperationDialog(
        std::unique_ptr<ExclusiveOperationController> controller,
        QWidget* parent = nullptr);
    [[nodiscard]] bool valid() const noexcept;
    void requestApplicationClose();

signals:
    void sessionDone();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void dispatch(const QString& event);
    void showCurrentPage();
    void rebuildActions();
    void applyPageLayout(core::ExclusiveOperationPageLayout layout);
    void requestCancel();
    void finishAndClose();
    [[nodiscard]] QString progressText(const QString& phase, quint64 completed,
        quint64 total, ExclusiveOperationProgressUnit unit,
        const QString& currentItem) const;

    std::unique_ptr<ExclusiveOperationController> controller_;
    core::ExclusiveOperationFlow flow_;
    QStackedWidget* pages_ = nullptr;
    QLabel* phase_ = nullptr;
    QLabel* diagnostics_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    std::unordered_map<std::string, QWidget*> pageWidgets_;
    bool valid_ = false;
    bool closing_ = false;
    bool initialSizeApplied_ = false;
};

}  // namespace salsa::qt
