#include "Sct/SctExportDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace salsa::qt {
namespace {

template <typename Enum>
void selectEnum(QComboBox* combo, const std::optional<Enum> value) {
    if (!value) return;
    const auto index = combo->findData(static_cast<int>(*value));
    if (index >= 0) combo->setCurrentIndex(index);
}

QString suggestedFilename(const std::filesystem::path& sourcePath) {
    const auto info = QFileInfo(QString::fromStdWString(sourcePath.wstring()));
    const auto suffix = info.suffix().isEmpty() ? QStringLiteral("SCT") : info.suffix();
    return info.completeBaseName() + QStringLiteral("_export.") + suffix;
}

}  // namespace

SctExportDialog::SctExportDialog(
    const core::AssetLocator& locator,
    const core::RevisionId revision,
    const core::SctPublicationDefaults& defaults,
    const std::filesystem::path& sourcePath,
    const QString& initialDirectory,
    QWidget* parent)
    : QDialog(parent), sourcePath_(sourcePath) {
    setWindowTitle(tr("Export Active SCT Document"));
    setMinimumWidth(620);

    auto* layout = new QVBoxLayout(this);
    auto* heading = new QLabel(tr("%1 — revision %2")
        .arg(QString::fromStdWString(locator.path().wstring()))
        .arg(revision.value), this);
    heading->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(heading);

    auto* form = new QFormLayout;
    auto* destinationRow = new QWidget(this);
    auto* destinationLayout = new QHBoxLayout(destinationRow);
    destinationLayout->setContentsMargins(0, 0, 0, 0);
    destinationEdit_ = new QLineEdit(destinationRow);
    auto* browse = new QPushButton(tr("Browse..."), destinationRow);
    destinationLayout->addWidget(destinationEdit_, 1);
    destinationLayout->addWidget(browse);
    form->addRow(tr("Destination"), destinationRow);

    platformCombo_ = new QComboBox(this);
    platformCombo_->addItem(tr("Select target platform..."));
    platformCombo_->addItem(tr("GameCube"),
        static_cast<int>(spice::sct::SctPlatform::GameCube));
    platformCombo_->addItem(tr("Dreamcast"),
        static_cast<int>(spice::sct::SctPlatform::Dreamcast));
    selectEnum(platformCombo_, defaults.platform);
    form->addRow(tr("Target platform"), platformCombo_);

    characterEncodingCombo_ = new QComboBox(this);
    characterEncodingCombo_->addItem(tr("Select character encoding..."));
    characterEncodingCombo_->addItem(tr("Windows-1252"),
        static_cast<int>(spice::sct::SctCharacterEncoding::Windows1252));
    characterEncodingCombo_->addItem(tr("Shift-JIS"),
        static_cast<int>(spice::sct::SctCharacterEncoding::ShiftJis));
    if (defaults.textEncoding) selectEnum(characterEncodingCombo_,
        std::optional{defaults.textEncoding->characters});
    form->addRow(tr("Character encoding"), characterEncodingCombo_);

    messageSpaceCombo_ = new QComboBox(this);
    messageSpaceCombo_->addItem(tr("Select message-space encoding..."));
    messageSpaceCombo_->addItem(tr("Byte 0x7F"),
        static_cast<int>(spice::sct::SctMessageSpaceEncoding::Byte7F));
    messageSpaceCombo_->addItem(tr("Shift-JIS 0x8140"),
        static_cast<int>(spice::sct::SctMessageSpaceEncoding::ShiftJis8140));
    if (defaults.textEncoding) selectEnum(messageSpaceCombo_,
        std::optional{defaults.textEncoding->messageSpace});
    form->addRow(tr("Message spaces"), messageSpaceCombo_);

    byteOrderCombo_ = new QComboBox(this);
    byteOrderCombo_->addItem(tr("Select byte order..."));
    byteOrderCombo_->addItem(tr("Big endian"),
        static_cast<int>(spice::sct::SctDocumentOutputByteOrder::BigEndian));
    byteOrderCombo_->addItem(tr("Little endian"),
        static_cast<int>(spice::sct::SctDocumentOutputByteOrder::LittleEndian));
    selectEnum(byteOrderCombo_, defaults.byteOrder);
    form->addRow(tr("Byte order"), byteOrderCombo_);

    wrapperCombo_ = new QComboBox(this);
    wrapperCombo_->addItem(tr("Raw SCT"),
        static_cast<int>(spice::sct::SctDocumentOutputWrapper::Raw));
    wrapperCombo_->addItem(tr("AKLZ compressed"),
        static_cast<int>(spice::sct::SctDocumentOutputWrapper::Aklz));
    selectEnum(wrapperCombo_, defaults.wrapper);
    form->addRow(tr("Output wrapper"), wrapperCombo_);
    layout->addLayout(form);

    auto* policy = new QLabel(tr(
        "Opaque source material must be preserved exactly. The imported SCT header is reused when available; otherwise the canonical header is used."), this);
    policy->setWordWrap(true);
    layout->addWidget(policy);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Save, this);
    buttons->button(QDialogButtonBox::Save)->setText(tr("Export"));
    layout->addWidget(buttons);
    connect(browse, &QPushButton::clicked, this, &SctExportDialog::chooseDestination);
    connect(buttons, &QDialogButtonBox::accepted, this, &SctExportDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SctExportDialog::reject);

    const auto directory = initialDirectory.isEmpty()
        ? QString::fromStdWString(sourcePath.parent_path().wstring())
        : initialDirectory;
    destinationEdit_->setText(QDir(directory).filePath(suggestedFilename(sourcePath)));
}

core::SctPublicationOptions SctExportDialog::options() const {
    return {
        static_cast<spice::sct::SctPlatform>(platformCombo_->currentData().toInt()),
        {static_cast<spice::sct::SctCharacterEncoding>(
             characterEncodingCombo_->currentData().toInt()),
         static_cast<spice::sct::SctMessageSpaceEncoding>(
             messageSpaceCombo_->currentData().toInt())},
        static_cast<spice::sct::SctDocumentOutputByteOrder>(
            byteOrderCombo_->currentData().toInt()),
        static_cast<spice::sct::SctDocumentOutputWrapper>(
            wrapperCombo_->currentData().toInt()),
    };
}

std::filesystem::path SctExportDialog::destination() const {
    return std::filesystem::path(destinationEdit_->text().toStdWString());
}

void SctExportDialog::accept() {
    if (destinationEdit_->text().trimmed().isEmpty()
        || platformCombo_->currentIndex() <= 0
        || characterEncodingCombo_->currentIndex() <= 0
        || messageSpaceCombo_->currentIndex() <= 0
        || byteOrderCombo_->currentIndex() <= 0) {
        QMessageBox::warning(this, tr("Export settings incomplete"),
            tr("Choose a destination and every required target setting."));
        return;
    }
    const QFileInfo destinationInfo(destinationEdit_->text());
    if (!destinationInfo.dir().exists()) {
        QMessageBox::warning(this, tr("Destination unavailable"),
            tr("The destination directory does not exist."));
        return;
    }
    QDialog::accept();
}

void SctExportDialog::chooseDestination() {
    const auto selected = QFileDialog::getSaveFileName(
        this, tr("Export SCT document"), destinationEdit_->text(),
        tr("SCT files (*.SCT *.sct);;All files (*)"));
    if (!selected.isEmpty()) destinationEdit_->setText(QDir::toNativeSeparators(selected));
}

}  // namespace salsa::qt
