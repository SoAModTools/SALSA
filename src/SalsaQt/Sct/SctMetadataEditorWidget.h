#pragma once

#include "SalsaCore/Project/AssetLocator.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"

#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace salsa::qt {

struct SctMetadataDraft final {
    std::optional<std::string> note{};
    std::optional<std::string> bookmarkLabel{};
    std::optional<std::uint32_t> colorRgb{};
    bool operator==(const SctMetadataDraft&) const = default;
};

struct SctMetadataBinding final {
    core::AssetLocator locator;
    core::SctNavigationTarget target;
    QString label;
    SctMetadataDraft metadata;
};

class SctMetadataEditorWidget final : public QWidget {
public:
    using CommitHandler = std::function<bool(
        const core::AssetLocator&, core::SctNavigationTarget,
        const SctMetadataDraft&)>;

    explicit SctMetadataEditorWidget(QWidget* parent = nullptr);

    void setCommitHandler(CommitHandler handler);
    [[nodiscard]] bool setBinding(std::optional<SctMetadataBinding> binding);
    [[nodiscard]] bool flush();
    void setEditingEnabled(bool enabled);
    [[nodiscard]] const std::optional<SctMetadataBinding>& binding() const noexcept;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void populate();
    void markDirty();
    [[nodiscard]] SctMetadataDraft draft() const;

    CommitHandler commitHandler_{};
    std::optional<SctMetadataBinding> binding_{};
    QLabel* target_ = nullptr;
    QLabel* empty_ = nullptr;
    QPlainTextEdit* note_ = nullptr;
    QCheckBox* bookmark_ = nullptr;
    QLineEdit* bookmarkLabel_ = nullptr;
    QPushButton* color_ = nullptr;
    QPushButton* clearColor_ = nullptr;
    std::optional<std::uint32_t> colorRgb_{};
    bool editingEnabled_ = false;
    bool populating_ = false;
    bool dirty_ = false;
};

}  // namespace salsa::qt
