#pragma once

#include "SalsaCore/Project/AssetLocator.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctMessageAuthoring.h"

#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QToolButton;
class QWidget;

namespace salsa::qt {

class SctMessageBodyEdit;
class SctMessageLineEdit;

class SctMessageEditorWidget final : public QWidget {
    Q_OBJECT

public:
    using CommitHandler = std::function<bool(
        const core::AssetLocator&,
        const core::SctMessageTarget&,
        const core::SctMessageDraft&,
        core::SctMessageEditKind)>;

    explicit SctMessageEditorWidget(QWidget* parent = nullptr);

    void setCommitHandler(CommitHandler handler);
    [[nodiscard]] bool bindMessage(
        core::AssetLocator locator,
        std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
        core::SctMessageTarget target);
    [[nodiscard]] bool refresh(
        std::shared_ptr<const core::SctDocumentSnapshot> snapshot);
    [[nodiscard]] bool flushPending();
    void clear();
    void focusEditor();

    [[nodiscard]] bool hasBinding() const noexcept;
    [[nodiscard]] bool isBoundTo(
        const core::AssetLocator& locator,
        const core::SctMessageTarget& target) const noexcept;
    [[nodiscard]] bool isCommitting() const noexcept;
    [[nodiscard]] const std::optional<core::AssetLocator>& boundLocator() const noexcept;
    [[nodiscard]] const std::optional<core::SctMessageTarget>& boundTarget() const noexcept;

signals:
    void statusMessageRequested(const QString& message);

private:
    enum class Surface { Body, Header };
    enum class Burst { None, Typing, Deletion };

    void buildUi();
    void showEmpty();
    void showBlocked(const std::vector<core::SctMessageProfileIssue>& issues);
    void loadDraft(const core::SctMessageDraft& draft, bool preserveCursor);
    [[nodiscard]] core::SctMessageDraft draftFromWidgets();
    [[nodiscard]] bool commitDraft(
        const core::SctMessageDraft& draft,
        core::SctMessageEditKind kind);
    [[nodiscard]] const spice::sct::SctMessage* findBoundMessage(
        const core::SctDocumentSnapshot& snapshot) const;
    void beforeMutation(Surface surface, int intent);
    void afterMutation(Surface surface, int intent);
    void beginBurst(Surface surface, Burst burst);
    void commitSemanticChange(core::SctMessageEditKind kind);
    void scheduleStandaloneCommit(core::SctMessageEditKind kind);
    void applyDoubleScale(bool enabled);
    void chooseColor();
    void clearFormatting();
    void updateToolbarFromCursor();

    CommitHandler commitHandler_{};
    std::optional<core::AssetLocator> locator_{};
    std::optional<core::SctMessageTarget> target_{};
    std::optional<core::SctMessageDraft> committedDraft_{};
    std::optional<core::SctMessageEditKind> failedCommitKind_{};
    Surface pendingSurface_ = Surface::Body;
    Burst pendingBurst_ = Burst::None;
    std::optional<core::SctMessageEditKind> pendingCommitKind_{};
    bool programmatic_ = false;
    bool committing_ = false;

    QTimer* commitTimer_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
    QLabel* blockedLabel_ = nullptr;
    QWidget* editorPage_ = nullptr;
    QLabel* identityLabel_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QCheckBox* headerPresent_ = nullptr;
    SctMessageLineEdit* header_ = nullptr;
    QToolButton* doubleScale_ = nullptr;
    QPushButton* color_ = nullptr;
    QPushButton* resetFormatting_ = nullptr;
    SctMessageBodyEdit* body_ = nullptr;
    QComboBox* completion_ = nullptr;
    QSpinBox* automaticDelay_ = nullptr;
    QToolButton* advancedToggle_ = nullptr;
    QWidget* advancedBody_ = nullptr;
    QComboBox* position_ = nullptr;
    QCheckBox* disableFastForward_ = nullptr;
    QCheckBox* openDurationEnabled_ = nullptr;
    QSpinBox* openDuration_ = nullptr;
    QCheckBox* closeDurationEnabled_ = nullptr;
    QSpinBox* closeDuration_ = nullptr;
};

}  // namespace salsa::qt
