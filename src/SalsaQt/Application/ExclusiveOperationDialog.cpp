#include "Application/ExclusiveOperationDialog.h"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace salsa::qt {
namespace {

QString unitText(const ExclusiveOperationProgressUnit unit) {
    switch (unit) {
    case ExclusiveOperationProgressUnit::Steps: return QObject::tr("steps");
    case ExclusiveOperationProgressUnit::Files: return QObject::tr("files");
    case ExclusiveOperationProgressUnit::Scripts: return QObject::tr("scripts");
    case ExclusiveOperationProgressUnit::Assets: return QObject::tr("assets");
    case ExclusiveOperationProgressUnit::Bytes: return QObject::tr("bytes");
    case ExclusiveOperationProgressUnit::None: break;
    }
    return {};
}

}  // namespace

ExclusiveOperationDialog::ExclusiveOperationDialog(
    std::unique_ptr<ExclusiveOperationController> controller, QWidget* parent)
    : QDialog(parent), controller_(std::move(controller)),
      flow_(controller_->flowDefinition()) {
    controller_->setParent(this);
    setWindowTitle(controller_->title());
    setWindowModality(Qt::ApplicationModal);
    setAttribute(Qt::WA_DeleteOnClose);
    phase_ = new QLabel(this);
    phase_->setWordWrap(true);
    phase_->hide();
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 0);
    progress_->hide();
    diagnostics_ = new QLabel(this);
    diagnostics_->setWordWrap(true);
    diagnostics_->hide();
    pages_ = new QStackedWidget(this);
    buttons_ = new QDialogButtonBox(this);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(phase_);
    layout->addWidget(progress_);
    layout->addWidget(diagnostics_);
    layout->addWidget(pages_, 1);
    layout->addWidget(buttons_);

    const auto errors = flow_.validationErrors();
    valid_ = errors.empty();
    if (!valid_) {
        QStringList messages;
        for (const auto& error : errors) messages.push_back(QString::fromStdString(error));
        phase_->setText(tr("The operation flow is invalid."));
        diagnostics_->setText(messages.join(QLatin1Char('\n')));
        diagnostics_->show();
        applyPageLayout(core::ExclusiveOperationPageLayout::Standard);
        auto* close = buttons_->addButton(QDialogButtonBox::Close);
        connect(close, &QPushButton::clicked, this, &ExclusiveOperationDialog::finishAndClose);
        return;
    }

    connect(controller_.get(), &ExclusiveOperationController::eventRaised,
        this, &ExclusiveOperationDialog::dispatch);
    connect(controller_.get(), &ExclusiveOperationController::presentationChanged,
        this, &ExclusiveOperationDialog::rebuildActions);
    connect(controller_.get(), &ExclusiveOperationController::progressChanged, this,
        [this](const QString& phase, const quint64 completed, const quint64 total,
            const int unit, const QString& item) {
            phase_->setText(progressText(phase, completed, total,
                static_cast<ExclusiveOperationProgressUnit>(unit), item));
            if (total == 0) progress_->setRange(0, 0);
            else {
                progress_->setRange(0, 1000);
                progress_->setValue(static_cast<int>(std::min<quint64>(
                    1000, completed * 1000 / total)));
            }
        });
    connect(controller_.get(), &ExclusiveOperationController::diagnosticsChanged,
        this, [this](const QString& text) {
            diagnostics_->setText(text);
            diagnostics_->setVisible(!text.isEmpty());
            if (!text.isEmpty())
                applyPageLayout(core::ExclusiveOperationPageLayout::Standard);
        });
    connect(controller_.get(), &ExclusiveOperationController::cancellationChanged,
        this, [this] { rebuildActions(); });
    connect(controller_.get(), &ExclusiveOperationController::finishingChanged,
        this, [this](const bool finishing) {
            if (finishing) phase_->setText(tr("Finishing safely…"));
            rebuildActions();
        });
    connect(controller_.get(), &ExclusiveOperationController::dismissalRequested,
        this, &ExclusiveOperationDialog::finishAndClose);
    showCurrentPage();
}

bool ExclusiveOperationDialog::valid() const noexcept { return valid_; }

void ExclusiveOperationDialog::requestApplicationClose() {
    const auto* node = flow_.currentNode();
    if (node && node->role != core::ExclusiveOperationPageRole::Summary
        && controller_->cancellable() && !controller_->finishing())
        requestCancel();
    show();
    raise();
    activateWindow();
}

void ExclusiveOperationDialog::closeEvent(QCloseEvent* event) {
    if (closing_) {
        QDialog::closeEvent(event);
        return;
    }
    const auto* node = flow_.currentNode();
    if (node && node->role == core::ExclusiveOperationPageRole::Summary) {
        finishAndClose();
    } else if (controller_->cancellable() && !controller_->finishing()) {
        requestCancel();
    }
    event->ignore();
}

void ExclusiveOperationDialog::dispatch(const QString& event) {
    if (!flow_.dispatch(event.toStdString(), [this](const std::string_view edge) {
            return controller_->edgeEnabled(edge);
        })) return;
    showCurrentPage();
}

void ExclusiveOperationDialog::showCurrentPage() {
    const auto id = flow_.currentPage();
    const auto* node = flow_.currentNode();
    const bool showProgress = node != nullptr
        && node->progress == core::ExclusiveOperationProgressVisibility::Visible;
    phase_->setVisible(showProgress);
    progress_->setVisible(showProgress);
    if (showProgress) {
        phase_->clear();
        progress_->setRange(0, 0);
    }
    auto found = pageWidgets_.find(id);
    if (found == pageWidgets_.end()) {
        auto* page = controller_->createPage(id, pages_);
        if (page == nullptr) page = new QWidget(pages_);
        pages_->addWidget(page);
        found = pageWidgets_.emplace(id, page).first;
    }
    pages_->setCurrentWidget(found->second);
    controller_->pageEntered(id);
    rebuildActions();
    if (node != nullptr) {
        const auto layout = node->layout;
        applyPageLayout(layout);
        QTimer::singleShot(0, this, [this, layout] { applyPageLayout(layout); });
    }
}

void ExclusiveOperationDialog::applyPageLayout(
    const core::ExclusiveOperationPageLayout layout) {
    QSize pageMinimum;
    if (pages_ != nullptr && pages_->currentWidget() != nullptr) {
        const auto pageHint = pages_->currentWidget()->sizeHint();
        if (pageHint.isValid()) pageMinimum = pageHint + QSize(80, 150);
    }
    QScreen* screen = nullptr;
    if (parentWidget() != nullptr)
        screen = QGuiApplication::screenAt(parentWidget()->mapToGlobal(
            parentWidget()->rect().center()));
    if (screen == nullptr) screen = QGuiApplication::primaryScreen();
    const auto available = screen != nullptr
        ? screen->availableGeometry().size() : QSize(1920, 1080);
    const auto target = core::resolveExclusiveOperationSize(layout,
        {pageMinimum.width(), pageMinimum.height()},
        {available.width(), available.height()},
        initialSizeApplied_ ? std::optional<core::ExclusiveOperationLogicalSize>{
            {width(), height()}} : std::nullopt);
    resize(target.width, target.height);
    initialSizeApplied_ = true;
}

void ExclusiveOperationDialog::rebuildActions() {
    for (auto* button : buttons_->buttons()) {
        buttons_->removeButton(button);
        button->deleteLater();
    }
    const auto* node = flow_.currentNode();
    if (node && node->role == core::ExclusiveOperationPageRole::Summary) {
        auto* done = buttons_->addButton(tr("Done"), QDialogButtonBox::AcceptRole);
        done->setDefault(true);
        connect(done, &QPushButton::clicked, this, &ExclusiveOperationDialog::finishAndClose);
        return;
    }
    for (const auto* edge : flow_.outgoing([this](const std::string_view id) {
            return controller_->edgeEnabled(id);
        })) {
        const auto action = controller_->actionForEdge(edge->id);
        if (!action) continue;
        const auto role = action->role == ExclusiveOperationActionRole::Primary
            ? QDialogButtonBox::AcceptRole
            : action->role == ExclusiveOperationActionRole::Destructive
                ? QDialogButtonBox::DestructiveRole : QDialogButtonBox::ActionRole;
        auto* button = buttons_->addButton(action->label, role);
        if (action->role == ExclusiveOperationActionRole::Primary) button->setDefault(true);
        connect(button, &QPushButton::clicked, this,
            [this, event = QString::fromStdString(edge->event)] {
                controller_->handleEvent(event.toStdString());
            });
    }
    if (controller_->cancellable() && !controller_->finishing()) {
        auto* cancel = buttons_->addButton(QDialogButtonBox::Cancel);
        connect(cancel, &QPushButton::clicked, this, &ExclusiveOperationDialog::requestCancel);
    }
}

void ExclusiveOperationDialog::requestCancel() {
    if (!controller_->cancellable() || controller_->finishing()) return;
    controller_->requestCancel();
}

void ExclusiveOperationDialog::finishAndClose() {
    if (closing_) return;
    closing_ = true;
    emit sessionDone();
    accept();
}

QString ExclusiveOperationDialog::progressText(const QString& phase,
    const quint64 completed, const quint64 total,
    const ExclusiveOperationProgressUnit unit, const QString& currentItem) const {
    QString text = phase;
    if (total != 0) {
        const auto name = unitText(unit);
        text += name.isEmpty() ? tr(" — %1 of %2").arg(completed).arg(total)
                               : tr(" — %1 of %2 %3").arg(completed).arg(total).arg(name);
    }
    if (!currentItem.isEmpty()) text += tr(" — %1").arg(currentItem);
    return text;
}

}  // namespace salsa::qt
