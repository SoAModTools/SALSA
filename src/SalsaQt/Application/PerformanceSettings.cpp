#include "Application/PerformanceSettings.h"
#include "SalsaCore/Foundation/BackgroundConcurrency.h"
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QSettings>
#include <QSpinBox>
#include <algorithm>
#include <thread>
namespace salsa::qt {
namespace {
constexpr auto Key = "performance/backgroundConcurrency";
unsigned automatic() { return core::automaticBackgroundWorkers(std::thread::hardware_concurrency()); }
void apply(int selected) {
    core::backgroundConcurrency()->setLimit(
        selected > 0
            ? std::min(static_cast<unsigned>(selected), std::max(256u, std::thread::hardware_concurrency()))
            : automatic());
}
} // namespace
PerformanceSettings::PerformanceSettings(QWidget *host) : QObject(host), host_(host) {
    apply(QSettings{}.value(Key, 0).toInt());
    action_ = new QAction(tr("Performance…"), this);
    connect(action_, &QAction::triggered, this, [this] { edit(); });
}
void PerformanceSettings::edit() {
    QDialog dialog(host_);
    dialog.setWindowTitle(tr("Performance"));
    auto *form = new QFormLayout(&dialog);
    auto *count = new QSpinBox(&dialog);
    count->setRange(0, static_cast<int>(std::max(256u, std::thread::hardware_concurrency())));
    count->setSpecialValueText(tr("Automatic (%1 workers)").arg(automatic()));
    count->setValue(QSettings{}.value(Key, 0).toInt());
    form->addRow(tr("Background concurrency"), count);
    auto *help = new QLabel(
        tr("Automatic uses half the available logical CPU cores, rounded down, with a minimum of one. This "
           "shared limit currently controls script import and is available for future parallel export. "
           "Memory limits may reduce active workers. Changes apply to subsequent operations."),
        &dialog);
    help->setWordWrap(true);
    form->addRow(help);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QSettings{}.setValue(Key, count->value());
    apply(count->value());
}
} // namespace salsa::qt
