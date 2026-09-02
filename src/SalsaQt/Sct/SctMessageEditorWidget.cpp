#include "Sct/SctMessageEditorWidget.h"

#include "SpiceSCT/SctDocumentIndex.h"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QFormLayout>
#include <QFont>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFormat>
#include <QTextFragment>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace salsa::qt {
namespace {

constexpr int IntentTyping = 0;
constexpr int IntentDeletion = 1;
constexpr int IntentPaste = 2;
constexpr int IntentReplacement = 3;
constexpr int IntentCut = 4;
constexpr int DoubleScaleProperty = QTextFormat::UserProperty + 1;
constexpr int ColorProperty = QTextFormat::UserProperty + 2;
constexpr int TextBurstDelayMs = 750;
constexpr int SemanticControlDelayMs = 250;

[[nodiscard]] bool cursorMovementKey(const int key) {
    switch (key) {
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] core::SctMessageTextStyle styleFromFormat(const QTextCharFormat& format) {
    core::SctMessageTextStyle style;
    style.doubleScale = format.property(DoubleScaleProperty).toBool();
    if (format.hasProperty(ColorProperty)) {
        const auto rgb = format.property(ColorProperty).toUInt();
        style.color = core::SctMessageRgb{
            static_cast<std::uint8_t>((rgb >> 16) & 0xffu),
            static_cast<std::uint8_t>((rgb >> 8) & 0xffu),
            static_cast<std::uint8_t>(rgb & 0xffu)};
    }
    return style;
}

[[nodiscard]] QTextCharFormat formatForStyle(const core::SctMessageTextStyle& style) {
    QTextCharFormat format;
    format.setProperty(DoubleScaleProperty, style.doubleScale);
    format.setFontWeight(style.doubleScale ? QFont::Bold : QFont::Normal);
    if (style.color.has_value()) {
        const auto rgb = (static_cast<std::uint32_t>(style.color->red) << 16)
            | (static_cast<std::uint32_t>(style.color->green) << 8)
            | style.color->blue;
        format.setProperty(ColorProperty, rgb);
        format.setForeground(QColor::fromRgb(rgb));
    }
    return format;
}

[[nodiscard]] bool isImmediateIntent(const int intent) {
    return intent == IntentPaste || intent == IntentReplacement || intent == IntentCut;
}

}  // namespace

class SctMessageBodyEdit final : public QTextEdit {
public:
    explicit SctMessageBodyEdit(QWidget* parent = nullptr) : QTextEdit(parent) {
        setAcceptRichText(false);
        setUndoRedoEnabled(false);
    }

    std::function<void(int)> beforeMutation{};
    std::function<void(int)> afterMutation{};
    std::function<void()> boundary{};

protected:
    bool canInsertFromMimeData(const QMimeData* source) const override {
        return source != nullptr && source->hasText();
    }

    void insertFromMimeData(const QMimeData* source) override {
        if (source == nullptr || !source->hasText()) return;
        if (beforeMutation) beforeMutation(IntentPaste);
        const auto before = document()->revision();
        insertPlainText(source->text());
        if (document()->revision() != before && afterMutation) afterMutation(IntentPaste);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->matches(QKeySequence::Paste)) {
            paste();
            return;
        }
        if (event->matches(QKeySequence::Cut)) {
            if (!textCursor().hasSelection()) return;
            if (beforeMutation) beforeMutation(IntentCut);
            const auto before = document()->revision();
            copy();
            auto cursor = textCursor();
            cursor.removeSelectedText();
            setTextCursor(cursor);
            if (document()->revision() != before && afterMutation) afterMutation(IntentCut);
            return;
        }
        if (cursorMovementKey(event->key()) || event->matches(QKeySequence::SelectAll)) {
            if (boundary) boundary();
            QTextEdit::keyPressEvent(event);
            return;
        }
        int intent = -1;
        if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
            intent = IntentDeletion;
        } else if (!event->text().isEmpty()
            || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            intent = textCursor().hasSelection() ? IntentReplacement : IntentTyping;
        }
        if (intent >= 0 && beforeMutation) beforeMutation(intent);
        const auto before = document()->revision();
        QTextEdit::keyPressEvent(event);
        if (intent >= 0 && document()->revision() != before && afterMutation)
            afterMutation(intent);
    }

    void inputMethodEvent(QInputMethodEvent* event) override {
        if (event->commitString().isEmpty()) {
            QTextEdit::inputMethodEvent(event);
            return;
        }
        const auto intent = textCursor().hasSelection() ? IntentReplacement : IntentTyping;
        if (beforeMutation) beforeMutation(intent);
        const auto before = document()->revision();
        QTextEdit::inputMethodEvent(event);
        if (document()->revision() != before && afterMutation) afterMutation(intent);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (boundary) boundary();
        QTextEdit::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        QMenu menu(this);
        auto* cutAction = menu.addAction(tr("Cut"));
        auto* copyAction = menu.addAction(tr("Copy"));
        auto* pasteAction = menu.addAction(tr("Paste"));
        menu.addSeparator();
        auto* selectAllAction = menu.addAction(tr("Select All"));
        cutAction->setEnabled(textCursor().hasSelection());
        copyAction->setEnabled(textCursor().hasSelection());
        pasteAction->setEnabled(canPaste());
        connect(cutAction, &QAction::triggered, this, [this]() {
            QKeyEvent key(QEvent::KeyPress, Qt::Key_X, Qt::ControlModifier);
            keyPressEvent(&key);
        });
        connect(copyAction, &QAction::triggered, this, &QTextEdit::copy);
        connect(pasteAction, &QAction::triggered, this, &QTextEdit::paste);
        connect(selectAllAction, &QAction::triggered, this, [this]() {
            if (boundary) boundary();
            selectAll();
        });
        menu.exec(event->globalPos());
    }
};

class SctMessageLineEdit final : public QLineEdit {
public:
    explicit SctMessageLineEdit(QWidget* parent = nullptr) : QLineEdit(parent) {}

    std::function<void(int)> beforeMutation{};
    std::function<void(int)> afterMutation{};
    std::function<void()> boundary{};

protected:
    void keyPressEvent(QKeyEvent* event) override {
        int intent = -1;
        if (event->matches(QKeySequence::Paste)) intent = IntentPaste;
        else if (event->matches(QKeySequence::Cut)) intent = IntentCut;
        else if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete)
            intent = IntentDeletion;
        else if (!event->text().isEmpty())
            intent = hasSelectedText() ? IntentReplacement : IntentTyping;
        else if (cursorMovementKey(event->key()) || event->matches(QKeySequence::SelectAll)) {
            if (boundary) boundary();
        }
        if (intent >= 0 && beforeMutation) beforeMutation(intent);
        const auto before = text();
        QLineEdit::keyPressEvent(event);
        if (intent >= 0 && text() != before && afterMutation) afterMutation(intent);
    }

    void inputMethodEvent(QInputMethodEvent* event) override {
        if (event->commitString().isEmpty()) {
            QLineEdit::inputMethodEvent(event);
            return;
        }
        const auto intent = hasSelectedText() ? IntentReplacement : IntentTyping;
        if (beforeMutation) beforeMutation(intent);
        const auto before = text();
        QLineEdit::inputMethodEvent(event);
        if (text() != before && afterMutation) afterMutation(intent);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (boundary) boundary();
        QLineEdit::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        QMenu menu(this);
        auto* cutAction = menu.addAction(tr("Cut"));
        auto* copyAction = menu.addAction(tr("Copy"));
        auto* pasteAction = menu.addAction(tr("Paste"));
        menu.addSeparator();
        auto* selectAllAction = menu.addAction(tr("Select All"));
        cutAction->setEnabled(hasSelectedText());
        copyAction->setEnabled(hasSelectedText());
        pasteAction->setEnabled(!QApplication::clipboard()->text().isEmpty());
        connect(cutAction, &QAction::triggered, this, [this]() {
            if (beforeMutation) beforeMutation(IntentCut);
            const auto before = text();
            cut();
            if (text() != before && afterMutation) afterMutation(IntentCut);
        });
        connect(copyAction, &QAction::triggered, this, &QLineEdit::copy);
        connect(pasteAction, &QAction::triggered, this, [this]() {
            if (beforeMutation) beforeMutation(IntentPaste);
            const auto before = text();
            paste();
            if (text() != before && afterMutation) afterMutation(IntentPaste);
        });
        connect(selectAllAction, &QAction::triggered, this, [this]() {
            if (boundary) boundary();
            selectAll();
        });
        menu.exec(event->globalPos());
    }
};

SctMessageEditorWidget::SctMessageEditorWidget(QWidget* parent) : QWidget(parent) {
    buildUi();
    showEmpty();
}

void SctMessageEditorWidget::setCommitHandler(CommitHandler handler) {
    commitHandler_ = std::move(handler);
}

void SctMessageEditorWidget::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    pages_ = new QStackedWidget(this);
    layout->addWidget(pages_);

    emptyLabel_ = new QLabel(
        tr("Double-click an editable SCT message or use Edit Message to begin."), pages_);
    emptyLabel_->setWordWrap(true);
    emptyLabel_->setAlignment(Qt::AlignCenter);
    pages_->addWidget(emptyLabel_);

    blockedLabel_ = new QLabel(pages_);
    blockedLabel_->setWordWrap(true);
    blockedLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    blockedLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    pages_->addWidget(blockedLabel_);

    auto* scroll = new QScrollArea(pages_);
    scroll->setWidgetResizable(true);
    editorPage_ = new QWidget(scroll);
    auto* editorLayout = new QVBoxLayout(editorPage_);
    identityLabel_ = new QLabel(editorPage_);
    identityLabel_->setWordWrap(true);
    auto identityFont = identityLabel_->font();
    identityFont.setBold(true);
    identityLabel_->setFont(identityFont);
    errorLabel_ = new QLabel(editorPage_);
    errorLabel_->setWordWrap(true);
    errorLabel_->setStyleSheet(QStringLiteral("color: palette(highlight);"));
    errorLabel_->hide();
    editorLayout->addWidget(identityLabel_);
    editorLayout->addWidget(errorLabel_);

    auto* headerRow = new QHBoxLayout;
    headerPresent_ = new QCheckBox(tr("Header"), editorPage_);
    header_ = new SctMessageLineEdit(editorPage_);
    header_->setPlaceholderText(tr("Optional speaker or dialogue header"));
    headerRow->addWidget(headerPresent_);
    headerRow->addWidget(header_, 1);
    editorLayout->addLayout(headerRow);

    auto* toolbar = new QHBoxLayout;
    doubleScale_ = new QToolButton(editorPage_);
    doubleScale_->setText(tr("Large"));
    doubleScale_->setToolTip(tr("Approximate the SCT double-glyph-scale command"));
    doubleScale_->setCheckable(true);
    color_ = new QPushButton(tr("Text color..."), editorPage_);
    resetFormatting_ = new QPushButton(tr("Clear formatting"), editorPage_);
    toolbar->addWidget(doubleScale_);
    toolbar->addWidget(color_);
    toolbar->addWidget(resetFormatting_);
    toolbar->addStretch(1);
    editorLayout->addLayout(toolbar);

    body_ = new SctMessageBodyEdit(editorPage_);
    body_->setPlaceholderText(tr("Dialogue text"));
    body_->setMinimumHeight(220);
    editorLayout->addWidget(body_, 1);

    auto* completionRow = new QHBoxLayout;
    completionRow->addWidget(new QLabel(tr("Completion"), editorPage_));
    completion_ = new QComboBox(editorPage_);
    completion_->addItem(tr("Continue on confirmation"), 0);
    completion_->addItem(tr("Close on confirmation"), 1);
    completion_->addItem(tr("Continue automatically"), 2);
    automaticDelay_ = new QSpinBox(editorPage_);
    automaticDelay_->setRange(0, 65535);
    automaticDelay_->setSuffix(tr(" ticks"));
    completionRow->addWidget(completion_, 1);
    completionRow->addWidget(automaticDelay_);
    editorLayout->addLayout(completionRow);

    advancedToggle_ = new QToolButton(editorPage_);
    advancedToggle_->setText(tr("Advanced options"));
    advancedToggle_->setCheckable(true);
    advancedToggle_->setChecked(false);
    advancedToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedToggle_->setArrowType(Qt::RightArrow);
    editorLayout->addWidget(advancedToggle_);
    advancedBody_ = new QWidget(editorPage_);
    auto* advanced = new QFormLayout(advancedBody_);
    position_ = new QComboBox(advancedBody_);
    position_->addItem(tr("Target default"), 0);
    position_->addItem(tr("Lower"), 1);
    position_->addItem(tr("Upper"), 2);
    disableFastForward_ = new QCheckBox(tr("Disable button fast-forward during reveal"), advancedBody_);
    auto* openRow = new QWidget(advancedBody_);
    auto* openLayout = new QHBoxLayout(openRow);
    openLayout->setContentsMargins(0, 0, 0, 0);
    openDurationEnabled_ = new QCheckBox(tr("Set"), openRow);
    openDuration_ = new QSpinBox(openRow);
    openDuration_->setRange(0, 65535);
    openDuration_->setSuffix(tr(" ticks"));
    openLayout->addWidget(openDurationEnabled_);
    openLayout->addWidget(openDuration_, 1);
    auto* closeRow = new QWidget(advancedBody_);
    auto* closeLayout = new QHBoxLayout(closeRow);
    closeLayout->setContentsMargins(0, 0, 0, 0);
    closeDurationEnabled_ = new QCheckBox(tr("Set"), closeRow);
    closeDuration_ = new QSpinBox(closeRow);
    closeDuration_->setRange(0, 65535);
    closeDuration_->setSuffix(tr(" ticks"));
    closeLayout->addWidget(closeDurationEnabled_);
    closeLayout->addWidget(closeDuration_, 1);
    advanced->addRow(tr("Position"), position_);
    advanced->addRow(QString{}, disableFastForward_);
    advanced->addRow(tr("Open duration"), openRow);
    advanced->addRow(tr("Close duration"), closeRow);
    advancedBody_->hide();
    editorLayout->addWidget(advancedBody_);
    scroll->setWidget(editorPage_);
    pages_->addWidget(scroll);

    commitTimer_ = new QTimer(this);
    commitTimer_->setSingleShot(true);
    commitTimer_->setInterval(TextBurstDelayMs);
    connect(commitTimer_, &QTimer::timeout, this, [this]() { (void)flushPending(); });

    body_->beforeMutation = [this](const int intent) { beforeMutation(Surface::Body, intent); };
    body_->afterMutation = [this](const int intent) { afterMutation(Surface::Body, intent); };
    body_->boundary = [this]() { (void)flushPending(); };
    header_->beforeMutation = [this](const int intent) { beforeMutation(Surface::Header, intent); };
    header_->afterMutation = [this](const int intent) { afterMutation(Surface::Header, intent); };
    header_->boundary = [this]() { (void)flushPending(); };

    connect(headerPresent_, &QCheckBox::toggled, this, [this](const bool enabled) {
        header_->setEnabled(enabled);
        if (!programmatic_) commitSemanticChange(core::SctMessageEditKind::Header);
    });
    connect(doubleScale_, &QToolButton::toggled,
        this, [this](const bool enabled) { if (!programmatic_) applyDoubleScale(enabled); });
    connect(color_, &QPushButton::clicked, this, &SctMessageEditorWidget::chooseColor);
    connect(resetFormatting_, &QPushButton::clicked,
        this, &SctMessageEditorWidget::clearFormatting);
    connect(body_, &QTextEdit::currentCharFormatChanged,
        this, [this](const QTextCharFormat&) { updateToolbarFromCursor(); });
    connect(completion_, &QComboBox::currentIndexChanged, this, [this]() {
        automaticDelay_->setVisible(completion_->currentData().toInt() == 2);
        if (!programmatic_) commitSemanticChange(core::SctMessageEditKind::Completion);
    });
    connect(automaticDelay_, &QSpinBox::valueChanged, this, [this]() {
        if (!programmatic_) commitSemanticChange(core::SctMessageEditKind::Completion);
    });
    connect(advancedToggle_, &QToolButton::toggled, this, [this](const bool shown) {
        advancedToggle_->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
        advancedBody_->setVisible(shown);
    });
    const auto optionsChanged = [this]() {
        if (!programmatic_) commitSemanticChange(core::SctMessageEditKind::Options);
    };
    connect(position_, &QComboBox::currentIndexChanged, this, optionsChanged);
    connect(disableFastForward_, &QCheckBox::toggled, this, optionsChanged);
    connect(openDurationEnabled_, &QCheckBox::toggled, this, [this, optionsChanged](const bool enabled) {
        openDuration_->setEnabled(enabled);
        optionsChanged();
    });
    connect(openDuration_, &QSpinBox::valueChanged, this, optionsChanged);
    connect(closeDurationEnabled_, &QCheckBox::toggled, this, [this, optionsChanged](const bool enabled) {
        closeDuration_->setEnabled(enabled);
        optionsChanged();
    });
    connect(closeDuration_, &QSpinBox::valueChanged, this, optionsChanged);
}

bool SctMessageEditorWidget::bindMessage(
    core::AssetLocator locator,
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
    core::SctMessageTarget target) {
    if (!flushPending()) return false;
    locator_ = std::move(locator);
    target_ = std::move(target);
    committedDraft_.reset();
    if (snapshot == nullptr) {
        clear();
        return false;
    }
    const auto* message = findBoundMessage(*snapshot);
    if (message == nullptr) {
        clear();
        return false;
    }
    identityLabel_->setText(tr("%1 — %2 %3")
        .arg(QString::fromStdWString(locator_->path().wstring()))
        .arg(std::holds_alternative<spice::sct::SctStringId>(*target_)
            ? tr("indexed string") : tr("footer message"))
        .arg(std::visit([](const auto id) { return QString::number(id.value()); }, *target_)));
    const auto projection = core::SctMessageAuthoringProfile::project(*message);
    if (!projection.supported()) {
        showBlocked(projection.issues);
        return true;
    }
    loadDraft(*projection.draft, false);
    pages_->setCurrentIndex(2);
    return true;
}

bool SctMessageEditorWidget::refresh(
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot) {
    if (!hasBinding() || snapshot == nullptr || committing_) return true;
    const auto* message = findBoundMessage(*snapshot);
    if (message == nullptr) {
        clear();
        return false;
    }
    return refreshMessage(*message);
}

bool SctMessageEditorWidget::refreshMessage(
    const spice::sct::SctMessage& message) {
    if (!hasBinding() || committing_) return true;
    const auto projection = core::SctMessageAuthoringProfile::project(message);
    if (!projection.supported()) {
        showBlocked(projection.issues);
        return true;
    }
    loadDraft(*projection.draft, true);
    pages_->setCurrentIndex(2);
    return true;
}

const spice::sct::SctMessage* SctMessageEditorWidget::findBoundMessage(
    const core::SctDocumentSnapshot& snapshot) const {
    if (!target_ || !snapshot.document) return nullptr;
    const auto index = spice::sct::SctDocumentIndex::build(*snapshot.document);
    const spice::sct::SctTextValue* value = std::visit([&](const auto id)
        -> const spice::sct::SctTextValue* {
        const auto* entity = index.find(id);
        return entity == nullptr ? nullptr : &entity->value;
    }, *target_);
    return value == nullptr ? nullptr : std::get_if<spice::sct::SctMessage>(value);
}

void SctMessageEditorWidget::showEmpty() {
    pages_->setCurrentWidget(emptyLabel_);
}

void SctMessageEditorWidget::showBlocked(
    const std::vector<core::SctMessageProfileIssue>& issues) {
    QString text = tr("This decoded message is outside the initial SALSA authoring profile and remains read-only.\n\n");
    for (const auto& issue : issues) {
        text += QStringLiteral("• ") + QString::fromStdString(issue.message) + QLatin1Char('\n');
    }
    blockedLabel_->setText(text.trimmed());
    pages_->setCurrentWidget(blockedLabel_);
}

void SctMessageEditorWidget::loadDraft(
    const core::SctMessageDraft& draft,
    const bool preserveCursor) {
    const auto oldPosition = preserveCursor ? body_->textCursor().position() : 0;
    const auto oldAnchor = preserveCursor ? body_->textCursor().anchor() : 0;
    programmatic_ = true;
    commitTimer_->stop();
    pendingBurst_ = Burst::None;
    pendingCommitKind_.reset();
    failedCommitKind_.reset();
    headerPresent_->setChecked(draft.headerUtf8.has_value());
    header_->setEnabled(draft.headerUtf8.has_value());
    const auto headerText = draft.headerUtf8.value_or("");
    header_->setText(QString::fromUtf8(
        headerText.data(), static_cast<qsizetype>(headerText.size())));

    body_->clear();
    QTextCursor cursor(body_->document());
    for (const auto& run : draft.body) {
        cursor.insertText(QString::fromUtf8(run.utf8.data(),
            static_cast<qsizetype>(run.utf8.size())), formatForStyle(run.style));
    }
    const auto limit = std::max(0, body_->document()->characterCount() - 1);
    QTextCursor restored(body_->document());
    restored.setPosition(std::clamp(oldAnchor, 0, limit));
    restored.setPosition(std::clamp(oldPosition, 0, limit), QTextCursor::KeepAnchor);
    body_->setTextCursor(restored);

    if (std::holds_alternative<core::SctMessageContinue>(draft.completion))
        completion_->setCurrentIndex(0);
    else if (std::holds_alternative<core::SctMessageClose>(draft.completion))
        completion_->setCurrentIndex(1);
    else {
        completion_->setCurrentIndex(2);
        automaticDelay_->setValue(std::get<core::SctMessageAutomatic>(draft.completion).delay);
    }
    automaticDelay_->setVisible(completion_->currentIndex() == 2);
    position_->setCurrentIndex(static_cast<int>(draft.position));
    disableFastForward_->setChecked(draft.disableFastForward);
    openDurationEnabled_->setChecked(draft.openDuration.has_value());
    openDuration_->setEnabled(draft.openDuration.has_value());
    openDuration_->setValue(draft.openDuration.value_or(0));
    closeDurationEnabled_->setChecked(draft.closeDuration.has_value());
    closeDuration_->setEnabled(draft.closeDuration.has_value());
    closeDuration_->setValue(draft.closeDuration.value_or(0));
    committedDraft_ = draft;
    errorLabel_->hide();
    programmatic_ = false;
    updateToolbarFromCursor();
}

core::SctMessageDraft SctMessageEditorWidget::draftFromWidgets() {
    core::SctMessageDraft draft;
    if (headerPresent_->isChecked()) {
        const auto utf8 = header_->text().toUtf8();
        draft.headerUtf8 = std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    }

    const auto appendText = [&draft](QString text, const core::SctMessageTextStyle& style) {
        text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        text.replace(QChar::LineSeparator, QLatin1Char('\n'));
        const auto utf8 = text.toUtf8();
        if (utf8.isEmpty()) return;
        const std::string encoded(utf8.constData(), static_cast<std::size_t>(utf8.size()));
        if (!draft.body.empty() && draft.body.back().style == style)
            draft.body.back().utf8 += encoded;
        else
            draft.body.push_back({encoded, style});
    };

    auto* document = body_->document();
    for (auto block = document->begin(); block.isValid(); block = block.next()) {
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
            const auto current = fragment.fragment();
            if (current.isValid())
                appendText(current.text(), styleFromFormat(current.charFormat()));
        }
        if (block.next().isValid())
            appendText(QStringLiteral("\n"), styleFromFormat(block.charFormat()));
    }

    draft.position = static_cast<core::SctMessagePosition>(position_->currentData().toInt());
    draft.disableFastForward = disableFastForward_->isChecked();
    switch (completion_->currentData().toInt()) {
    case 1: draft.completion = core::SctMessageClose{}; break;
    case 2: draft.completion = core::SctMessageAutomatic{
        static_cast<std::uint16_t>(automaticDelay_->value())}; break;
    default: draft.completion = core::SctMessageContinue{}; break;
    }
    if (openDurationEnabled_->isChecked())
        draft.openDuration = static_cast<std::uint16_t>(openDuration_->value());
    if (closeDurationEnabled_->isChecked())
        draft.closeDuration = static_cast<std::uint16_t>(closeDuration_->value());
    return draft;
}

bool SctMessageEditorWidget::commitDraft(
    const core::SctMessageDraft& draft,
    const core::SctMessageEditKind kind) {
    if (!hasBinding() || !commitHandler_) return false;
    if (committedDraft_.has_value() && *committedDraft_ == draft) {
        failedCommitKind_.reset();
        return true;
    }
    committing_ = true;
    const bool success = commitHandler_(*locator_, *target_, draft, kind);
    committing_ = false;
    if (success) {
        committedDraft_ = draft;
        failedCommitKind_.reset();
        errorLabel_->hide();
    } else {
        failedCommitKind_ = kind;
        errorLabel_->setText(tr("The message edit could not be committed. Review Diagnostics for details."));
        errorLabel_->show();
    }
    return success;
}

bool SctMessageEditorWidget::flushPending() {
    if (!pendingCommitKind_.has_value()) {
        if (!failedCommitKind_.has_value()) return true;
        return commitDraft(draftFromWidgets(), *failedCommitKind_);
    }
    commitTimer_->stop();
    const auto pendingSurface = pendingSurface_;
    const auto pendingBurst = pendingBurst_;
    const auto kind = *pendingCommitKind_;
    auto draft = draftFromWidgets();
    if (pendingBurst_ != Burst::None && committedDraft_.has_value()) {
        if (pendingSurface_ == Surface::Body) {
            draft.headerUtf8 = committedDraft_->headerUtf8;
            draft.position = committedDraft_->position;
            draft.disableFastForward = committedDraft_->disableFastForward;
            draft.completion = committedDraft_->completion;
            draft.openDuration = committedDraft_->openDuration;
            draft.closeDuration = committedDraft_->closeDuration;
        } else {
            draft.body = committedDraft_->body;
            draft.position = committedDraft_->position;
            draft.disableFastForward = committedDraft_->disableFastForward;
            draft.completion = committedDraft_->completion;
            draft.openDuration = committedDraft_->openDuration;
            draft.closeDuration = committedDraft_->closeDuration;
        }
    }
    pendingBurst_ = Burst::None;
    pendingCommitKind_.reset();
    if (!commitDraft(draft, kind)) {
        pendingSurface_ = pendingSurface;
        pendingBurst_ = pendingBurst;
        pendingCommitKind_ = kind;
        return false;
    }
    return true;
}

void SctMessageEditorWidget::beforeMutation(const Surface surface, const int intent) {
    if (programmatic_) return;
    if (failedCommitKind_.has_value() && !pendingCommitKind_.has_value())
        (void)flushPending();
    const auto nextBurst = intent == IntentTyping ? Burst::Typing
        : intent == IntentDeletion ? Burst::Deletion : Burst::None;
    if (isImmediateIntent(intent)
        || (pendingCommitKind_.has_value()
            && (pendingSurface_ != surface || pendingBurst_ != nextBurst))) {
        (void)flushPending();
    }
}

void SctMessageEditorWidget::afterMutation(const Surface surface, const int intent) {
    if (programmatic_) return;
    if (intent == IntentTyping) beginBurst(surface, Burst::Typing);
    else if (intent == IntentDeletion) beginBurst(surface, Burst::Deletion);
    else {
        const auto kind = intent == IntentPaste ? core::SctMessageEditKind::Paste
            : core::SctMessageEditKind::Replacement;
        scheduleStandaloneCommit(
            surface == Surface::Header ? core::SctMessageEditKind::Header : kind);
    }
}

void SctMessageEditorWidget::beginBurst(const Surface surface, const Burst burst) {
    pendingSurface_ = surface;
    pendingBurst_ = burst;
    pendingCommitKind_ = surface == Surface::Header
        ? core::SctMessageEditKind::Header
        : burst == Burst::Typing
            ? core::SctMessageEditKind::Typing
            : core::SctMessageEditKind::Deletion;
    commitTimer_->start(TextBurstDelayMs);
}

void SctMessageEditorWidget::commitSemanticChange(const core::SctMessageEditKind kind) {
    if (programmatic_ || !hasBinding()) return;
    if (pendingCommitKind_.has_value() && *pendingCommitKind_ != kind
        && !flushPending()) return;
    pendingBurst_ = Burst::None;
    pendingCommitKind_ = kind;
    commitTimer_->start(SemanticControlDelayMs);
}

void SctMessageEditorWidget::scheduleStandaloneCommit(
    const core::SctMessageEditKind kind) {
    if (programmatic_ || !hasBinding()) return;
    if (pendingCommitKind_.has_value() && !flushPending()) return;
    pendingBurst_ = Burst::None;
    pendingCommitKind_ = kind;
    commitTimer_->start(0);
}

void SctMessageEditorWidget::applyDoubleScale(const bool enabled) {
    if (!flushPending()) return;
    auto cursor = body_->textCursor();
    QTextCharFormat format;
    format.setProperty(DoubleScaleProperty, enabled);
    format.setFontWeight(enabled ? QFont::Bold : QFont::Normal);
    if (cursor.hasSelection()) {
        cursor.mergeCharFormat(format);
        scheduleStandaloneCommit(core::SctMessageEditKind::Formatting);
    } else {
        body_->mergeCurrentCharFormat(format);
    }
}

void SctMessageEditorWidget::chooseColor() {
    const auto selected = QColorDialog::getColor(Qt::white, this, tr("Choose SCT text color"));
    if (!selected.isValid() || !flushPending()) return;
    const auto rgb = selected.rgb() & 0x00ffffffu;
    QTextCharFormat format;
    format.setProperty(ColorProperty, rgb);
    format.setForeground(QColor::fromRgb(rgb));
    auto cursor = body_->textCursor();
    if (cursor.hasSelection()) {
        cursor.mergeCharFormat(format);
        scheduleStandaloneCommit(core::SctMessageEditKind::Formatting);
    } else {
        body_->mergeCurrentCharFormat(format);
    }
}

void SctMessageEditorWidget::clearFormatting() {
    if (!flushPending()) return;
    auto cursor = body_->textCursor();
    if (cursor.hasSelection()) {
        cursor.setCharFormat(QTextCharFormat{});
        scheduleStandaloneCommit(core::SctMessageEditKind::Formatting);
    } else {
        body_->setCurrentCharFormat(QTextCharFormat{});
    }
    updateToolbarFromCursor();
}

void SctMessageEditorWidget::updateToolbarFromCursor() {
    const QSignalBlocker blocker(doubleScale_);
    doubleScale_->setChecked(styleFromFormat(body_->currentCharFormat()).doubleScale);
}

void SctMessageEditorWidget::clear() {
    commitTimer_->stop();
    pendingBurst_ = Burst::None;
    pendingCommitKind_.reset();
    locator_.reset();
    target_.reset();
    committedDraft_.reset();
    failedCommitKind_.reset();
    errorLabel_->hide();
    showEmpty();
}

void SctMessageEditorWidget::focusEditor() {
    if (pages_->currentIndex() == 2) {
        if (headerPresent_->isChecked()) header_->setFocus();
        else body_->setFocus();
    } else {
        blockedLabel_->setFocus();
    }
}

bool SctMessageEditorWidget::hasBinding() const noexcept {
    return locator_.has_value() && target_.has_value();
}

bool SctMessageEditorWidget::isBoundTo(
    const core::AssetLocator& locator,
    const core::SctMessageTarget& target) const noexcept {
    return locator_ == locator && target_ == target;
}

bool SctMessageEditorWidget::isCommitting() const noexcept { return committing_; }

const std::optional<core::AssetLocator>&
SctMessageEditorWidget::boundLocator() const noexcept { return locator_; }

const std::optional<core::SctMessageTarget>&
SctMessageEditorWidget::boundTarget() const noexcept { return target_; }

}  // namespace salsa::qt
