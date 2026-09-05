#include "Sct/SctMetadataEditorWidget.h"

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

namespace salsa::qt {

SctMetadataEditorWidget::SctMetadataEditorWidget(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    target_ = new QLabel(this);
    auto font = target_->font();
    font.setBold(true);
    target_->setFont(font);
    target_->setWordWrap(true);
    empty_ = new QLabel(tr("Select a document, section, instruction, string, supplementary text, or section folder to edit its authoring metadata."), this);
    empty_->setWordWrap(true);
    auto* form = new QFormLayout;
    note_ = new QPlainTextEdit(this);
    note_->setPlaceholderText(tr("Plain multiline note"));
    note_->setMaximumHeight(120);
    bookmark_ = new QCheckBox(tr("Bookmark this entity"), this);
    bookmarkLabel_ = new QLineEdit(this);
    bookmarkLabel_->setPlaceholderText(tr("Optional bookmark label"));
    auto* colorRow = new QHBoxLayout;
    color_ = new QPushButton(tr("Choose color..."), this);
    clearColor_ = new QPushButton(tr("Clear color"), this);
    colorRow->addWidget(color_);
    colorRow->addWidget(clearColor_);
    colorRow->addStretch(1);
    form->addRow(tr("Note"), note_);
    form->addRow(QString{}, bookmark_);
    form->addRow(tr("Bookmark label"), bookmarkLabel_);
    form->addRow(tr("Row accent"), colorRow);
    layout->addWidget(target_);
    layout->addWidget(empty_);
    layout->addLayout(form);
    layout->addStretch(1);

    note_->installEventFilter(this);
    connect(note_, &QPlainTextEdit::textChanged, this, &SctMetadataEditorWidget::markDirty);
    connect(bookmarkLabel_, &QLineEdit::textChanged, this, &SctMetadataEditorWidget::markDirty);
    connect(bookmarkLabel_, &QLineEdit::editingFinished, this, [this] {
        if (!flush()) QTimer::singleShot(0, bookmarkLabel_, [this] {
            bookmarkLabel_->setFocus(Qt::OtherFocusReason);
        });
    });
    connect(bookmark_, &QCheckBox::toggled, this, [this](const bool enabled) {
        bookmarkLabel_->setEnabled(enabled && editingEnabled_ && binding_.has_value());
        markDirty();
        if (!populating_) (void)flush();
    });
    connect(color_, &QPushButton::clicked, this, [this] {
        const auto initial = colorRgb_ ? QColor::fromRgb(*colorRgb_)
            : palette().color(QPalette::Highlight);
        const auto selected = QColorDialog::getColor(initial, this, tr("Choose row accent"));
        if (!selected.isValid()) return;
        colorRgb_ = selected.rgb() & 0xffffffu;
        color_->setText(selected.name(QColor::HexRgb).toUpper());
        markDirty();
        (void)flush();
    });
    connect(clearColor_, &QPushButton::clicked, this, [this] {
        colorRgb_.reset();
        color_->setText(tr("Choose color..."));
        markDirty();
        (void)flush();
    });
    populate();
}

void SctMetadataEditorWidget::setCommitHandler(CommitHandler handler) {
    commitHandler_ = std::move(handler);
}

bool SctMetadataEditorWidget::setBinding(
    std::optional<SctMetadataBinding> binding) {
    const bool same = binding_ && binding
        && binding_->locator == binding->locator
        && binding_->target == binding->target;
    if (!same && !flush()) return false;
    if (same && dirty_) return true;
    binding_ = std::move(binding);
    dirty_ = false;
    populate();
    return true;
}

bool SctMetadataEditorWidget::flush() {
    if (!dirty_ || !binding_) return true;
    const auto candidate = draft();
    if (candidate == binding_->metadata) {
        dirty_ = false;
        return true;
    }
    if (!commitHandler_ || !commitHandler_(binding_->locator, binding_->target, candidate))
        return false;
    binding_->metadata = candidate;
    dirty_ = false;
    return true;
}

void SctMetadataEditorWidget::setEditingEnabled(const bool enabled) {
    if (editingEnabled_ == enabled) return;
    editingEnabled_ = enabled;
    if (dirty_) {
        const bool available = binding_.has_value();
        const bool active = available && editingEnabled_;
        note_->setEnabled(active);
        bookmark_->setEnabled(active);
        bookmarkLabel_->setEnabled(active && bookmark_->isChecked());
        color_->setEnabled(active);
        clearColor_->setEnabled(active && colorRgb_.has_value());
        return;
    }
    populate();
}

const std::optional<SctMetadataBinding>&
SctMetadataEditorWidget::binding() const noexcept { return binding_; }

bool SctMetadataEditorWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == note_ && event->type() == QEvent::FocusOut && !flush())
        QTimer::singleShot(0, note_, [this] { note_->setFocus(Qt::OtherFocusReason); });
    return QWidget::eventFilter(watched, event);
}

void SctMetadataEditorWidget::populate() {
    populating_ = true;
    const QSignalBlocker noteBlocker(note_);
    const QSignalBlocker bookmarkBlocker(bookmark_);
    const QSignalBlocker labelBlocker(bookmarkLabel_);
    const bool available = binding_.has_value();
    target_->setVisible(available);
    empty_->setVisible(!available);
    target_->setText(available ? binding_->label : QString{});
    const auto metadata = available ? binding_->metadata : SctMetadataDraft{};
    note_->setPlainText(metadata.note ? QString::fromStdString(*metadata.note) : QString{});
    bookmark_->setChecked(metadata.bookmarkLabel.has_value());
    bookmarkLabel_->setText(metadata.bookmarkLabel
        ? QString::fromStdString(*metadata.bookmarkLabel) : QString{});
    colorRgb_ = metadata.colorRgb;
    color_->setText(colorRgb_ ? QStringLiteral("#%1").arg(*colorRgb_, 6, 16,
        QLatin1Char('0')).toUpper() : tr("Choose color..."));
    const bool enabled = available && editingEnabled_;
    note_->setEnabled(enabled);
    bookmark_->setEnabled(enabled);
    bookmarkLabel_->setEnabled(enabled && bookmark_->isChecked());
    color_->setEnabled(enabled);
    clearColor_->setEnabled(enabled && colorRgb_.has_value());
    populating_ = false;
}

void SctMetadataEditorWidget::markDirty() {
    if (!populating_ && binding_ && editingEnabled_) dirty_ = true;
}

SctMetadataDraft SctMetadataEditorWidget::draft() const {
    SctMetadataDraft result;
    const auto note = note_->toPlainText();
    if (!note.isEmpty()) result.note = note.toStdString();
    if (bookmark_->isChecked()) result.bookmarkLabel = bookmarkLabel_->text().toStdString();
    result.colorRgb = colorRgb_;
    return result;
}

}  // namespace salsa::qt
