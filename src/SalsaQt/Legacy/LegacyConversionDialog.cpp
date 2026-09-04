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
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace salsa::qt {
namespace {

QString conversionPhaseText(const core::LegacyConversionPhase phase) {
    switch (phase) {
    case core::LegacyConversionPhase::Staging: return QObject::tr("Staging controlled input");
    case core::LegacyConversionPhase::Hashing: return QObject::tr("Hashing project");
    case core::LegacyConversionPhase::Parsing: return QObject::tr("Reading legacy project");
    case core::LegacyConversionPhase::Script: return QObject::tr("Analyzing scripts");
    case core::LegacyConversionPhase::Writing: return QObject::tr("Writing script records");
    case core::LegacyConversionPhase::Validating: return QObject::tr("Validating capsule");
    case core::LegacyConversionPhase::Finalizing: return QObject::tr("Finalizing capsule");
    }
    return QObject::tr("Converting legacy project");
}

QString importPhaseText(const core::FreshLegacyImportPhase phase) {
    switch (phase) {
    case core::FreshLegacyImportPhase::ValidatingCapsule: return QObject::tr("Validating capsule");
    case core::FreshLegacyImportPhase::InspectingDestinations: return QObject::tr("Checking fresh destinations");
    case core::FreshLegacyImportPhase::ConvertingScripts: return QObject::tr("Converting scripts");
    case core::FreshLegacyImportPhase::FinalizingPlan: return QObject::tr("Finalizing import plan");
    case core::FreshLegacyImportPhase::StagingArtifacts: return QObject::tr("Staging scripts");
    case core::FreshLegacyImportPhase::CreatingWorkspace: return QObject::tr("Creating workspace");
    case core::FreshLegacyImportPhase::PublishingDestinations: return QObject::tr("Publishing destinations");
    case core::FreshLegacyImportPhase::VerifyingCommit: return QObject::tr("Verifying imported workspace");
    }
    return QObject::tr("Importing legacy project");
}

template <typename Enum>
void addEnum(QComboBox* combo, const QString& label, const Enum value) {
    combo->addItem(label, static_cast<int>(value));
}

QWidget* pathRow(QLineEdit*& edit, const QString& buttonText, QWidget* parent,
    const std::function<void()>& browse) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    edit = new QLineEdit(row);
    auto* button = new QPushButton(buttonText, row);
    layout->addWidget(edit, 1);
    layout->addWidget(button);
    QObject::connect(button, &QPushButton::clicked, row, browse);
    return row;
}

}  // namespace

LegacyImportController::LegacyImportController(Completion completion, QObject* parent)
    : ExclusiveOperationController(parent), completion_(std::move(completion)) {
    connect(&conversionWatcher_, &QFutureWatcherBase::finished,
        this, &LegacyImportController::finishConversion);
    connect(&preparationWatcher_, &QFutureWatcherBase::finished,
        this, &LegacyImportController::finishPreparation);
    connect(&commitWatcher_, &QFutureWatcherBase::finished,
        this, &LegacyImportController::finishCommit);
}

LegacyImportController::~LegacyImportController() {
    stopSource_.request_stop();
    if (conversionWatcher_.isRunning()) conversionWatcher_.waitForFinished();
    if (preparationWatcher_.isRunning()) preparationWatcher_.waitForFinished();
    if (commitWatcher_.isRunning()) commitWatcher_.waitForFinished();
    cleanupTransientArtifacts();
    if (commit_ && commit_->succeeded() && completion_)
        completion_(QString::fromStdWString(commit_->dataset->root.wstring()),
            QString::fromStdWString(commit_->workspaceDirectory.wstring()));
}

QString LegacyImportController::title() const { return tr("Import Legacy SALSA Project"); }

core::ExclusiveOperationFlowDefinition LegacyImportController::flowDefinition() const {
    using Role = core::ExclusiveOperationPageRole;
    return {"configure",
        {{"configure", Role::Configuration}, {"processing", Role::Processing},
         {"review", Role::Review}, {"preparing", Role::Processing},
         {"commit", Role::Commit}, {"committing", Role::Finishing},
         {"summary", Role::Summary}},
        {{"convert", "configure", "convert", "processing"},
         {"configure-cancelled", "configure", "cancelled", "summary"},
         {"converted", "processing", "converted", "review"},
         {"conversion-failed", "processing", "failed", "summary"},
         {"conversion-cancelled", "processing", "cancelled", "summary"},
         {"prepare", "review", "prepare", "preparing"},
         {"review-cancelled", "review", "cancelled", "summary"},
         {"prepared", "preparing", "prepared", "commit"},
         {"blocked", "preparing", "blocked", "review"},
         {"preparation-failed", "preparing", "failed", "summary"},
         {"preparation-cancelled", "preparing", "cancelled", "summary"},
         {"back", "commit", "back", "review"},
         {"commit-cancelled", "commit", "cancelled", "summary"},
         {"commit-import", "commit", "commit", "committing"},
         {"committed", "committing", "complete", "summary"},
         {"commit-failed", "committing", "failed", "summary"}}};
}

QWidget* LegacyImportController::createPage(
    const std::string_view pageId, QWidget* parent) {
    if (pageId == "configure") return createConfigurationPage(parent);
    if (pageId == "review") return createReviewPage(parent);
    if (pageId == "commit") return createCommitPage(parent);
    if (pageId == "summary") return createSummaryPage(parent);
    return createProcessingPage(parent);
}

std::optional<ExclusiveOperationAction> LegacyImportController::actionForEdge(
    const std::string_view edgeId) const {
    if (edgeId == "convert") return ExclusiveOperationAction{
        tr("Read Project"), ExclusiveOperationActionRole::Primary};
    if (edgeId == "prepare") return ExclusiveOperationAction{
        tr("Prepare Import"), ExclusiveOperationActionRole::Primary};
    if (edgeId == "back") return ExclusiveOperationAction{
        tr("Back"), ExclusiveOperationActionRole::Secondary};
    if (edgeId == "commit-import") return ExclusiveOperationAction{
        tr("Import"), ExclusiveOperationActionRole::Primary};
    return std::nullopt;
}

bool LegacyImportController::edgeEnabled(const std::string_view edgeId) const {
    if (edgeId == "convert") {
        const bool custom = target_ && target_->currentData().toInt()
            == static_cast<int>(core::LegacyImportTargetScope::UnknownCustom);
        return source_ && QFileInfo(source_->text()).isFile()
            && sourceDestination_ && !sourceDestination_->text().trimmed().isEmpty()
            && QFileInfo(sourceDestination_->text()).dir().exists()
            && workspaceDestination_ && !workspaceDestination_->text().trimmed().isEmpty()
            && QFileInfo(workspaceDestination_->text()).dir().exists()
            && trusted_ && trusted_->isChecked()
            && target_ && target_->currentIndex() >= 0
            && platform_ && platform_->currentIndex() >= 0
            && region_ && region_->currentIndex() >= 0
            && characterEncoding_ && characterEncoding_->currentIndex() >= 0
            && messageSpace_ && messageSpace_->currentIndex() >= 0
            && byteOrder_ && byteOrder_->currentIndex() >= 0
            && wrapper_ && wrapper_->currentIndex() >= 0
            && (!custom || (customTarget_ && !customTarget_->text().trimmed().isEmpty()))
            && (!disableLimits_->isChecked() || disableLimitsConfirmed_->isChecked());
    }
    if (edgeId == "prepare") return conversion_ && conversion_->capsule
        && scripts_ && scripts_->rowCount() > 0;
    if (edgeId == "commit-import") return preparation_ && preparation_->plan.ready();
    return true;
}

void LegacyImportController::handleEvent(const std::string_view event) {
    if (event == "convert") startConversion();
    else if (event == "prepare") startPreparation();
    else if (event == "commit") startCommit();
    else ExclusiveOperationController::handleEvent(event);
}

void LegacyImportController::requestCancel() {
    if (commitWatcher_.isRunning()) return;
    stopSource_.request_stop();
    setCancellable(false);
    if (conversionWatcher_.isRunning() || preparationWatcher_.isRunning()) {
        if (processingStatus_) processingStatus_->setText(tr("Cancelling safely…"));
        return;
    }
    cleanupTransientArtifacts();
    if (summaryStatus_) summaryStatus_->setText(tr("Import was cancelled. No destination was changed."));
    raiseEvent("cancelled");
}

QWidget* LegacyImportController::createConfigurationPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    auto* explanation = new QLabel(tr(
        "Import creates a new SCT source directory and a separate new SALSA workspace. "
        "Both destinations must be missing or completely empty and on the same volume."), page);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* form = new QFormLayout;
    form->addRow(tr("Legacy version-7 project:"), pathRow(source_, tr("Browse…"), page,
        [this] { chooseSource(); }));
    form->addRow(tr("New SCT source directory:"), pathRow(sourceDestination_,
        tr("Choose Parent…"), page, [this] { chooseSourceDestination(); }));
    form->addRow(tr("New SALSA workspace:"), pathRow(workspaceDestination_,
        tr("Choose Parent…"), page, [this] { chooseWorkspaceDestination(); }));

    target_ = new QComboBox(page);
    addEnum(target_, tr("GameCube"), core::LegacyImportTargetScope::GameCube);
    addEnum(target_, tr("Dreamcast Disc 1"), core::LegacyImportTargetScope::DreamcastDisc1);
    addEnum(target_, tr("Dreamcast Disc 2"), core::LegacyImportTargetScope::DreamcastDisc2);
    addEnum(target_, tr("Unknown / custom"), core::LegacyImportTargetScope::UnknownCustom);
    form->addRow(tr("Target scope:"), target_);
    platform_ = new QComboBox(page);
    addEnum(platform_, tr("GameCube"), spice::sct::SctPlatform::GameCube);
    addEnum(platform_, tr("Dreamcast"), spice::sct::SctPlatform::Dreamcast);
    form->addRow(tr("Platform:"), platform_);
    customTarget_ = new QLineEdit(page);
    customTarget_->setPlaceholderText(tr("Required for unknown/custom targets"));
    form->addRow(tr("Custom target name:"), customTarget_);
    region_ = new QComboBox(page);
    addEnum(region_, tr("North America"), core::LegacyImportRegion::NorthAmerica);
    addEnum(region_, tr("Europe"), core::LegacyImportRegion::Europe);
    addEnum(region_, tr("Japan"), core::LegacyImportRegion::Japan);
    addEnum(region_, tr("Unknown"), core::LegacyImportRegion::Unknown);
    form->addRow(tr("Region:"), region_);

    characterEncoding_ = new QComboBox(page);
    addEnum(characterEncoding_, tr("Windows-1252"), spice::sct::SctCharacterEncoding::Windows1252);
    addEnum(characterEncoding_, tr("Shift-JIS"), spice::sct::SctCharacterEncoding::ShiftJis);
    form->addRow(tr("Character encoding:"), characterEncoding_);
    messageSpace_ = new QComboBox(page);
    addEnum(messageSpace_, tr("Byte 0x7F"), spice::sct::SctMessageSpaceEncoding::Byte7F);
    addEnum(messageSpace_, tr("Shift-JIS 0x8140"), spice::sct::SctMessageSpaceEncoding::ShiftJis8140);
    form->addRow(tr("Message spaces:"), messageSpace_);
    byteOrder_ = new QComboBox(page);
    addEnum(byteOrder_, tr("Big endian"), spice::sct::SctDocumentOutputByteOrder::BigEndian);
    addEnum(byteOrder_, tr("Little endian"), spice::sct::SctDocumentOutputByteOrder::LittleEndian);
    form->addRow(tr("Byte order:"), byteOrder_);
    wrapper_ = new QComboBox(page);
    addEnum(wrapper_, tr("Raw SCT"), spice::sct::SctDocumentOutputWrapper::Raw);
    addEnum(wrapper_, tr("AKLZ compressed"), spice::sct::SctDocumentOutputWrapper::Aklz);
    form->addRow(tr("Output wrapper:"), wrapper_);

    scriptWorkers_ = new QComboBox(page);
    scriptWorkers_->addItem(tr("Auto (up to 4)"), 0);
    scriptWorkers_->addItem(tr("Serial"), 1);
    scriptWorkers_->addItem(tr("2"), 2);
    scriptWorkers_->addItem(tr("3"), 3);
    scriptWorkers_->addItem(tr("4"), 4);
    const auto remembered = QSettings{}.value(
        QStringLiteral("legacyConversion/scriptWorkers"), 0).toInt();
    const auto rememberedIndex = scriptWorkers_->findData(remembered);
    scriptWorkers_->setCurrentIndex(rememberedIndex < 0 ? 0 : rememberedIndex);
    form->addRow(tr("Script workers:"), scriptWorkers_);
    layout->addLayout(form);

    trusted_ = new QCheckBox(tr(
        "I trust this project file and understand that legacy project parsing is isolated but not risk-free."), page);
    retainOriginal_ = new QCheckBox(tr("Retain the original .prj as inert capsule evidence"), page);
    disableLimits_ = new QCheckBox(tr("Disable resource-limit safeguards for this trusted import"), page);
    disableLimitsConfirmed_ = new QCheckBox(tr(
        "I understand that disabling resource limits can consume substantial memory and disk space."), page);
    disableLimitsConfirmed_->setVisible(false);
    layout->addWidget(trusted_);
    layout->addWidget(retainOriginal_);
    layout->addWidget(disableLimits_);
    layout->addWidget(disableLimitsConfirmed_);
    configurationStatus_ = new QLabel(tr(
        "Only projects created or resaved by the official final legacy SALSA version 7 are supported."), page);
    configurationStatus_->setWordWrap(true);
    layout->addWidget(configurationStatus_);
    layout->addStretch();

    const auto changed = [this] { emit presentationChanged(); };
    for (auto* edit : {source_, sourceDestination_, workspaceDestination_, customTarget_})
        connect(edit, &QLineEdit::textChanged, this, changed);
    for (auto* combo : {target_, platform_, region_, characterEncoding_, messageSpace_, byteOrder_, wrapper_, scriptWorkers_})
        connect(combo, &QComboBox::currentIndexChanged, this, changed);
    for (auto* check : {trusted_, retainOriginal_, disableLimits_, disableLimitsConfirmed_})
        connect(check, &QCheckBox::toggled, this, changed);
    connect(disableLimits_, &QCheckBox::toggled, disableLimitsConfirmed_, &QWidget::setVisible);
    connect(target_, &QComboBox::currentIndexChanged, this, [this] {
        const auto scope = static_cast<core::LegacyImportTargetScope>(target_->currentData().toInt());
        const bool custom = scope == core::LegacyImportTargetScope::UnknownCustom;
        customTarget_->setEnabled(custom);
        if (scope == core::LegacyImportTargetScope::GameCube) {
            platform_->setCurrentIndex(platform_->findData(
                static_cast<int>(spice::sct::SctPlatform::GameCube)));
            byteOrder_->setCurrentIndex(byteOrder_->findData(
                static_cast<int>(spice::sct::SctDocumentOutputByteOrder::BigEndian)));
        } else if (!custom) {
            platform_->setCurrentIndex(platform_->findData(
                static_cast<int>(spice::sct::SctPlatform::Dreamcast)));
            byteOrder_->setCurrentIndex(byteOrder_->findData(
                static_cast<int>(spice::sct::SctDocumentOutputByteOrder::LittleEndian)));
        }
        platform_->setEnabled(custom);
    });
    connect(region_, &QComboBox::currentIndexChanged, this, [this] {
        const auto region = static_cast<core::LegacyImportRegion>(
            region_->currentData().toInt());
        if (region == core::LegacyImportRegion::Europe) {
            characterEncoding_->setCurrentIndex(characterEncoding_->findData(
                static_cast<int>(spice::sct::SctCharacterEncoding::Windows1252)));
        } else if (region == core::LegacyImportRegion::NorthAmerica
            || region == core::LegacyImportRegion::Japan) {
            characterEncoding_->setCurrentIndex(characterEncoding_->findData(
                static_cast<int>(spice::sct::SctCharacterEncoding::ShiftJis)));
        }
    });
    target_->setCurrentIndex(1);
    region_->setCurrentIndex(-1);
    return page;
}

QWidget* LegacyImportController::createProcessingPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    processingStatus_ = new QLabel(tr("Preparing the legacy import…"), page);
    processingStatus_->setWordWrap(true);
    layout->addWidget(processingStatus_);
    layout->addStretch();
    reportProgress(tr("Preparing"), 0, 0, ExclusiveOperationProgressUnit::Steps);
    return page;
}

QWidget* LegacyImportController::createReviewPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    reviewStatus_ = new QLabel(page);
    reviewStatus_->setWordWrap(true);
    layout->addWidget(reviewStatus_);
    scripts_ = new QTableWidget(page);
    scripts_->setColumnCount(10);
    scripts_->setHorizontalHeaderLabels({tr("Include"), tr("Legacy key"),
        tr("Stored name"), tr("Capsule"), tr("Output stem"), tr("Encoding"),
        tr("Spaces"), tr("Byte order"), tr("Wrapper"), tr("Plan status")});
    scripts_->setSelectionBehavior(QAbstractItemView::SelectRows);
    scripts_->setAlternatingRowColors(true);
    scripts_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    scripts_->horizontalHeader()->setSectionResizeMode(9, QHeaderView::Stretch);
    layout->addWidget(scripts_, 1);
    connect(scripts_, &QTableWidget::cellChanged,
        this, &ExclusiveOperationController::presentationChanged);
    auto* metadataLabel = new QLabel(tr(
        "Metadata requiring an explicit discard decision appears below. The immutable capsule is retained even when a record is discarded."), page);
    metadataLabel->setWordWrap(true);
    layout->addWidget(metadataLabel);
    metadata_ = new QTableWidget(page);
    metadata_->setColumnCount(4);
    metadata_->setHorizontalHeaderLabels({tr("Discard"), tr("Owner"), tr("Field"), tr("Reason")});
    metadata_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    metadata_->setMaximumHeight(180);
    layout->addWidget(metadata_);
    connect(metadata_, &QTableWidget::cellChanged,
        this, &ExclusiveOperationController::presentationChanged);
    populateReview();
    return page;
}

QWidget* LegacyImportController::createCommitPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    commitStatus_ = new QLabel(page);
    commitStatus_->setWordWrap(true);
    layout->addWidget(commitStatus_);
    layout->addStretch();
    populateCommitReview();
    return page;
}

QWidget* LegacyImportController::createSummaryPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    summaryStatus_ = new QLabel(page);
    summaryStatus_->setWordWrap(true);
    if (commit_ && commit_->succeeded()) {
        summaryStatus_->setText(tr(
            "The legacy project was imported and the new workspace was verified. "
            "Creating these SCT files does not prove that the game can reach every script."));
    } else if (commit_) {
        summaryStatus_->setText(diagnosticText(commit_->diagnostics));
    } else if (preparationWatcher_.isFinished() && !preparation_) {
        summaryStatus_->setText(tr("Import preparation failed or was cancelled. No destination was changed."));
    } else if (conversion_) {
        summaryStatus_->setText(diagnosticText(conversion_->diagnostics));
    } else {
        summaryStatus_->setText(tr("Import was cancelled. No destination was changed."));
    }
    layout->addWidget(summaryStatus_);
    layout->addStretch();
    return page;
}

void LegacyImportController::chooseSource() {
    const auto selected = QFileDialog::getOpenFileName(source_->window(),
        tr("Select Legacy SALSA Project"), source_->text(),
        tr("Legacy SALSA projects (*.prj);;All files (*)"));
    if (selected.isEmpty()) return;
    source_->setText(QDir::toNativeSeparators(selected));
    const QFileInfo info(selected);
    if (sourceDestination_->text().isEmpty())
        sourceDestination_->setText(QDir::toNativeSeparators(
            info.dir().filePath(info.completeBaseName() + QStringLiteral("-source"))));
    if (workspaceDestination_->text().isEmpty())
        workspaceDestination_->setText(QDir::toNativeSeparators(
            info.dir().filePath(info.completeBaseName() + QStringLiteral("-workspace"))));
    trusted_->setChecked(false);
}

void LegacyImportController::chooseSourceDestination() {
    const QFileInfo current(sourceDestination_->text());
    const auto selected = QFileDialog::getExistingDirectory(sourceDestination_->window(),
        tr("Select Parent for New SCT Source Directory"), current.dir().absolutePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) return;
    auto name = current.fileName();
    if (name.isEmpty()) name = QFileInfo(source_->text()).completeBaseName() + QStringLiteral("-source");
    sourceDestination_->setText(QDir::toNativeSeparators(QDir(selected).filePath(name)));
}

void LegacyImportController::chooseWorkspaceDestination() {
    const QFileInfo current(workspaceDestination_->text());
    const auto selected = QFileDialog::getExistingDirectory(workspaceDestination_->window(),
        tr("Select Parent for New SALSA Workspace"), current.dir().absolutePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) return;
    auto name = current.fileName();
    if (name.isEmpty()) name = QFileInfo(source_->text()).completeBaseName() + QStringLiteral("-workspace");
    workspaceDestination_->setText(QDir::toNativeSeparators(QDir(selected).filePath(name)));
}

void LegacyImportController::startConversion() {
    const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto sourceFinal = std::filesystem::path(sourceDestination_->text().toStdWString());
    const auto workspaceFinal = std::filesystem::path(workspaceDestination_->text().toStdWString());
    capsuleStage_ = sourceFinal.parent_path()
        / (L".salsa-capsule-stage-" + token.toStdWString());
    sourceStage_ = sourceFinal.parent_path()
        / (L".salsa-source-stage-" + token.toStdWString());
    workspaceStage_ = workspaceFinal.parent_path()
        / (L".salsa-workspace-stage-" + token.toStdWString());
    recoveryRegistry_ = std::filesystem::path(QDir(
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(
        QStringLiteral("migration-transactions")).toStdWString());
    core::LegacyConversionRequest request;
    request.source = std::filesystem::path(source_->text().toStdWString());
    request.destination = capsuleStage_;
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
    conversion_.reset();
    setCancellable(true);
    raiseEvent("convert");
    const auto stop = stopSource_.get_token();
    conversionWatcher_.setFuture(QtConcurrent::run([this, request = std::move(request), stop] {
        return core::LegacyConversionService::convert(request, stop,
            [this](const core::LegacyConversionProgress& progress) {
                QMetaObject::invokeMethod(this, [this, progress] {
                    const auto unit = progress.phase == core::LegacyConversionPhase::Script
                        || progress.phase == core::LegacyConversionPhase::Writing
                        ? ExclusiveOperationProgressUnit::Scripts
                        : progress.phase == core::LegacyConversionPhase::Hashing
                            ? ExclusiveOperationProgressUnit::Bytes
                            : ExclusiveOperationProgressUnit::Steps;
                    reportProgress(conversionPhaseText(progress.phase), progress.completed,
                        progress.total, unit, QString::fromStdString(progress.current));
                }, Qt::QueuedConnection);
            });
    }));
}

void LegacyImportController::finishConversion() {
    conversion_ = conversionWatcher_.result();
    setCancellable(true);
    if (conversion_->status == core::LegacyConversionStatus::Cancelled) {
        cleanupTransientArtifacts();
        raiseEvent("cancelled");
    } else if (!conversion_->capsule) {
        cleanupTransientArtifacts();
        raiseEvent("failed");
    } else {
        populateReview();
        raiseEvent("converted");
    }
}

void LegacyImportController::populateReview() {
    if (!reviewStatus_ || !scripts_ || !conversion_ || !conversion_->capsule) return;
    const auto& capsule = *conversion_->capsule;
    QSignalBlocker blocker(scripts_);
    scripts_->setRowCount(static_cast<int>(capsule.scripts.size()));
    for (int row = 0; row < scripts_->rowCount(); ++row) {
        const auto& script = capsule.scripts[static_cast<std::size_t>(row)];
        auto* include = new QTableWidgetItem;
        include->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        include->setCheckState(Qt::Checked);
        include->setData(Qt::UserRole, script.ordinal);
        scripts_->setItem(row, 0, include);
        scripts_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(script.key)));
        scripts_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(script.storedName)));
        scripts_->setItem(row, 3, new QTableWidgetItem(
            script.status == core::LegacyScriptStatus::Accepted ? tr("Accepted") : tr("Action required")));
        scripts_->setItem(row, 4, new QTableWidgetItem(QString::fromStdString(script.key)));
        auto* encoding = new QComboBox(scripts_);
        addEnum(encoding, tr("Windows-1252"), spice::sct::SctCharacterEncoding::Windows1252);
        addEnum(encoding, tr("Shift-JIS"), spice::sct::SctCharacterEncoding::ShiftJis);
        encoding->setCurrentIndex(encoding->findData(characterEncoding_->currentData()));
        scripts_->setCellWidget(row, 5, encoding);
        auto* spaces = new QComboBox(scripts_);
        addEnum(spaces, tr("0x7F"), spice::sct::SctMessageSpaceEncoding::Byte7F);
        addEnum(spaces, tr("0x8140"), spice::sct::SctMessageSpaceEncoding::ShiftJis8140);
        spaces->setCurrentIndex(spaces->findData(messageSpace_->currentData()));
        scripts_->setCellWidget(row, 6, spaces);
        auto* order = new QComboBox(scripts_);
        addEnum(order, tr("Big"), spice::sct::SctDocumentOutputByteOrder::BigEndian);
        addEnum(order, tr("Little"), spice::sct::SctDocumentOutputByteOrder::LittleEndian);
        order->setCurrentIndex(order->findData(byteOrder_->currentData()));
        scripts_->setCellWidget(row, 7, order);
        auto* wrapper = new QComboBox(scripts_);
        addEnum(wrapper, tr("Raw"), spice::sct::SctDocumentOutputWrapper::Raw);
        addEnum(wrapper, tr("AKLZ"), spice::sct::SctDocumentOutputWrapper::Aklz);
        wrapper->setCurrentIndex(wrapper->findData(wrapper_->currentData()));
        scripts_->setCellWidget(row, 8, wrapper);
        for (auto* combo : {encoding, spaces, order, wrapper})
            connect(combo, &QComboBox::currentIndexChanged,
                this, &ExclusiveOperationController::presentationChanged);
        scripts_->setItem(row, 9, new QTableWidgetItem(tr("Not prepared")));
        for (const int column : {1, 2, 3, 9})
            scripts_->item(row, column)->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    }
    reviewStatus_->setText(tr(
        "%1 scripts were found. Every failed or blocked script must be explicitly excluded; "
        "SALSA will not silently omit it. Edit an output stem only when an explicit remap is necessary.")
        .arg(scripts_->rowCount()));
}

core::FreshLegacyImportRequest LegacyImportController::importRequest() const {
    core::FreshLegacyImportRequest request;
    request.capsuleRoot = capsuleStage_;
    request.sourceDirectory = std::filesystem::path(sourceDestination_->text().toStdWString());
    request.workspaceDirectory = std::filesystem::path(workspaceDestination_->text().toStdWString());
    request.targetScope = static_cast<core::LegacyImportTargetScope>(target_->currentData().toInt());
    request.region = static_cast<core::LegacyImportRegion>(region_->currentData().toInt());
    request.customTargetName = customTarget_->text().trimmed().toStdString();
    request.publication.platform = static_cast<spice::sct::SctPlatform>(
        platform_->currentData().toInt());
    request.publication.textEncoding.characters =
        static_cast<spice::sct::SctCharacterEncoding>(characterEncoding_->currentData().toInt());
    request.publication.textEncoding.messageSpace =
        static_cast<spice::sct::SctMessageSpaceEncoding>(messageSpace_->currentData().toInt());
    request.publication.byteOrder = static_cast<spice::sct::SctDocumentOutputByteOrder>(
        byteOrder_->currentData().toInt());
    request.publication.wrapper = static_cast<spice::sct::SctDocumentOutputWrapper>(
        wrapper_->currentData().toInt());
    for (int row = 0; scripts_ && row < scripts_->rowCount(); ++row) {
        core::FreshLegacyScriptDecision decision;
        decision.ordinal = scripts_->item(row, 0)->data(Qt::UserRole).toUInt();
        decision.include = scripts_->item(row, 0)->checkState() == Qt::Checked;
        const auto stem = scripts_->item(row, 4)->text().toStdString();
        if (stem != scripts_->item(row, 1)->text().toStdString())
            decision.remappedStem = stem;
        auto options = request.publication;
        options.textEncoding.characters = static_cast<spice::sct::SctCharacterEncoding>(
            qobject_cast<QComboBox*>(scripts_->cellWidget(row, 5))->currentData().toInt());
        options.textEncoding.messageSpace = static_cast<spice::sct::SctMessageSpaceEncoding>(
            qobject_cast<QComboBox*>(scripts_->cellWidget(row, 6))->currentData().toInt());
        options.byteOrder = static_cast<spice::sct::SctDocumentOutputByteOrder>(
            qobject_cast<QComboBox*>(scripts_->cellWidget(row, 7))->currentData().toInt());
        options.wrapper = static_cast<spice::sct::SctDocumentOutputWrapper>(
            qobject_cast<QComboBox*>(scripts_->cellWidget(row, 8))->currentData().toInt());
        if (options != request.publication) decision.publicationOverride = options;
        request.scriptDecisions.push_back(std::move(decision));
    }
    for (int row = 0; metadata_ && row < metadata_->rowCount(); ++row) {
        const auto* discard = metadata_->item(row, 0);
        if (discard && discard->checkState() == Qt::Checked)
            request.metadataDecisions.push_back({discard->data(Qt::UserRole).toString().toStdString(),
                core::LegacyMetadataDecisionAction::Drop});
    }
    return request;
}

void LegacyImportController::startPreparation() {
    std::error_code ignored;
    std::filesystem::remove_all(sourceStage_, ignored);
    preparation_.reset();
    stopSource_ = std::stop_source{};
    setCancellable(true);
    raiseEvent("prepare");
    const auto request = importRequest();
    const auto stop = stopSource_.get_token();
    preparationWatcher_.setFuture(QtConcurrent::run([this, request, stop] {
        return core::LegacyFreshImportPreparer::prepare(request, sourceStage_, {}, stop,
            [this](const core::FreshLegacyImportProgress& progress) {
                QMetaObject::invokeMethod(this, [this, progress] {
                    const auto scripts = progress.phase == core::FreshLegacyImportPhase::ConvertingScripts
                        || progress.phase == core::FreshLegacyImportPhase::StagingArtifacts;
                    reportProgress(importPhaseText(progress.phase), progress.completed,
                        progress.total, scripts ? ExclusiveOperationProgressUnit::Scripts
                                                : ExclusiveOperationProgressUnit::Steps,
                        QString::fromStdString(progress.currentScript));
                }, Qt::QueuedConnection);
            });
    }));
}

void LegacyImportController::finishPreparation() {
    auto result = preparationWatcher_.result();
    setCancellable(true);
    if (!result) {
        reportDiagnostics(diagnosticText(result.diagnostics()));
        cleanupTransientArtifacts();
        raiseEvent(result.diagnostics().front().code == core::DiagnosticCode::Cancelled
            ? "cancelled" : "failed");
        return;
    }
    preparation_ = std::move(result).takeValue();
    if (!preparation_->plan.ready()) {
        QSignalBlocker blocker(scripts_);
        for (const auto& script : preparation_->plan.scripts) {
            if (static_cast<int>(script.ordinal) >= scripts_->rowCount()) continue;
            QString status = script.status == core::FreshLegacyScriptPlanStatus::Ready
                ? tr("Ready") : script.status == core::FreshLegacyScriptPlanStatus::Excluded
                    ? tr("Explicitly excluded")
                    : tr("Blocked: %1").arg(QString::fromStdString(
                        script.reasons.empty() ? "Unknown reason" : script.reasons.front()));
            scripts_->item(static_cast<int>(script.ordinal), 9)->setText(status);
        }
        if (metadata_) {
            QSignalBlocker metadataBlocker(metadata_);
            const auto actionable = std::ranges::count_if(preparation_->plan.metadata,
                [](const auto& record) {
                    return record.disposition == core::LegacyMetadataDisposition::Blocked
                        || record.disposition == core::LegacyMetadataDisposition::Unsupported
                        || record.disposition == core::LegacyMetadataDisposition::Invalid;
                });
            metadata_->setRowCount(static_cast<int>(actionable));
            int row = 0;
            for (const auto& record : preparation_->plan.metadata) {
                if (record.disposition != core::LegacyMetadataDisposition::Blocked
                    && record.disposition != core::LegacyMetadataDisposition::Unsupported
                    && record.disposition != core::LegacyMetadataDisposition::Invalid) continue;
                auto* discard = new QTableWidgetItem;
                discard->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
                discard->setCheckState(Qt::Unchecked);
                discard->setData(Qt::UserRole, QString::fromStdString(record.recordId));
                metadata_->setItem(row, 0, discard);
                metadata_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(record.owner)));
                metadata_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(record.field)));
                metadata_->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(record.reason)));
                for (const int column : {1, 2, 3})
                    metadata_->item(row, column)->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                ++row;
            }
        }
        reviewStatus_->setText(tr(
            "Preparation found blockers. Explicitly exclude each blocked script, correct its output stem, or discard each listed invalid metadata record, then prepare again."));
        raiseEvent("blocked");
        return;
    }
    populateCommitReview();
    raiseEvent("prepared");
}

void LegacyImportController::populateCommitReview() {
    if (!commitStatus_ || !preparation_) return;
    const auto imported = std::ranges::count_if(preparation_->plan.scripts,
        [](const auto& script) { return script.status == core::FreshLegacyScriptPlanStatus::Ready; });
    const auto excluded = std::ranges::count_if(preparation_->plan.scripts,
        [](const auto& script) { return script.status == core::FreshLegacyScriptPlanStatus::Excluded; });
    const auto pending = std::ranges::count_if(preparation_->plan.metadata,
        [](const auto& record) { return record.disposition == core::LegacyMetadataDisposition::Pending; });
    commitStatus_->setText(tr(
        "%1 scripts are ready and %2 are explicitly excluded. %3 metadata records will remain pending in the immutable capsule.\n\n"
        "Import will publish both fresh destinations together. Cancellation is disabled once publication begins.")
        .arg(imported).arg(excluded).arg(pending));
}

void LegacyImportController::startCommit() {
    if (!preparation_ || !preparation_->plan.ready()) return;
    std::error_code ignored;
    std::filesystem::remove_all(workspaceStage_, ignored);
    setCancellable(false);
    setFinishing(true);
    raiseEvent("commit");
    core::FreshLegacyImportCommitRequest request{*preparation_,
        std::filesystem::path(source_->text().toStdWString()), workspaceStage_, recoveryRegistry_};
    commitWatcher_.setFuture(QtConcurrent::run([this, request = std::move(request)] {
        return core::LegacyFreshImportCommitService::commit(request, {},
            [this](const core::FreshLegacyImportProgress& progress) {
                QMetaObject::invokeMethod(this, [this, progress] {
                    reportProgress(importPhaseText(progress.phase), progress.completed,
                        progress.total, ExclusiveOperationProgressUnit::Steps,
                        QString::fromStdString(progress.currentScript));
                }, Qt::QueuedConnection);
            });
    }));
}

void LegacyImportController::finishCommit() {
    commit_ = commitWatcher_.result();
    setFinishing(false);
    setCancellable(false);
    if (!commit_->succeeded()) reportDiagnostics(diagnosticText(commit_->diagnostics));
    raiseEvent(commit_->succeeded() ? "complete" : "failed");
}

void LegacyImportController::cleanupTransientArtifacts() {
    if (commit_ && preparation_
        && (commit_->status == core::FreshLegacyImportCommitStatus::Interrupted
            || commit_->status == core::FreshLegacyImportCommitStatus::RecoveryBlocked)
        && std::filesystem::is_directory(
            recoveryRegistry_ / preparation_->plan.planId)) return;
    std::error_code ignored;
    std::filesystem::remove_all(capsuleStage_, ignored);
    std::filesystem::remove_all(sourceStage_, ignored);
    std::filesystem::remove_all(workspaceStage_, ignored);
}

QString LegacyImportController::diagnosticText(
    const std::vector<core::Diagnostic>& diagnostics) const {
    QStringList messages;
    for (const auto& diagnostic : diagnostics)
        messages.push_back(QString::fromStdString(diagnostic.message));
    return messages.isEmpty() ? tr("The operation did not provide a diagnostic.")
                              : messages.join(QLatin1Char('\n'));
}

}  // namespace salsa::qt
