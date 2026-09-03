#pragma once

#include "SalsaCore/Sct/SctPublication.h"

#include <QDialog>

#include <filesystem>
#include <optional>

class QComboBox;
class QLineEdit;

namespace salsa::qt {

class SctExportDialog final : public QDialog {
public:
    SctExportDialog(
        const core::AssetLocator& locator,
        core::RevisionId revision,
        const core::SctPublicationDefaults& defaults,
        const std::filesystem::path& sourcePath,
        const QString& initialDirectory,
        QWidget* parent = nullptr);

    [[nodiscard]] core::SctPublicationOptions options() const;
    [[nodiscard]] std::filesystem::path destination() const;

protected:
    void accept() override;

private:
    void chooseDestination();

    std::filesystem::path sourcePath_{};
    QLineEdit* destinationEdit_ = nullptr;
    QComboBox* platformCombo_ = nullptr;
    QComboBox* characterEncodingCombo_ = nullptr;
    QComboBox* messageSpaceCombo_ = nullptr;
    QComboBox* byteOrderCombo_ = nullptr;
    QComboBox* wrapperCombo_ = nullptr;
};

}  // namespace salsa::qt
