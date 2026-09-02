#include "Sct/SctMessageEditorWidget.h"

#include "SalsaCore/Sct/SctGlyphCatalog.h"
#include "SalsaCore/Foundation/Hashing.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctTextCodec.h"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFont>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
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

[[nodiscard]] QString textPreview(const spice::sct::SctTextValue& value) {
    return std::visit([](const auto& text) -> QString {
        using T = std::decay_t<decltype(text)>;
        if constexpr (std::is_same_v<T, spice::sct::SctPlainText>)
            return QString::fromUtf8(text.utf8.data(), static_cast<qsizetype>(text.utf8.size()));
        else if constexpr (std::is_same_v<T, spice::sct::SctMessage>) {
            QString result;
            if (text.headerUtf8) result += QObject::tr("Header: %1\n").arg(
                QString::fromUtf8(text.headerUtf8->data(), static_cast<qsizetype>(text.headerUtf8->size())));
            for (const auto& element : text.body.elements) {
                if (const auto* chunk = std::get_if<spice::sct::SctTextChunk>(&element))
                    result += QString::fromUtf8(chunk->utf8.data(), static_cast<qsizetype>(chunk->utf8.size()));
                else result += QObject::tr(" [command] ");
            }
            return result;
        } else if constexpr (std::is_same_v<T, spice::sct::SctOpaqueText>)
            return QObject::tr("%1 opaque bytes").arg(text.bytes.size());
        else return QObject::tr("Empty indexed text");
    }, value);
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

void SctMessageEditorWidget::setPlainTextCommitHandler(PlainTextCommitHandler handler) {
    plainTextCommitHandler_ = std::move(handler);
}

void SctMessageEditorWidget::setTextValueCommitHandler(TextValueCommitHandler handler) {
    textValueCommitHandler_ = std::move(handler);
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
    glyphs_ = new QPushButton(tr("Glyphs..."), editorPage_);
    toolbar->addWidget(doubleScale_);
    toolbar->addWidget(color_);
    toolbar->addWidget(resetFormatting_);
    toolbar->addWidget(glyphs_);
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

    plainPage_ = new QWidget(pages_);
    auto* plainLayout = new QVBoxLayout(plainPage_);
    auto* plainHelp = new QLabel(tr(
        "Plain footer text is edited as exact Unicode. Formatting commands are not available."),
        plainPage_);
    plainHelp->setWordWrap(true);
    auto* plainGlyphs = new QPushButton(tr("Glyphs..."), plainPage_);
    plainText_ = new QTextEdit(plainPage_);
    plainText_->setAcceptRichText(false);
    plainText_->setUndoRedoEnabled(false);
    plainLayout->addWidget(plainHelp);
    plainLayout->addWidget(plainGlyphs, 0, Qt::AlignLeft);
    plainLayout->addWidget(plainText_, 1);
    pages_->addWidget(plainPage_);

    opaquePage_ = new QWidget(pages_);
    opaqueLayout_ = new QVBoxLayout(opaquePage_);
    pages_->addWidget(opaquePage_);

    commitTimer_ = new QTimer(this);
    commitTimer_->setSingleShot(true);
    commitTimer_->setInterval(TextBurstDelayMs);
    connect(commitTimer_, &QTimer::timeout, this, [this]() { (void)flushPending(); });
    connect(plainText_, &QTextEdit::textChanged, this, [this]() {
        if (!programmatic_ && mode_ == Mode::Plain) commitTimer_->start(TextBurstDelayMs);
    });
    connect(plainGlyphs, &QPushButton::clicked,
        this, &SctMessageEditorWidget::openGlyphPalette);

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
    connect(glyphs_, &QPushButton::clicked,
        this, &SctMessageEditorWidget::openGlyphPalette);
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

bool SctMessageEditorWidget::bindText(
    core::AssetLocator locator, core::SctTextTarget target,
    const spice::sct::SctTextValue& value, const spice::sct::SctTextKind kind,
    const spice::sct::SctTextStorage storage) {
    if (!flushPending()) return false;
    locator_ = std::move(locator);
    target_ = std::move(target);
    textKind_ = kind;
    textStorage_ = storage;
    committedDraft_.reset();
    identityLabel_->setText(tr("%1 — %2 %3")
        .arg(QString::fromStdWString(locator_->path().wstring()))
        .arg(std::holds_alternative<spice::sct::SctStringId>(*target_)
            ? tr("indexed string") : tr("footer text"))
        .arg(std::visit([](const auto id) { return QString::number(id.value()); }, *target_)));
    return refreshText(value);
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
    mode_ = Mode::Message;
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
    mode_ = Mode::Message;
    pages_->setCurrentIndex(2);
    return true;
}

bool SctMessageEditorWidget::refreshText(const spice::sct::SctTextValue& value) {
    if (!hasBinding() || committing_) return false;
    if (const auto* message = std::get_if<spice::sct::SctMessage>(&value))
        return refreshMessage(*message);
    if (const auto* plain = std::get_if<spice::sct::SctPlainText>(&value)) {
        loadPlainText(*plain);
        return true;
    }
    if (const auto* opaque = std::get_if<spice::sct::SctOpaqueText>(&value)) {
        loadOpaqueText(*opaque);
        return true;
    }
    blockedLabel_->setText(tr(
        "This indexed text record is empty. Delete it or replace it through a future repair workflow."));
    mode_ = Mode::Blocked;
    pages_->setCurrentWidget(blockedLabel_);
    return true;
}

const spice::sct::SctMessage* SctMessageEditorWidget::findBoundMessage(
    const core::SctDocumentSnapshot& snapshot) const {
    if (!target_ || !snapshot.document) return nullptr;
    if (!snapshot.analysis) return nullptr;
    const auto& index = snapshot.analysis->entities;
    const spice::sct::SctTextValue* value = std::visit([&](const auto id)
        -> const spice::sct::SctTextValue* {
        const auto* entity = index.find(*snapshot.document, id);
        return entity == nullptr ? nullptr : &entity->value;
    }, *target_);
    return value == nullptr ? nullptr : std::get_if<spice::sct::SctMessage>(value);
}

void SctMessageEditorWidget::showEmpty() {
    mode_ = Mode::None;
    pages_->setCurrentWidget(emptyLabel_);
}

void SctMessageEditorWidget::showBlocked(
    const std::vector<core::SctMessageProfileIssue>& issues) {
    QString text = tr("This decoded message is outside the initial SALSA authoring profile and remains read-only.\n\n");
    for (const auto& issue : issues) {
        text += QStringLiteral("• ") + QString::fromStdString(issue.message) + QLatin1Char('\n');
    }
    blockedLabel_->setText(text.trimmed());
    mode_ = Mode::Blocked;
    pages_->setCurrentWidget(blockedLabel_);
}

void SctMessageEditorWidget::loadPlainText(const spice::sct::SctPlainText& text) {
    programmatic_ = true;
    committedPlainText_ = text.utf8;
    plainText_->setPlainText(QString::fromUtf8(
        text.utf8.data(), static_cast<qsizetype>(text.utf8.size())));
    programmatic_ = false;
    mode_ = Mode::Plain;
    pages_->setCurrentWidget(plainPage_);
}

void SctMessageEditorWidget::loadOpaqueText(const spice::sct::SctOpaqueText& text) {
    while (auto* item = opaqueLayout_->takeAt(0)) {
        if (auto* widget = item->widget()) widget->deleteLater();
        delete item;
    }
    auto* heading = new QLabel(tr(
        "Opaque text can be repaired by selecting a complete known interpretation. "
        "This records only how these source bytes were interpreted; it does not choose an export encoding."),
        opaquePage_);
    heading->setWordWrap(true);
    opaqueLayout_->addWidget(heading);
    const auto inspection = spice::sct::SctTextInspectionService::inspectKnownConventions(
        text, textKind_, textStorage_);
    std::vector<std::byte> digestBytes(text.bytes.size());
    std::ranges::transform(text.bytes, digestBytes.begin(), [](const std::uint8_t value) {
        return static_cast<std::byte>(value);
    });
    const auto digest = core::sha256(digestBytes);
    const auto sourceDigest = digest ? digest.value().toHex() : std::string{};
    for (const auto& interpretation : inspection.interpretations) {
        auto* panel = new QWidget(opaquePage_);
        auto* layout = new QVBoxLayout(panel);
        const auto* descriptor = interpretation.knownConvention
            ? spice::sct::findSctKnownTextConvention(*interpretation.knownConvention) : nullptr;
        auto* name = new QLabel(descriptor == nullptr ? tr("Custom interpretation")
            : QString::fromUtf8(descriptor->stableName.data(),
                static_cast<qsizetype>(descriptor->stableName.size())), panel);
        auto font = name->font();
        font.setBold(true);
        name->setFont(font);
        layout->addWidget(name);
        auto* preview = new QTextEdit(panel);
        preview->setReadOnly(true);
        preview->setMaximumHeight(130);
        preview->setPlainText(interpretation.semanticValue
            ? textPreview(*interpretation.semanticValue)
            : tr("No complete semantic value could be decoded."));
        layout->addWidget(preview);
        for (const auto& issue : interpretation.issues) {
            auto* issueLabel = new QLabel(QStringLiteral("• ")
                + QString::fromStdString(issue.message), panel);
            issueLabel->setWordWrap(true);
            layout->addWidget(issueLabel);
        }
        auto* use = new QPushButton(tr("Use This Interpretation"), panel);
        use->setEnabled(interpretation.complete && interpretation.semanticValue.has_value());
        if (interpretation.semanticValue) {
            const auto semanticValue = *interpretation.semanticValue;
            const core::SctTextRepairProvenance provenance{
                interpretation.encoding, interpretation.knownConvention, sourceDigest};
            connect(use, &QPushButton::clicked, this, [this, semanticValue, provenance]() {
                if (!locator_ || !target_ || !textValueCommitHandler_) return;
                committing_ = true;
                const auto success = textValueCommitHandler_(*locator_, *target_,
                    semanticValue, "Repair opaque text interpretation", provenance);
                committing_ = false;
                if (success) (void)refreshText(semanticValue);
            });
        }
        layout->addWidget(use);
        opaqueLayout_->addWidget(panel);
    }
    opaqueLayout_->addStretch(1);
    mode_ = Mode::Opaque;
    pages_->setCurrentWidget(opaquePage_);
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
    if (mode_ == Mode::Plain) return flushPlainText();
    if (mode_ != Mode::Message) return true;
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

bool SctMessageEditorWidget::flushPlainText() {
    commitTimer_->stop();
    if (!locator_ || !target_ || !plainTextCommitHandler_) return true;
    const auto value = plainText_->toPlainText().toUtf8().toStdString();
    if (value == committedPlainText_) return true;
    committing_ = true;
    const auto success = plainTextCommitHandler_(*locator_, *target_, value);
    committing_ = false;
    if (success) committedPlainText_ = value;
    else emit statusMessageRequested(tr(
        "The plain-text edit could not be committed. Review Diagnostics for details."));
    return success;
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

void SctMessageEditorWidget::openGlyphPalette() {
    if (glyphPalette_ != nullptr) {
        glyphPalette_->show();
        glyphPalette_->raise();
        glyphPalette_->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    glyphPalette_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("SCT Glyph Palette"));
    dialog->resize(620, 520);
    dialog->setProperty("insertHeader", mode_ == Mode::Message && header_->hasFocus());
    auto* layout = new QVBoxLayout(dialog);
    auto* help = new QLabel(tr(
        "These glyphs come from legacy SALSA authoring sets. Availability is evidence, not an export guarantee."),
        dialog);
    help->setWordWrap(true);
    layout->addWidget(help);
    auto* controls = new QHBoxLayout;
    auto* search = new QLineEdit(dialog);
    search->setPlaceholderText(tr("Search glyph, U+ code, category, or provenance"));
    auto* membership = new QComboBox(dialog);
    membership->addItem(tr("All legacy sets"), 0);
    membership->addItem(tr("European"), static_cast<int>(core::SctGlyphMembership::European));
    membership->addItem(tr("US / Japanese"), static_cast<int>(core::SctGlyphMembership::UsJapanese));
    controls->addWidget(search, 1);
    controls->addWidget(membership);
    layout->addLayout(controls);
    auto* list = new QListWidget(dialog);
    layout->addWidget(list, 1);
    auto* favorite = new QPushButton(tr("Toggle Favorite"), dialog);
    auto* close = new QPushButton(tr("Close"), dialog);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(favorite);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    std::function<void()> rebuild = [search, membership, list]() {
        QSettings settings(QStringLiteral("jahorta"), QStringLiteral("SALSA"));
        const auto favorites = settings.value(QStringLiteral("TextEditor/GlyphFavorites"))
            .toStringList();
        const auto recent = settings.value(QStringLiteral("TextEditor/GlyphRecents"))
            .toStringList();
        const auto requested = static_cast<core::SctGlyphMembership>(
            membership->currentData().toInt());
        const auto results = core::SctGlyphCatalog::legacySupportedSet().search(
            search->text().toStdString(), requested);
        list->clear();
        for (const auto& entry : results) {
            const auto glyph = QString::fromUtf8(entry.utf8.data(),
                static_cast<qsizetype>(entry.utf8.size()));
            const auto prefix = favorites.contains(glyph) ? QStringLiteral("★ ")
                : recent.contains(glyph) ? QStringLiteral("↺ ") : QString{};
            auto* item = new QListWidgetItem(prefix + glyph + QStringLiteral("   ")
                + QString::fromStdString(entry.displayName) + QStringLiteral("   ")
                + QString::fromStdString(entry.category), list);
            item->setData(Qt::UserRole, glyph);
            item->setToolTip(QString::fromStdString(entry.provenance + "\n" + entry.confidence));
        }
    };
    rebuild();
    connect(search, &QLineEdit::textChanged, dialog, [rebuild](const QString&) { rebuild(); });
    connect(membership, &QComboBox::currentIndexChanged, dialog, [rebuild]() { rebuild(); });
    connect(favorite, &QPushButton::clicked, dialog, [list, rebuild]() {
        const auto* item = list->currentItem();
        if (item == nullptr) return;
        const auto glyph = item->data(Qt::UserRole).toString();
        QSettings settings(QStringLiteral("jahorta"), QStringLiteral("SALSA"));
        auto favorites = settings.value(QStringLiteral("TextEditor/GlyphFavorites")).toStringList();
        if (favorites.contains(glyph)) favorites.removeAll(glyph);
        else favorites.prepend(glyph);
        settings.setValue(QStringLiteral("TextEditor/GlyphFavorites"), favorites);
        rebuild();
    });
    const auto insert = [this, dialog, list](QListWidgetItem* item) {
        if (item == nullptr) return;
        const auto glyph = item->data(Qt::UserRole).toString();
        if (mode_ == Mode::Plain) {
            auto cursor = plainText_->textCursor();
            cursor.insertText(glyph);
            plainText_->setTextCursor(cursor);
        } else if (mode_ == Mode::Message) {
            if (dialog->property("insertHeader").toBool()) {
                beforeMutation(Surface::Header, IntentTyping);
                header_->insert(glyph);
                afterMutation(Surface::Header, IntentTyping);
            } else {
                beforeMutation(Surface::Body, IntentTyping);
                auto cursor = body_->textCursor();
                cursor.insertText(glyph);
                body_->setTextCursor(cursor);
                afterMutation(Surface::Body, IntentTyping);
            }
        }
        QSettings settings(QStringLiteral("jahorta"), QStringLiteral("SALSA"));
        auto recent = settings.value(QStringLiteral("TextEditor/GlyphRecents")).toStringList();
        recent.removeAll(glyph);
        recent.prepend(glyph);
        while (recent.size() > 30) recent.removeLast();
        settings.setValue(QStringLiteral("TextEditor/GlyphRecents"), recent);
    };
    connect(list, &QListWidget::itemDoubleClicked, dialog, insert);
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    connect(dialog, &QObject::destroyed, this, [this]() { glyphPalette_ = nullptr; });
    dialog->show();
}

void SctMessageEditorWidget::clear() {
    commitTimer_->stop();
    pendingBurst_ = Burst::None;
    pendingCommitKind_.reset();
    locator_.reset();
    target_.reset();
    committedDraft_.reset();
    committedPlainText_.clear();
    failedCommitKind_.reset();
    errorLabel_->hide();
    if (glyphPalette_) glyphPalette_->close();
    showEmpty();
}

void SctMessageEditorWidget::focusEditor() {
    if (mode_ == Mode::Message) {
        if (headerPresent_->isChecked()) header_->setFocus();
        else body_->setFocus();
    } else if (mode_ == Mode::Plain) {
        plainText_->setFocus();
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
