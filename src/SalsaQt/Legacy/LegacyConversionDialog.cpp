#include "Legacy/LegacyConversionDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace salsa::qt {
namespace {

QString phaseText(const core::LegacyConversionPhase phase) {
    switch (phase) {
    case core::LegacyConversionPhase::Staging: return QObject::tr("Staging controlled input");
    case core::LegacyConversionPhase::Hashing: return QObject::tr("Hashing project");
    case core::LegacyConversionPhase::Parsing: return QObject::tr("Reading legacy project");
    case core::LegacyConversionPhase::Script: return QObject::tr("Analyzing scripts");
    case core::LegacyConversionPhase::Writing: return QObject::tr("Writing script records");
    case core::LegacyConversionPhase::Validating: return QObject::tr("Validating capsule");
    case core::LegacyConversionPhase::Finalizing: return QObject::tr("Finalizing capsule");
    }
    return QObject::tr("Converting");
}

QString diagnosticText(const std::vector<core::Diagnostic>& diagnostics) {
    QStringList messages;
    for (const auto& diagnostic : diagnostics)
        messages.push_back(QString::fromStdString(diagnostic.message));
    return messages.join(QLatin1Char('\n'));
}

}  // namespace

LegacyConversionController::LegacyConversionController(QObject* parent)
    : ExclusiveOperationController(parent) {
    connect(&watcher_, &QFutureWatcherBase::finished,
        this, &LegacyConversionController::finishConversion);
}

LegacyConversionController::~LegacyConversionController() {
    if (!watcher_.isRunning()) return;
    stopSource_.request_stop();
    watcher_.waitForFinished();
}

QString LegacyConversionController::title() const { return tr("Convert Legacy SALSA Project"); }

core::ExclusiveOperationFlowDefinition LegacyConversionController::flowDefinition() const {
    using Role = core::ExclusiveOperationPageRole;
    return {"configure",
        {{"configure", Role::Configuration}, {"processing", Role::Processing},
         {"summary", Role::Summary}},
        {{"start", "configure", "start", "processing"},
         {"abandon", "configure", "cancelled", "summary"},
         {"complete", "processing", "complete", "summary"},
         {"failed", "processing", "failed", "summary"},
         {"cancelled", "processing", "cancelled", "summary"}}};
}

QWidget* LegacyConversionController::createPage(
    const std::string_view pageId, QWidget* parent) {
    if (pageId == "configure") return createConfigurationPage(parent);
    if (pageId == "processing") return createProcessingPage(parent);
    return createSummaryPage(parent);
}

std::optional<ExclusiveOperationAction> LegacyConversionController::actionForEdge(
    const std::string_view edgeId) const {
    if (edgeId == "start") return ExclusiveOperationAction{
        tr("Convert"), ExclusiveOperationActionRole::Primary};
    return std::nullopt;
}

bool LegacyConversionController::edgeEnabled(const std::string_view edgeId) const {
    if (edgeId != "start") return true;
    return source_ && destination_ && trusted_
        && QFileInfo(source_->text()).isFile()
        && !destination_->text().trimmed().isEmpty() && trusted_->isChecked()
        && (!disableLimits_->isChecked() || disableLimitsConfirmed_->isChecked());
}

void LegacyConversionController::handleEvent(const std::string_view event) {
    if (event == "start") startConversion();
    else ExclusiveOperationController::handleEvent(event);
}

void LegacyConversionController::requestCancel() {
    if (!watcher_.isRunning()) {
        result_.emplace();
        result_->status = core::LegacyConversionStatus::Cancelled;
        raiseEvent("cancelled");
        return;
    }
    stopSource_.request_stop();
    setCancellable(false);
    if (processingStatus_) processingStatus_->setText(tr("Cancelling conversion…"));
}

QWidget* LegacyConversionController::createConfigurationPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    source_ = new QLineEdit(page);
    destination_ = new QLineEdit(page);
    auto* sourceBrowse = new QPushButton(tr("Browse…"), page);
    auto* destinationBrowse = new QPushButton(tr("Choose Parent…"), page);
    auto* sourceRow = new QWidget(page);
    auto* sourceLayout = new QHBoxLayout(sourceRow);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceLayout->addWidget(source_);
    sourceLayout->addWidget(sourceBrowse);
    auto* destinationRow = new QWidget(page);
    auto* destinationLayout = new QHBoxLayout(destinationRow);
    destinationLayout->setContentsMargins(0, 0, 0, 0);
    destinationLayout->addWidget(destination_);
    destinationLayout->addWidget(destinationBrowse);
    auto* form = new QFormLayout;
    form->addRow(tr("Legacy project:"), sourceRow);
    form->addRow(tr("Capsule folder:"), destinationRow);
    scriptWorkers_ = new QComboBox(page);
    scriptWorkers_->addItem(tr("Auto (up to 4)"), 0);
    scriptWorkers_->addItem(tr("Serial"), 1);
    scriptWorkers_->addItem(tr("2"), 2);
    scriptWorkers_->addItem(tr("3"), 3);
    scriptWorkers_->addItem(tr("4"), 4);
    scriptWorkers_->setCurrentIndex(std::clamp(QSettings{}.value(
        QStringLiteral("legacyConversion/scriptWorkers"), 0).toInt(), 0, 4));
    form->addRow(tr("Script processing:"), scriptWorkers_);
    trusted_ = new QCheckBox(tr("I trust the source of this legacy .prj file"), page);
    retainOriginal_ = new QCheckBox(
        tr("Retain an inert copy of the original .prj as capsule evidence"), page);
    disableLimits_ = new QCheckBox(
        tr("Advanced: disable resource limits for this conversion"), page);
    disableLimitsConfirmed_ = new QCheckBox(tr(
        "I understand that size, memory, and structural limits will be removed"), page);
    disableLimitsConfirmed_->hide();
    configurationStatus_ = new QLabel(
        tr("Select an official final SALSA version 7 project."), page);
    configurationStatus_->setWordWrap(true);
    auto* layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addWidget(trusted_);
    layout->addWidget(retainOriginal_);
    layout->addWidget(disableLimits_);
    layout->addWidget(disableLimitsConfirmed_);
    layout->addWidget(configurationStatus_);
    layout->addStretch();
    connect(sourceBrowse, &QPushButton::clicked, this, &LegacyConversionController::chooseSource);
    connect(destinationBrowse, &QPushButton::clicked,
        this, &LegacyConversionController::chooseDestinationParent);
    for (auto* edit : {source_, destination_})
        connect(edit, &QLineEdit::textChanged, this,
            &ExclusiveOperationController::presentationChanged);
    connect(trusted_, &QCheckBox::toggled, this,
        &ExclusiveOperationController::presentationChanged);
    connect(disableLimits_, &QCheckBox::toggled, this, [this](const bool checked) {
        disableLimitsConfirmed_->setVisible(checked);
        if (!checked) disableLimitsConfirmed_->setChecked(false);
        emit presentationChanged();
    });
    connect(disableLimitsConfirmed_, &QCheckBox::toggled, this,
        &ExclusiveOperationController::presentationChanged);
    return page;
}

QWidget* LegacyConversionController::createProcessingPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    processingStatus_ = new QLabel(tr(
        "SALSA is converting the project in its isolated helper process."), page);
    processingStatus_->setWordWrap(true);
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(processingStatus_);
    layout->addStretch();
    return page;
}

QWidget* LegacyConversionController::createSummaryPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    summaryStatus_ = new QLabel(page);
    summaryStatus_->setWordWrap(true);
    scripts_ = new QTableWidget(page);
    scripts_->setColumnCount(6);
    scripts_->setHorizontalHeaderLabels({tr("Script"), tr("Status"), tr("Sections"),
        tr("Instructions"), tr("Parameters"), tr("Diagnostics")});
    scripts_->setSelectionBehavior(QAbstractItemView::SelectRows);
    scripts_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    scripts_->setAlternatingRowColors(true);
    scripts_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(summaryStatus_);
    layout->addWidget(scripts_, 1);
    populateSummary();
    return page;
}

void LegacyConversionController::chooseSource() {
    const auto selected = QFileDialog::getOpenFileName(source_->window(),
        tr("Select Legacy SALSA Project"), source_->text(),
        tr("Legacy SALSA projects (*.prj);;All files (*)"));
    if (selected.isEmpty()) return;
    source_->setText(QDir::toNativeSeparators(selected));
    if (destination_->text().isEmpty()) {
        const QFileInfo info(selected);
        destination_->setText(QDir::toNativeSeparators(info.dir().filePath(
            info.completeBaseName() + QStringLiteral(".salsa-legacy"))));
    }
    trusted_->setChecked(false);
}

void LegacyConversionController::chooseDestinationParent() {
    const QFileInfo current(destination_->text());
    const auto selected = QFileDialog::getExistingDirectory(destination_->window(),
        tr("Select Capsule Parent Folder"), current.dir().absolutePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) return;
    auto name = current.fileName();
    if (name.isEmpty()) name = QFileInfo(source_->text()).completeBaseName()
        + QStringLiteral(".salsa-legacy");
    destination_->setText(QDir::toNativeSeparators(QDir(selected).filePath(name)));
}

void LegacyConversionController::startConversion() {
    if (!edgeEnabled("start")) return;
    if (QFileInfo::exists(destination_->text())) {
        configurationStatus_->setText(tr(
            "Choose a new destination. Existing capsules are immutable and are never overwritten."));
        return;
    }
    core::LegacyConversionRequest request{};
    request.source = std::filesystem::path(source_->text().toStdWString());
    request.destination = std::filesystem::path(destination_->text().toStdWString());
    request.converterExecutable = std::filesystem::path(QDir(
        QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("SalsaLegacyConverter.exe")).toStdWString());
    request.receiptDirectory = std::filesystem::path(QDir(
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(
            QStringLiteral("migration-receipts")).toStdWString());
    request.trustedInputConfirmed = trusted_->isChecked();
    request.retainOriginal = retainOriginal_->isChecked();
    request.disableResourceLimits = disableLimits_->isChecked();
    request.scriptWorkers = scriptWorkers_->currentData().toUInt();
    QSettings{}.setValue(QStringLiteral("legacyConversion/scriptWorkers"),
        static_cast<int>(request.scriptWorkers));
    stopSource_ = std::stop_source{};
    result_.reset();
    setCancellable(true);
    raiseEvent("start");
    const auto token = stopSource_.get_token();
    watcher_.setFuture(QtConcurrent::run([this, request = std::move(request), token] {
        return core::LegacyConversionService::convert(request, token,
            [this](const core::LegacyConversionProgress& value) {
                QMetaObject::invokeMethod(this,
                    [this, value] { updateProgress(value); }, Qt::QueuedConnection);
            });
    }));
}

void LegacyConversionController::updateProgress(
    const core::LegacyConversionProgress& value) {
    if (value.phase == core::LegacyConversionPhase::Finalizing) {
        setCancellable(false);
        setFinishing(true);
    }
    const auto unit = value.phase == core::LegacyConversionPhase::Script
        || value.phase == core::LegacyConversionPhase::Writing
        ? ExclusiveOperationProgressUnit::Scripts
        : value.phase == core::LegacyConversionPhase::Hashing
            ? ExclusiveOperationProgressUnit::Bytes
            : ExclusiveOperationProgressUnit::Steps;
    reportProgress(phaseText(value.phase), value.completed, value.total, unit,
        QString::fromStdString(value.current));
}

void LegacyConversionController::finishConversion() {
    result_ = watcher_.result();
    setFinishing(false);
    setCancellable(false);
    populateSummary();
    if (result_->status == core::LegacyConversionStatus::Cancelled)
        raiseEvent("cancelled");
    else if (result_->capsule)
        raiseEvent("complete");
    else
        raiseEvent("failed");
}

void LegacyConversionController::populateSummary() {
    if (!summaryStatus_ || !scripts_ || !result_) return;
    if (!result_->capsule) {
        scripts_->setRowCount(0);
        summaryStatus_->setText(result_->status == core::LegacyConversionStatus::Cancelled
            ? tr("Conversion was cancelled. No capsule was created.")
            : diagnosticText(result_->diagnostics));
        return;
    }
    const auto& summary = *result_->capsule;
    summaryStatus_->setText(result_->status == core::LegacyConversionStatus::Ready
        ? tr("The capsule was created and validated.")
        : tr("The capsule was created and validated; some scripts require decisions during import."));
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
