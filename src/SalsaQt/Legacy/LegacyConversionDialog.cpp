#include "Legacy/LegacyConversionDialog.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace salsa::qt {
namespace {

[[nodiscard]] QString phaseText(const core::LegacyConversionPhase phase) {
    switch (phase) {
    case core::LegacyConversionPhase::Staging: return QObject::tr("Staging controlled input");
    case core::LegacyConversionPhase::Hashing: return QObject::tr("Hashing project");
    case core::LegacyConversionPhase::Parsing: return QObject::tr("Reading legacy project");
    case core::LegacyConversionPhase::Script: return QObject::tr("Converting scripts");
    case core::LegacyConversionPhase::Writing: return QObject::tr("Writing capsule");
    case core::LegacyConversionPhase::Validating: return QObject::tr("Validating capsule");
    case core::LegacyConversionPhase::Finalizing: return QObject::tr("Finalizing capsule");
    }
    return QObject::tr("Converting");
}

[[nodiscard]] QString diagnosticText(const std::vector<core::Diagnostic>& diagnostics) {
    QStringList messages;
    for (const auto& diagnostic : diagnostics)
        messages.push_back(QString::fromStdString(diagnostic.message));
    return messages.join(QLatin1Char('\n'));
}

}  // namespace

LegacyConversionDialog::LegacyConversionDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Convert Legacy SALSA Project"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 620);

    source_ = new QLineEdit(this);
    destination_ = new QLineEdit(this);
    sourceBrowse_ = new QPushButton(tr("Browse…"), this);
    destinationBrowse_ = new QPushButton(tr("Choose Parent…"), this);
    auto* sourceRow = new QWidget(this); auto* sourceLayout = new QHBoxLayout(sourceRow);
    sourceLayout->setContentsMargins(0, 0, 0, 0); sourceLayout->addWidget(source_); sourceLayout->addWidget(sourceBrowse_);
    auto* destinationRow = new QWidget(this); auto* destinationLayout = new QHBoxLayout(destinationRow);
    destinationLayout->setContentsMargins(0, 0, 0, 0); destinationLayout->addWidget(destination_); destinationLayout->addWidget(destinationBrowse_);

    auto* form = new QFormLayout;
    form->addRow(tr("Legacy project:"), sourceRow);
    form->addRow(tr("Capsule folder:"), destinationRow);

    trusted_ = new QCheckBox(tr("I trust the source of this legacy .prj file"), this);
    trusted_->setToolTip(tr("Legacy project files are Python pickle containers. Conversion is isolated, but only trusted files should be selected."));
    retainOriginal_ = new QCheckBox(tr("Retain an inert copy of the original .prj as capsule evidence"), this);
    disableLimits_ = new QCheckBox(tr("Advanced: disable resource limits for this conversion"), this);
    status_ = new QLabel(tr("Select an official final SALSA version 7 project."), this);
    status_->setWordWrap(true);
    progress_ = new QProgressBar(this); progress_->setRange(0, 1); progress_->setValue(0);
    scripts_ = new QTableWidget(this); scripts_->setColumnCount(6);
    scripts_->setHorizontalHeaderLabels({tr("Script"), tr("Status"), tr("Sections"),
        tr("Instructions"), tr("Parameters"), tr("Diagnostics")});
    scripts_->setSelectionBehavior(QAbstractItemView::SelectRows);
    scripts_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    scripts_->setAlternatingRowColors(true);
    scripts_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);

    convert_ = new QPushButton(tr("Convert"), this);
    cancel_ = new QPushButton(tr("Cancel Conversion"), this); cancel_->setEnabled(false);
    close_ = new QPushButton(tr("Close"), this);
    auto* buttons = new QHBoxLayout; buttons->addStretch(); buttons->addWidget(convert_);
    buttons->addWidget(cancel_); buttons->addWidget(close_);

    auto* layout = new QVBoxLayout(this); layout->addLayout(form); layout->addWidget(trusted_);
    layout->addWidget(retainOriginal_); layout->addWidget(disableLimits_); layout->addWidget(status_);
    layout->addWidget(progress_); layout->addWidget(scripts_, 1); layout->addLayout(buttons);

    connect(sourceBrowse_, &QPushButton::clicked, this, &LegacyConversionDialog::chooseSource);
    connect(destinationBrowse_, &QPushButton::clicked, this, &LegacyConversionDialog::chooseDestinationParent);
    connect(convert_, &QPushButton::clicked, this, &LegacyConversionDialog::startConversion);
    connect(cancel_, &QPushButton::clicked, this, &LegacyConversionDialog::cancelConversion);
    connect(close_, &QPushButton::clicked, this, &QDialog::close);
    connect(source_, &QLineEdit::textChanged, this, &LegacyConversionDialog::updateActions);
    connect(destination_, &QLineEdit::textChanged, this, &LegacyConversionDialog::updateActions);
    connect(trusted_, &QCheckBox::toggled, this, &LegacyConversionDialog::updateActions);
    connect(&watcher_, &QFutureWatcherBase::finished, this, &LegacyConversionDialog::finishConversion);
    updateActions();
}

LegacyConversionDialog::~LegacyConversionDialog() {
    if (watcher_.isRunning()) { stopSource_.request_stop(); watcher_.waitForFinished(); }
}

void LegacyConversionDialog::chooseSource() {
    const auto selected = QFileDialog::getOpenFileName(this, tr("Select Legacy SALSA Project"),
        source_->text(), tr("Legacy SALSA projects (*.prj);;All files (*)"));
    if (selected.isEmpty()) return;
    source_->setText(QDir::toNativeSeparators(selected));
    if (destination_->text().isEmpty()) {
        const QFileInfo sourceInfo(selected);
        destination_->setText(QDir::toNativeSeparators(sourceInfo.dir().filePath(
            sourceInfo.completeBaseName() + QStringLiteral(".salsa-legacy"))));
    }
    trusted_->setChecked(false);
}

void LegacyConversionDialog::chooseDestinationParent() {
    const auto current = QFileInfo(destination_->text());
    const auto selected = QFileDialog::getExistingDirectory(this, tr("Select Capsule Parent Folder"),
        current.dir().absolutePath(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) return;
    auto name = current.fileName();
    if (name.isEmpty()) name = QFileInfo(source_->text()).completeBaseName() + QStringLiteral(".salsa-legacy");
    destination_->setText(QDir::toNativeSeparators(QDir(selected).filePath(name)));
}

void LegacyConversionDialog::startConversion() {
    if (!convert_->isEnabled()) return;
    if (disableLimits_->isChecked() && QMessageBox::warning(this, tr("Disable resource limits?"),
            tr("This removes size, memory, and structural safeguards for this conversion. "
               "AppContainer isolation, no-network execution, strict version validation, and capsule validation remain enabled."),
            QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok) return;
    if (QFileInfo::exists(destination_->text())) {
        QMessageBox::warning(this, tr("Capsule destination exists"),
            tr("Choose a new folder. Existing capsules are immutable and are never overwritten."));
        return;
    }
    scripts_->setRowCount(0); stopSource_ = std::stop_source{};
    core::LegacyConversionRequest request{};
    request.source = std::filesystem::path(source_->text().toStdWString());
    request.destination = std::filesystem::path(destination_->text().toStdWString());
    request.converterExecutable = std::filesystem::path(
        QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("SalsaLegacyConverter.exe")).toStdWString());
    request.receiptDirectory = std::filesystem::path(QDir(
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(
            QStringLiteral("migration-receipts")).toStdWString());
    request.trustedInputConfirmed = trusted_->isChecked();
    request.retainOriginal = retainOriginal_->isChecked();
    request.disableResourceLimits = disableLimits_->isChecked();
    const auto token = stopSource_.get_token();
    status_->setText(tr("Preparing isolated conversion…"));
    convert_->setEnabled(false); cancel_->setEnabled(true); close_->setEnabled(false);
    watcher_.setFuture(QtConcurrent::run([this, request = std::move(request), token] {
        return core::LegacyConversionService::convert(request, token,
            [this](const core::LegacyConversionProgress& value) {
                QMetaObject::invokeMethod(this, [this, value] { updateProgress(value); }, Qt::QueuedConnection);
            });
    }));
    updateActions();
}

void LegacyConversionDialog::cancelConversion() {
    stopSource_.request_stop(); cancel_->setEnabled(false); status_->setText(tr("Cancelling conversion…"));
}

void LegacyConversionDialog::updateActions() {
    const bool running = watcher_.isRunning();
    convert_->setEnabled(!running && trusted_->isChecked()
        && QFileInfo(source_->text()).isFile() && !destination_->text().trimmed().isEmpty());
    source_->setEnabled(!running); destination_->setEnabled(!running); trusted_->setEnabled(!running);
    sourceBrowse_->setEnabled(!running); destinationBrowse_->setEnabled(!running);
    retainOriginal_->setEnabled(!running); disableLimits_->setEnabled(!running);
}

void LegacyConversionDialog::updateProgress(const core::LegacyConversionProgress& value) {
    const auto text = value.current.empty() ? phaseText(value.phase)
        : tr("%1: %2").arg(phaseText(value.phase), QString::fromStdString(value.current));
    status_->setText(text);
    if (value.total == 0) progress_->setRange(0, 0);
    else { progress_->setRange(0, 1000); progress_->setValue(static_cast<int>(
        std::min<std::uint64_t>(1000, value.completed * 1000 / value.total))); }
}

void LegacyConversionDialog::finishConversion() {
    cancel_->setEnabled(false); close_->setEnabled(true); progress_->setRange(0, 1); progress_->setValue(1);
    const auto result = watcher_.result();
    if (result.capsule) {
        showSummary(*result.capsule);
        status_->setText(result.status == core::LegacyConversionStatus::Ready
            ? tr("Capsule created and validated.")
            : tr("Capsule created and validated. One or more script decisions will be required during import."));
    } else {
        status_->setText(result.status == core::LegacyConversionStatus::Cancelled
            ? tr("Conversion cancelled. No capsule was created.")
            : diagnosticText(result.diagnostics));
        if (result.status != core::LegacyConversionStatus::Cancelled)
            QMessageBox::warning(this, tr("Legacy conversion did not complete"), status_->text());
    }
    trusted_->setChecked(false); updateActions();
}

void LegacyConversionDialog::showSummary(const core::LegacyCapsuleSummary& summary) {
    scripts_->setRowCount(static_cast<int>(summary.scripts.size()));
    for (int row = 0; row < scripts_->rowCount(); ++row) {
        const auto& script = summary.scripts[static_cast<std::size_t>(row)];
        const QStringList values{QString::fromStdString(script.key),
            script.status == core::LegacyScriptStatus::Accepted ? tr("Accepted") : tr("Failed"),
            QString::number(script.sectionCount), QString::number(script.instructionCount),
            QString::number(script.parameterCount), QString::number(script.diagnosticCount)};
        for (int column = 0; column < values.size(); ++column)
            scripts_->setItem(row, column, new QTableWidgetItem(values[column]));
    }
}

}  // namespace salsa::qt
