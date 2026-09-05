#include "Sct/SctExportDialog.h"
#include "Sct/SctDocumentController.h"
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
namespace salsa::qt {
namespace {
template <typename Enum> void selectEnum(QComboBox* combo, const std::optional<Enum> value) {
    if (!value) return;
    const auto index = combo->findData(static_cast<int>(*value));
    if (index >= 0) combo->setCurrentIndex(index);
}
QString suggestedFilename(const std::filesystem::path& sourcePath) {
    const auto info = QFileInfo(QString::fromStdWString(sourcePath.wstring()));
    const auto suffix = info.suffix().isEmpty() ? QStringLiteral("SCT") : info.suffix();
    return info.completeBaseName() + QStringLiteral("_export.") + suffix;
}
QString normalizedPath(const std::filesystem::path& path) {
    return QDir::cleanPath(QFileInfo(QString::fromStdWString(path.wstring())).absoluteFilePath())
        .toLower();
}
}
SctPublicationController::SctPublicationController(SctDocumentController* documents,
    core::LocalGameProject project, core::AssetLocator locator,
    const core::RevisionId revision, core::SctPublicationDefaults defaults,
    std::filesystem::path sourcePath, QString initialDirectory, QObject* parent)
    : ExclusiveOperationController(parent), documents_(documents), project_(std::move(project)),
      locator_(std::move(locator)), revision_(revision), defaults_(std::move(defaults)),
      sourcePath_(std::move(sourcePath)), initialDirectory_(std::move(initialDirectory)) {
    connect(documents_, &SctDocumentController::publicationCompleted, this,
        [this](const QString& identityKey, const bool success, const bool cancelled,
            const QString& message, const bool replacedSource) {
            if (!started_ || identityKey != QString::fromStdString(locator_.identityKey())) return;
            summary_ = message;
            if (success && replacedSource) summary_ += tr(
                "\n\nThe loaded source was replaced. Reload this document before another checkpoint or publication.");
            if (summaryStatus_) summaryStatus_->setText(summary_);
            setFinishing(false);
            setCancellable(false);
            raiseEvent(cancelled ? "cancelled" : success ? "complete" : "failed");
        });
}
QString SctPublicationController::title() const { return tr("Export Active SCT Document"); }
core::ExclusiveOperationFlowDefinition SctPublicationController::flowDefinition() const {
    using Role = core::ExclusiveOperationPageRole;
    using Progress = core::ExclusiveOperationProgressVisibility;
    using Layout = core::ExclusiveOperationPageLayout;
    return {"configure", {{"configure", Role::Configuration, Progress::Hidden, Layout::Standard},
        {"review", Role::Review, Progress::Hidden, Layout::Standard},
        {"processing", Role::Processing, Progress::Visible, Layout::Compact},
        {"summary", Role::Summary, Progress::Hidden, Layout::Compact}},
        {{"direct", "configure", "start", "processing"},
         {"review", "configure", "review", "review"},
         {"cancel_config", "configure", "cancelled", "summary"},
         {"confirm", "review", "confirm", "processing"},
         {"back", "review", "back", "configure"},
         {"cancel_review", "review", "cancelled", "summary"},
         {"complete", "processing", "complete", "summary"},
         {"failed", "processing", "failed", "summary"},
         {"cancelled", "processing", "cancelled", "summary"}}};
}
QWidget* SctPublicationController::createPage(const std::string_view id, QWidget* parent) {
    if (id == "configure") return createConfigurationPage(parent);
    if (id == "review") return createReviewPage(parent);
    if (id == "processing") return createProcessingPage(parent);
    return createSummaryPage(parent);
}
std::optional<ExclusiveOperationAction> SctPublicationController::actionForEdge(
    const std::string_view edge) const {
    if (edge == "direct" || edge == "review") return ExclusiveOperationAction{
        tr("Export"), ExclusiveOperationActionRole::Primary};
    if (edge == "confirm") return ExclusiveOperationAction{
        tr("Replace Source and Export"), ExclusiveOperationActionRole::Destructive};
    if (edge == "back") return ExclusiveOperationAction{
        tr("Back"), ExclusiveOperationActionRole::Secondary};
    return std::nullopt;
}
bool SctPublicationController::edgeEnabled(const std::string_view edge) const {
    if (edge == "direct") return settingsComplete() && !replacingSource();
    if (edge == "review") return settingsComplete() && replacingSource();
    return true;
}
void SctPublicationController::handleEvent(const std::string_view event) {
    if (event == "start" || event == "review") {
        if (!settingsComplete()) return;
        if (event == "review") {
            if (reviewStatus_) reviewStatus_->setText(tr(
                "This will atomically replace the SCT in the loaded dataset. The open document remains bound to the previous source revision until reloaded."));
            raiseEvent(event);
        } else {
            raiseEvent(event);
            startPublication();
        }
    } else if (event == "confirm") {
        raiseEvent(event);
        startPublication();
    } else ExclusiveOperationController::handleEvent(event);
}
void SctPublicationController::requestCancel() {
    if (!started_) {
        summary_ = tr("Publication was cancelled before any file was written.");
        raiseEvent("cancelled");
        return;
    }
    documents_->cancel();
    setCancellable(false);
    if (processingStatus_) processingStatus_->setText(tr("Cancelling publication safely…"));
}
QWidget* SctPublicationController::createConfigurationPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    auto* heading = new QLabel(tr("%1 — revision %2")
        .arg(QString::fromStdWString(locator_.path().wstring())).arg(revision_.value), page);
    layout->addWidget(heading);
    auto* form = new QFormLayout;
    auto* row = new QWidget(page); auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    destinationEdit_ = new QLineEdit(row); auto* browse = new QPushButton(tr("Browse…"), row);
    rowLayout->addWidget(destinationEdit_, 1); rowLayout->addWidget(browse);
    form->addRow(tr("Destination"), row);
    platformCombo_ = new QComboBox(page); platformCombo_->addItem(tr("Select target platform…"));
    platformCombo_->addItem(tr("GameCube"), static_cast<int>(spice::sct::SctPlatform::GameCube));
    platformCombo_->addItem(tr("Dreamcast"), static_cast<int>(spice::sct::SctPlatform::Dreamcast));
    selectEnum(platformCombo_, defaults_.platform); form->addRow(tr("Target platform"), platformCombo_);
    characterEncodingCombo_ = new QComboBox(page); characterEncodingCombo_->addItem(tr("Select character encoding…"));
    characterEncodingCombo_->addItem(tr("Windows-1252"), static_cast<int>(spice::sct::SctCharacterEncoding::Windows1252));
    characterEncodingCombo_->addItem(tr("Shift-JIS"), static_cast<int>(spice::sct::SctCharacterEncoding::ShiftJis));
    if (defaults_.textEncoding) selectEnum(characterEncodingCombo_, std::optional{defaults_.textEncoding->characters});
    form->addRow(tr("Character encoding"), characterEncodingCombo_);
    messageSpaceCombo_ = new QComboBox(page); messageSpaceCombo_->addItem(tr("Select message-space encoding…"));
    messageSpaceCombo_->addItem(tr("Byte 0x7F"), static_cast<int>(spice::sct::SctMessageSpaceEncoding::Byte7F));
    messageSpaceCombo_->addItem(tr("Shift-JIS 0x8140"), static_cast<int>(spice::sct::SctMessageSpaceEncoding::ShiftJis8140));
    if (defaults_.textEncoding) selectEnum(messageSpaceCombo_, std::optional{defaults_.textEncoding->messageSpace});
    form->addRow(tr("Message spaces"), messageSpaceCombo_);
    byteOrderCombo_ = new QComboBox(page); byteOrderCombo_->addItem(tr("Select byte order…"));
    byteOrderCombo_->addItem(tr("Big endian"), static_cast<int>(spice::sct::SctDocumentOutputByteOrder::BigEndian));
    byteOrderCombo_->addItem(tr("Little endian"), static_cast<int>(spice::sct::SctDocumentOutputByteOrder::LittleEndian));
    selectEnum(byteOrderCombo_, defaults_.byteOrder); form->addRow(tr("Byte order"), byteOrderCombo_);
    wrapperCombo_ = new QComboBox(page); wrapperCombo_->addItem(tr("Raw SCT"), static_cast<int>(spice::sct::SctDocumentOutputWrapper::Raw));
    wrapperCombo_->addItem(tr("AKLZ compressed"), static_cast<int>(spice::sct::SctDocumentOutputWrapper::Aklz));
    selectEnum(wrapperCombo_, defaults_.wrapper); form->addRow(tr("Output wrapper"), wrapperCombo_);
    layout->addLayout(form);
    configurationStatus_ = new QLabel(tr("Opaque source material must be preserved exactly."), page);
    configurationStatus_->setWordWrap(true); layout->addWidget(configurationStatus_); layout->addStretch();
    const auto directory = initialDirectory_.isEmpty()
        ? QString::fromStdWString(sourcePath_.parent_path().wstring()) : initialDirectory_;
    destinationEdit_->setText(QDir(directory).filePath(suggestedFilename(sourcePath_)));
    connect(browse, &QPushButton::clicked, this, &SctPublicationController::chooseDestination);
    connect(destinationEdit_, &QLineEdit::textChanged, this, &ExclusiveOperationController::presentationChanged);
    for (auto* combo : {platformCombo_, characterEncodingCombo_, messageSpaceCombo_, byteOrderCombo_, wrapperCombo_})
        connect(combo, &QComboBox::currentIndexChanged, this, &ExclusiveOperationController::presentationChanged);
    return page;
}
QWidget* SctPublicationController::createReviewPage(QWidget* parent) {
    auto* page = new QWidget(parent); auto* layout = new QVBoxLayout(page);
    reviewStatus_ = new QLabel(page); reviewStatus_->setWordWrap(true);
    layout->addWidget(reviewStatus_); layout->addStretch(); return page;
}
QWidget* SctPublicationController::createProcessingPage(QWidget* parent) {
    auto* page = new QWidget(parent); auto* layout = new QVBoxLayout(page);
    processingStatus_ = new QLabel(tr("Validating and encoding the captured SCT revision…"), page);
    processingStatus_->setWordWrap(true); layout->addWidget(processingStatus_); layout->addStretch();
    reportProgress(tr("Preparing publication"), 0, 0, ExclusiveOperationProgressUnit::Steps);
    return page;
}
QWidget* SctPublicationController::createSummaryPage(QWidget* parent) {
    auto* page = new QWidget(parent); auto* layout = new QVBoxLayout(page);
    summaryStatus_ = new QLabel(summary_, page); summaryStatus_->setWordWrap(true);
    layout->addWidget(summaryStatus_); layout->addStretch(); return page;
}
void SctPublicationController::chooseDestination() {
    const auto selected = QFileDialog::getSaveFileName(destinationEdit_->window(), tr("Export SCT document"),
        destinationEdit_->text(), tr("SCT files (*.SCT *.sct);;All files (*)"));
    if (!selected.isEmpty()) destinationEdit_->setText(QDir::toNativeSeparators(selected));
}
bool SctPublicationController::settingsComplete() const {
    return destinationEdit_ && !destinationEdit_->text().trimmed().isEmpty()
        && QFileInfo(destinationEdit_->text()).dir().exists()
        && platformCombo_->currentIndex() > 0 && characterEncodingCombo_->currentIndex() > 0
        && messageSpaceCombo_->currentIndex() > 0 && byteOrderCombo_->currentIndex() > 0;
}
bool SctPublicationController::replacingSource() const {
    return destinationEdit_ && normalizedPath(destination()) == normalizedPath(sourcePath_);
}
core::SctPublicationOptions SctPublicationController::options() const {
    return {static_cast<spice::sct::SctPlatform>(platformCombo_->currentData().toInt()),
        {static_cast<spice::sct::SctCharacterEncoding>(characterEncodingCombo_->currentData().toInt()),
         static_cast<spice::sct::SctMessageSpaceEncoding>(messageSpaceCombo_->currentData().toInt())},
        static_cast<spice::sct::SctDocumentOutputByteOrder>(byteOrderCombo_->currentData().toInt()),
        static_cast<spice::sct::SctDocumentOutputWrapper>(wrapperCombo_->currentData().toInt())};
}
std::filesystem::path SctPublicationController::destination() const {
    return std::filesystem::path(destinationEdit_->text().toStdWString());
}
void SctPublicationController::startPublication() {
    const auto target = destination();
    const bool replace = replacingSource();
    if (!documents_->exportDocument(project_, locator_, options(), target, replace,
            [this](const core::SctPublicationProgress& progress) {
                QMetaObject::invokeMethod(this, [this, progress] {
                    QString phase;
                    switch (progress.phase) {
                    case core::SctPublicationPhase::Preflight: phase = tr("Checking source revision"); break;
                    case core::SctPublicationPhase::Materializing: phase = tr("Materializing document"); break;
                    case core::SctPublicationPhase::Encoding: phase = tr("Encoding SCT output"); break;
                    case core::SctPublicationPhase::Hashing: phase = tr("Hashing output"); break;
                    case core::SctPublicationPhase::Installing:
                        phase = tr("Installing output atomically");
                        setCancellable(false); setFinishing(true); break;
                    }
                    reportProgress(phase, progress.completed, progress.total,
                        ExclusiveOperationProgressUnit::Steps);
                }, Qt::QueuedConnection);
            })) {
        summary_ = tr("The SCT publication could not be started.");
        raiseEvent("failed"); return;
    }
    started_ = true;
    QSettings{}.setValue(QStringLiteral("publication/lastDirectory"),
        QString::fromStdWString(target.parent_path().wstring()));
}
}  // namespace salsa::qt
