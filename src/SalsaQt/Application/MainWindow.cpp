#include "Application/MainWindow.h"
#include "Application/ExclusiveOperationCoordinator.h"
#include "Application/DisabledActionHintPresenter.h"
#include "Application/HelpWindow.h"
#include "Application/SalsaLogging.h"
#include "Application/TransientNoticePresenter.h"

#include "SalsaCore/Application/ApplicationInfo.h"
#include "SalsaCore/Application/ShellPresentation.h"
#include "SalsaCore/Legacy/LegacyFreshImport.h"
#include "SalsaCore/Legacy/LegacyMetadataPromotion.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include "SalsaCore/Sct/SctParameterAuthoring.h"
#include "SalsaCore/Sct/SctAuthoringCatalog.h"
#include "Sct/SctCatalogEditorDialog.h"
#include "Sct/SctDocumentController.h"
#include "Sct/SctDocumentWidget.h"
#include "Sct/SctRebaseDialog.h"
#include "Sct/SctExportDialog.h"
#include "Sct/SctMessageEditorWidget.h"
#include "Sct/SctMetadataEditorWidget.h"
#include "Sct/SctScptEditorWidget.h"
#include "Sct/SctSemanticNavigatorWidget.h"
#include "Legacy/LegacyConversionDialog.h"
#include "Ui/UiConstants.h"
#include "Workspace/ActivityLogModel.h"
#include "Workspace/DiagnosticsModel.h"
#include "Workspace/DatasetOverviewWidget.h"
#include "Workspace/WorkspaceModel.h"
#include "Workspace/WorkspaceOperationController.h"
#include "Workspace/WorkspaceMaintenanceController.h"
#include "SpiceSCT/SctInstructionFactory.h"

#include <QAction>
#include <QBrush>
#include <QCloseEvent>
#include <QCheckBox>
#include <QColorDialog>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMessageBox>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSpinBox>
#include <QStyle>
#include <QTableView>
#include <QTableWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <type_traits>
#include <sstream>
#include <span>
#include <unordered_set>

namespace salsa::qt {
namespace {

constexpr int SettingsStateVersion = 1;
constexpr qsizetype MaximumRecentDatasets = 10;

[[nodiscard]] QString shellBlockReasonText(
    const core::ShellActionBlockReason reason) {
    switch (reason) {
    case core::ShellActionBlockReason::None: return {};
    case core::ShellActionBlockReason::OperationActive:
        return QObject::tr("Wait for the current operation to finish.");
    case core::ShellActionBlockReason::DatasetRequired:
        return QObject::tr("Open a dataset first.");
    case core::ShellActionBlockReason::WorkspaceRequired:
        return QObject::tr("Open or create a workspace first.");
    case core::ShellActionBlockReason::DocumentsMustClose:
        return QObject::tr("Close all SCT documents first.");
    case core::ShellActionBlockReason::NoRecentDatasets:
        return QObject::tr("No recent datasets are available.");
    }
    return {};
}

[[nodiscard]] bool hasTextEditingFocus() {
    const auto* focus = QApplication::focusWidget();
    return focus != nullptr && (focus->inherits("QLineEdit")
        || focus->inherits("QTextEdit") || focus->inherits("QPlainTextEdit"));
}

[[nodiscard]] QString draftWord(const std::uint32_t value) {
    return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
}

[[nodiscard]] QString draftValueText(
    const spice::sct::SctDocumentParameterValue& value,
    const std::optional<std::uint32_t> managedTerminator = std::nullopt) {
    return std::visit([managedTerminator](const auto& typed) -> QString {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctEncodedWordValue>) {
            return draftWord(typed.value);
        } else if constexpr (std::is_same_v<T, spice::sct::SctCanonicalExpression>) {
            const auto projected = core::SctExpressionLanguage::project(typed);
            return projected.text.empty()
                ? QObject::tr("Advanced SCPT program")
                : QString::fromStdString(projected.text);
        } else if constexpr (std::is_same_v<T,
                spice::sct::SctTerminatedWordSequenceValue>) {
            QStringList words;
            auto displayEnd = typed.words.end();
            if (managedTerminator && !typed.words.empty()
                && typed.words.back() == *managedTerminator)
                --displayEnd;
            for (auto word = typed.words.begin(); word != displayEnd; ++word)
                words.push_back(draftWord(*word));
            return words.join(QLatin1Char(' '));
        } else if constexpr (std::is_same_v<T, spice::sct::SctInstructionReference>) {
            return QObject::tr("Instruction %1").arg(typed.target.value());
        } else if constexpr (std::is_same_v<T, spice::sct::SctStringReference>) {
            return QObject::tr("Indexed string %1").arg(typed.target.value());
        } else if constexpr (std::is_same_v<T, spice::sct::SctSupplementaryTextReference>) {
            return QObject::tr("Supplementary text %1").arg(typed.target.value());
        } else if constexpr (std::is_same_v<T,
                spice::sct::SctUnresolvedReferenceValue>) {
            return QObject::tr("Choose a compatible target");
        } else {
            QStringList words;
            for (const auto word : typed.words) words.push_back(draftWord(word));
            return words.join(QLatin1Char(' '));
        }
    }, value);
}

[[nodiscard]] spice::sct::SctDocumentParameterValue referenceValue(
    const spice::sct::SctDocumentReferenceTarget& target) {
    return std::visit([](const auto id) -> spice::sct::SctDocumentParameterValue {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
            return spice::sct::SctInstructionReference{id};
        else if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return spice::sct::SctStringReference{id};
        else
            return spice::sct::SctSupplementaryTextReference{id};
    }, target);
}

[[nodiscard]] QString normalizedRecentDatasetPath(const QString& path) {
    const auto cleanedInput = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
    if (cleanedInput.isEmpty()) return {};
    const QFileInfo info(cleanedInput);
    auto normalized = info.canonicalFilePath();
    if (normalized.isEmpty()) normalized = info.absoluteFilePath();
    normalized = QDir::cleanPath(QDir::fromNativeSeparators(normalized));
    return QDir::toNativeSeparators(normalized);
}

[[nodiscard]] bool sameDatasetPath(const QString& left, const QString& right) {
    return normalizedRecentDatasetPath(left).compare(
        normalizedRecentDatasetPath(right), Qt::CaseInsensitive) == 0;
}

[[nodiscard]] bool isTextOnlyTransition(const SctDocumentUpdate& update) {
    if (update.kind != SctDocumentUpdateKind::RevisionTransition
        || !update.transition.has_value()) return false;
    const auto& changes = update.transition->changes;
    if (!changes.sections.empty() || !changes.instructions.empty()
        || !changes.supplementaryText.empty() || changes.textValues.empty()) return false;
    return std::ranges::all_of(changes.modified, [](const auto target) {
        return target.kind == core::SctNavigationKind::String
            || target.kind == core::SctNavigationKind::SupplementaryText;
    });
}

[[nodiscard]] bool isIncrementalStructuralTransition(
    const SctDocumentUpdate& update) {
    if (update.kind != SctDocumentUpdateKind::RevisionTransition
        || !update.transition.has_value()) return false;
    const auto& changes = update.transition->changes;
    return !changes.sections.empty() || !changes.instructions.empty()
        || !changes.supplementaryText.empty();
}

[[nodiscard]] bool affectsMessageTarget(
    const SctDocumentUpdate& update,
    const core::SctMessageTarget& target) {
    if (update.kind == SctDocumentUpdateKind::SourceStatus) return false;
    if (!update.transition.has_value()) return true;
    const auto navigation = std::visit([](const auto id) {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return core::SctNavigationTarget{core::SctNavigationKind::String, id.value()};
        else
            return core::SctNavigationTarget{core::SctNavigationKind::SupplementaryText, id.value()};
    }, target);
    return std::ranges::find(update.transition->changes.modified, navigation)
        != update.transition->changes.modified.end();
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : MainWindow(Mode::Application, parent) {}

MainWindow::MainWindow(const Mode mode, QWidget* parent)
    : QMainWindow(parent), mode_(mode) {
    const auto name = core::applicationName();
    setWindowTitle(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
    resize(1100, 700);

    personalCatalogPath_ = std::filesystem::path(QDir(
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath(
        QStringLiteral("sct-catalog-v1.json")).toStdWString());
    if (const auto loaded = core::SctPersonalCatalogStore(personalCatalogPath_).load(); loaded)
        personalCatalog_ = loaded.value();
    core::SctCatalogResolver::install(
        std::make_shared<const core::SctPersonalCatalog>(personalCatalog_));
    buildUi();
    connectWorkspace();
    if (mode_ == Mode::Application) restoreApplicationSettings();
    syncWorkspace();
    syncDiagnostics();
    syncSemanticNavigator();
    syncActions();
    recordActiveNavigation();
    statusBar()->showMessage(tr("Ready"));
    if (mode_ == Mode::Application) {
        const auto registry = std::filesystem::path(QDir(
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(
            QStringLiteral("migration-transactions")).toStdWString());
        const auto recoveries = core::LegacyFreshImportRecoveryService::recoverAll(registry);
        const auto blocked = std::ranges::count_if(recoveries, [](const auto& result) {
            return result.status == core::FreshLegacyImportCommitStatus::RecoveryBlocked;
        });
        if (blocked != 0) statusBar()->showMessage(tr(
            "%1 interrupted legacy import transaction(s) require attention.").arg(blocked),
            15000);
        QTimer::singleShot(0, this, &MainWindow::attemptRestoreDataset);
    }
}

bool MainWindow::installSemanticCandidate(
    const core::AssetLocator& locator,
    std::shared_ptr<const core::SctDocumentSnapshot> provenanceSnapshot,
    const core::SctSemanticState& state) {
    return mode_ == Mode::IsolatedDocumentEditor
        && documentController_->installTransientDocument(
            locator, std::move(provenanceSnapshot), state);
}

std::optional<core::SctSemanticState> MainWindow::captureSemanticCandidate(
    const core::AssetLocator& locator) {
    if (mode_ != Mode::IsolatedDocumentEditor
        || !prepareScptEditor(locator) || !flushPendingEditors()) return std::nullopt;
    return documentController_->semanticState(locator);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (mode_ == Mode::IsolatedDocumentEditor) {
        QMainWindow::closeEvent(event);
        return;
    }
    if (exclusiveOperations_ && exclusiveOperations_->active()) {
        exclusiveOperations_->requestApplicationClose();
        event->ignore();
        return;
    }
    if (!prepareScptEditor() || !flushPendingEditors() || !confirmDiscardAll(
            tr("exit SALSA"), PendingLifecycle::Exit)) {
        event->ignore();
        return;
    }
    saveWorkspaceSession();
    saveApplicationSettings();
    controller_->cancel();
    documentController_->cancel();
    QMainWindow::closeEvent(event);
}

void MainWindow::buildUi() {
    controller_ = new WorkspaceController(this);
    exclusiveOperations_ = new ExclusiveOperationCoordinator(this);
    documentController_ = new SctDocumentController(this);
    workspaceModel_ = new WorkspaceModel(this);
    diagnosticsModel_ = new DiagnosticsModel(this);
    activityLogModel_ = new ActivityLogModel(this);
    noticePresenter_ = new TransientNoticePresenter(this);
    actionHints_ = new DisabledActionHintPresenter(this);
    details_ = new DatasetOverviewWidget(this);
    tabs_ = new QTabWidget(this);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->addTab(details_, tr("Dataset Overview"));
    tabs_->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);
    setCentralWidget(tabs_);

    projectTree_ = new QTreeView(this);
    projectTree_->setModel(workspaceModel_);
    projectTree_->setIndentation(ui::TreeIndentation);
    projectTree_->setAlternatingRowColors(true);
    projectTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    projectTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    projectDock_ = new QDockWidget(tr("Dataset Explorer"), this);
    projectDock_->setObjectName(QStringLiteral("ProjectExplorerDock"));
    projectDock_->setWidget(projectTree_);
    addDockWidget(Qt::LeftDockWidgetArea, projectDock_);

    diagnosticsView_ = new QTableView(this);
    diagnosticsView_->setModel(diagnosticsModel_);
    diagnosticsView_->setAlternatingRowColors(true);
    diagnosticsView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    diagnosticsView_->setSelectionMode(QAbstractItemView::SingleSelection);
    diagnosticsView_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    diagnosticsView_->horizontalHeader()->setStretchLastSection(false);
    diagnosticsView_->horizontalHeader()->resizeSection(0, 90);
    diagnosticsView_->horizontalHeader()->resizeSection(1, 180);
    diagnosticsView_->horizontalHeader()->resizeSection(2, 500);
    diagnosticsView_->horizontalHeader()->resizeSection(3, 320);

    diagnosticsDock_ = new QDockWidget(tr("Diagnostics"), this);
    diagnosticsDock_->setObjectName(QStringLiteral("DiagnosticsDock"));
    diagnosticsDock_->setWidget(diagnosticsView_);
    addDockWidget(Qt::BottomDockWidgetArea, diagnosticsDock_);

    activityLogView_ = new QTableView(this);
    activityLogView_->setModel(activityLogModel_);
    activityLogView_->setAlternatingRowColors(true);
    activityLogView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    activityLogView_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    activityLogView_->horizontalHeader()->setStretchLastSection(false);
    activityLogView_->horizontalHeader()->resizeSection(0, 110);
    activityLogView_->horizontalHeader()->resizeSection(1, 90);
    activityLogView_->horizontalHeader()->resizeSection(2, 100);
    activityLogView_->horizontalHeader()->resizeSection(3, 180);
    activityLogView_->horizontalHeader()->resizeSection(4, 500);
    activityLogView_->horizontalHeader()->resizeSection(5, 320);
    activityLogDock_ = new QDockWidget(tr("Activity Log"), this);
    activityLogDock_->setObjectName(QStringLiteral("ActivityLogDock"));
    activityLogDock_->setWidget(activityLogView_);
    addDockWidget(Qt::BottomDockWidgetArea, activityLogDock_);
    tabifyDockWidget(diagnosticsDock_, activityLogDock_);
    diagnosticsDock_->raise();
    activityLogDock_->hide();

    semanticNavigator_ = new SctSemanticNavigatorWidget(this);
    semanticNavigatorDock_ = new QDockWidget(tr("Semantic Navigator"), this);
    semanticNavigatorDock_->setObjectName(QStringLiteral("SemanticNavigatorDock"));
    semanticNavigatorDock_->setWidget(semanticNavigator_);
    addDockWidget(Qt::RightDockWidgetArea, semanticNavigatorDock_);

    metadataEditor_ = new SctMetadataEditorWidget(this);
    metadataEditor_->setCommitHandler([this](const core::AssetLocator& locator,
            const core::SctNavigationTarget target,
            const SctMetadataDraft& draft) {
        const auto commitAtMetadataDock = [this](auto&& commit) {
            if (metadataDock_ != nullptr) pendingInteractionPosition_ =
                metadataDock_->mapToGlobal(QPoint(16, 32));
            const bool accepted = commit();
            pendingInteractionPosition_.reset();
            return accepted;
        };
        if (target.kind == core::SctNavigationKind::SectionFolder) {
            const auto state = documentController_->semanticState(locator);
            if (!state) return false;
            const auto found = std::ranges::find(state->folders, target.id,
                [](const auto& folder) { return folder.id.value; });
            if (found == state->folders.end()) return false;
            auto folder = *found;
            folder.note = draft.note;
            folder.bookmarkLabel = draft.bookmarkLabel;
            folder.colorRgb = draft.colorRgb;
            return commitAtMetadataDock([&] {
                return documentController_->updateSectionFolder(locator, std::move(folder));
            });
        }
        std::optional<core::SctAuthoringTargetKind> kind;
        switch (target.kind) {
        case core::SctNavigationKind::Document: kind = core::SctAuthoringTargetKind::Document; break;
        case core::SctNavigationKind::Section: kind = core::SctAuthoringTargetKind::Section; break;
        case core::SctNavigationKind::Instruction: kind = core::SctAuthoringTargetKind::Instruction; break;
        case core::SctNavigationKind::String: kind = core::SctAuthoringTargetKind::String; break;
        case core::SctNavigationKind::SupplementaryText: kind = core::SctAuthoringTargetKind::SupplementaryText; break;
        default: break;
        }
        if (!kind) return false;
        core::SctEntityAnnotation annotation{{*kind, target.id}};
        annotation.note = draft.note;
        annotation.bookmarkLabel = draft.bookmarkLabel;
        annotation.colorRgb = draft.colorRgb;
        return commitAtMetadataDock([&] {
            return documentController_->setAnnotation(locator, std::move(annotation));
        });
    });
    metadataDock_ = new QDockWidget(tr("Metadata"), this);
    metadataDock_->setObjectName(QStringLiteral("SctMetadataDock"));
    metadataDock_->setWidget(metadataEditor_);
    addDockWidget(Qt::RightDockWidgetArea, metadataDock_);
    tabifyDockWidget(semanticNavigatorDock_, metadataDock_);

    messageEditor_ = new SctMessageEditorWidget(this);
    messageEditor_->setCommitHandler(
        [this](const core::AssetLocator& locator,
            const core::SctMessageTarget& target,
            const core::SctMessageDraft& draft,
            const core::SctMessageEditKind kind) {
            return documentController_->replaceMessage(locator, target, draft, kind);
        });
    messageEditor_->setPlainTextCommitHandler(
        [this](const core::AssetLocator& locator, const core::SctTextTarget& target,
            std::string utf8) {
            return documentController_->replacePlainText(locator, target, std::move(utf8));
        });
    messageEditor_->setTextValueCommitHandler(
        [this](const core::AssetLocator& locator, const core::SctTextTarget& target,
            spice::sct::SctTextValue value, std::string description,
            std::optional<core::SctTextRepairProvenance> repairProvenance) {
            return documentController_->replaceTextValue(
                locator, target, std::move(value), std::move(description),
                std::move(repairProvenance));
        });
    messageEditorDock_ = new QDockWidget(tr("SCT Text Editor"), this);
    messageEditorDock_->setObjectName(QStringLiteral("SctMessageEditorDock"));
    messageEditorDock_->setWidget(messageEditor_);
    addDockWidget(Qt::RightDockWidgetArea, messageEditorDock_);
    tabifyDockWidget(semanticNavigatorDock_, messageEditorDock_);
    semanticNavigatorDock_->raise();
    messageEditorDock_->hide();

    scptEditor_ = new SctScptEditorWidget(this);
    scptEditor_->setCommitHandler([this](const core::AssetLocator& locator,
            const spice::sct::SctParameterSite& site,
            const spice::sct::SctCanonicalExpression& expression) {
        return documentController_->replaceParameterValue(locator, site, expression);
    });
    scptEditorDock_ = new QDockWidget(tr("Advanced SCPT Editor"), this);
    scptEditorDock_->setObjectName(QStringLiteral("AdvancedScptEditorDock"));
    scptEditorDock_->setWidget(scptEditor_);
    addDockWidget(Qt::RightDockWidgetArea, scptEditorDock_);
    tabifyDockWidget(semanticNavigatorDock_, scptEditorDock_);
    scptEditorDock_->hide();

    auto* snippetPane = new QWidget(this);
    auto* snippetLayout = new QVBoxLayout(snippetPane);
    snippetLayout->setContentsMargins(6, 6, 6, 6);
    snippetSearch_ = new QLineEdit(snippetPane);
    snippetSearch_->setPlaceholderText(tr("Search snippets..."));
    snippetList_ = new QListWidget(snippetPane);
    auto* snippetButtons = new QHBoxLayout;
    pasteSnippetButton_ = new QPushButton(tr("Insert"), snippetPane);
    deleteSnippetButton_ = new QPushButton(tr("Delete"), snippetPane);
    snippetButtons->addWidget(pasteSnippetButton_);
    snippetButtons->addWidget(deleteSnippetButton_);
    snippetLayout->addWidget(snippetSearch_);
    snippetLayout->addWidget(snippetList_, 1);
    snippetLayout->addLayout(snippetButtons);
    snippetLibraryDock_ = new QDockWidget(tr("Snippet Library"), this);
    snippetLibraryDock_->setObjectName(QStringLiteral("SnippetLibraryDock"));
    snippetLibraryDock_->setWidget(snippetPane);
    addDockWidget(Qt::RightDockWidgetArea, snippetLibraryDock_);
    tabifyDockWidget(semanticNavigatorDock_, snippetLibraryDock_);
    snippetLibraryDock_->hide();

    auto* aliasPane = new QWidget(this);
    auto* aliasLayout = new QVBoxLayout(aliasPane);
    aliasLayout->setContentsMargins(6, 6, 6, 6);
    aliasTable_ = new QTableWidget(0, 4, aliasPane);
    aliasTable_->setHorizontalHeaderLabels(
        {tr("Scope"), tr("Kind"), tr("Index"), tr("Alias")});
    aliasTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    aliasTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    aliasTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    auto* aliasButtons = new QHBoxLayout;
    auto* addDocumentAlias = new QPushButton(tr("Add document"), aliasPane);
    auto* addProjectAlias = new QPushButton(tr("Add workspace"), aliasPane);
    auto* removeAlias = new QPushButton(tr("Remove"), aliasPane);
    editAliasMetadataButton_ = new QPushButton(tr("Metadata..."), aliasPane);
    aliasButtons->addWidget(addDocumentAlias);
    aliasButtons->addWidget(addProjectAlias);
    aliasButtons->addWidget(removeAlias);
    aliasButtons->addWidget(editAliasMetadataButton_);
    aliasLayout->addWidget(aliasTable_, 1);
    aliasLayout->addLayout(aliasButtons);
    aliasDock_ = new QDockWidget(tr("Variable Aliases"), this);
    aliasDock_->setObjectName(QStringLiteral("VariableAliasesDock"));
    aliasDock_->setWidget(aliasPane);
    addDockWidget(Qt::RightDockWidgetArea, aliasDock_);
    tabifyDockWidget(semanticNavigatorDock_, aliasDock_);
    aliasDock_->hide();

    bookmarkTable_ = new QTableWidget(0, 3, this);
    bookmarkTable_->setHorizontalHeaderLabels(
        {tr("Document"), tr("Entity"), tr("Label")});
    bookmarkTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    bookmarkTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    bookmarkTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    bookmarkDock_ = new QDockWidget(tr("Bookmarks"), this);
    bookmarkDock_->setObjectName(QStringLiteral("BookmarksDock"));
    bookmarkDock_->setWidget(bookmarkTable_);
    addDockWidget(Qt::RightDockWidgetArea, bookmarkDock_);
    tabifyDockWidget(semanticNavigatorDock_, bookmarkDock_);
    bookmarkDock_->hide();
    metadataDock_->raise();

    connect(addDocumentAlias, &QPushButton::clicked, this,
        [this] { addVariableAlias(false); });
    connect(addProjectAlias, &QPushButton::clicked, this,
        [this] { addVariableAlias(true); });
    connect(removeAlias, &QPushButton::clicked,
        this, &MainWindow::removeSelectedVariableAlias);
    connect(editAliasMetadataButton_, &QPushButton::clicked,
        this, &MainWindow::editSelectedVariableMetadata);
    connect(aliasTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto row = aliasTable_->currentRow();
        editAliasMetadataButton_->setEnabled(row >= 0
            && !aliasTable_->item(row, 0)->data(Qt::UserRole).toBool());
    });
    editAliasMetadataButton_->setEnabled(false);
    connect(bookmarkTable_, &QTableWidget::cellDoubleClicked,
        this, [this](const int row, int) {
            const auto identity = bookmarkTable_->item(row, 0)->data(Qt::UserRole).toString();
            const auto kind = bookmarkTable_->item(row, 1)->data(Qt::UserRole).toInt();
            const auto id = bookmarkTable_->item(row, 1)->data(Qt::UserRole + 1).toULongLong();
            focusDocument(identity);
            if (static_cast<core::SctNavigationKind>(kind)
                    == core::SctNavigationKind::Variable) {
                const auto variableKind = bookmarkTable_->item(row, 1)
                    ->data(Qt::UserRole + 2).toInt();
                aliasDock_->show();
                aliasDock_->raise();
                for (int aliasRow = 0; aliasRow < aliasTable_->rowCount(); ++aliasRow) {
                    if (!aliasTable_->item(aliasRow, 0)->data(Qt::UserRole).toBool()
                        && aliasTable_->item(aliasRow, 1)->data(Qt::UserRole).toInt()
                            == variableKind
                        && aliasTable_->item(aliasRow, 2)->data(Qt::UserRole).toULongLong()
                            == id) {
                        aliasTable_->selectRow(aliasRow);
                        break;
                    }
                }
                return;
            }
            if (auto* widget = activeDocumentWidget())
                widget->selectTarget({static_cast<core::SctNavigationKind>(kind), id});
        });

    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    openAction_ = fileMenu->addAction(tr("&Open Dataset..."));
    openAction_->setShortcut(QKeySequence::Open);
    recentMenu_ = fileMenu->addMenu(tr("Open &Recent"));
    convertLegacyProjectAction_ = fileMenu->addAction(
        tr("Import &Legacy Project..."));
    fileMenu->addSeparator();
    closeWorkspaceAction_ = fileMenu->addAction(tr("&Close Dataset"));
    fileMenu->addSeparator();
    auto* exitAction = fileMenu->addAction(tr("E&xit"));
    exitAction->setShortcut(QKeySequence::Quit);

    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    undoAction_ = editMenu->addAction(tr("&Undo"));
    undoAction_->setShortcut(QKeySequence::Undo);
    undoAction_->setIcon(QIcon::fromTheme(QStringLiteral("edit-undo"),
        style()->standardIcon(QStyle::SP_ArrowBack)));
    redoAction_ = editMenu->addAction(tr("&Redo"));
    redoAction_->setShortcut(QKeySequence::Redo);
    redoAction_->setIcon(QIcon::fromTheme(QStringLiteral("edit-redo"),
        style()->standardIcon(QStyle::SP_ArrowForward)));
    editMenu->addSeparator();
    cutAction_ = editMenu->addAction(tr("Cu&t"));
    cutAction_->setShortcut(QKeySequence::Cut);
    copyAction_ = editMenu->addAction(tr("&Copy"));
    copyAction_->setShortcut(QKeySequence::Copy);
    pasteAction_ = editMenu->addAction(tr("&Paste"));
    pasteAction_->setShortcut(QKeySequence::Paste);
    duplicateAction_ = editMenu->addAction(tr("&Duplicate"));
    duplicateAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    deleteInstructionAction_ = editMenu->addAction(tr("&Delete Selection"));
    deleteInstructionAction_->setShortcut(QKeySequence::Delete);
    saveSnippetAction_ = editMenu->addAction(tr("Save Selection as Snippet..."));

    auto* navigateMenu = menuBar()->addMenu(tr("&Navigate"));
    navigationBackAction_ = navigateMenu->addAction(tr("&Back"));
    navigationBackAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Left));
    navigationBackAction_->setIcon(QIcon::fromTheme(QStringLiteral("go-previous"),
        style()->standardIcon(QStyle::SP_ArrowBack)));
    navigationForwardAction_ = navigateMenu->addAction(tr("&Forward"));
    navigationForwardAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Right));
    navigationForwardAction_->setIcon(QIcon::fromTheme(QStringLiteral("go-next"),
        style()->standardIcon(QStyle::SP_ArrowForward)));
    navigateMenu->addSeparator();
    datasetOverviewAction_ = navigateMenu->addAction(tr("Dataset &Overview"));
    navigateMenu->addAction(semanticNavigatorDock_->toggleViewAction());
    navigateMenu->addAction(bookmarkDock_->toggleViewAction());

    auto* documentMenu = menuBar()->addMenu(tr("&Document"));
    saveAction_ = documentMenu->addAction(tr("&Save Patch Checkpoint"));
    saveAction_->setShortcut(QKeySequence::Save);
    exportAction_ = documentMenu->addAction(tr("&Export SCT..."));
    exportAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    documentMenu->addSeparator();
    editMessageAction_ = documentMenu->addAction(tr("Edit &Text"));
    insertInstructionAction_ = documentMenu->addAction(tr("&Insert Instruction..."));
    insertInstructionAction_->setShortcut(QKeySequence(Qt::Key_Insert));
    moveInstructionUpAction_ = documentMenu->addAction(tr("Move Instruction &Up"));
    moveInstructionUpAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    moveInstructionDownAction_ = documentMenu->addAction(tr("Move Instruction &Down"));
    moveInstructionDownAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Down));
    documentMenu->addSeparator();
    createScriptSectionAction_ = documentMenu->addAction(tr("New Script Section..."));
    createIndexedStringAction_ = documentMenu->addAction(tr("New Indexed String..."));
    renameSectionAction_ = documentMenu->addAction(tr("Rename Section..."));
    deleteSectionAction_ = documentMenu->addAction(tr("Delete Script Section"));
    moveSectionUpAction_ = documentMenu->addAction(tr("Move Section Up"));
    moveSectionDownAction_ = documentMenu->addAction(tr("Move Section Down"));
    documentMenu->addSeparator();
    createSupplementaryTextMessageAction_ = documentMenu->addAction(
        tr("New Supplementary Message"));
    deleteTextAction_ = documentMenu->addAction(tr("Delete Text Entity"));
    documentMenu->addSeparator();
    editInstructionCatalogAction_ = documentMenu->addAction(
        tr("Personal Instruction Catalog..."));

    auto* workspaceMenu = menuBar()->addMenu(tr("&Workspace"));
    associatePatchWorkspaceAction_ = workspaceMenu->addAction(
        tr("Open or Create &Workspace..."));
    disconnectPatchWorkspaceAction_ = workspaceMenu->addAction(tr("Close Workspace"));
    workspaceMenu->addSeparator();
    refreshAction_ = workspaceMenu->addAction(tr("&Refresh Dataset"));
    refreshAction_->setShortcut(QKeySequence::Refresh);
    rebasePatchesAction_ = workspaceMenu->addAction(tr("Rebase Stale Patches..."));
    editProjectOpcodeColorsAction_ = workspaceMenu->addAction(
        tr("Workspace Opcode Colors..."));
    auto* legacyMenu = workspaceMenu->addMenu(tr("Legacy Migration"));
    promoteLegacyMetadataAction_ = legacyMenu->addAction(
        tr("Promote Retained Legacy Metadata..."));
    auto* maintenanceMenu = workspaceMenu->addMenu(tr("Maintenance"));
    cleanWorkspaceEvidenceAction_ = maintenanceMenu->addAction(
        tr("Clean Workspace Recovery Evidence..."));

    editingToolbar_ = addToolBar(tr("Editing"));
    editingToolbar_->setObjectName(QStringLiteral("EditingToolbar"));
    editingToolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    editingToolbar_->addAction(navigationBackAction_);
    editingToolbar_->addAction(navigationForwardAction_);
    editingToolbar_->addSeparator();
    editingToolbar_->addAction(undoAction_);
    editingToolbar_->addAction(redoAction_);

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    auto* panelsMenu = viewMenu->addMenu(tr("Panels"));
    panelsMenu->addAction(projectDock_->toggleViewAction());
    panelsMenu->addAction(metadataDock_->toggleViewAction());
    panelsMenu->addSeparator();
    panelsMenu->addAction(diagnosticsDock_->toggleViewAction());
    panelsMenu->addAction(activityLogDock_->toggleViewAction());
    panelsMenu->addAction(semanticNavigatorDock_->toggleViewAction());
    panelsMenu->addAction(messageEditorDock_->toggleViewAction());
    panelsMenu->addAction(scptEditorDock_->toggleViewAction());
    panelsMenu->addAction(snippetLibraryDock_->toggleViewAction());
    panelsMenu->addAction(aliasDock_->toggleViewAction());
    panelsMenu->addAction(bookmarkDock_->toggleViewAction());
    auto* toolbarsMenu = viewMenu->addMenu(tr("Toolbars"));
    toolbarsMenu->addAction(editingToolbar_->toggleViewAction());
    viewMenu->addSeparator();
    auto* resetLayoutAction = viewMenu->addAction(tr("Reset Window Layout..."));

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    auto* userGuideAction = helpMenu->addAction(tr("SALSA User Guide"));
    userGuideAction->setShortcut(QKeySequence::HelpContents);
    auto* legacyHelpAction = helpMenu->addAction(tr("Importing Legacy Projects"));
    auto* exportHelpAction = helpMenu->addAction(tr("Exporting SCT"));
    auto* shortcutsHelpAction = helpMenu->addAction(tr("Keyboard Shortcuts"));
    helpMenu->addSeparator();
    auto* aboutAction = helpMenu->addAction(tr("About SALSA"));

    for (auto* menu : {fileMenu, editMenu, navigateMenu, documentMenu, workspaceMenu,
            legacyMenu, maintenanceMenu, viewMenu, panelsMenu, toolbarsMenu, helpMenu})
        actionHints_->registerMenu(menu);

#if defined(_DEBUG)
    developerMenu_ = menuBar()->addMenu(tr("DEV"));
    logSctEditTimingsAction_ = developerMenu_->addAction(
        tr("Log SCT Edit Timings"));
    logSctEditTimingsAction_->setCheckable(true);
    logSctEditTimingsAction_->setStatusTip(
        tr("Log foreground edit and background verification phase timings."));
    connect(logSctEditTimingsAction_, &QAction::toggled, this,
        [this](const bool enabled) {
            editTimingsEnabled_ = enabled;
            documentController_->setEditTimingsEnabled(enabled);
            statusBar()->showMessage(enabled
                ? tr("SCT edit timing logging enabled.")
                : tr("SCT edit timing logging disabled."), 5000);
        });
    developerMenu_->addSeparator();
    showStructuredBasicBlocksAction_ = developerMenu_->addAction(
        tr("Show Semantic Basic Blocks"));
    showStructuredBasicBlocksAction_->setCheckable(true);
    showStructuredBasicBlocksAction_->setStatusTip(
        tr("Show derived basic-block wrapper rows in Semantic views."));
    connect(showStructuredBasicBlocksAction_, &QAction::toggled, this,
        [this](const bool enabled) {
            showStructuredBasicBlocks_ = enabled;
            for (int index = 1; index < tabs_->count(); ++index) {
                if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index))) {
                    widget->setStructuredDeveloperOptions(
                        enabled, showRejectedStructureEvidence_,
                        showSemanticControlFlowInstructions_);
                }
            }
        });
    showRejectedStructureEvidenceAction_ = developerMenu_->addAction(
        tr("Show Rejected Structure Evidence"));
    showRejectedStructureEvidenceAction_->setCheckable(true);
    showRejectedStructureEvidenceAction_->setStatusTip(
        tr("Show conservative structure-analysis issue rows and evidence."));
    connect(showRejectedStructureEvidenceAction_, &QAction::toggled, this,
        [this](const bool enabled) {
            showRejectedStructureEvidence_ = enabled;
            for (int index = 1; index < tabs_->count(); ++index) {
                if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index))) {
                    widget->setStructuredDeveloperOptions(
                        showStructuredBasicBlocks_, enabled,
                        showSemanticControlFlowInstructions_);
                }
            }
        });
    showSemanticControlFlowInstructionsAction_ = developerMenu_->addAction(
        tr("Show Semantic Control-Flow Instructions"));
    showSemanticControlFlowInstructionsAction_->setCheckable(true);
    showSemanticControlFlowInstructionsAction_->setStatusTip(
        tr("Reveal managed and inferred jump scaffolding in the Semantic view."));
    connect(showSemanticControlFlowInstructionsAction_, &QAction::toggled, this,
        [this](const bool enabled) {
            showSemanticControlFlowInstructions_ = enabled;
            for (int index = 1; index < tabs_->count(); ++index) {
                if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index))) {
                    widget->setStructuredDeveloperOptions(showStructuredBasicBlocks_,
                        showRejectedStructureEvidence_, enabled);
                }
            }
        });
    logStructureAnalysisAction_ = developerMenu_->addAction(
        tr("Log Structure Analysis Timings"));
    logStructureAnalysisAction_->setCheckable(true);
    logStructureAnalysisAction_->setStatusTip(
        tr("Log structured-control-flow timings and result statistics."));
    connect(logStructureAnalysisAction_, &QAction::toggled, this,
        [this](const bool enabled) {
            structureAnalysisTimingsEnabled_ = enabled;
            documentController_->setStructureTimingsEnabled(enabled);
            statusBar()->showMessage(enabled
                ? tr("Structure-analysis timing logging enabled.")
                : tr("Structure-analysis timing logging disabled."), 5000);
        });
#endif

    progressBar_ = new QProgressBar(this);
    progressBar_->setTextVisible(true);
    progressBar_->setMinimumWidth(180);
    progressBar_->hide();
    statusBar()->addPermanentWidget(progressBar_);

    cancelButton_ = new QToolButton(this);
    cancelButton_->setText(tr("Cancel"));
    cancelButton_->hide();
    statusBar()->addPermanentWidget(cancelButton_);

    workspaceSessionSaveTimer_ = new QTimer(this);
    workspaceSessionSaveTimer_->setSingleShot(true);
    workspaceSessionSaveTimer_->setInterval(500);
    connect(workspaceSessionSaveTimer_, &QTimer::timeout,
        this, &MainWindow::saveWorkspaceSession);

    connect(openAction_, &QAction::triggered, this, &MainWindow::chooseDataset);
    connect(convertLegacyProjectAction_, &QAction::triggered,
        this, &MainWindow::convertLegacyProject);
    connect(saveAction_, &QAction::triggered, this, &MainWindow::saveActiveDocument);
    connect(exportAction_, &QAction::triggered, this, &MainWindow::exportActiveDocument);
    connect(associatePatchWorkspaceAction_, &QAction::triggered,
        this, &MainWindow::associatePatchWorkspace);
    connect(disconnectPatchWorkspaceAction_, &QAction::triggered,
        this, &MainWindow::disconnectPatchWorkspace);
    connect(rebasePatchesAction_, &QAction::triggered,
        this, &MainWindow::rebaseStalePatches);
    connect(cleanWorkspaceEvidenceAction_, &QAction::triggered,
        this, &MainWindow::cleanWorkspaceEvidence);
    connect(promoteLegacyMetadataAction_, &QAction::triggered,
        this, &MainWindow::promoteLegacyMetadata);
    connect(editInstructionCatalogAction_, &QAction::triggered,
        this, &MainWindow::editInstructionCatalog);
    connect(editProjectOpcodeColorsAction_, &QAction::triggered,
        this, &MainWindow::editProjectOpcodeColors);
    connect(closeWorkspaceAction_, &QAction::triggered, this, [this]() {
        if (!prepareScptEditor() || !flushPendingEditors() || !confirmDiscardAll(
                tr("close the dataset"), PendingLifecycle::CloseDataset)) return;
        closeDataset();
    });
    connect(refreshAction_, &QAction::triggered, this, [this]() {
        if (!flushPendingEditors()) return;
        (void)exclusiveOperations_->open(std::make_unique<WorkspaceOperationController>(
            controller_, WorkspaceController::Operation::Refreshing), this);
    });
    connect(cancelButton_, &QToolButton::clicked, this, [this]() {
        controller_->cancel();
        documentController_->cancel();
    });
    connect(exitAction, &QAction::triggered, this, &QWidget::close);
    connect(datasetOverviewAction_, &QAction::triggered, this, [this] {
        tabs_->setCurrentIndex(0);
        tabs_->setFocus();
    });
    connect(resetLayoutAction, &QAction::triggered, this, &MainWindow::resetWindowLayout);
    connect(userGuideAction, &QAction::triggered, this,
        [this] { showHelpTopic(QStringLiteral("README.md")); });
    connect(legacyHelpAction, &QAction::triggered, this,
        [this] { showHelpTopic(QStringLiteral("LegacyImport.md")); });
    connect(exportHelpAction, &QAction::triggered, this,
        [this] { showHelpTopic(QStringLiteral("Exporting.md")); });
    connect(shortcutsHelpAction, &QAction::triggered, this,
        [this] { showHelpTopic(QStringLiteral("KeyboardShortcuts.md")); });
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("About SALSA"), tr(
            "<b>SALSA %1</b><br>Skies of Arcadia Legends script authoring.")
            .arg(QApplication::applicationVersion()));
    });
    connect(details_, &DatasetOverviewWidget::openDatasetRequested,
        openAction_, &QAction::trigger);
    connect(details_, &DatasetOverviewWidget::importLegacyProjectRequested,
        convertLegacyProjectAction_, &QAction::trigger);
    connect(details_, &DatasetOverviewWidget::associateWorkspaceRequested,
        associatePatchWorkspaceAction_, &QAction::trigger);
    connect(details_, &DatasetOverviewWidget::openSelectedAssetRequested,
        this, &MainWindow::activateSelectedAsset);
    connect(exclusiveOperations_, &ExclusiveOperationCoordinator::activeChanged,
        this, [this] { syncActions(); (void)syncMetadataEditor(); });
    connect(exclusiveOperations_, &ExclusiveOperationCoordinator::activityRaised,
        this, [this](const int outcome, const QString& code,
                const QString& message, const QString& location) {
            auto converted = ActivityOutcome::Failed;
            switch (static_cast<ExclusiveOperationActivityOutcome>(outcome)) {
            case ExclusiveOperationActivityOutcome::Completed:
                converted = ActivityOutcome::Completed; break;
            case ExclusiveOperationActivityOutcome::Failed:
                converted = ActivityOutcome::Failed; break;
            case ExclusiveOperationActivityOutcome::Cancelled:
                converted = ActivityOutcome::Cancelled; break;
            }
            activityLogModel_->appendOperation(converted, code, message, location);
        });
    connect(undoAction_, &QAction::triggered, this, &MainWindow::undoActiveDocument);
    connect(redoAction_, &QAction::triggered, this, &MainWindow::redoActiveDocument);
    connect(cutAction_, &QAction::triggered, this, &MainWindow::cutSelection);
    connect(copyAction_, &QAction::triggered, this, &MainWindow::copySelection);
    connect(pasteAction_, &QAction::triggered, this, &MainWindow::pasteSelection);
    connect(duplicateAction_, &QAction::triggered, this, &MainWindow::duplicateSelection);
    connect(saveSnippetAction_, &QAction::triggered,
        this, &MainWindow::saveSelectionAsSnippet);
    connect(QApplication::clipboard(), &QClipboard::dataChanged,
        this, &MainWindow::syncEditActions);
    connect(qApp, &QApplication::focusChanged, this,
        [this](QWidget*, QWidget*) { syncEditActions(); });
    connect(navigationBackAction_, &QAction::triggered, this, &MainWindow::navigateBack);
    connect(navigationForwardAction_, &QAction::triggered, this, &MainWindow::navigateForward);
    connect(editMessageAction_, &QAction::triggered, this, &MainWindow::editSelectedMessage);
    connect(createScriptSectionAction_, &QAction::triggered,
        this, &MainWindow::createScriptSection);
    connect(createIndexedStringAction_, &QAction::triggered,
        this, &MainWindow::createIndexedString);
    connect(renameSectionAction_, &QAction::triggered,
        this, &MainWindow::renameSelectedSection);
    connect(deleteSectionAction_, &QAction::triggered,
        this, &MainWindow::deleteSelectedSection);
    connect(moveSectionUpAction_, &QAction::triggered, this, [this]() {
        moveSelectedSection(core::SctSectionMoveDirection::Up);
    });
    connect(moveSectionDownAction_, &QAction::triggered, this, [this]() {
        moveSelectedSection(core::SctSectionMoveDirection::Down);
    });
    connect(createSupplementaryTextMessageAction_, &QAction::triggered, this, [this]() {
        createSupplementaryText(core::SctCreatedSupplementaryTextKind::Message);
    });
    connect(deleteTextAction_, &QAction::triggered, this, &MainWindow::deleteSelectedText);
    connect(insertInstructionAction_, &QAction::triggered, this, &MainWindow::insertInstruction);
    connect(deleteInstructionAction_, &QAction::triggered, this, &MainWindow::deleteSelection);
    connect(moveInstructionUpAction_, &QAction::triggered, this, [this]() {
        moveInstruction(core::SctInstructionMoveDirection::Up);
    });
    connect(moveInstructionDownAction_, &QAction::triggered, this, [this]() {
        moveInstruction(core::SctInstructionMoveDirection::Down);
    });
    connect(pasteSnippetButton_, &QPushButton::clicked,
        this, &MainWindow::pasteSelectedSnippet);
    connect(deleteSnippetButton_, &QPushButton::clicked,
        this, &MainWindow::deleteSelectedSnippet);
    connect(snippetList_, &QListWidget::itemDoubleClicked,
        this, [this](QListWidgetItem*) { pasteSelectedSnippet(); });
    connect(snippetSearch_, &QLineEdit::textChanged, this,
        [this](const QString& filter) {
            for (int row = 0; row < snippetList_->count(); ++row)
                snippetList_->item(row)->setHidden(
                    !snippetList_->item(row)->text().contains(
                        filter, Qt::CaseInsensitive)
                    && !snippetList_->item(row)->toolTip().contains(
                        filter, Qt::CaseInsensitive));
        });
    connect(snippetList_, &QListWidget::currentRowChanged, this, [this](const int row) {
        pasteSnippetButton_->setEnabled(row >= 0 && activeDocumentWidget() != nullptr);
        deleteSnippetButton_->setEnabled(row >= 0);
    });

    connect(
        projectTree_->selectionModel(),
        &QItemSelectionModel::currentChanged,
        this,
        [this](const QModelIndex& current) {
            const auto asset = workspaceModel_->assetAt(current);
            controller_->selectAsset(asset.has_value()
                ? std::optional<core::AssetLocator>(asset->locator)
                : std::nullopt);
            scheduleWorkspaceSessionSave();
        });
    connect(projectTree_, &QTreeView::expanded,
        this, [this](const QModelIndex&) { scheduleWorkspaceSessionSave(); });
    connect(projectTree_, &QTreeView::collapsed,
        this, [this](const QModelIndex&) { scheduleWorkspaceSessionSave(); });
    connect(projectTree_, &QTreeView::activated, this, [this](const QModelIndex&) {
        activateSelectedAsset();
    });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::closeDocumentTab);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
        if (restoringMetadataTransition_) return;
        if (!flushMetadataEditor()) {
            const auto& binding = metadataEditor_->binding();
            if (binding) {
                restoringMetadataTransition_ = true;
                focusDocument(QString::fromStdString(binding->locator.identityKey()));
                restoringMetadataTransition_ = false;
                (void)syncMetadataEditor();
            }
            return;
        }
        if (!restoringTabAfterCommitFailure_ && messageEditor_->hasBinding()) {
            auto* active = activeDocumentWidget();
            const auto& bound = messageEditor_->boundLocator();
            if (active == nullptr || !bound.has_value() || active->locator() != *bound) {
                if (!flushMessageEditor()) {
                    if (bound.has_value()) {
                        restoringTabAfterCommitFailure_ = true;
                        focusDocument(QString::fromStdString(bound->identityKey()));
                        restoringTabAfterCommitFailure_ = false;
                    }
                    return;
                }
                messageEditor_->clear();
            }
        }
        syncDiagnostics();
        syncSemanticNavigator();
        syncEditActions();
        reloadAuthoringDocks();
        (void)syncMetadataEditor();
        recordActiveNavigation();
        scheduleWorkspaceSessionSave();
    });
    connect(tabs_->tabBar(), &QTabBar::tabMoved,
        this, [this](int, int) { scheduleWorkspaceSessionSave(); });
    connect(diagnosticsView_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        const auto* row = diagnosticsModel_->rowAt(index.row());
        if (row == nullptr || !row->locator.has_value()) return;
        replayingNavigation_ = true;
        focusDocument(QString::fromStdString(row->locator->identityKey()));
        if (row->inspectionLocation.has_value()) {
            if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->currentWidget()))
                (void)widget->selectLocation(*row->inspectionLocation);
        }
        replayingNavigation_ = false;
        recordActiveNavigation();
    });
    connect(semanticNavigator_, &SctSemanticNavigatorWidget::navigationRequested,
        this, [this](const QString& identityKey, const core::SctInspectionLocation location) {
            replayingNavigation_ = true;
            focusDocument(identityKey);
            if (auto* widget = activeDocumentWidget();
                widget == nullptr || !widget->selectLocation(location)) {
                statusBar()->showMessage(
                    tr("That semantic location is not present in the current document revision."),
                    8000);
            }
            replayingNavigation_ = false;
            recordActiveNavigation();
        });
    connect(semanticNavigator_, &SctSemanticNavigatorWidget::statusMessageRequested,
        this, [this](const QString& message) { statusBar()->showMessage(message, 8000); });
    connect(messageEditor_, &SctMessageEditorWidget::statusMessageRequested,
        this, [this](const QString& message) { statusBar()->showMessage(message, 8000); });
    connect(messageEditorDock_, &QDockWidget::visibilityChanged, this, [this](const bool visible) {
        if (!visible && !flushMessageEditor()) {
            messageEditorDock_->show();
            messageEditorDock_->raise();
            messageEditor_->focusEditor();
        }
    });
    pasteSnippetButton_->setEnabled(false);
    deleteSnippetButton_->setEnabled(false);
    reloadSnippets();

    projectDock_->show();
    metadataDock_->show();
    metadataDock_->raise();
    diagnosticsDock_->hide();
    activityLogDock_->hide();
    semanticNavigatorDock_->hide();
    messageEditorDock_->hide();
    scptEditorDock_->hide();
    snippetLibraryDock_->hide();
    aliasDock_->hide();
    bookmarkDock_->hide();
    editingToolbar_->show();
    defaultWindowState_ = saveState(SettingsStateVersion);
}

void MainWindow::connectWorkspace() {
    connect(controller_, &WorkspaceController::datasetChanged, this, &MainWindow::syncWorkspace);
    connect(controller_, &WorkspaceController::selectionChanged, this, &MainWindow::syncSelection);
    connect(controller_, &WorkspaceController::diagnosticsChanged, this, &MainWindow::syncDiagnostics);
    connect(controller_, &WorkspaceController::operationStateChanged, this, [this]() {
        const bool busy = controller_->busy();
        const bool showStatusProgress = busy
            && !(exclusiveOperations_ && exclusiveOperations_->active());
        progressBar_->setVisible(showStatusProgress);
        cancelButton_->setVisible(showStatusProgress);
        if (showStatusProgress) {
            progressBar_->setRange(0, 0);
            progressBar_->setValue(0);
            statusBar()->showMessage(
                controller_->operation() == WorkspaceController::Operation::Opening
                    ? tr("Inspecting dataset...")
                    : tr("Refreshing dataset..."));
        }
        syncActions();
    });
    connect(
        controller_,
        &WorkspaceController::progressChanged,
        this,
        [this](
            const bool determinate,
            const int completed,
            const int total,
            const QString& currentPath) {
            if (exclusiveOperations_ && exclusiveOperations_->active()) return;
            if (determinate) {
                progressBar_->setRange(0, total);
                progressBar_->setValue(completed);
            } else {
                progressBar_->setRange(0, 0);
            }
            const auto fileName = QFileInfo(currentPath).fileName();
            statusBar()->showMessage(
                determinate
                    ? tr("Hashing %1 (%2 of %3)").arg(fileName).arg(completed).arg(total)
                    : tr("Discovering SCT assets: %1").arg(fileName));
        });
    connect(
        controller_,
        &WorkspaceController::operationCompleted,
        this,
        &MainWindow::handleOperationCompleted);
    connect(
        controller_,
        &WorkspaceController::datasetOpened,
        this,
        &MainWindow::recordRecentDataset);

    connect(documentController_, &SctDocumentController::busyChanged, this, [this]() {
        const bool busy = documentController_->busy();
        if (busy) {
            progressBar_->setRange(0, 0);
            progressBar_->show();
            cancelButton_->show();
            statusBar()->showMessage(tr("Decoding SCT document..."));
        } else if (!controller_->busy()) {
            progressBar_->hide();
            cancelButton_->hide();
        }
        syncActions();
    });
    connect(documentController_, &SctDocumentController::documentChanged,
        this, &MainWindow::syncDocument);
    connect(documentController_, &SctDocumentController::focusRequested,
        this, &MainWindow::focusDocument);
    connect(documentController_, &SctDocumentController::selectionRequested,
        this, [this](const QString& identityKey, const int kind, const qulonglong id) {
            focusDocument(identityKey);
            if (auto* widget = activeDocumentWidget())
                widget->selectTarget(
                    { static_cast<core::SctNavigationKind>(kind), id }, false);
        });
    connect(documentController_, &SctDocumentController::selectionRangeRequested,
        this, [this](const QString& identityKey, const QList<int>& kinds,
            const QList<qulonglong>& ids) {
            focusDocument(identityKey);
            if (kinds.size() != ids.size()) return;
            std::vector<core::SctNavigationTarget> targets;
            targets.reserve(static_cast<std::size_t>(ids.size()));
            for (qsizetype index = 0; index < ids.size(); ++index)
                targets.push_back({static_cast<core::SctNavigationKind>(kinds[index]),
                    ids[index]});
            if (auto* widget = activeDocumentWidget())
                widget->selectTargets(targets, false);
        });
    connect(documentController_, &SctDocumentController::documentClosed,
        this, [this](const QString& identityKey) {
            if (messageEditor_->boundLocator().has_value()
                && QString::fromStdString(messageEditor_->boundLocator()->identityKey()) == identityKey)
                messageEditor_->clear();
            if (scptEditor_->boundLocator().has_value()
                && QString::fromStdString(scptEditor_->boundLocator()->identityKey()) == identityKey)
                scptEditor_->clear();
            for (int i = 1; i < tabs_->count(); ++i) {
                auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(i));
                if (widget != nullptr
                    && QString::fromStdString(widget->locator().identityKey()) == identityKey) {
                    tabs_->removeTab(i);
                    widget->deleteLater();
                    break;
                }
            }
            rebuildDocumentTabTitles();
            pruneNavigationHistory();
            syncDiagnostics();
            syncSemanticNavigator();
            syncActions();
            scheduleWorkspaceSessionSave();
        });
    connect(documentController_, &SctDocumentController::operationCompleted,
        this, [this](const QString& identityKey, bool success, bool cancelled,
                const QString& message) {
            if (!controller_->busy()) {
                progressBar_->hide();
                cancelButton_->hide();
            }
            statusBar()->showMessage(message, 8000);
            activityLogModel_->appendOperation(
                cancelled ? ActivityOutcome::Cancelled
                    : success ? ActivityOutcome::Completed : ActivityOutcome::Failed,
                QStringLiteral("SctOperation"), message, identityKey);
            syncDiagnostics();
            syncActions();
            continueWorkspaceSessionRestore(
                identityKey, success, cancelled, message);
        });
    connect(documentController_, &SctDocumentController::editCommitted,
        this, [this](const QString&, const QString& message) {
            statusBar()->showMessage(message, 8000);
        });
    connect(documentController_, &SctDocumentController::editRejected,
        this, &MainWindow::showInteractionNotice);
    connect(documentController_, &SctDocumentController::checkpointCompleted,
        this, [this](const QString& identityKey, const bool success, const bool cancelled,
            const QString& message) {
            activityLogModel_->appendOperation(
                cancelled ? ActivityOutcome::Cancelled
                    : success ? ActivityOutcome::Completed : ActivityOutcome::Failed,
                QStringLiteral("Checkpoint"), message, identityKey);
            statusBar()->showMessage(message, 8000);
            rebuildDocumentTabTitles();
            syncDiagnostics();
            syncActions();
            const bool anySaving = std::ranges::any_of(
                documentController_->openLocators(), [this](const auto& locator) {
                    return documentController_->isSaving(locator);
                });
            if (!anySaving && !controller_->busy() && !documentController_->busy()) {
                progressBar_->hide();
                cancelButton_->hide();
            }
            continuePendingLifecycle(identityKey, success, cancelled);
            scheduleWorkspaceSessionSave();
        });
    connect(documentController_, &SctDocumentController::publicationCompleted,
        this, [this](const QString& identityKey, const bool success,
            const bool cancelled, const QString& message, const bool replacedSource) {
            progressBar_->hide();
            cancelButton_->hide();
            statusBar()->showMessage(message, 10000);
            activityLogModel_->appendOperation(
                cancelled ? ActivityOutcome::Cancelled
                    : success ? ActivityOutcome::Completed : ActivityOutcome::Failed,
                QStringLiteral("Publication"), message, identityKey);
            syncDiagnostics();
            syncActions();
            if (!success && !cancelled) {
                if (!documentController_->failureDiagnostics().empty()
                    && !(exclusiveOperations_ && exclusiveOperations_->active())) {
                    QMessageBox::warning(this, tr("SCT export failed"), message);
                }
            } else if (success && replacedSource) {
                statusBar()->showMessage(
                    tr("Source SCT replaced. Reload the document before saving another patch checkpoint."),
                    12000);
            }
        });
}

void MainWindow::convertLegacyProject() {
    if (exclusiveOperations_->active()) { exclusiveOperations_->focusActive(); return; }
    if (!flushPendingEditors()) return;
    (void)exclusiveOperations_->open(std::make_unique<LegacyImportController>(
        [this](const QString source, const QString workspace) {
            rememberPatchWorkspaceAssociation(source, workspace);
            QTimer::singleShot(0, this, [this, source] { openDataset(source); });
        }), this);
}

void MainWindow::chooseDataset() {
    if (exclusiveOperations_->active()) { exclusiveOperations_->focusActive(); return; }
    const auto initial = recentDatasets_.isEmpty() ? QDir::homePath() : recentDatasets_.front();
    const auto root = QFileDialog::getExistingDirectory(
        this,
        tr("Open extracted game-data dataset"),
        initial,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!root.isEmpty()) {
        openDataset(root);
    }
}

void MainWindow::openDataset(const QString& rootPath) {
    if (exclusiveOperations_->active()) { exclusiveOperations_->focusActive(); return; }
    pendingDatasetRoot_ = rootPath;
    if (!prepareScptEditor() || !flushPendingEditors()
        || !confirmDiscardAll(tr("open another dataset"),
            PendingLifecycle::OpenDataset)) return;
    saveWorkspaceSession();
    scptEditor_->clear();
    auto operation = std::make_unique<WorkspaceOperationController>(
        controller_, WorkspaceController::Operation::Opening, rootPath);
    activeDatasetOperation_ = operation.get();
    if (!exclusiveOperations_->open(std::move(operation), this))
        activeDatasetOperation_.clear();
}

void MainWindow::associatePatchWorkspace() {
    if (exclusiveOperations_->active()) { exclusiveOperations_->focusActive(); return; }
    const auto* dataset = controller_->dataset();
    if (dataset == nullptr || !documentController_->openLocators().empty()) return;
    const auto datasetRoot = QString::fromStdWString(dataset->root.wstring());
    const auto selected = QFileDialog::getExistingDirectory(
        this, tr("Open or create SALSA workspace"),
        patchWorkspace_ ? QString::fromStdWString(
            patchWorkspace_->descriptor().root.wstring()) : datasetRoot,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) return;

    const auto path = std::filesystem::path(selected.toStdWString());
    const auto datasetCopy = *dataset;
    auto operation = WorkspaceMaintenanceController::openWorkspace(
        path, datasetCopy, [this, selected](core::LocalSalsaWorkspace opened) {
            saveWorkspaceSession();
            detachPatchWorkspace(false);
            patchWorkspace_ = std::make_shared<const core::LocalSalsaWorkspace>(
                std::move(opened));
            documentController_->setWorkspace(patchWorkspace_);
            loadWorkspaceAuthoring();
            reloadSnippets();
            const auto* current = controller_->dataset();
            if (current) rememberPatchWorkspaceAssociation(
                QString::fromStdWString(current->root.wstring()), selected);
            syncActions();
            restoreWorkspaceSession();
            QTimer::singleShot(0, this, [this] {
                if (activeWorkspaceMaintenance_ && !restoringWorkspaceSession_)
                    activeWorkspaceMaintenance_->completeRestoration();
            });
            return tr("SALSA workspace opened: %1")
                .arg(QDir::toNativeSeparators(selected));
        });
    activeWorkspaceMaintenance_ = operation.get();
    if (!exclusiveOperations_->open(std::move(operation), this))
        activeWorkspaceMaintenance_.clear();
}

void MainWindow::rebaseStalePatches() {
    if (exclusiveOperations_->active()) { exclusiveOperations_->focusActive(); return; }
    if (patchWorkspace_ == nullptr || controller_->busy()
        || documentController_->busy() || documentController_->isPublishing()) return;
    if (!prepareScptEditor() || !flushPendingEditors()) return;
    const auto dirty = documentController_->dirtyLocators();
    if (!confirmDiscardAll(tr("rebase stale patches"),
            PendingLifecycle::RebasePatches)) return;
    // A true return with pre-existing dirty documents means the user chose
    // Discard. Close just those sessions so their uncheckpointed state cannot
    // leak into the independently reconstructed rebase inputs.
    for (const auto& locator : dirty) documentController_->closeDocument(locator);

    const auto project = controller_->projectSnapshot();
    if (!project) return;
    (void)exclusiveOperations_->open(std::make_unique<SctRebaseController>(
        *project, patchWorkspace_, [this, project](const auto& assets) {
            bool adoptedEveryOpenDocument = true;
            for (const auto& locator : assets) {
                if (!documentController_->contains(locator)) continue;
                adoptedEveryOpenDocument = documentController_->adoptRebasedDocument(
                    *project, locator) && adoptedEveryOpenDocument;
            }
            scheduleWorkspaceSessionSave();
            syncActions();
            return adoptedEveryOpenDocument
                ? tr("The selected stale patches were rebased.")
                : tr("The selected patches were committed, but one open document could not "
                     "adopt the result. Close and reopen that document to load the durable patch.");
        }), this);
}

void MainWindow::cleanWorkspaceEvidence() {
    if (exclusiveOperations_->active()) { exclusiveOperations_->focusActive(); return; }
    if (patchWorkspace_ == nullptr) return;
    (void)exclusiveOperations_->open(WorkspaceMaintenanceController::cleanup(
        patchWorkspace_->descriptor().root,
        patchWorkspace_->descriptor().components.transactions,
        [this] { syncActions(); }), this);
}

void MainWindow::promoteLegacyMetadata() {
    if (!patchWorkspace_ || !documentController_->openLocators().empty()) return;
    const auto root = patchWorkspace_->componentPath(
        patchWorkspace_->descriptor().components.importState);
    QStringList capsuleIds;
    std::error_code issue;
    for (const auto& entry : std::filesystem::directory_iterator(root, issue)) {
        if (issue) break;
        if (!entry.is_regular_file(issue) || issue || entry.path().extension() != L".json") {
            issue.clear();
            continue;
        }
        capsuleIds.push_back(QString::fromStdWString(entry.path().stem().wstring()));
    }
    capsuleIds.sort(Qt::CaseInsensitive);
    if (issue || capsuleIds.isEmpty()) {
        QMessageBox::information(this, tr("Promote Legacy Metadata"),
            issue ? tr("The legacy import-state directory could not be inspected.")
                  : tr("This workspace has no retained legacy import state."));
        return;
    }
    bool accepted = capsuleIds.size() == 1;
    const auto capsuleId = capsuleIds.size() == 1 ? capsuleIds.front()
        : QInputDialog::getItem(this, tr("Promote Legacy Metadata"),
            tr("Retained import"), capsuleIds, 0, false, &accepted);
    if (!accepted) return;

    core::LegacyMetadataPromotionRegistry registry;
    const auto registered = core::registerBuiltInLegacyMetadataPromotionAdapters(registry);
    if (!registered) {
        QMessageBox::warning(this, tr("Promotion unavailable"),
            QString::fromStdString(registered.diagnostics().front().message));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto preview = core::LegacyMetadataPromotionService::preview(
        *patchWorkspace_, capsuleId.toStdString(), registry);
    QApplication::restoreOverrideCursor();
    if (!preview) {
        QMessageBox::warning(this, tr("Promotion preview failed"),
            QString::fromStdString(preview.diagnostics().front().message));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Promote Retained Legacy Metadata"));
    dialog.resize(900, 540);
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(tr(
        "Select compatible metadata to copy from the immutable v7 capsule. Conflicts and unsupported fields remain retained for later features."), &dialog);
    explanation->setWordWrap(true);
    auto* table = new QTableWidget(static_cast<int>(preview.value().items.size()), 4, &dialog);
    table->setHorizontalHeaderLabels({tr("Apply"), tr("Owner"), tr("Field"), tr("Status")});
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto& item = preview.value().items[static_cast<std::size_t>(row)];
        auto* selected = new QTableWidgetItem;
        selected->setData(Qt::UserRole, QString::fromStdString(item.record.recordId));
        const bool eligible = item.assessment.eligible && !item.assessment.conflict;
        selected->setCheckState(eligible ? Qt::Checked : Qt::Unchecked);
        if (!eligible) selected->setFlags(selected->flags() & ~Qt::ItemIsEnabled);
        table->setItem(row, 0, selected);
        table->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(
            item.record.scriptOrdinal
                ? item.record.owner + " " + std::to_string(*item.record.scriptOrdinal)
                : item.record.owner)));
        table->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(item.record.field)));
        table->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(
            item.assessment.reason.empty() ? item.record.reason : item.assessment.reason)));
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply
        | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Apply)->setEnabled(preview.value().hasEligible());
    layout->addWidget(explanation);
    layout->addWidget(table, 1);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    std::vector<std::string> selected;
    for (int row = 0; row < table->rowCount(); ++row)
        if (table->item(row, 0)->checkState() == Qt::Checked)
            selected.push_back(table->item(row, 0)->data(Qt::UserRole).toString().toStdString());
    if (selected.empty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto result = core::LegacyMetadataPromotionService::promote(
        *patchWorkspace_, preview.value(), selected, registry);
    QApplication::restoreOverrideCursor();
    if (!result.applied) {
        QMessageBox::warning(this, tr("Promotion failed"), result.diagnostics.empty()
            ? tr("The selected legacy metadata was not applied.")
            : QString::fromStdString(result.diagnostics.front().message));
        return;
    }
    loadWorkspaceAuthoring();
    statusBar()->showMessage(tr("Promoted %1 legacy metadata records.")
        .arg(result.appliedRecordIds.size()), 8000);
}

void MainWindow::editInstructionCatalog() {
    if (exclusiveOperations_ && exclusiveOperations_->active()) {
        exclusiveOperations_->focusActive();
        return;
    }
    SctCatalogEditorDialog dialog(personalCatalog_, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const auto saved = core::SctPersonalCatalogStore(personalCatalogPath_).save(
        dialog.catalog());
    if (!saved) {
        QMessageBox::warning(this, tr("Catalog could not be saved"),
            QString::fromStdString(saved.diagnostics().front().message));
        return;
    }
    personalCatalog_ = dialog.catalog();
    core::SctCatalogResolver::install(
        std::make_shared<const core::SctPersonalCatalog>(personalCatalog_));
    for (int index = 1; index < tabs_->count(); ++index) {
        if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index)))
            widget->setSnapshot(documentController_->snapshot(widget->locator()),
                static_cast<int>(documentController_->sourceStatus(widget->locator())));
    }
    statusBar()->showMessage(tr("Personal instruction catalog saved."), 5000);
}

bool MainWindow::saveWorkspaceAuthoring() {
    if (!patchWorkspace_) return false;
    const auto path = patchWorkspace_->componentPath(
        patchWorkspace_->descriptor().components.authoring / L"workspace.json");
    const auto saved = core::SctWorkspaceAuthoringStore(path).save(workspaceAuthoring_);
    if (!saved) {
        QMessageBox::warning(this, tr("Workspace metadata could not be saved"),
            QString::fromStdString(saved.diagnostics().front().message));
        return false;
    }
    reloadAuthoringDocks();
    return true;
}

void MainWindow::loadWorkspaceAuthoring() {
    workspaceAuthoring_ = {};
    if (!patchWorkspace_) { reloadAuthoringDocks(); return; }
    const auto path = patchWorkspace_->componentPath(
        patchWorkspace_->descriptor().components.authoring / L"workspace.json");
    const auto loaded = core::SctWorkspaceAuthoringStore(path).load();
    if (loaded) workspaceAuthoring_ = loaded.value();
    else statusBar()->showMessage(QString::fromStdString(
        loaded.diagnostics().front().message), 12000);
    reloadAuthoringDocks();
}

void MainWindow::addVariableAlias(const bool projectScope) {
    auto* document = activeDocumentWidget();
    if (!projectScope && !document) return;
    QDialog dialog(this);
    dialog.setWindowTitle(projectScope ? tr("Add workspace alias")
                                       : tr("Add document alias"));
    auto* layout = new QFormLayout(&dialog);
    auto* kind = new QComboBox(&dialog);
    kind->addItem(tr("Bit"), static_cast<int>(core::SctVariableKind::Bit));
    kind->addItem(tr("Byte"), static_cast<int>(core::SctVariableKind::Byte));
    kind->addItem(tr("Integer"), static_cast<int>(core::SctVariableKind::Integer));
    kind->addItem(tr("Float"), static_cast<int>(core::SctVariableKind::Float));
    auto* index = new QSpinBox(&dialog);
    index->setRange(0, (std::numeric_limits<int>::max)());
    auto* alias = new QLineEdit(&dialog);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok
        | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(tr("Variable kind"), kind);
    layout->addRow(tr("Index"), index);
    layout->addRow(tr("Alias"), alias);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || alias->text().trimmed().isEmpty()) return;
    const core::SctVariableKey key{
        static_cast<core::SctVariableKind>(kind->currentData().toInt()),
        static_cast<std::uint32_t>(index->value())};
    const auto normalizedAlias = alias->text().trimmed().toStdString();
    if (projectScope) {
        auto candidate = workspaceAuthoring_;
        const auto found = std::ranges::find(candidate.projectAliases,
            key, &core::SctVariableAlias::variable);
        if (found == candidate.projectAliases.end())
            candidate.projectAliases.push_back({key, normalizedAlias});
        else found->alias = normalizedAlias;
        const auto encoded = core::SctAuthoringCatalogCodec::serializeWorkspace(candidate);
        if (!encoded) {
            QMessageBox::warning(this, tr("Invalid workspace alias"),
                QString::fromStdString(encoded.diagnostics().front().message));
            return;
        }
        const auto previous = workspaceAuthoring_;
        workspaceAuthoring_ = std::move(candidate);
        if (!saveWorkspaceAuthoring()) workspaceAuthoring_ = previous;
    } else {
        (void)documentController_->setVariableAlias(document->locator(), key,
            normalizedAlias);
    }
}

void MainWindow::removeSelectedVariableAlias() {
    const auto row = aliasTable_->currentRow();
    if (row < 0) return;
    const auto key = core::SctVariableKey{
        static_cast<core::SctVariableKind>(
            aliasTable_->item(row, 1)->data(Qt::UserRole).toInt()),
        aliasTable_->item(row, 2)->data(Qt::UserRole).toUInt()};
    if (aliasTable_->item(row, 0)->data(Qt::UserRole).toBool()) {
        auto candidate = workspaceAuthoring_;
        std::erase_if(candidate.projectAliases,
            [&](const auto& alias) { return alias.variable == key; });
        const auto previous = workspaceAuthoring_;
        workspaceAuthoring_ = std::move(candidate);
        if (!saveWorkspaceAuthoring()) workspaceAuthoring_ = previous;
    } else if (auto* document = activeDocumentWidget()) {
        (void)documentController_->setVariableAlias(document->locator(), key, std::nullopt);
    }
}

void MainWindow::editSelectedVariableMetadata() {
    const auto row = aliasTable_->currentRow();
    auto* document = activeDocumentWidget();
    if (row < 0 || document == nullptr
        || aliasTable_->item(row, 0)->data(Qt::UserRole).toBool()) return;
    const core::SctVariableKey variable{
        static_cast<core::SctVariableKind>(
            aliasTable_->item(row, 1)->data(Qt::UserRole).toInt()),
        aliasTable_->item(row, 2)->data(Qt::UserRole).toUInt()};
    const core::SctAuthoringTarget target{core::SctAuthoringTargetKind::Variable,
        variable.index, variable.kind};
    std::optional<core::SctEntityAnnotation> current;
    if (const auto state = documentController_->semanticState(document->locator())) {
        const auto found = std::ranges::find(state->annotations, target,
            &core::SctEntityAnnotation::target);
        if (found != state->annotations.end()) current = *found;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Variable Metadata"));
    auto* layout = new QFormLayout(&dialog);
    auto* note = new QPlainTextEdit(&dialog);
    note->setPlainText(current && current->note
        ? QString::fromStdString(*current->note) : QString{});
    auto* bookmark = new QCheckBox(tr("Bookmark this variable"), &dialog);
    bookmark->setChecked(current && current->bookmarkLabel.has_value());
    auto* label = new QLineEdit(&dialog);
    label->setText(current && current->bookmarkLabel
        ? QString::fromStdString(*current->bookmarkLabel) : QString{});
    label->setEnabled(bookmark->isChecked());
    auto color = current ? current->colorRgb : std::optional<std::uint32_t>{};
    auto* colorButton = new QPushButton(color
        ? QStringLiteral("#%1").arg(*color, 6, 16, QLatin1Char('0')).toUpper()
        : tr("Choose color..."), &dialog);
    auto* clearColor = new QPushButton(tr("Clear"), &dialog);
    auto* colorRow = new QHBoxLayout;
    colorRow->addWidget(colorButton); colorRow->addWidget(clearColor);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save
        | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(tr("Note"), note);
    layout->addRow(bookmark);
    layout->addRow(tr("Bookmark label"), label);
    layout->addRow(tr("Row accent"), colorRow);
    layout->addRow(buttons);
    connect(bookmark, &QCheckBox::toggled, label, &QWidget::setEnabled);
    connect(colorButton, &QPushButton::clicked, &dialog, [&] {
        const auto selected = QColorDialog::getColor(color
            ? QColor::fromRgb(*color) : QColor{}, &dialog, tr("Variable row accent"));
        if (!selected.isValid()) return;
        color = selected.rgb() & 0xffffffu;
        colorButton->setText(QStringLiteral("#%1")
            .arg(*color, 6, 16, QLatin1Char('0')).toUpper());
    });
    connect(clearColor, &QPushButton::clicked, &dialog, [&] {
        color.reset(); colorButton->setText(tr("Choose color..."));
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    core::SctEntityAnnotation annotation{target};
    if (!note->toPlainText().isEmpty()) annotation.note = note->toPlainText().toStdString();
    if (bookmark->isChecked()) annotation.bookmarkLabel = label->text().toStdString();
    annotation.colorRgb = color;
    (void)documentController_->setAnnotation(document->locator(), std::move(annotation));
}

void MainWindow::editProjectOpcodeColors() {
    if (!patchWorkspace_) return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Workspace Opcode Colors"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* table = new QTableWidget(0, 2, &dialog);
    table->setHorizontalHeaderLabels({tr("Opcode"), tr("Color")});
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    const auto append = [&](const core::SctOpcodeColor color) {
        const auto row = table->rowCount(); table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(QString::number(color.opcode)));
        table->setItem(row, 1, new QTableWidgetItem(
            QStringLiteral("#%1").arg(color.colorRgb, 6, 16, QLatin1Char('0')).toUpper()));
    };
    for (const auto color : workspaceAuthoring_.opcodeColors) append(color);
    auto* actions = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"), &dialog);
    auto* remove = new QPushButton(tr("Remove"), &dialog);
    actions->addWidget(add); actions->addWidget(remove); actions->addStretch(1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save
        | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(table); layout->addLayout(actions); layout->addWidget(buttons);
    connect(add, &QPushButton::clicked, &dialog, [&, this] {
        bool accepted = false;
        const auto opcode = QInputDialog::getInt(&dialog, tr("Opcode"), tr("Opcode"),
            0, 0, 65535, 1, &accepted);
        if (!accepted || !spice::sct::findSctOpcodeSchema(
                static_cast<std::uint16_t>(opcode))) return;
        const auto color = QColorDialog::getColor({}, &dialog, tr("Opcode color"));
        if (color.isValid()) append({static_cast<std::uint16_t>(opcode),
            color.rgb() & 0xffffffu});
    });
    connect(remove, &QPushButton::clicked, &dialog, [table] {
        if (table->currentRow() >= 0) table->removeRow(table->currentRow());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    std::vector<core::SctOpcodeColor> colors;
    for (int row = 0; row < table->rowCount(); ++row) {
        bool opcodeValid = false, colorValid = false;
        const auto opcode = table->item(row, 0)->text().toUShort(&opcodeValid);
        auto text = table->item(row, 1)->text().trimmed();
        if (text.startsWith(QLatin1Char('#'))) text.removeFirst();
        const auto color = text.toUInt(&colorValid, 16);
        if (!opcodeValid || !colorValid || text.size() != 6) {
            QMessageBox::warning(this, tr("Invalid opcode color"),
                tr("Each row needs a valid opcode and #RRGGBB color.")); return;
        }
        colors.push_back({opcode, color});
    }
    auto candidate = workspaceAuthoring_;
    candidate.opcodeColors = std::move(colors);
    const auto encoded = core::SctAuthoringCatalogCodec::serializeWorkspace(candidate);
    if (!encoded) {
        QMessageBox::warning(this, tr("Invalid opcode colors"),
            QString::fromStdString(encoded.diagnostics().front().message));
        return;
    }
    const auto previous = workspaceAuthoring_;
    workspaceAuthoring_ = std::move(candidate);
    if (!saveWorkspaceAuthoring()) workspaceAuthoring_ = previous;
}

bool MainWindow::flushMetadataEditor() {
    return metadataEditor_ == nullptr || metadataEditor_->flush();
}

bool MainWindow::syncMetadataEditor() {
    if (metadataEditor_ == nullptr) return true;
    auto* document = activeDocumentWidget();
    if (document == nullptr || !document->currentTarget()) {
        metadataEditor_->setEditingEnabled(false);
        return metadataEditor_->setBinding(std::nullopt);
    }
    const auto state = documentController_->semanticState(document->locator());
    if (!state) {
        metadataEditor_->setEditingEnabled(false);
        return metadataEditor_->setBinding(std::nullopt);
    }
    const auto target = *document->currentTarget();
    SctMetadataDraft draft;
    bool supported = false;
    if (target.kind == core::SctNavigationKind::SectionFolder) {
        const auto found = std::ranges::find(state->folders, target.id,
            [](const auto& folder) { return folder.id.value; });
        if (found != state->folders.end()) {
            draft.note = found->note;
            draft.bookmarkLabel = found->bookmarkLabel;
            draft.colorRgb = found->colorRgb;
            supported = true;
        }
    } else {
        std::optional<core::SctAuthoringTargetKind> kind;
        switch (target.kind) {
        case core::SctNavigationKind::Document: kind = core::SctAuthoringTargetKind::Document; break;
        case core::SctNavigationKind::Section: kind = core::SctAuthoringTargetKind::Section; break;
        case core::SctNavigationKind::Instruction: kind = core::SctAuthoringTargetKind::Instruction; break;
        case core::SctNavigationKind::String: kind = core::SctAuthoringTargetKind::String; break;
        case core::SctNavigationKind::SupplementaryText: kind = core::SctAuthoringTargetKind::SupplementaryText; break;
        default: break;
        }
        if (kind) {
            const auto found = std::ranges::find(state->annotations,
                core::SctAuthoringTarget{*kind, target.id},
                &core::SctEntityAnnotation::target);
            if (found != state->annotations.end()) {
                draft.note = found->note;
                draft.bookmarkLabel = found->bookmarkLabel;
                draft.colorRgb = found->colorRgb;
            }
            supported = true;
        }
    }
    if (!supported) {
        metadataEditor_->setEditingEnabled(false);
        return metadataEditor_->setBinding(std::nullopt);
    }
    const bool editable = patchWorkspace_ != nullptr
        && !controller_->busy() && !documentController_->busy()
        && !(exclusiveOperations_ && exclusiveOperations_->active())
        && documentController_->structurallyValid(document->locator());
    metadataEditor_->setEditingEnabled(editable);
    return metadataEditor_->setBinding(SctMetadataBinding{
        document->locator(), target, document->targetLabel(target), std::move(draft)});
}

void MainWindow::reloadAuthoringDocks() {
    if (!aliasTable_ || !bookmarkTable_) return;
    aliasTable_->setRowCount(0);
    const auto activeState = activeDocumentWidget()
        ? documentController_->semanticState(activeDocumentWidget()->locator())
        : std::optional<core::SctSemanticState>{};
    const auto appendAlias = [&](const bool project, const core::SctVariableAlias& alias) {
        const auto row = aliasTable_->rowCount(); aliasTable_->insertRow(row);
        auto* scope = new QTableWidgetItem(project ? tr("Workspace") : tr("Document"));
        scope->setData(Qt::UserRole, project);
        const auto kindName = [&] {
            switch (alias.variable.kind) {
            case core::SctVariableKind::Bit: return tr("Bit");
            case core::SctVariableKind::Byte: return tr("Byte");
            case core::SctVariableKind::Integer: return tr("Integer");
            case core::SctVariableKind::Float: return tr("Float");
            }
            return tr("Unknown");
        }();
        auto* kind = new QTableWidgetItem(kindName);
        kind->setData(Qt::UserRole, static_cast<int>(alias.variable.kind));
        auto* index = new QTableWidgetItem(QString::number(alias.variable.index));
        index->setData(Qt::UserRole, alias.variable.index);
        aliasTable_->setItem(row, 0, scope); aliasTable_->setItem(row, 1, kind);
        aliasTable_->setItem(row, 2, index);
        aliasTable_->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(alias.alias)));
        if (!project && activeState) {
            const core::SctAuthoringTarget target{core::SctAuthoringTargetKind::Variable,
                alias.variable.index, alias.variable.kind};
            const auto annotation = std::ranges::find(activeState->annotations, target,
                &core::SctEntityAnnotation::target);
            if (annotation != activeState->annotations.end()) {
                for (int column = 0; column < aliasTable_->columnCount(); ++column) {
                    if (annotation->colorRgb)
                        aliasTable_->item(row, column)->setBackground(
                            QBrush(QColor::fromRgb(*annotation->colorRgb)));
                    if (annotation->note)
                        aliasTable_->item(row, column)->setToolTip(
                            QString::fromStdString(*annotation->note));
                }
            }
        }
    };
    for (const auto& alias : workspaceAuthoring_.projectAliases) appendAlias(true, alias);
    if (auto* document = activeDocumentWidget()) {
        if (const auto state = documentController_->semanticState(document->locator()))
            for (const auto& alias : state->aliases) appendAlias(false, alias);
    }
    bookmarkTable_->setRowCount(0);
    for (const auto& locator : documentController_->openLocators()) {
        const auto state = documentController_->semanticState(locator);
        if (!state) continue;
        const auto appendBookmark = [&](const core::SctNavigationKind kind,
                                        const std::uint64_t id,
                                        const std::string& label,
                                        const std::optional<core::SctVariableKind> variableKind
                                            = std::nullopt) {
            const auto row = bookmarkTable_->rowCount(); bookmarkTable_->insertRow(row);
            auto* document = new QTableWidgetItem(
                QString::fromStdWString(locator.path().filename().wstring()));
            document->setData(Qt::UserRole, QString::fromStdString(locator.identityKey()));
            auto* entity = new QTableWidgetItem(QString::number(static_cast<int>(kind)));
            entity->setData(Qt::UserRole, static_cast<int>(kind));
            entity->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(id));
            if (variableKind)
                entity->setData(Qt::UserRole + 2, static_cast<int>(*variableKind));
            const auto entityName = [&] {
                switch (kind) {
                case core::SctNavigationKind::Document: return tr("Document");
                case core::SctNavigationKind::Section: return tr("Section");
                case core::SctNavigationKind::Instruction: return tr("Instruction");
                case core::SctNavigationKind::String: return tr("String");
                case core::SctNavigationKind::SupplementaryText: return tr("Supplementary text");
                case core::SctNavigationKind::SectionFolder: return tr("Section folder");
                case core::SctNavigationKind::Variable: return tr("Variable");
                default: return tr("Entity");
                }
            }();
            entity->setText(entityName);
            bookmarkTable_->setItem(row, 0, document);
            bookmarkTable_->setItem(row, 1, entity);
            bookmarkTable_->setItem(row, 2, new QTableWidgetItem(
                label.empty() ? tr("(entity label)") : QString::fromStdString(label)));
        };
        for (const auto& annotation : state->annotations) {
            if (!annotation.bookmarkLabel) continue;
            core::SctNavigationKind kind;
            switch (annotation.target.kind) {
            case core::SctAuthoringTargetKind::Document: kind = core::SctNavigationKind::Document; break;
            case core::SctAuthoringTargetKind::Section: kind = core::SctNavigationKind::Section; break;
            case core::SctAuthoringTargetKind::Instruction: kind = core::SctNavigationKind::Instruction; break;
            case core::SctAuthoringTargetKind::String: kind = core::SctNavigationKind::String; break;
            case core::SctAuthoringTargetKind::SupplementaryText: kind = core::SctNavigationKind::SupplementaryText; break;
            case core::SctAuthoringTargetKind::Variable:
                if (!annotation.target.variableKind) continue;
                appendBookmark(core::SctNavigationKind::Variable,
                    annotation.target.id, *annotation.bookmarkLabel,
                    annotation.target.variableKind);
                continue;
            }
            appendBookmark(kind, annotation.target.id, *annotation.bookmarkLabel);
        }
        for (const auto& folder : state->folders)
            if (folder.bookmarkLabel)
                appendBookmark(core::SctNavigationKind::SectionFolder,
                    folder.id.value, *folder.bookmarkLabel);
    }
}

bool MainWindow::openPatchWorkspace(
    const QString& workspaceRoot, const bool allowConfirmation) {
    const auto* dataset = controller_->dataset();
    if (dataset == nullptr || !documentController_->openLocators().empty()) return false;
    const auto path = std::filesystem::path(workspaceRoot.toStdWString());
    auto assessment = core::LocalSalsaWorkspace::assess(path, *dataset);
    if (!assessment) {
        const auto message = assessment.diagnostics().empty()
            ? tr("The SALSA workspace could not be assessed.")
            : QString::fromStdString(assessment.diagnostics().front().message);
        if (allowConfirmation)
            QMessageBox::warning(this, tr("Workspace could not be opened"), message);
        else statusBar()->showMessage(message, 10000);
        return false;
    }
    auto acceptance = core::WorkspaceDatasetAcceptance::ExactOnly;
    if (assessment.value().requiresReassociation()) {
        if (!allowConfirmation) {
            statusBar()->showMessage(tr(
                "The associated workspace needs explicit dataset reassociation; the dataset remains open."),
                12000);
            return false;
        }
        const auto previous = assessment.value().storedDatasetRoot
            ? QString::fromStdWString(assessment.value().storedDatasetRoot->wstring())
            : tr("Unknown");
        const auto current = QString::fromStdWString(dataset->root.wstring());
        const auto answer = QMessageBox::warning(this, tr("Reassociate workspace?"),
            tr("%1\n\nPrevious dataset:\n%2\n\nSelected dataset:\n%3\n\n"
               "Reassociation updates the workspace binding. Individual SCT patches still "
               "require their recorded source revision to match.")
                .arg(QString::fromStdString(assessment.value().message),
                    QDir::toNativeSeparators(previous),
                    QDir::toNativeSeparators(current)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return false;
        acceptance = core::WorkspaceDatasetAcceptance::UserConfirmedReassociation;
    }
    saveWorkspaceSession();
    auto opened = core::LocalSalsaWorkspace::openOrCreate(path, *dataset, acceptance);
    if (!opened) {
        const auto message = opened.diagnostics().empty()
            ? tr("The SALSA workspace could not be opened.")
            : QString::fromStdString(opened.diagnostics().front().message);
        if (allowConfirmation)
            QMessageBox::warning(this, tr("Workspace could not be opened"), message);
        else statusBar()->showMessage(message, 10000);
        return false;
    }
    detachPatchWorkspace(false);
    patchWorkspace_ = std::make_shared<const core::LocalSalsaWorkspace>(
        std::move(opened).takeValue());
    documentController_->setWorkspace(patchWorkspace_);
    loadWorkspaceAuthoring();
    reloadSnippets();
    rememberPatchWorkspaceAssociation(
        QString::fromStdWString(dataset->root.wstring()), workspaceRoot);
    statusBar()->showMessage(tr("SALSA workspace opened: %1")
        .arg(QDir::toNativeSeparators(workspaceRoot)), 8000);
    syncActions();
    restoreWorkspaceSession();
    return true;
}

void MainWindow::disconnectPatchWorkspace() {
    if (!documentController_->openLocators().empty()) return;
    detachPatchWorkspace(true);
    statusBar()->showMessage(tr("SALSA workspace disconnected."), 5000);
    syncActions();
}

void MainWindow::detachPatchWorkspace(const bool saveSession) {
    if (saveSession) saveWorkspaceSession();
    if (workspaceSessionSaveTimer_) workspaceSessionSaveTimer_->stop();
    restoringWorkspaceSession_ = false;
    restoringWorkspaceSessionState_.reset();
    restoringWorkspaceDocumentIndex_ = 0;
    workspaceRestoreMessages_.clear();
    documentController_->setWorkspace(nullptr);
    patchWorkspace_.reset();
    workspaceAuthoring_ = {};
    reloadAuthoringDocks();
    reloadSnippets();
}

void MainWindow::restorePatchWorkspaceAssociation() {
    documentController_->setWorkspace(nullptr);
    patchWorkspace_.reset();
    workspaceAuthoring_ = {};
    reloadAuthoringDocks();
    reloadSnippets();
    const auto* dataset = controller_->dataset();
    if (dataset == nullptr) return;
    const auto datasetRoot = normalizedRecentDatasetPath(
        QString::fromStdWString(dataset->root.wstring()));
    for (qsizetype index = 0; index + 1 < patchWorkspaceAssociations_.size(); index += 2) {
        if (!sameDatasetPath(patchWorkspaceAssociations_[index], datasetRoot)) continue;
        const auto workspaceRoot = patchWorkspaceAssociations_[index + 1];
        const QDir workspaceDirectory(workspaceRoot);
        const bool hasManifest = QFileInfo(workspaceDirectory.filePath(
            QStringLiteral("project.json"))).isFile();
        const bool hasRollback = QFileInfo(workspaceDirectory.filePath(
            QStringLiteral("project.json.schema1.rollback"))).isFile();
        if (!workspaceDirectory.exists()
            || (!hasManifest && !hasRollback)) {
            statusBar()->showMessage(tr(
                "The last associated SALSA workspace is missing; the dataset remains open."),
                10000);
            return;
        }
        (void)openPatchWorkspace(workspaceRoot, false);
        return;
    }
}

void MainWindow::rememberPatchWorkspaceAssociation(
    const QString& datasetRoot, const QString& workspaceRoot) {
    const auto normalizedDataset = normalizedRecentDatasetPath(datasetRoot);
    const auto normalizedWorkspace = normalizedRecentDatasetPath(workspaceRoot);
    for (qsizetype index = patchWorkspaceAssociations_.size() - 2; index >= 0; index -= 2) {
        if (sameDatasetPath(patchWorkspaceAssociations_[index], normalizedDataset)
            && sameDatasetPath(patchWorkspaceAssociations_[index + 1], normalizedWorkspace)) {
            patchWorkspaceAssociations_.removeAt(index + 1);
            patchWorkspaceAssociations_.removeAt(index);
        }
        if (index < 2) break;
    }
    patchWorkspaceAssociations_.prepend(normalizedWorkspace);
    patchWorkspaceAssociations_.prepend(normalizedDataset);
    QSettings{}.setValue(QStringLiteral("workspace/patchAssociations"),
        patchWorkspaceAssociations_);
}

void MainWindow::saveActiveDocument() {
    if (!flushPendingEditors()) return;
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    if (documentController_->saveDocument(widget->locator())) {
        statusBar()->showMessage(tr("Saving SCT patch checkpoint..."));
        progressBar_->setRange(0, 0);
        progressBar_->show();
        cancelButton_->show();
        rebuildDocumentTabTitles();
        syncActions();
    }
}

void MainWindow::exportActiveDocument() {
    if (exclusiveOperations_->active()) { exclusiveOperations_->focusActive(); return; }
    auto* widget = activeDocumentWidget();
    if (widget == nullptr || !prepareScptEditor(widget->locator())
        || !flushPendingEditors()) return;
    const auto project = controller_->projectSnapshot();
    const auto snapshot = documentController_->snapshot(widget->locator());
    if (!project || !snapshot || !snapshot->provenance) return;

    const auto sourcePath = project->dataset().root / widget->locator().path();
    const auto defaults = core::SctPublicationService::defaultsFor(
        project->dataset(), *snapshot);
    lastExportDirectory_ = QSettings{}.value(
        QStringLiteral("publication/lastDirectory"), lastExportDirectory_).toString();
    (void)exclusiveOperations_->open(std::make_unique<SctPublicationController>(
        documentController_, *project, widget->locator(),
        documentController_->workingRevision(widget->locator()), defaults,
        sourcePath, lastExportDirectory_), this);
}

void MainWindow::applyActionAvailability(
    QAction* action, ActionAvailability availability) {
    if (action == nullptr) return;
    action->setEnabled(availability.enabled);
    DisabledActionHintPresenter::setReason(
        action, availability.enabled ? QString{} : std::move(availability.reason));
}

void MainWindow::syncDatasetOverview() {
    if (details_ == nullptr) return;
    if (patchWorkspace_) {
        details_->setWorkspace(
            QString::fromStdWString(patchWorkspace_->descriptor().root.wstring()),
            QString::fromStdString(patchWorkspace_->descriptor().workspaceId));
    } else {
        details_->clearWorkspace();
    }
    const auto open = documentController_->openLocators();
    const auto dirty = documentController_->dirtyLocators();
    const auto conflicts = std::ranges::count_if(open, [this](const auto& locator) {
        return documentController_->patchConflict(locator);
    });
    details_->setSessionCounts(open.size(), dirty.size(),
        static_cast<std::size_t>(conflicts));

    const bool exclusive = exclusiveOperations_ && exclusiveOperations_->active();
    const bool shellAvailable = !exclusive && !controller_->busy()
        && !documentController_->busy() && !documentController_->isPublishing();
    const bool hasDataset = controller_->hasDataset();
    const bool workspaceAvailable = shellAvailable && hasDataset && open.empty();
    QString workspaceReason;
    if (!shellAvailable) workspaceReason = tr("Wait for the current operation to finish.");
    else if (!hasDataset) workspaceReason = tr("Open a dataset first.");
    else if (!open.empty()) workspaceReason = tr(
        "Close all SCT documents before opening or creating a workspace.");
    const bool hasSelection = controller_->selectedLocator().has_value();
    const bool assetAvailable = shellAvailable && hasSelection;
    const auto assetReason = hasSelection && !assetAvailable
        ? tr("Wait for the current operation to finish.") : QString{};
    details_->setPrimaryActionAvailability(shellAvailable, workspaceAvailable,
        workspaceReason, assetAvailable, assetReason);
}

void MainWindow::syncWindowTitle() {
    QString documentName;
    bool modified = false;
    if (const auto* widget = activeDocumentWidget()) {
        documentName = QString::fromStdWString(widget->locator().path().filename().wstring());
        modified = documentController_->isDirty(widget->locator());
    }
    QString datasetName;
    if (const auto* dataset = controller_->dataset()) {
        datasetName = QFileInfo(QString::fromStdWString(dataset->root.wstring())).fileName();
        if (datasetName.isEmpty()) datasetName = QString::fromStdWString(dataset->root.wstring());
    }
    const auto applicationName = core::applicationName();
    const auto datasetUtf8 = datasetName.toUtf8();
    const auto documentUtf8 = documentName.toUtf8();
    const auto title = core::composeShellWindowTitle(applicationName,
        {datasetUtf8.constData(), static_cast<std::size_t>(datasetUtf8.size())},
        {documentUtf8.constData(), static_cast<std::size_t>(documentUtf8.size())}, modified);
    setWindowTitle(QString::fromUtf8(title.data(), static_cast<qsizetype>(title.size())));
}

void MainWindow::showHelpTopic(const QString& topic) {
    if (helpWindow_ == nullptr) helpWindow_ = new HelpWindow(this);
    helpWindow_->showTopic(topic);
    helpWindow_->show();
    helpWindow_->raise();
    helpWindow_->activateWindow();
}

void MainWindow::resetWindowLayout() {
    const auto answer = QMessageBox::question(this, tr("Reset window layout?"), tr(
        "Restore the default panels and editing toolbar? The window size and position, "
        "open dataset, and documents will not change."));
    if (answer != QMessageBox::Yes) return;
    restoreState(defaultWindowState_, SettingsStateVersion);
    statusBar()->showMessage(tr("Window panels and toolbars were reset."), 5000);
}

void MainWindow::syncWorkspace() {
    const auto* dataset = controller_->dataset();
    const auto* catalog = controller_->catalog();
    if (dataset == nullptr || catalog == nullptr) {
        workspaceModel_->clear();
        details_->clearDataset();
        projectTree_->setEnabled(false);
    } else {
        workspaceModel_->setSnapshot(*catalog);
        details_->setDataset(*dataset, *catalog);
        projectTree_->setEnabled(true);
        projectTree_->collapseAll();
        if (controller_->selectedLocator().has_value()) {
            const auto index = workspaceModel_->indexForLocator(*controller_->selectedLocator());
            if (index.isValid()) {
                projectTree_->setCurrentIndex(index);
            }
        }
    }
    syncSelection();
    syncActions();
}

void MainWindow::syncSelection() {
    details_->setSelectedAsset(controller_->selectedAsset());
    if (!controller_->selectedLocator().has_value()) {
        projectTree_->clearSelection();
        projectTree_->setCurrentIndex({});
    }
}

void MainWindow::syncDiagnostics() {
    auto workspaceDiagnostics = controller_->diagnostics();
    std::vector<core::SctPipelineDiagnostic> documentDiagnostics;
    if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->currentWidget())) {
        auto current = documentController_->currentDiagnostics(widget->locator());
        documentDiagnostics.insert(documentDiagnostics.end(),
            std::make_move_iterator(current.begin()),
            std::make_move_iterator(current.end()));
    }
    diagnosticsModel_->setCombinedDiagnostics(workspaceDiagnostics, documentDiagnostics);

    std::vector<core::SctPipelineDiagnostic> globalDocumentDiagnostics;
    for (const auto& locator : documentController_->openLocators()) {
        auto current = documentController_->currentDiagnostics(locator);
        globalDocumentDiagnostics.insert(globalDocumentDiagnostics.end(),
            std::make_move_iterator(current.begin()),
            std::make_move_iterator(current.end()));
    }
    activityLogModel_->observeIssues(DiagnosticsModel::rowsFor(
        workspaceDiagnostics, globalDocumentDiagnostics));
}

void MainWindow::queueDiagnosticsSync() {
    if (diagnosticsSyncPending_) return;
    diagnosticsSyncPending_ = true;
    QTimer::singleShot(0, this, [this] {
        diagnosticsSyncPending_ = false;
        QElapsedTimer timer;
        timer.start();
        syncDiagnostics();
        if (editTimingsEnabled_) {
            qCInfo(salsaSctEditLog).noquote() << QStringLiteral(
                "SALSA edit timing: diagnostics-delivery=%1us")
                .arg(timer.nsecsElapsed() / 1000);
        }
    });
}

void MainWindow::showInteractionNotice(InteractionNotice notice) {
    if (pendingInteractionPosition_ && !notice.globalPosition)
        notice.globalPosition = pendingInteractionPosition_;
    if (notice.prominent) {
        activityLogModel_->appendOperation(
            notice.code == QStringLiteral("BackgroundVerificationRestored")
                ? ActivityOutcome::Completed : ActivityOutcome::Failed,
            QStringLiteral("EditVerification"), notice.message,
            notice.documentIdentity);
    }
    if (notice.globalPosition) {
        noticePresenter_->showNotice(notice);
        return;
    }
    auto* widget = activeDocumentWidget();
    if (widget == nullptr
        || QString::fromStdString(widget->locator().identityKey())
            != notice.documentIdentity) {
        noticePresenter_->showNotice(notice);
        return;
    }
    const QPointer<SctDocumentWidget> guarded(widget);
    if (notice.target) {
        const auto target = *notice.target;
        noticePresenter_->showNotice(notice, [guarded, target] {
            return guarded ? guarded->globalRectForTarget(target) : QRect{};
        });
    } else {
        noticePresenter_->showNotice(notice, [guarded] {
            return guarded ? QRect(guarded->mapToGlobal(QPoint{}), guarded->size()) : QRect{};
        });
    }
}

void MainWindow::syncSemanticNavigator() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) {
        semanticNavigator_->clear();
        return;
    }
    const auto snapshot = documentController_->snapshot(widget->locator());
    if (snapshot == nullptr || snapshot->document == nullptr) {
        semanticNavigator_->clear();
        return;
    }
    semanticNavigator_->setDocument(widget->locator(), *snapshot);
}

void MainWindow::syncActions() {
    const bool busy = controller_->busy() || documentController_->busy();
    const bool publishing = documentController_->isPublishing();
    const bool exclusive = exclusiveOperations_ && exclusiveOperations_->active();
    const bool hasDataset = controller_->hasDataset();
    const bool hasOpenDocuments = !documentController_->openLocators().empty();
    const core::ShellPresentationState shellState{
        .operationActive = busy || publishing || exclusive,
        .datasetLoaded = hasDataset,
        .workspaceAssociated = patchWorkspace_ != nullptr,
        .documentsOpen = hasOpenDocuments,
        .recentDatasetsAvailable = !recentDatasets_.isEmpty(),
    };
    const auto applyShell = [this, &shellState](QAction* action,
            const core::ShellAction shellAction) {
        const auto reason = core::shellActionBlockReason(shellAction, shellState);
        applyActionAvailability(action, {
            reason == core::ShellActionBlockReason::None,
            shellBlockReasonText(reason)});
    };
    applyShell(openAction_, core::ShellAction::OpenDataset);
    applyShell(recentMenu_->menuAction(), core::ShellAction::OpenRecentDataset);
    recentMenu_->setEnabled(recentMenu_->menuAction()->isEnabled());
    applyShell(convertLegacyProjectAction_, core::ShellAction::ImportLegacyProject);
    applyShell(closeWorkspaceAction_, core::ShellAction::CloseDataset);
    applyShell(associatePatchWorkspaceAction_, core::ShellAction::AssociateWorkspace);
    applyShell(disconnectPatchWorkspaceAction_, core::ShellAction::CloseWorkspace);
    applyShell(rebasePatchesAction_, core::ShellAction::RebasePatches);
    applyShell(cleanWorkspaceEvidenceAction_, core::ShellAction::CleanWorkspaceEvidence);
    applyShell(promoteLegacyMetadataAction_, core::ShellAction::PromoteLegacyMetadata);
    applyShell(editInstructionCatalogAction_, core::ShellAction::EditPersonalCatalog);
    applyShell(editProjectOpcodeColorsAction_, core::ShellAction::EditWorkspaceAppearance);
    applyShell(refreshAction_, core::ShellAction::RefreshDataset);
    projectTree_->setEnabled(hasDataset && !busy && !publishing && !exclusive);
    messageEditor_->setEnabled(!busy && !exclusive);
    syncEditActions();
    if (mode_ == Mode::IsolatedDocumentEditor) {
        const auto isolated = tr("This command is unavailable in the isolated document editor.");
        for (auto* action : {openAction_, recentMenu_->menuAction(),
                convertLegacyProjectAction_, closeWorkspaceAction_,
                associatePatchWorkspaceAction_, disconnectPatchWorkspaceAction_,
                rebasePatchesAction_, cleanWorkspaceEvidenceAction_,
                promoteLegacyMetadataAction_, refreshAction_, saveAction_, exportAction_})
            applyActionAvailability(action, {false, isolated});
        recentMenu_->setEnabled(false);
        projectTree_->setEnabled(false);
    }
    syncDatasetOverview();
    syncWindowTitle();
}

SctDocumentWidget* MainWindow::activeDocumentWidget() const {
    return qobject_cast<SctDocumentWidget*>(tabs_->currentWidget());
}

void MainWindow::recordActiveNavigation() {
    if (replayingNavigation_) return;
    if (tabs_->currentIndex() == 0) {
        recordNavigation({});
    } else if (auto* widget = activeDocumentWidget(); widget != nullptr) {
        if (const auto target = widget->currentTarget())
            recordNavigation({widget->locator(), *target});
    }
}

void MainWindow::recordNavigation(NavigationEntry entry) {
    if (replayingNavigation_ || !navigationEntryAvailable(entry)) return;
    pruneNavigationHistory();
    if (!navigationHistory_.empty()
        && navigationHistory_[navigationHistoryIndex_] == entry) {
        syncNavigationActions();
        return;
    }
    if (!navigationHistory_.empty()
        && navigationHistoryIndex_ + 1u < navigationHistory_.size()) {
        navigationHistory_.erase(
            navigationHistory_.begin() + static_cast<std::ptrdiff_t>(
                navigationHistoryIndex_ + 1u), navigationHistory_.end());
    }
    navigationHistory_.push_back(std::move(entry));
    navigationHistoryIndex_ = navigationHistory_.size() - 1u;
    syncNavigationActions();
    scheduleWorkspaceSessionSave();
}

bool MainWindow::navigationEntryAvailable(const NavigationEntry& entry) const {
    if (!entry.locator) return !entry.target;
    for (int index = 1; index < tabs_->count(); ++index) {
        const auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index));
        if (widget != nullptr && widget->locator() == *entry.locator)
            return entry.target && widget->containsTarget(*entry.target);
    }
    return false;
}

void MainWindow::pruneNavigationHistory() {
    if (navigationHistory_.empty()) {
        syncNavigationActions();
        return;
    }
    std::vector<NavigationEntry> retained;
    retained.reserve(navigationHistory_.size());
    std::size_t retainedThroughCurrent = 0;
    for (std::size_t index = 0; index < navigationHistory_.size(); ++index) {
        if (!navigationEntryAvailable(navigationHistory_[index])) continue;
        retained.push_back(navigationHistory_[index]);
        if (index <= navigationHistoryIndex_) retainedThroughCurrent = retained.size();
    }
    navigationHistory_ = std::move(retained);
    if (navigationHistory_.empty()) navigationHistoryIndex_ = 0;
    else navigationHistoryIndex_ = retainedThroughCurrent == 0 ? 0
        : std::min(retainedThroughCurrent - 1u, navigationHistory_.size() - 1u);
    syncNavigationActions();
}

bool MainWindow::navigateTo(const NavigationEntry& entry) {
    if (!navigationEntryAvailable(entry)) return false;
    replayingNavigation_ = true;
    if (!entry.locator) {
        tabs_->setCurrentIndex(0);
    } else {
        focusDocument(QString::fromStdString(entry.locator->identityKey()));
        auto* widget = activeDocumentWidget();
        if (widget == nullptr || widget->locator() != *entry.locator
            || !entry.target || !widget->selectLocation(*entry.target)) {
            replayingNavigation_ = false;
            return false;
        }
    }
    replayingNavigation_ = false;
    syncNavigationActions();
    return true;
}

void MainWindow::navigateBack() {
    pruneNavigationHistory();
    while (!navigationHistory_.empty() && navigationHistoryIndex_ > 0u) {
        --navigationHistoryIndex_;
        if (navigateTo(navigationHistory_[navigationHistoryIndex_])) {
            scheduleWorkspaceSessionSave();
            return;
        }
    }
    syncNavigationActions();
}

void MainWindow::navigateForward() {
    pruneNavigationHistory();
    while (!navigationHistory_.empty()
        && navigationHistoryIndex_ + 1u < navigationHistory_.size()) {
        ++navigationHistoryIndex_;
        if (navigateTo(navigationHistory_[navigationHistoryIndex_])) {
            scheduleWorkspaceSessionSave();
            return;
        }
    }
    syncNavigationActions();
}

QString MainWindow::navigationEntryLabel(const NavigationEntry& entry) const {
    if (!entry.locator) return tr("Dataset Overview");
    auto label = QString::fromStdWString(entry.locator->path().filename().wstring());
    if (entry.target) {
        for (int index = 1; index < tabs_->count(); ++index) {
            const auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index));
            if (widget == nullptr || widget->locator() != *entry.locator) continue;
            const auto target = widget->targetLabel(*entry.target);
            label += target.isEmpty() ? tr(" — item %1").arg(entry.target->id)
                                      : QStringLiteral(" — ") + target;
            break;
        }
    }
    return label;
}

void MainWindow::syncNavigationActions() {
    if (navigationBackAction_ == nullptr || navigationForwardAction_ == nullptr) return;
    const bool canBack = !navigationHistory_.empty() && navigationHistoryIndex_ > 0u;
    const bool canForward = !navigationHistory_.empty()
        && navigationHistoryIndex_ + 1u < navigationHistory_.size();
    const bool exclusive = exclusiveOperations_ && exclusiveOperations_->active();
    applyActionAvailability(navigationBackAction_, {canBack && !exclusive,
        exclusive ? tr("Wait for the current operation to finish.")
                  : tr("There is no earlier visited location.")});
    applyActionAvailability(navigationForwardAction_, {canForward && !exclusive,
        exclusive ? tr("Wait for the current operation to finish.")
                  : tr("There is no later visited location.")});
    navigationBackAction_->setToolTip(canBack
        ? tr("Back to %1 (Alt+Left)").arg(
            navigationEntryLabel(navigationHistory_[navigationHistoryIndex_ - 1u]))
        : tr("Back (Alt+Left)"));
    navigationForwardAction_->setToolTip(canForward
        ? tr("Forward to %1 (Alt+Right)").arg(
            navigationEntryLabel(navigationHistory_[navigationHistoryIndex_ + 1u]))
        : tr("Forward (Alt+Right)"));
}

void MainWindow::syncEditActions() {
    auto* widget = activeDocumentWidget();
    const bool exclusive = exclusiveOperations_ && exclusiveOperations_->active();
    const bool available = widget != nullptr
        && !controller_->busy() && !documentController_->busy() && !exclusive;
    const bool editable = available
        && documentController_->structurallyValid(widget->locator());
    const auto unavailableReason = exclusive || controller_->busy()
            || documentController_->busy() || documentController_->isPublishing()
        ? tr("Wait for the current operation to finish.")
        : widget == nullptr ? tr("Open an SCT document first.")
        : !documentController_->structurallyValid(widget->locator())
            ? tr("Resolve the current document errors before editing.") : QString{};
    const bool canSave = available && patchWorkspace_ != nullptr
        && documentController_->isDirty(widget->locator())
        && !documentController_->isSaving(widget->locator())
        && !documentController_->patchConflict(widget->locator())
        && !documentController_->isPublishing()
        && documentController_->sourceStatus(widget->locator())
            == SctDocumentController::SourceStatus::Current;
    QString saveReason = unavailableReason;
    if (saveReason.isEmpty() && patchWorkspace_ == nullptr)
        saveReason = tr("Open or create a workspace to save patch checkpoints.");
    else if (saveReason.isEmpty() && !documentController_->isDirty(widget->locator()))
        saveReason = tr("The active document has no changes to checkpoint.");
    else if (saveReason.isEmpty() && documentController_->isSaving(widget->locator()))
        saveReason = tr("A patch checkpoint is already being saved.");
    else if (saveReason.isEmpty() && documentController_->patchConflict(widget->locator()))
        saveReason = tr("Rebase the document's patch conflict before saving.");
    else if (saveReason.isEmpty() && documentController_->sourceStatus(widget->locator())
            != SctDocumentController::SourceStatus::Current)
        saveReason = tr("Refresh and reconcile the changed source before saving.");
    applyActionAvailability(saveAction_, {canSave, saveReason});
    const bool canExport = editable && !documentController_->isPublishing()
        && !documentController_->isSaving(widget->locator());
    applyActionAvailability(exportAction_, {canExport, unavailableReason.isEmpty()
        ? tr("Wait for the active document operation to finish.") : unavailableReason});
    for (int i = 1; i < tabs_->count(); ++i) {
        if (auto* document = qobject_cast<SctDocumentWidget*>(tabs_->widget(i))) {
            document->setEditingEnabled(!controller_->busy() && !documentController_->busy()
                && !exclusive
                && documentController_->structurallyValid(document->locator()));
        }
    }

    const auto undoDescription = available
        ? documentController_->undoDescription(widget->locator()) : std::nullopt;
    const auto redoDescription = available
        ? documentController_->redoDescription(widget->locator()) : std::nullopt;
    const bool canUndo = available && documentController_->canUndo(widget->locator());
    const bool canRedo = available && documentController_->canRedo(widget->locator());
    applyActionAvailability(undoAction_, {canUndo,
        unavailableReason.isEmpty() ? tr("There is nothing to undo.") : unavailableReason});
    applyActionAvailability(redoAction_, {canRedo,
        unavailableReason.isEmpty() ? tr("There is nothing to redo.") : unavailableReason});
    undoAction_->setText(tr("&Undo"));
    redoAction_->setText(tr("&Redo"));
    undoAction_->setToolTip(undoDescription.has_value()
        ? tr("Undo %1").arg(QString::fromStdString(*undoDescription)) : tr("Undo"));
    redoAction_->setToolTip(redoDescription.has_value()
        ? tr("Redo %1").arg(QString::fromStdString(*redoDescription)) : tr("Redo"));
    applyActionAvailability(editMessageAction_, {
        editable && widget->canEditSelectedMessage(), unavailableReason.isEmpty()
            ? tr("Select an editable text entity first.") : unavailableReason});
    const bool fragmentShortcutContext = !hasTextEditingFocus();
    const bool fragmentSelection = editable && fragmentShortcutContext
        && (!widget->selectedInstructions().empty()
            || !widget->selectedSections().empty());
    const auto selectionReason = unavailableReason.isEmpty()
        ? fragmentShortcutContext ? tr("Select one or more instructions or sections first.")
            : tr("Finish editing the active text field first.") : unavailableReason;
    applyActionAvailability(cutAction_, {fragmentSelection, selectionReason});
    applyActionAvailability(copyAction_, {fragmentSelection, selectionReason});
    applyActionAvailability(duplicateAction_, {fragmentSelection, selectionReason});
    applyActionAvailability(saveSnippetAction_, {fragmentSelection, selectionReason});
    const auto fragmentMime = QString::fromLatin1(
        core::SctFragmentCodec::MimeType.data(),
        static_cast<qsizetype>(core::SctFragmentCodec::MimeType.size()));
    const auto* clipboardData = QApplication::clipboard()->mimeData();
    const bool canPaste = editable && fragmentShortcutContext
        && clipboardData != nullptr
        && clipboardData->hasFormat(fragmentMime);
    applyActionAvailability(pasteAction_, {canPaste, unavailableReason.isEmpty()
        ? !fragmentShortcutContext ? tr("Finish editing the active text field first.")
            : tr("The clipboard does not contain SALSA instructions or sections.")
        : unavailableReason});
    if (snippetList_ != nullptr) {
        const bool snippetSelected = snippetList_->currentRow() >= 0;
        pasteSnippetButton_->setEnabled(editable && snippetSelected);
        deleteSnippetButton_->setEnabled(!exclusive && snippetSelected);
    }
    applyActionAvailability(createScriptSectionAction_, {editable, unavailableReason});
    applyActionAvailability(createIndexedStringAction_, {editable, unavailableReason});
    const bool sectionSelected = editable && widget->selectedSection().has_value();
    const auto sectionReason = unavailableReason.isEmpty()
        ? tr("Select a script section first.") : unavailableReason;
    for (auto* action : {renameSectionAction_, deleteSectionAction_,
            moveSectionUpAction_, moveSectionDownAction_})
        applyActionAvailability(action, {sectionSelected, sectionReason});
    applyActionAvailability(createSupplementaryTextMessageAction_, {editable, unavailableReason});
    applyActionAvailability(deleteTextAction_, {
        editable && widget->selectedTextTarget().has_value(), unavailableReason.isEmpty()
            ? tr("Select a text entity first.") : unavailableReason});
    applyActionAvailability(insertInstructionAction_, {
        editable && widget->insertionContext().has_value(), unavailableReason.isEmpty()
            ? tr("Select an instruction or section insertion point first.") : unavailableReason});
    applyActionAvailability(deleteInstructionAction_, {
        editable && widget->canDeleteSelected(), unavailableReason.isEmpty()
            ? tr("The current selection cannot be deleted.") : unavailableReason});
    applyActionAvailability(moveInstructionUpAction_, {editable
        && widget->canMoveSelected(core::SctInstructionMoveDirection::Up),
        unavailableReason.isEmpty() ? tr("The current instruction cannot move up.")
                                    : unavailableReason});
    applyActionAvailability(moveInstructionDownAction_, {editable
        && widget->canMoveSelected(core::SctInstructionMoveDirection::Down),
        unavailableReason.isEmpty() ? tr("The current instruction cannot move down.")
                                    : unavailableReason});
    syncNavigationActions();
    syncDatasetOverview();
    syncWindowTitle();
}

std::optional<core::SctInstructionAuthoringDraft>
MainWindow::chooseInstructionDraft(
    const core::AssetLocator& locator, const bool allowReturn) {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Instruction"));
    dialog.resize(460, 520);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Choose an opcode. Required parameters are configured next."), &dialog));
    auto* filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(tr("Filter by opcode or mnemonic"));
    layout->addWidget(filter);
    auto* list = new QListWidget(&dialog);
    int firstEnabled = -1;
    for (const auto& schema : spice::sct::sctOpcodeSchemas()) {
        if (schema.opcode == 9u
            || schema.documentRole == spice::sct::SctOpcodeDocumentRole::FoldedModifier)
            continue;
        const auto resolved = core::SctCatalogResolver::resolve(schema.opcode);
        const auto mnemonic = resolved.mnemonic.empty()
            ? tr("Opcode %1").arg(schema.opcode)
            : QString::fromStdString(resolved.mnemonic);
        auto* item = new QListWidgetItem(
            QStringLiteral("%1  %2").arg(schema.opcode, 3, 10, QLatin1Char('0'))
                .arg(mnemonic), list);
        item->setData(Qt::UserRole, schema.opcode);
        const auto draft = documentController_->createInstructionDraft(locator, schema.opcode);
        if ((schema.opcode == 12u && !allowReturn) || !draft.draft) {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            item->setToolTip(schema.opcode == 12u && !allowReturn
                ? tr("Return can only be inserted at the end of a section.")
                : draft.diagnostics.empty()
                    ? tr("The frozen schema cannot safely construct this instruction.")
                    : QString::fromStdString(draft.diagnostics.front().message));
        } else if (firstEnabled < 0) {
            firstEnabled = list->count() - 1;
        }
    }
    layout->addWidget(list, 1);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);
    connect(filter, &QLineEdit::textChanged, &dialog, [list](const QString& text) {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setHidden(!list->item(i)->text().contains(text, Qt::CaseInsensitive));
    });
    connect(list, &QListWidget::currentItemChanged, &dialog,
        [buttons](QListWidgetItem* current) {
            buttons->button(QDialogButtonBox::Ok)->setEnabled(current != nullptr
                && !current->isHidden() && (current->flags() & Qt::ItemIsEnabled));
        });
    connect(list, &QListWidget::itemDoubleClicked, &dialog,
        [&dialog](QListWidgetItem* item) {
            if (item != nullptr && (item->flags() & Qt::ItemIsEnabled))
                dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (firstEnabled >= 0) list->setCurrentRow(firstEnabled);
    filter->setFocus();
    if (dialog.exec() != QDialog::Accepted || list->currentItem() == nullptr
        || list->currentItem()->isHidden()
        || !(list->currentItem()->flags() & Qt::ItemIsEnabled)) return std::nullopt;
    const auto opcode = static_cast<std::uint16_t>(
        list->currentItem()->data(Qt::UserRole).toUInt());
    auto created = documentController_->createInstructionDraft(locator, opcode);
    if (!created.draft) {
        const auto message = created.diagnostics.empty()
            ? tr("This opcode cannot be authored safely.")
            : QString::fromStdString(created.diagnostics.front().message);
        QMessageBox::warning(this, tr("New Instruction"), message);
        return std::nullopt;
    }
    auto draft = std::move(*created.draft);

    struct DraftEditorRow final {
        std::size_t parameterIndex = 0;
        std::optional<std::size_t> ownedTextIndex{};
        QLineEdit* line = nullptr;
        QComboBox* choices = nullptr;
        QCheckBox* acceptSuggestion = nullptr;
        std::vector<core::SctReferenceCandidate> candidates{};
        bool newSupplementaryTextMessageChoice = false;
        QString initialText{};
    };

    QDialog editor(this);
    const auto selectedEntry = core::SctCatalogResolver::resolve(opcode);
    const auto selectedName = selectedEntry.mnemonic.empty()
        ? tr("Opcode %1").arg(opcode)
        : QString::fromStdString(selectedEntry.mnemonic);
    editor.setWindowTitle(tr("Configure %1").arg(selectedName));
    editor.resize(820, 480);
    auto* editorLayout = new QVBoxLayout(&editor);
    auto* help = new QLabel(tr(
        "Resolve every required value. Suggested values must be explicitly accepted or replaced."),
        &editor);
    help->setWordWrap(true);
    editorLayout->addWidget(help);
    auto* table = new QTableWidget(
        static_cast<int>(draft.instruction.parameters.size()), 3, &editor);
    table->setHorizontalHeaderLabels({tr("Parameter"), tr("Value"), tr("Notes")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->resizeSection(0, 210);
    table->horizontalHeader()->resizeSection(1, 360);
    table->horizontalHeader()->resizeSection(2, 210);
    table->verticalHeader()->hide();
    std::vector<DraftEditorRow> rows;
    rows.reserve(draft.instruction.parameters.size());
    const auto* opcodeSchema = spice::sct::findSctOpcodeSchema(opcode);
    for (std::size_t i = 0; i < draft.instruction.parameters.size(); ++i) {
        auto& parameter = draft.instruction.parameters[i];
        DraftEditorRow row;
        row.parameterIndex = i;
        const auto* parameterSchema = opcodeSchema == nullptr ? nullptr
            : spice::sct::sctOpcodeParameterSchema(
                *opcodeSchema, parameter.address.schemaIndex);
        auto role = parameterSchema != nullptr && !parameterSchema->role.empty()
            ? QString::fromUtf8(parameterSchema->role.data(),
                static_cast<qsizetype>(parameterSchema->role.size()))
            : tr("Parameter %1").arg(parameter.address.schemaIndex);
        if (parameter.address.repeatedGroupOrdinal)
            role += tr(" (group %1)").arg(*parameter.address.repeatedGroupOrdinal + 1u);
        table->setItem(static_cast<int>(i), 0, new QTableWidgetItem(role));

        const auto owned = std::ranges::find(draft.ownedSupplementaryText,
            parameter.address, &core::SctOwnedSupplementaryTextDraft::parameter);
        if (owned != draft.ownedSupplementaryText.end()) {
            row.ownedTextIndex = static_cast<std::size_t>(
                std::distance(draft.ownedSupplementaryText.begin(), owned));
            if (owned->kind == spice::sct::SctTextKind::PlainString) {
                row.line = new QLineEdit(&editor);
                row.line->setPlaceholderText(tr("Instruction-owned supplementary text"));
                if (const auto* plain = std::get_if<spice::sct::SctPlainText>(&owned->value))
                    row.line->setText(QString::fromStdString(plain->utf8));
                row.initialText = row.line->text();
                table->setCellWidget(static_cast<int>(i), 1, row.line);
                table->setItem(static_cast<int>(i), 2,
                    new QTableWidgetItem(tr("A private supplementary text is created with the instruction.")));
            } else {
                row.choices = new QComboBox(&editor);
                row.choices->addItem(tr("Create a new default supplementary message"));
                row.candidates = documentController_->draftReferenceCandidates(
                    locator, opcode, parameter.address);
                for (const auto& candidate : row.candidates)
                    row.choices->addItem(QString::fromStdString(candidate.label));
                row.newSupplementaryTextMessageChoice = true;
                table->setCellWidget(static_cast<int>(i), 1, row.choices);
                table->setItem(static_cast<int>(i), 2,
                    new QTableWidgetItem(tr("Choose an existing message or create a new default one.")));
            }
        } else {
            const auto* effective = parameter.value ? &*parameter.value
                : parameter.suggestedValue ? &*parameter.suggestedValue : nullptr;
            const bool reference = parameterSchema != nullptr
                && parameterSchema->referenceKind
                    != spice::sct::SctOpcodeReferenceKind::None;
            if (reference) {
                row.choices = new QComboBox(&editor);
                row.candidates = documentController_->draftReferenceCandidates(
                    locator, opcode, parameter.address);
                for (const auto& candidate : row.candidates)
                    row.choices->addItem(QString::fromStdString(candidate.label));
                if (effective != nullptr) {
                    const auto target = std::visit([](const auto& typed)
                            -> std::optional<spice::sct::SctDocumentReferenceTarget> {
                        using T = std::decay_t<decltype(typed)>;
                        if constexpr (std::is_same_v<T, spice::sct::SctInstructionReference>
                            || std::is_same_v<T, spice::sct::SctStringReference>
                            || std::is_same_v<T, spice::sct::SctSupplementaryTextReference>)
                            return typed.target;
                        return std::nullopt;
                    }, *effective);
                    if (target) {
                        const auto found = std::ranges::find(row.candidates, *target,
                            &core::SctReferenceCandidate::target);
                        if (found != row.candidates.end()) row.choices->setCurrentIndex(
                            static_cast<int>(std::distance(row.candidates.begin(), found)));
                    }
                }
                table->setCellWidget(static_cast<int>(i), 1, row.choices);
            } else if (effective != nullptr) {
                const auto terminator = parameterSchema != nullptr
                        && parameterSchema->terminator
                    ? std::optional{parameterSchema->terminator->encodedWord}
                    : std::nullopt;
                row.line = new QLineEdit(
                    draftValueText(*effective, terminator), &editor);
                row.initialText = row.line->text();
                table->setCellWidget(static_cast<int>(i), 1, row.line);
            } else {
                auto* unavailable = new QTableWidgetItem(tr("No safe authoring value available"));
                unavailable->setFlags(unavailable->flags() & ~Qt::ItemIsEditable);
                table->setItem(static_cast<int>(i), 1, unavailable);
            }
            if (!parameter.value && parameter.suggestedValue) {
                row.acceptSuggestion = new QCheckBox(tr("Accept suggested value"), &editor);
                table->setCellWidget(static_cast<int>(i), 2, row.acceptSuggestion);
            } else {
                table->setItem(static_cast<int>(i), 2, new QTableWidgetItem(
                    effective == nullptr ? tr("Required value") : tr("Factory default")));
            }
        }
        rows.push_back(std::move(row));
    }
    editorLayout->addWidget(table, 1);
    auto* editorButtons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
    editorButtons->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    editorLayout->addWidget(editorButtons);
    connect(editorButtons, &QDialogButtonBox::accepted, &editor, &QDialog::accept);
    connect(editorButtons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);

    while (editor.exec() == QDialog::Accepted) {
        auto candidate = draft;
        std::vector<std::size_t> removeOwned;
        QString error;
        for (const auto& row : rows) {
            auto& parameter = candidate.instruction.parameters[row.parameterIndex];
            if (row.ownedTextIndex) {
                auto& ownedText = candidate.ownedSupplementaryText[*row.ownedTextIndex];
                if (ownedText.kind == spice::sct::SctTextKind::PlainString) {
                    ownedText.value = spice::sct::SctPlainText{
                        row.line->text().toStdString()};
                } else if (row.choices->currentIndex() > 0) {
                    const auto selected = static_cast<std::size_t>(
                        row.choices->currentIndex() - 1);
                    parameter.value = referenceValue(row.candidates[selected].target);
                    removeOwned.push_back(*row.ownedTextIndex);
                }
                continue;
            }
            if (row.choices != nullptr) {
                if (row.choices->currentIndex() < 0 || row.candidates.empty()) {
                    error = tr("%1 requires a compatible reference target.")
                        .arg(table->item(static_cast<int>(row.parameterIndex), 0)->text());
                    break;
                }
                parameter.value = referenceValue(row.candidates[
                    static_cast<std::size_t>(row.choices->currentIndex())].target);
                continue;
            }
            const auto* current = parameter.value ? &*parameter.value
                : parameter.suggestedValue ? &*parameter.suggestedValue : nullptr;
            if (current == nullptr || row.line == nullptr) {
                error = tr("%1 has no safe value yet.")
                    .arg(table->item(static_cast<int>(row.parameterIndex), 0)->text());
                break;
            }
            const bool changed = row.line->text() != row.initialText;
            if (!parameter.value && parameter.suggestedValue
                && !changed && (row.acceptSuggestion == nullptr
                    || !row.acceptSuggestion->isChecked())) {
                error = tr("Explicitly accept or replace the suggested value for %1.")
                    .arg(table->item(static_cast<int>(row.parameterIndex), 0)->text());
                break;
            }
            if (changed) {
                const auto parsed = core::SctParameterAuthoringService::parseDraftValue(
                    opcode, parameter.address, *current, row.line->text().toStdString());
                if (!parsed.succeeded()) {
                    error = QString::fromStdString(parsed.error);
                    break;
                }
                parameter.value = *parsed.value;
            } else {
                parameter.value = *current;
            }
        }
        if (error.isEmpty()) {
            std::ranges::sort(removeOwned, std::greater{});
            for (const auto index : removeOwned)
                candidate.ownedSupplementaryText.erase(candidate.ownedSupplementaryText.begin() + index);
            return candidate;
        }
        QMessageBox::warning(&editor, tr("Incomplete Instruction Draft"), error);
    }
    return std::nullopt;
}

void MainWindow::insertInstruction() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto context = widget->insertionContext();
    if (!context.has_value()) return;
    auto draft = chooseInstructionDraft(widget->locator(), context->allowReturn);
    if (!draft.has_value()) return;
    (void)documentController_->createInstructionAfter(
        widget->locator(), context->anchor, std::move(*draft));
}

std::optional<std::uint16_t> MainWindow::chooseSemanticArmOpcode() {
    QStringList choices;
    std::vector<std::uint16_t> opcodes;
    for (const auto& choice : core::SctEditSession::insertableOpcodes()) {
        if (choice.opcode == 12u) continue;
        choices.push_back(QStringLiteral("%1  %2")
            .arg(choice.opcode, 3, 10, QLatin1Char('0'))
            .arg(QString::fromStdString(choice.mnemonic)));
        opcodes.push_back(choice.opcode);
    }
    bool accepted = false;
    const auto selected = QInputDialog::getItem(this,
        tr("First Semantic-Arm Instruction"),
        tr("Opcode (only instructions requiring no parameter draft are available here):"),
        choices, 0, false, &accepted);
    const auto index = choices.indexOf(selected);
    return accepted && index >= 0
        ? std::optional{opcodes[static_cast<std::size_t>(index)]}
        : std::nullopt;
}

std::optional<spice::sct::SctDocumentRepeatedParameterGroup>
MainWindow::configureRepeatedGroupDraft(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId instruction,
    spice::sct::SctRepeatedParameterGroupDraft draft) {
    struct Row final {
        std::size_t parameterIndex = 0;
        QLineEdit* line = nullptr;
        QComboBox* choices = nullptr;
        QCheckBox* acceptSuggestion = nullptr;
        QString initialText;
        std::vector<core::SctReferenceCandidate> candidates;
    };
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Repeated Group — Instruction %1")
        .arg(instruction.value()));
    dialog.resize(760, 380);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr(
        "Resolve required values and explicitly accept or replace provisional suggestions."),
        &dialog));
    auto* table = new QTableWidget(static_cast<int>(draft.parameters.size()), 3, &dialog);
    table->setHorizontalHeaderLabels({tr("Parameter"), tr("Value"), tr("Notes")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->resizeSection(0, 190);
    table->horizontalHeader()->resizeSection(1, 350);
    table->horizontalHeader()->resizeSection(2, 190);
    table->verticalHeader()->hide();
    const auto* opcodeSchema = spice::sct::findSctOpcodeSchema(draft.opcode);
    std::vector<Row> rows;
    for (std::size_t i = 0; i < draft.parameters.size(); ++i) {
        auto& parameter = draft.parameters[i];
        Row row;
        row.parameterIndex = i;
        const spice::sct::SctParameterAddress address{parameter.schemaIndex, 0u};
        const auto* schema = opcodeSchema == nullptr ? nullptr
            : spice::sct::sctOpcodeParameterSchema(*opcodeSchema, parameter.schemaIndex);
        const auto role = schema != nullptr && !schema->role.empty()
            ? QString::fromUtf8(schema->role.data(),
                static_cast<qsizetype>(schema->role.size()))
            : tr("Parameter %1").arg(parameter.schemaIndex);
        table->setItem(static_cast<int>(i), 0, new QTableWidgetItem(role));
        const auto* effective = parameter.value ? &*parameter.value
            : parameter.suggestedValue ? &*parameter.suggestedValue : nullptr;
        const bool reference = schema != nullptr
            && schema->referenceKind != spice::sct::SctOpcodeReferenceKind::None;
        if (reference) {
            row.choices = new QComboBox(&dialog);
            row.candidates = documentController_->draftReferenceCandidates(
                locator, draft.opcode, address);
            for (const auto& candidate : row.candidates)
                row.choices->addItem(QString::fromStdString(candidate.label));
            table->setCellWidget(static_cast<int>(i), 1, row.choices);
        } else if (effective != nullptr) {
            const auto terminator = schema != nullptr && schema->terminator
                ? std::optional{schema->terminator->encodedWord}
                : std::nullopt;
            row.line = new QLineEdit(
                draftValueText(*effective, terminator), &dialog);
            row.initialText = row.line->text();
            table->setCellWidget(static_cast<int>(i), 1, row.line);
        } else {
            table->setItem(static_cast<int>(i), 1,
                new QTableWidgetItem(tr("No safe value available")));
        }
        if (!parameter.value && parameter.suggestedValue) {
            row.acceptSuggestion = new QCheckBox(tr("Accept suggested value"), &dialog);
            table->setCellWidget(static_cast<int>(i), 2, row.acceptSuggestion);
        } else {
            table->setItem(static_cast<int>(i), 2, new QTableWidgetItem(
                effective == nullptr ? tr("Required value") : tr("Factory default")));
        }
        rows.push_back(std::move(row));
    }
    layout->addWidget(table, 1);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    while (dialog.exec() == QDialog::Accepted) {
        auto candidate = draft;
        QString error;
        for (const auto& row : rows) {
            auto& parameter = candidate.parameters[row.parameterIndex];
            const spice::sct::SctParameterAddress address{parameter.schemaIndex, 0u};
            if (row.choices != nullptr) {
                if (row.choices->currentIndex() < 0 || row.candidates.empty()) {
                    error = tr("%1 requires a compatible reference target.")
                        .arg(table->item(static_cast<int>(row.parameterIndex), 0)->text());
                    break;
                }
                parameter.value = referenceValue(row.candidates[
                    static_cast<std::size_t>(row.choices->currentIndex())].target);
                continue;
            }
            const auto* current = parameter.value ? &*parameter.value
                : parameter.suggestedValue ? &*parameter.suggestedValue : nullptr;
            if (current == nullptr || row.line == nullptr) {
                error = tr("%1 is unresolved.")
                    .arg(table->item(static_cast<int>(row.parameterIndex), 0)->text());
                break;
            }
            const bool changed = row.line->text() != row.initialText;
            if (!parameter.value && parameter.suggestedValue && !changed
                && (row.acceptSuggestion == nullptr
                    || !row.acceptSuggestion->isChecked())) {
                error = tr("Explicitly accept or replace the suggested value for %1.")
                    .arg(table->item(static_cast<int>(row.parameterIndex), 0)->text());
                break;
            }
            if (changed) {
                const auto parsed = core::SctParameterAuthoringService::parseDraftValue(
                    candidate.opcode, address, *current,
                    row.line->text().toStdString());
                if (!parsed.succeeded()) {
                    error = QString::fromStdString(parsed.error);
                    break;
                }
                parameter.value = *parsed.value;
            } else parameter.value = *current;
        }
        if (error.isEmpty()) {
            const auto materialized =
                spice::sct::SctInstructionFactory::materializeRepeatedGroup(candidate);
            if (materialized.group) return *materialized.group;
            error = materialized.diagnostics.empty()
                ? tr("The repeated group is incomplete.")
                : QString::fromStdString(materialized.diagnostics.front().message);
        }
        QMessageBox::warning(&dialog, tr("Incomplete Repeated Group"), error);
    }
    return std::nullopt;
}

std::optional<core::SctSemanticFragment> MainWindow::captureSelectedFragment(
    const bool reportFailure) {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return std::nullopt;
    core::Result<core::SctSemanticFragment> captured = [&] {
        const auto instructions = widget->selectedInstructions();
        if (!instructions.empty())
            return documentController_->captureInstructions(
                widget->locator(), instructions);
        const auto sections = widget->selectedSections();
        if (!sections.empty())
            return documentController_->captureSections(widget->locator(), sections);
        return core::Result<core::SctSemanticFragment>::failure(core::Diagnostic{
            core::DiagnosticSeverity::Error, core::DiagnosticCode::InvalidSctFragment,
            "Select a contiguous instruction range or contiguous physical sections.",
            widget->locator().path()});
    }();
    if (!captured) {
        if (reportFailure) QMessageBox::warning(this, tr("Selection Cannot Be Copied"),
            captured.diagnostics().empty() ? tr("The selection is not copyable.")
                : QString::fromStdString(captured.diagnostics().front().message));
        return std::nullopt;
    }
    return std::move(captured).takeValue();
}

bool MainWindow::copyFragmentToClipboard(
    const core::SctSemanticFragment& fragment) {
    const auto encoded = core::SctFragmentCodec::serialize(fragment);
    if (!encoded) {
        QMessageBox::warning(this, tr("Copy Failed"),
            QString::fromStdString(encoded.diagnostics().front().message));
        return false;
    }
    auto* data = new QMimeData;
    data->setData(QString::fromLatin1(core::SctFragmentCodec::MimeType.data(),
        static_cast<qsizetype>(core::SctFragmentCodec::MimeType.size())),
        QByteArray(reinterpret_cast<const char*>(encoded.value().data()),
            static_cast<qsizetype>(encoded.value().size())));
    data->setText(fragment.kind == core::SctFragmentKind::InstructionRange
        ? tr("SALSA instruction fragment (%1 instructions)")
            .arg(fragment.instructions.size())
        : tr("SALSA section fragment (%1 sections)").arg(fragment.sections.size()));
    QApplication::clipboard()->setMimeData(data);
    return true;
}

void MainWindow::copySelection() {
    const auto fragment = captureSelectedFragment();
    if (fragment) (void)copyFragmentToClipboard(*fragment);
}

void MainWindow::cutSelection() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto fragment = captureSelectedFragment();
    if (!fragment || !copyFragmentToClipboard(*fragment)) return;
    const auto instructions = widget->selectedInstructions();
    if (!instructions.empty()) {
        (void)documentController_->deleteInstructions(widget->locator(), instructions);
        return;
    }
    const auto sections = widget->selectedSections();
    if (!sections.empty())
        (void)documentController_->deleteSections(widget->locator(), sections);
}

bool MainWindow::pasteFragment(const core::SctSemanticFragment& fragment) {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return false;
    core::SctFragmentPasteDestination destination;
    if (fragment.kind == core::SctFragmentKind::InstructionRange) {
        const auto selected = widget->selectedInstructions();
        destination.instructionAfter = selected.empty()
            ? widget->selectedInstruction()
            : std::optional<spice::sct::SctInstructionId>{selected.back()};
        if (!destination.instructionAfter) {
            QMessageBox::information(this, tr("Choose an Insertion Point"),
                tr("Select a physical instruction after which the fragment should be inserted."));
            return false;
        }
    } else {
        const auto selected = widget->selectedSections();
        destination.sectionAfter = selected.empty()
            ? widget->selectedSection()
            : std::optional<spice::sct::SctSectionId>{selected.back()};
        auto names = documentController_->suggestSectionNames(
            widget->locator(), fragment);
        if (names.size() != fragment.sections.size()) return false;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Review Pasted Section Names"));
        auto* layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(tr(
            "Review the unique physical names that will be used for the pasted sections."),
            &dialog));
        std::vector<QLineEdit*> editors;
        editors.reserve(names.size());
        for (std::size_t ordinal = 0; ordinal < names.size(); ++ordinal) {
            auto* row = new QHBoxLayout;
            row->addWidget(new QLabel(QString::fromUtf8(
                fragment.sections[ordinal].nameBytes.data(),
                static_cast<qsizetype>(fragment.sections[ordinal].nameBytes.size())),
                &dialog));
            auto* editor = new QLineEdit(QString::fromStdString(names[ordinal]), &dialog);
            editor->setMaxLength(16);
            row->addWidget(editor, 1);
            layout->addLayout(row);
            editors.push_back(editor);
        }
        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return false;
        static const QRegularExpression validName(
            QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
        std::unordered_set<std::string> unique;
        for (auto* editor : editors) {
            if (!validName.match(editor->text()).hasMatch()
                || !unique.insert(editor->text().toStdString()).second) {
                QMessageBox::warning(this, tr("Invalid Section Names"), tr(
                    "Every pasted section name must match [A-Za-z0-9_]{1,16} and be unique."));
                return false;
            }
            destination.sectionNames.push_back(editor->text().toStdString());
        }
    }
    return documentController_->pasteFragment(
        widget->locator(), fragment, std::move(destination));
}

void MainWindow::pasteSelection() {
    const auto* data = QApplication::clipboard()->mimeData();
    const auto mime = QString::fromLatin1(core::SctFragmentCodec::MimeType.data(),
        static_cast<qsizetype>(core::SctFragmentCodec::MimeType.size()));
    if (data == nullptr || !data->hasFormat(mime)) return;
    const auto raw = data->data(mime);
    const auto bytes = std::as_bytes(std::span{raw.constData(),
        static_cast<std::size_t>(raw.size())});
    const auto fragment = core::SctFragmentCodec::deserialize(bytes);
    if (!fragment) {
        QMessageBox::warning(this, tr("Paste Failed"),
            QString::fromStdString(fragment.diagnostics().front().message));
        return;
    }
    (void)pasteFragment(fragment.value());
}

void MainWindow::duplicateSelection() {
    const auto fragment = captureSelectedFragment();
    if (fragment) (void)pasteFragment(*fragment);
}

void MainWindow::deleteSelection() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto instructions = widget->selectedInstructions();
    if (!instructions.empty()) {
        (void)documentController_->deleteInstructions(widget->locator(), instructions);
        return;
    }
    const auto sections = widget->selectedSections();
    if (!sections.empty()) {
        if (QMessageBox::question(this, tr("Delete Sections"),
                tr("Delete the selected sections and their contents?")) == QMessageBox::Yes)
            (void)documentController_->deleteSections(widget->locator(), sections);
    }
}

void MainWindow::saveSelectionAsSnippet() {
    const auto fragment = captureSelectedFragment();
    if (!fragment) return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Save Snippet"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Name:"), &dialog));
    auto* name = new QLineEdit(&dialog);
    name->setMaxLength(120);
    layout->addWidget(name);
    layout->addWidget(new QLabel(tr("Description (optional):"), &dialog));
    auto* description = new QLineEdit(&dialog);
    layout->addWidget(description);
    auto* scope = new QComboBox(&dialog);
    scope->addItem(tr("Personal"), false);
    if (patchWorkspace_) scope->addItem(tr("Workspace"), true);
    layout->addWidget(scope);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setEnabled(false);
    connect(name, &QLineEdit::textChanged, &dialog, [buttons](const QString& value) {
        buttons->button(QDialogButtonBox::Save)->setEnabled(!value.trimmed().isEmpty());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;
    const bool workspace = scope->currentData().toBool();
    const auto root = workspace
        ? patchWorkspace_->componentPath(
            patchWorkspace_->descriptor().components.authoring / L"snippets")
        : std::filesystem::path(QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation).toStdWString()) / L"snippets";
    core::SctSnippetStore store(root);
    const auto saved = store.save({name->text().trimmed().toStdString(),
        description->text().toStdString(), *fragment});
    if (!saved) QMessageBox::warning(this, tr("Snippet Save Failed"),
        QString::fromStdString(saved.diagnostics().front().message));
    else reloadSnippets();
}

void MainWindow::reloadSnippets() {
    if (snippetList_ == nullptr) return;
    loadedSnippets_.clear();
    snippetList_->clear();
    const auto append = [&](const std::filesystem::path& root, const bool workspace) {
        const core::SctSnippetStore store(root);
        const auto loaded = store.loadAll();
        if (!loaded) return;
        for (const auto& snippet : loaded.value())
            loadedSnippets_.push_back({workspace, snippet});
    };
    append(std::filesystem::path(QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation).toStdWString()) / L"snippets", false);
    if (patchWorkspace_) append(patchWorkspace_->componentPath(
        patchWorkspace_->descriptor().components.authoring / L"snippets"), true);
    for (std::size_t index = 0; index < loadedSnippets_.size(); ++index) {
        const auto& loaded = loadedSnippets_[index];
        auto* item = new QListWidgetItem(
            QString::fromStdString(loaded.snippet.name)
                + (loaded.workspace ? tr("  [Workspace]") : tr("  [Personal]")),
            snippetList_);
        item->setToolTip(QString::fromStdString(loaded.snippet.description));
        item->setData(Qt::UserRole, static_cast<qulonglong>(index));
    }
}

void MainWindow::pasteSelectedSnippet() {
    const auto* item = snippetList_->currentItem();
    if (item == nullptr) return;
    const auto index = item->data(Qt::UserRole).toULongLong();
    if (index < loadedSnippets_.size())
        (void)pasteFragment(loadedSnippets_[static_cast<std::size_t>(index)].snippet.fragment);
}

void MainWindow::deleteSelectedSnippet() {
    const auto* item = snippetList_->currentItem();
    if (item == nullptr) return;
    const auto index = item->data(Qt::UserRole).toULongLong();
    if (index >= loadedSnippets_.size()) return;
    const auto& loaded = loadedSnippets_[static_cast<std::size_t>(index)];
    if (QMessageBox::question(this, tr("Delete Snippet"),
            tr("Delete snippet '%1'?").arg(QString::fromStdString(loaded.snippet.name)))
        != QMessageBox::Yes) return;
    const auto root = loaded.workspace && patchWorkspace_
        ? patchWorkspace_->componentPath(
            patchWorkspace_->descriptor().components.authoring / L"snippets")
        : std::filesystem::path(QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation).toStdWString()) / L"snippets";
    const auto removed = core::SctSnippetStore(root).remove(loaded.snippet.name);
    if (!removed) QMessageBox::warning(this, tr("Snippet Delete Failed"),
        QString::fromStdString(removed.diagnostics().front().message));
    else reloadSnippets();
}

void MainWindow::deleteInstruction() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto instruction = widget->selectedInstruction();
    if (instruction.has_value())
        (void)documentController_->deleteInstruction(widget->locator(), *instruction);
}

void MainWindow::moveInstruction(const core::SctInstructionMoveDirection direction) {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto instructions = widget->selectedInstructions();
    if (instructions.size() > 1u) {
        const auto anchor = widget->rangeMoveAnchor(direction);
        if (anchor) (void)documentController_->moveInstructionsAfter(
            widget->locator(), instructions, *anchor);
        return;
    }
    if (const auto instruction = widget->selectedInstruction())
        (void)documentController_->moveInstruction(
            widget->locator(), *instruction, direction);
}

void MainWindow::createScriptSection() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Script Section"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Name ([A-Za-z0-9_], 1-16 characters):"), &dialog));
    auto* name = new QLineEdit(&dialog);
    name->setMaxLength(16);
    layout->addWidget(name);
    auto* includeReturn = new QCheckBox(tr("End with Return (12)"), &dialog);
    includeReturn->setChecked(true);
    layout->addWidget(includeReturn);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);
    connect(name, &QLineEdit::textChanged, &dialog, [buttons](const QString& value) {
        static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(pattern.match(value).hasMatch());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    (void)documentController_->createScriptSection(widget->locator(),
        name->text().toStdString(), widget->selectedSection(), includeReturn->isChecked());
}

void MainWindow::createIndexedString() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(this, tr("New Indexed String"),
        tr("Section name ([A-Za-z0-9_], 1-16 characters):"),
        QLineEdit::Normal, {}, &accepted);
    if (!accepted) return;
    (void)documentController_->createIndexedString(widget->locator(),
        name.toStdString(), widget->selectedSection());
}

void MainWindow::renameSelectedSection() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto section = widget->selectedSection();
    if (!section) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(this, tr("Rename Section"),
        tr("New name ([A-Za-z0-9_], 1-16 characters):"),
        QLineEdit::Normal, {}, &accepted);
    if (accepted) (void)documentController_->renameSection(
        widget->locator(), *section, name.toStdString());
}

void MainWindow::deleteSelectedSection() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto section = widget->selectedSection();
    if (!section) return;
    if (QMessageBox::question(this, tr("Delete Script Section"),
            tr("Delete the selected script section and all instructions it owns?"))
        != QMessageBox::Yes) return;
    (void)documentController_->deleteSection(widget->locator(), *section);
}

void MainWindow::moveSelectedSection(const core::SctSectionMoveDirection direction) {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    if (const auto section = widget->selectedSection())
        (void)documentController_->moveSection(widget->locator(), *section, direction);
}

void MainWindow::createSupplementaryText(const core::SctCreatedSupplementaryTextKind kind) {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    std::optional<spice::sct::SctSupplementaryTextId> after;
    if (const auto target = widget->selectedTextTarget()) {
        if (const auto* footer = std::get_if<spice::sct::SctSupplementaryTextId>(&*target))
            after = *footer;
    }
    (void)documentController_->createSupplementaryText(widget->locator(), kind, after);
}

void MainWindow::deleteSelectedText() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto target = widget->selectedTextTarget();
    if (!target) return;
    if (QMessageBox::question(this, tr("Delete Text Entity"),
            tr("Delete the selected text entity?")) != QMessageBox::Yes) return;
    (void)documentController_->deleteTextEntity(widget->locator(), *target);
}

void MainWindow::editSelectedMessage() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr || !widget->canEditSelectedMessage()) return;
    const auto target = widget->selectedTextTarget();
    const auto snapshot = documentController_->snapshot(widget->locator());
    const auto value = target ? documentController_->workingText(widget->locator(), *target)
        : std::nullopt;
    if (!target.has_value() || snapshot == nullptr || !value) return;
    if (!messageEditor_->isBoundTo(widget->locator(), *target)
        ) {
        spice::sct::SctTextKind kind = std::holds_alternative<spice::sct::SctMessage>(*value)
            ? spice::sct::SctTextKind::SctString : spice::sct::SctTextKind::PlainString;
        spice::sct::SctTextStorage storage = std::holds_alternative<spice::sct::SctStringId>(*target)
            ? spice::sct::SctTextStorage::IndexedSection : spice::sct::SctTextStorage::Footer;
        if (snapshot->analysis && snapshot->document) {
            std::visit([&](const auto id) {
                if (const auto* entity = snapshot->analysis->entities.find(*snapshot->document, id))
                    kind = entity->kind;
            }, *target);
        }
        if (!messageEditor_->bindText(widget->locator(), *target, *value, kind, storage)) {
            statusBar()->showMessage(tr("The selected text could not be opened for editing."), 8000);
            return;
        }
    }
    messageEditorDock_->show();
    messageEditorDock_->raise();
    messageEditor_->focusEditor();
}

bool MainWindow::flushMessageEditor() {
    return messageEditor_ == nullptr || messageEditor_->flushPending();
}

bool MainWindow::flushPendingEditors() {
    return flushMetadataEditor() && flushMessageEditor();
}

bool MainWindow::prepareScptEditor(
    const std::optional<core::AssetLocator>& locator) {
    if (scptEditor_ == nullptr || !scptEditor_->boundLocator()) return true;
    if (locator && *scptEditor_->boundLocator() != *locator) return true;
    return scptEditor_->prepareToClear();
}

void MainWindow::undoActiveDocument() {
    if (!flushPendingEditors()) return;
    if (auto* widget = activeDocumentWidget())
        (void)documentController_->undo(widget->locator());
}

void MainWindow::redoActiveDocument() {
    if (!flushPendingEditors()) return;
    if (auto* widget = activeDocumentWidget())
        (void)documentController_->redo(widget->locator());
}

bool MainWindow::confirmDiscardDocument(
    const core::AssetLocator& locator, const QString& action,
    const PendingLifecycle pending) {
    if (!documentController_->isDirty(locator)) return true;
    QMessageBox message(this);
    message.setIcon(QMessageBox::Warning);
    message.setWindowTitle(tr("Save document changes?"));
    message.setText(tr("%1 has changes that are not in a patch checkpoint.")
        .arg(QString::fromStdWString(locator.path().wstring())));
    message.setInformativeText(patchWorkspace_ && !documentController_->patchConflict(locator)
        ? tr("Save a patch checkpoint, discard the changes, or cancel before you %1?").arg(action)
        : tr("No writable SALSA workspace is available. Discard the changes or cancel before you %1?")
            .arg(action));
    auto buttons = QMessageBox::Discard | QMessageBox::Cancel;
    if (patchWorkspace_ && !documentController_->patchConflict(locator))
        buttons |= QMessageBox::Save;
    message.setStandardButtons(buttons);
    message.setDefaultButton(QMessageBox::Cancel);
    const auto choice = message.exec();
    if (choice == QMessageBox::Discard) return true;
    if (choice != QMessageBox::Save) return false;
    pendingLifecycle_ = pending;
    pendingLifecycleDocument_ = pending == PendingLifecycle::CloseDocument
        ? std::optional<core::AssetLocator>{locator} : std::nullopt;
    pendingLifecycleSaves_ = {locator};
    if (!documentController_->isSaving(locator)
        && !documentController_->saveDocument(locator)) {
        pendingLifecycle_ = PendingLifecycle::None;
        pendingLifecycleDocument_.reset();
        pendingLifecycleSaves_.clear();
        statusBar()->showMessage(tr("The document checkpoint could not be started."), 8000);
    }
    return false;
}

bool MainWindow::confirmDiscardAll(
    const QString& action, const PendingLifecycle pending) {
    const auto dirty = documentController_->dirtyLocators();
    if (dirty.empty()) return true;
    QMessageBox message(this);
    message.setIcon(QMessageBox::Warning);
    message.setWindowTitle(tr("Save document changes?"));
    message.setText(dirty.size() == 1
        ? tr("One open SCT document has changes outside its patch checkpoint.")
        : tr("%1 open SCT documents have changes outside their patch checkpoints.")
            .arg(dirty.size()));
    const bool canSaveAll = patchWorkspace_ && std::ranges::none_of(
        dirty, [this](const auto& locator) {
            return documentController_->patchConflict(locator);
        });
    message.setInformativeText(canSaveAll
        ? tr("Save patch checkpoints, discard all changes, or cancel before you %1?").arg(action)
        : tr("Not every document has a writable, conflict-free SALSA workspace. Discard all changes or cancel before you %1?")
            .arg(action));
    auto buttons = QMessageBox::Discard | QMessageBox::Cancel;
    if (canSaveAll) buttons |= QMessageBox::Save;
    message.setStandardButtons(buttons);
    message.setDefaultButton(QMessageBox::Cancel);
    const auto choice = message.exec();
    if (choice == QMessageBox::Discard) return true;
    if (choice != QMessageBox::Save) return false;
    pendingLifecycle_ = pending;
    pendingLifecycleDocument_.reset();
    pendingLifecycleSaves_.clear();
    bool failedToStart = false;
    for (const auto& locator : dirty) {
        pendingLifecycleSaves_.push_back(locator);
        if (!documentController_->isSaving(locator))
            failedToStart = !documentController_->saveDocument(locator) || failedToStart;
    }
    if (failedToStart) {
        pendingLifecycle_ = PendingLifecycle::None;
        pendingLifecycleSaves_.clear();
        statusBar()->showMessage(tr(
            "One or more document checkpoints could not be started."), 8000);
    } else {
        progressBar_->setRange(0, 0);
        progressBar_->show();
        cancelButton_->show();
    }
    return false;
}

void MainWindow::continuePendingLifecycle(
    const QString& identityKey, const bool success, const bool cancelled) {
    if (pendingLifecycleSaves_.empty()) return;
    if (!success || cancelled) {
        pendingLifecycle_ = PendingLifecycle::None;
        pendingLifecycleDocument_.reset();
        pendingLifecycleSaves_.clear();
        return;
    }
    std::erase_if(pendingLifecycleSaves_, [&identityKey](const auto& locator) {
        return QString::fromStdString(locator.identityKey()) == identityKey;
    });
    if (!pendingLifecycleSaves_.empty()) return;

    if (pendingLifecycleDocument_.has_value()
        && documentController_->isDirty(*pendingLifecycleDocument_)) {
        statusBar()->showMessage(tr(
            "The document changed while its checkpoint was being saved; it remains open."),
            8000);
        pendingLifecycle_ = PendingLifecycle::None;
        pendingLifecycleDocument_.reset();
        return;
    }
    if (pendingLifecycle_ != PendingLifecycle::CloseDocument
        && !documentController_->dirtyLocators().empty()) {
        statusBar()->showMessage(tr(
            "A document changed while checkpoints were being saved; the requested action was cancelled."),
            8000);
        pendingLifecycle_ = PendingLifecycle::None;
        return;
    }

    const auto action = pendingLifecycle_;
    const auto document = pendingLifecycleDocument_;
    pendingLifecycle_ = PendingLifecycle::None;
    pendingLifecycleDocument_.reset();
    if (action == PendingLifecycle::CloseDocument && document.has_value()) {
        documentController_->closeDocument(*document);
    } else if (action == PendingLifecycle::CloseDataset) {
        closeDataset();
    } else if (action == PendingLifecycle::OpenDataset) {
        const auto root = std::exchange(pendingDatasetRoot_, {});
        QTimer::singleShot(0, this, [this, root] { openDataset(root); });
    } else if (action == PendingLifecycle::RebasePatches) {
        QTimer::singleShot(0, this, [this]() { rebaseStalePatches(); });
    } else if (action == PendingLifecycle::Exit) {
        QTimer::singleShot(0, this, &QWidget::close);
    }
}

void MainWindow::closeDataset() {
    scptEditor_->clear();
    messageEditor_->clear();
    saveWorkspaceSession();
    documentController_->closeAll();
    detachPatchWorkspace(false);
    controller_->closeDataset();
    lastDataset_.clear();
    QSettings{}.setValue(QStringLiteral("workspace/lastDataset"), QString{});
    statusBar()->showMessage(tr("Dataset closed."), 5000);
}

void MainWindow::activateSelectedAsset() {
    const auto locator = controller_->selectedLocator();
    const auto project = controller_->projectSnapshot();
    if (!locator.has_value() || !project.has_value()
        || controller_->busy() || documentController_->busy()) return;
    if (documentController_->openDocument(*project, *locator))
        statusBar()->showMessage(tr("Decoding %1...")
            .arg(QString::fromStdWString(locator->path().filename().wstring())));
}

void MainWindow::syncDocument(
    const QString& identityKey,
    const SctDocumentUpdate& update) {
    const auto locators = documentController_->openLocators();
    const auto found = std::ranges::find_if(locators, [&identityKey](const auto& locator) {
        return QString::fromStdString(locator.identityKey()) == identityKey;
    });
    if (found == locators.end()) return;
    SctDocumentWidget* widget = nullptr;
    for (int i = 1; i < tabs_->count(); ++i) {
        auto* candidate = qobject_cast<SctDocumentWidget*>(tabs_->widget(i));
        if (candidate != nullptr && candidate->locator() == *found) {
            widget = candidate;
            break;
        }
    }
    const bool createdWidget = widget == nullptr;
    if (createdWidget) {
        widget = new SctDocumentWidget(*found, tabs_);
        widget->setParameterPresentationProvider([this, widget](const auto instruction) {
            return documentController_->parameterPresentation(
                widget->locator(), instruction);
        });
        widget->setParameterCommitHandler([this, widget](const auto& site,
                std::string text) {
            return documentController_->editParameterText(
                widget->locator(), site, std::move(text));
        });
        widget->setStructuredDeveloperOptions(
            showStructuredBasicBlocks_, showRejectedStructureEvidence_,
            showSemanticControlFlowInstructions_);
        tabs_->addTab(widget, QString::fromStdWString(found->path().filename().wstring()));
        connect(widget, &SctDocumentWidget::textConventionRequested, this,
            [this, widget](const QString&, const int convention) {
                if (!prepareScptEditor(widget->locator()) || !flushPendingEditors()
                    || !confirmDiscardDocument(widget->locator(), tr("change text interpretation"))) return;
                scptEditor_->clear();
                messageEditor_->clear();
                (void)documentController_->selectTextConvention(widget->locator(),
                    static_cast<spice::sct::SctKnownTextConvention>(convention));
            });
        connect(widget, &SctDocumentWidget::reloadRequested, this,
            [this, widget](const QString&) {
                if (!prepareScptEditor(widget->locator()) || !flushPendingEditors()
                    || !confirmDiscardDocument(widget->locator(), tr("reload the document"))) return;
                scptEditor_->clear();
                messageEditor_->clear();
                if (const auto project = controller_->projectSnapshot(); project.has_value())
                    (void)documentController_->reloadDocument(*project, widget->locator());
            });
        connect(widget, &SctDocumentWidget::becameActive, this,
            [this](const QString&) { syncDiagnostics(); syncEditActions(); });
        connect(widget, &SctDocumentWidget::navigationChanged, this,
            [this, widget](const QString&, const int kind, const qulonglong id) {
                if (restoringMetadataTransition_) return;
                const core::SctNavigationTarget target{
                    static_cast<core::SctNavigationKind>(kind), id};
                if (!syncMetadataEditor()) {
                    const auto& binding = metadataEditor_->binding();
                    if (binding && binding->locator == widget->locator()) {
                        restoringMetadataTransition_ = true;
                        widget->selectTarget(binding->target);
                        restoringMetadataTransition_ = false;
                    }
                    return;
                }
                recordNavigation({widget->locator(), target});
            });
        connect(widget, &SctDocumentWidget::activeViewChanged, this,
            [this](const QString&, int) { scheduleWorkspaceSessionSave(); });
        connect(widget, &SctDocumentWidget::editContextChanged,
            this, &MainWindow::syncEditActions);
        connect(widget, &SctDocumentWidget::createSectionFolderRequested,
            this, [this, widget](const QString&, const QList<qulonglong>& ids) {
                bool accepted = false;
                const auto name = QInputDialog::getText(this, tr("Create section folder"),
                    tr("Folder name"), QLineEdit::Normal, {}, &accepted).trimmed();
                if (!accepted || name.isEmpty()) return;
                std::vector<spice::sct::SctSectionId> sections;
                for (const auto id : ids) sections.emplace_back(id);
                std::optional<core::SctSectionFolderId> parent;
                if (const auto state = documentController_->semanticState(widget->locator())) {
                    const core::SctSectionFolder* narrowest = nullptr;
                    for (const auto& folder : state->folders) {
                        if (!std::ranges::all_of(sections, [&](const auto section) {
                                return std::ranges::find(folder.sections, section)
                                    != folder.sections.end();
                            })) continue;
                        if (narrowest == nullptr
                            || folder.sections.size() < narrowest->sections.size())
                            narrowest = &folder;
                    }
                    if (narrowest) parent = narrowest->id;
                }
                (void)documentController_->createSectionFolder(
                    widget->locator(), name.toStdString(), sections, parent);
            });
        connect(widget, &SctDocumentWidget::editSectionFolderRequested,
            this, [this, widget](const QString&, const qulonglong id) {
                const auto state = documentController_->semanticState(widget->locator());
                if (!state) return;
                const auto found = std::ranges::find(state->folders, id,
                    [](const auto& folder) { return folder.id.value; });
                if (found == state->folders.end()) return;
                QDialog dialog(this);
                dialog.setWindowTitle(tr("Edit Section Folder"));
                auto* layout = new QFormLayout(&dialog);
                auto* name = new QLineEdit(QString::fromStdString(found->name), &dialog);
                auto* parent = new QComboBox(&dialog);
                parent->addItem(tr("(top level)"), QVariant::fromValue<qulonglong>(0));
                for (const auto& candidate : state->folders) {
                    if (candidate.id == found->id) continue;
                    parent->addItem(QString::fromStdString(candidate.name),
                        QVariant::fromValue<qulonglong>(candidate.id.value));
                    if (found->parent && *found->parent == candidate.id)
                        parent->setCurrentIndex(parent->count() - 1);
                }
                auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save
                    | QDialogButtonBox::Cancel, &dialog);
                layout->addRow(tr("Name"), name);
                layout->addRow(tr("Parent folder"), parent);
                layout->addRow(buttons);
                connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
                connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
                if (dialog.exec() != QDialog::Accepted || name->text().trimmed().isEmpty()) return;
                auto updated = *found;
                updated.name = name->text().trimmed().toStdString();
                const auto parentId = parent->currentData().toULongLong();
                updated.parent = parentId == 0 ? std::nullopt
                    : std::optional{core::SctSectionFolderId{parentId}};
                (void)documentController_->updateSectionFolder(
                    widget->locator(), std::move(updated));
            });
        connect(widget, &SctDocumentWidget::removeSectionFolderRequested,
            this, [this, widget](const QString&, const qulonglong id) {
                (void)documentController_->removeSectionFolder(
                    widget->locator(), core::SctSectionFolderId{id});
            });
        connect(widget, &SctDocumentWidget::insertInstructionRequested,
            this, [this](const QString&) { insertInstruction(); });
        connect(widget, &SctDocumentWidget::deleteInstructionRequested,
            this, [this](const QString&) { deleteSelection(); });
        connect(widget, &SctDocumentWidget::moveInstructionRequested,
            this, [this](const QString&, const int direction) {
                moveInstruction(static_cast<core::SctInstructionMoveDirection>(direction));
            });
        connect(widget, &SctDocumentWidget::moveInstructionRangeRequested,
            this, [this, widget](const QString&, const QList<qulonglong>& values,
                const qulonglong anchor, const QPoint& globalPosition) {
                std::vector<spice::sct::SctInstructionId> instructions;
                instructions.reserve(static_cast<std::size_t>(values.size()));
                for (const auto value : values)
                    instructions.emplace_back(value);
                pendingInteractionPosition_ = globalPosition;
                (void)documentController_->moveInstructionsAfter(widget->locator(),
                    instructions, spice::sct::SctInstructionId(anchor));
                pendingInteractionPosition_.reset();
            });
        connect(widget, &SctDocumentWidget::editMessageRequested,
            this, [this](const QString&) { editSelectedMessage(); });
        connect(widget, &SctDocumentWidget::parameterNavigationRequested,
            this, [this, widget](const QString&, const int kind, const qulonglong id) {
                const core::SctNavigationTarget target{
                    static_cast<core::SctNavigationKind>(kind), id};
                widget->selectTarget(target);
                if (target.kind == core::SctNavigationKind::String
                    || target.kind == core::SctNavigationKind::SupplementaryText)
                    editSelectedMessage();
            });
        connect(widget, &SctDocumentWidget::advancedScptRequested,
            this, [this, widget](const QString&, const qulonglong instruction,
                    const quint32 schemaIndex, const int repeatedOrdinal) {
                spice::sct::SctParameterSite site{
                    spice::sct::SctInstructionId(instruction),
                    {schemaIndex, repeatedOrdinal < 0
                        ? std::optional<std::uint32_t>{}
                        : std::optional{static_cast<std::uint32_t>(repeatedOrdinal)}}};
                const auto expression = documentController_->workingParameterExpression(
                    widget->locator(), site);
                if (!expression) return;
                if (scptEditor_->bind(widget->locator(), site, *expression)) {
                    scptEditorDock_->show();
                    scptEditorDock_->raise();
                }
            });
        connect(widget, &SctDocumentWidget::changeParameterReferenceRequested,
            this, [this, widget](const QString&, const qulonglong instruction,
                    const quint32 schemaIndex, const int repeatedOrdinal) {
                const spice::sct::SctParameterSite site{
                    spice::sct::SctInstructionId(instruction),
                    {schemaIndex, repeatedOrdinal < 0
                        ? std::optional<std::uint32_t>{}
                        : std::optional{static_cast<std::uint32_t>(repeatedOrdinal)}}};
                const auto candidates = documentController_->referenceCandidates(
                    widget->locator(), site);
                QStringList labels;
                for (const auto& candidate : candidates)
                    labels.push_back(QString::fromStdString(candidate.label));
                bool accepted = false;
                const auto choice = QInputDialog::getItem(this,
                    tr("Select Reference Target"), tr("Compatible target:"),
                    labels, 0, false, &accepted);
                const auto selected = labels.indexOf(choice);
                if (accepted && selected >= 0)
                    (void)documentController_->retargetParameter(
                        widget->locator(), site,
                        candidates[static_cast<std::size_t>(selected)].target);
            });
        connect(widget, &SctDocumentWidget::replaceOpaqueParameterRequested,
            this, [this, widget](const QString&, const qulonglong instruction,
                    const quint32 schemaIndex, const int repeatedOrdinal,
                    const int editorKind) {
                const spice::sct::SctParameterSite site{
                    spice::sct::SctInstructionId(instruction),
                    {schemaIndex, repeatedOrdinal < 0
                        ? std::optional<std::uint32_t>{}
                        : std::optional{static_cast<std::uint32_t>(repeatedOrdinal)}}};
                const auto kind = static_cast<core::SctInlineParameterEditorKind>(
                    editorKind);
                if (kind == core::SctInlineParameterEditorKind::Reference) {
                    const auto candidates = documentController_->referenceCandidates(
                        widget->locator(), site);
                    QStringList labels;
                    for (const auto& candidate : candidates)
                        labels.push_back(QString::fromStdString(candidate.label));
                    bool accepted = false;
                    const auto choice = QInputDialog::getItem(this,
                        tr("Replace With Typed Reference"), tr("Compatible target:"),
                        labels, 0, false, &accepted);
                    const auto selected = labels.indexOf(choice);
                    if (accepted && selected >= 0)
                        (void)documentController_->retargetParameter(
                            widget->locator(), site,
                            candidates[static_cast<std::size_t>(selected)].target);
                    return;
                }
                if (kind == core::SctInlineParameterEditorKind::AdvancedScpt) {
                    const auto expression =
                        spice::sct::SctExpressionFactory::encodedDecimalLiteral(0);
                    if (scptEditor_->bind(widget->locator(), site, expression)) {
                        scptEditorDock_->show();
                        scptEditorDock_->raise();
                    }
                    return;
                }
                bool accepted = false;
                const auto text = QInputDialog::getText(this,
                    tr("Replace With Typed Value"),
                    kind == core::SctInlineParameterEditorKind::TerminatedWords
                        ? tr("Word list (the terminator is managed automatically):")
                        : tr("32-bit value:"),
                    QLineEdit::Normal, {}, &accepted);
                if (accepted)
                    (void)documentController_->editParameterText(
                        widget->locator(), site, text.toStdString());
            });
        connect(widget, &SctDocumentWidget::addRepeatedGroupRequested,
            this, [this, widget](const QString&, const qulonglong instructionId,
                    const quint32 ordinal) {
                const auto instruction = documentController_->workingInstruction(
                    widget->locator(), spice::sct::SctInstructionId(instructionId));
                if (!instruction) return;
                auto draftResult = spice::sct::SctInstructionFactory::createRepeatedGroupDraft(
                    instruction->opcode);
                if (!draftResult.draft) {
                    QMessageBox::warning(this, tr("Add Repeated Group"),
                        tr("The opcode schema cannot construct a repeated group draft."));
                    return;
                }
                const auto group = configureRepeatedGroupDraft(widget->locator(),
                    instruction->id, std::move(*draftResult.draft));
                if (!group) return;
                (void)documentController_->insertRepeatedGroup(widget->locator(),
                    instruction->id, ordinal, *group);
            });
        connect(widget, &SctDocumentWidget::deleteRepeatedGroupRequested,
            this, [this, widget](const QString&, const qulonglong instruction,
                    const quint32 ordinal) {
                (void)documentController_->deleteRepeatedGroup(widget->locator(),
                    spice::sct::SctInstructionId(instruction), ordinal);
            });
        connect(widget, &SctDocumentWidget::moveRepeatedGroupRequested,
            this, [this, widget](const QString&, const qulonglong instruction,
                    const quint32 ordinal, const int direction) {
                (void)documentController_->moveRepeatedGroup(widget->locator(),
                    spice::sct::SctInstructionId(instruction), ordinal,
                    static_cast<core::SctRepeatedGroupMoveDirection>(direction));
            });
        connect(widget, &SctDocumentWidget::createScriptSectionRequested,
            this, [this](const QString&) { createScriptSection(); });
        connect(widget, &SctDocumentWidget::createIndexedStringRequested,
            this, [this](const QString&) { createIndexedString(); });
        connect(widget, &SctDocumentWidget::renameSectionRequested,
            this, [this](const QString&) { renameSelectedSection(); });
        connect(widget, &SctDocumentWidget::deleteSectionRequested,
            this, [this](const QString&) { deleteSelectedSection(); });
        connect(widget, &SctDocumentWidget::moveSectionRequested,
            this, [this](const QString&, const int direction) {
                moveSelectedSection(static_cast<core::SctSectionMoveDirection>(direction));
            });
        connect(widget, &SctDocumentWidget::createSupplementaryTextRequested,
            this, [this](const QString&, const int kind) {
                createSupplementaryText(static_cast<core::SctCreatedSupplementaryTextKind>(kind));
            });
        connect(widget, &SctDocumentWidget::deleteTextRequested,
            this, [this](const QString&) { deleteSelectedText(); });
        connect(widget, &SctDocumentWidget::addElseRequested,
            this, [this, widget](const QString&, const qulonglong controller) {
                (void)documentController_->addVirtualElse(widget->locator(),
                    spice::sct::SctInstructionId(controller));
            });
        connect(widget, &SctDocumentWidget::addCaseRequested,
            this, [this, widget](const QString&, const qulonglong controller) {
                (void)documentController_->addVirtualCase(widget->locator(),
                    spice::sct::SctInstructionId(controller));
            });
        connect(widget, &SctDocumentWidget::setCaseValueRequested,
            this, [this, widget](const QString&, const qulonglong arm) {
                bool accepted = false;
                const auto value = QInputDialog::getInt(this, tr("Switch Case Value"),
                    tr("Signed case value:"), 0,
                    (std::numeric_limits<int>::lowest)(),
                    (std::numeric_limits<int>::max)(), 1, &accepted);
                if (accepted) {
                    (void)documentController_->setVirtualCaseValue(widget->locator(),
                        core::SctAuthoredArmId{arm}, static_cast<std::int32_t>(value));
                }
            });
        connect(widget, &SctDocumentWidget::removeSemanticArmRequested,
            this, [this, widget](const QString&, const qulonglong arm) {
                (void)documentController_->removeVirtualArm(
                    widget->locator(), core::SctAuthoredArmId{arm});
            });
        connect(widget, &SctDocumentWidget::insertIntoSemanticArmRequested,
            this, [this, widget](const QString&, const qulonglong arm) {
                const auto opcode = chooseSemanticArmOpcode();
                if (opcode) {
                    (void)documentController_->insertInstructionIntoAuthoredArm(
                        widget->locator(), core::SctAuthoredArmId{arm}, *opcode);
                }
            });
        connect(widget, &SctDocumentWidget::insertIntoStructuredArmRequested,
            this, [this, widget](const QString&, const qulonglong controller,
                const int armKind) {
                const auto opcode = chooseSemanticArmOpcode();
                if (opcode) {
                    (void)documentController_->insertInstructionIntoStructuredArm(
                        widget->locator(), spice::sct::SctInstructionId(controller),
                        static_cast<spice::sct::SctStructuredArmKind>(armKind),
                        *opcode);
                }
            });
        connect(widget, &SctDocumentWidget::returnSemanticArmToEmptyRequested,
            this, [this, widget](const QString&, const qulonglong arm,
                const qulonglong instruction) {
                (void)documentController_->deleteOnlyInstructionFromAuthoredArm(
                    widget->locator(), core::SctAuthoredArmId{arm},
                    spice::sct::SctInstructionId(instruction));
            });
    }
    const auto snapshot = update.snapshot != nullptr
        ? update.snapshot : documentController_->snapshot(*found);
    const auto sourceStatus = static_cast<int>(documentController_->sourceStatus(*found));
    const bool textOnly = !createdWidget && isTextOnlyTransition(update);
    const bool instructionDelta = !createdWidget
        && isIncrementalStructuralTransition(update);
    const bool authoringOnly = !createdWidget && update.transition
        && !update.transition->changes.structuredAuthoring.empty()
        && !update.transition->changes.documentChanged;
    bool incrementalInstructionApplied = false;
    if (update.kind == SctDocumentUpdateKind::VerifiedMaterialization) {
        widget->installVerifiedSnapshot(snapshot, sourceStatus);
    } else if (authoringOnly) {
        widget->setSourceStatus(sourceStatus);
    } else if (createdWidget || update.kind == SctDocumentUpdateKind::Replacement
        || (update.kind == SctDocumentUpdateKind::RevisionTransition
            && !textOnly && !instructionDelta)) {
        widget->setSnapshot(snapshot, sourceStatus);
    } else if (instructionDelta) {
        incrementalInstructionApplied = widget->applyInstructionChanges(
            snapshot, sourceStatus, update.transition->changes);
        if (!incrementalInstructionApplied) widget->setSourceStatus(sourceStatus);
    } else if (textOnly) {
        widget->applyTextOnlySnapshot(snapshot, sourceStatus, update.transition->changes);
    } else {
        widget->setSourceStatus(sourceStatus);
    }
    widget->setSemanticProjection(update.semanticProjection != nullptr
        ? update.semanticProjection : documentController_->semanticProjection(*found));
    if (const auto state = documentController_->semanticState(*found))
        widget->setAuthoringMetadata(state->aliases, state->annotations,
            state->folders, workspaceAuthoring_.opcodeColors);
    reloadAuthoringDocks();
    (void)syncMetadataEditor();
    if (!messageEditor_->isCommitting() && messageEditor_->boundLocator().has_value()
        && *messageEditor_->boundLocator() == *found
        && messageEditor_->boundTarget().has_value()
        && affectsMessageTarget(update, *messageEditor_->boundTarget())) {
        if (const auto text = documentController_->workingText(
                widget->locator(), *messageEditor_->boundTarget()); text.has_value()) {
            (void)messageEditor_->refreshText(*text);
        }
    }
    if (update.transition && scptEditor_->boundLocator()
        && *scptEditor_->boundLocator() == *found && scptEditor_->boundSite()) {
        scptEditor_->refreshOrMarkStale(*found, update.transition->changes,
            documentController_->workingParameterExpression(
                *found, *scptEditor_->boundSite()));
    }
    rebuildDocumentTabTitles();
    queueDiagnosticsSync();
    if (widget == activeDocumentWidget()
        && (createdWidget || update.kind == SctDocumentUpdateKind::Replacement
            || update.kind == SctDocumentUpdateKind::VerifiedMaterialization
            || (update.kind == SctDocumentUpdateKind::RevisionTransition && !textOnly))) {
        if (update.kind == SctDocumentUpdateKind::VerifiedMaterialization) {
            semanticNavigator_->installVerifiedDocument(widget->locator(), *snapshot);
        } else if (instructionDelta) {
            (void)semanticNavigator_->applyInstructionChanges(
                widget->locator(), *snapshot, update.transition->changes);
        } else if (!incrementalInstructionApplied) {
            syncSemanticNavigator();
        }
    }
    pruneNavigationHistory();
    syncEditActions();
    if (createdWidget) scheduleWorkspaceSessionSave();
}

void MainWindow::focusDocument(const QString& identityKey) {
    for (int i = 1; i < tabs_->count(); ++i) {
        auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(i));
        if (widget != nullptr
            && QString::fromStdString(widget->locator().identityKey()) == identityKey) {
            tabs_->setCurrentIndex(i);
            return;
        }
    }
}

void MainWindow::closeDocumentTab(const int index) {
    if (index <= 0 || index >= tabs_->count()) return;
    if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index))) {
        if (scptEditor_->boundLocator().has_value()
            && *scptEditor_->boundLocator() == widget->locator()
            && !scptEditor_->prepareToClear()) return;
        if (!flushPendingEditors()
            || !confirmDiscardDocument(widget->locator(), tr("close the document"),
                PendingLifecycle::CloseDocument)) return;
        documentController_->closeDocument(widget->locator());
    }
}

void MainWindow::rebuildDocumentTabTitles() {
    std::vector<SctDocumentWidget*> widgets;
    for (int i = 1; i < tabs_->count(); ++i)
        if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(i))) widgets.push_back(widget);
    for (const auto* widget : widgets) {
        const auto path = QString::fromStdWString(widget->locator().path().wstring());
        const auto parts = QDir::fromNativeSeparators(path).split('/', Qt::SkipEmptyParts);
        QString title = parts.isEmpty() ? path : parts.back();
        for (qsizetype depth = 2; depth <= parts.size(); ++depth) {
            const bool collision = std::ranges::any_of(widgets, [widget, depth, &title](const auto* other) {
                if (other == widget) return false;
                const auto otherParts = QDir::fromNativeSeparators(
                    QString::fromStdWString(other->locator().path().wstring())).split('/', Qt::SkipEmptyParts);
                if (otherParts.isEmpty()) return false;
                const auto start = std::max<qsizetype>(0, otherParts.size() - (depth - 1));
                return otherParts.mid(start).join('/') == title;
            });
            if (!collision) break;
            const auto start = std::max<qsizetype>(0, parts.size() - depth);
            title = parts.mid(start).join('/');
        }
        const auto tabIndex = tabs_->indexOf(const_cast<SctDocumentWidget*>(widget));
        if (documentController_->isDirty(widget->locator())) title += QLatin1Char('*');
        if (documentController_->isSaving(widget->locator())) title += tr(" [Saving]");
        if (documentController_->patchConflict(widget->locator())) title += tr(" [Patch conflict]");
        tabs_->setTabText(tabIndex, title);
        tabs_->setTabToolTip(tabIndex, path);
    }
    syncWindowTitle();
}

void MainWindow::scheduleWorkspaceSessionSave() {
    if (restoringWorkspaceSession_ || patchWorkspace_ == nullptr
        || workspaceSessionSaveTimer_ == nullptr) return;
    workspaceSessionSaveTimer_->start();
}

std::optional<core::WorkspaceSessionState> MainWindow::captureWorkspaceSession() const {
    if (patchWorkspace_ == nullptr) return std::nullopt;
    core::WorkspaceSessionState state;
    state.workspaceId = patchWorkspace_->descriptor().workspaceId;
    for (int index = 1; index < tabs_->count(); ++index) {
        const auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index));
        if (widget == nullptr) continue;
        const bool checkpointed = !documentController_->isDirty(widget->locator());
        state.documents.push_back({widget->locator(), widget->activeView(),
            checkpointed ? widget->currentTarget()
                         : std::optional<core::SctNavigationTarget>{}});
    }
    if (const auto* active = activeDocumentWidget()) state.activeDocument = active->locator();
    state.selectedProjectAsset = controller_->selectedLocator();

    std::function<void(const QModelIndex&)> collectExpanded;
    collectExpanded = [this, &state, &collectExpanded](const QModelIndex& parent) {
        for (int row = 0; row < workspaceModel_->rowCount(parent); ++row) {
            const auto index = workspaceModel_->index(row, 0, parent);
            if (!index.isValid()) continue;
            if (projectTree_->isExpanded(index)) {
                if (const auto path = workspaceModel_->logicalDirectoryAt(index))
                    state.expandedProjectDirectories.push_back(*path);
            }
            collectExpanded(index);
        }
    };
    collectExpanded({});

    std::size_t retainedThroughCurrent = 0;
    for (std::size_t index = 0; index < navigationHistory_.size(); ++index) {
        const auto& entry = navigationHistory_[index];
        bool include = !entry.locator && !entry.target;
        if (entry.locator && entry.target) {
            include = !documentController_->isDirty(*entry.locator)
                && navigationEntryAvailable(entry);
        }
        if (!include) continue;
        state.navigation.push_back({entry.locator, entry.target});
        if (index <= navigationHistoryIndex_) retainedThroughCurrent = state.navigation.size();
    }
    state.navigationIndex = state.navigation.empty() || retainedThroughCurrent == 0u
        ? 0u : retainedThroughCurrent - 1u;
    return state;
}

void MainWindow::saveWorkspaceSession() {
    if (restoringWorkspaceSession_ || patchWorkspace_ == nullptr) return;
    if (workspaceSessionSaveTimer_) workspaceSessionSaveTimer_->stop();
    const auto state = captureWorkspaceSession();
    if (!state) return;
    core::WorkspaceSessionStore store;
    const auto saved = store.checkpoint(patchWorkspace_->sessionPath(), *state);
    if (!saved) {
        const auto message = saved.diagnostics().empty()
            ? tr("The workspace session could not be saved.")
            : QString::fromStdString(saved.diagnostics().front().message);
        statusBar()->showMessage(message, 10000);
    }
}

void MainWindow::restoreWorkspaceSession() {
    if (patchWorkspace_ == nullptr) return;
    if (workspaceSessionSaveTimer_) workspaceSessionSaveTimer_->stop();
    core::WorkspaceSessionStore store;
    auto loaded = store.load(patchWorkspace_->sessionPath(),
        patchWorkspace_->descriptor().workspaceId);
    navigationHistory_.clear();
    navigationHistoryIndex_ = 0;
    if (!loaded) {
        const auto message = loaded.diagnostics().empty()
            ? tr("The optional workspace session was ignored.")
            : QString::fromStdString(loaded.diagnostics().front().message);
        statusBar()->showMessage(tr("%1 Durable workspace content remains available.")
            .arg(message), 12000);
        scheduleWorkspaceSessionSave();
        return;
    }
    if (!loaded.value()) {
        recordActiveNavigation();
        scheduleWorkspaceSessionSave();
        return;
    }
    restoringWorkspaceSession_ = true;
    replayingNavigation_ = true;
    restoringWorkspaceSessionState_ = std::move(*loaded.value());
    restoringWorkspaceDocumentIndex_ = 0;
    workspaceRestoreMessages_.clear();
    restoreNextWorkspaceDocument();
}

void MainWindow::restoreNextWorkspaceDocument() {
    if (!restoringWorkspaceSession_ || !restoringWorkspaceSessionState_) return;
    const auto& documents = restoringWorkspaceSessionState_->documents;
    if (activeDatasetOperation_)
        activeDatasetOperation_->reportRestorationProgress(
            restoringWorkspaceDocumentIndex_, documents.size(),
            restoringWorkspaceDocumentIndex_ < documents.size()
                ? QString::fromStdWString(
                    documents[restoringWorkspaceDocumentIndex_].locator.path().wstring())
                : QString{});
    if (activeWorkspaceMaintenance_)
        activeWorkspaceMaintenance_->reportRestorationProgress(
            restoringWorkspaceDocumentIndex_, documents.size(),
            restoringWorkspaceDocumentIndex_ < documents.size()
                ? QString::fromStdWString(
                    documents[restoringWorkspaceDocumentIndex_].locator.path().wstring())
                : QString{});
    if (restoringWorkspaceDocumentIndex_ >= documents.size()) {
        finishWorkspaceSessionRestore();
        return;
    }
    const auto& document = documents[restoringWorkspaceDocumentIndex_];
    const auto* catalog = controller_->catalog();
    const bool exists = catalog != nullptr && std::ranges::any_of(
        catalog->assets, [&document](const auto& asset) {
            return asset.locator == document.locator;
        });
    const auto project = controller_->projectSnapshot();
    if (!exists || !project) {
        workspaceRestoreMessages_.push_back(tr("Missing document: %1")
            .arg(QString::fromStdWString(document.locator.path().wstring())));
        ++restoringWorkspaceDocumentIndex_;
        QTimer::singleShot(0, this, &MainWindow::restoreNextWorkspaceDocument);
        return;
    }
    if (!documentController_->openDocument(*project, document.locator)) {
        workspaceRestoreMessages_.push_back(tr("Could not start restoration of %1")
            .arg(QString::fromStdWString(document.locator.path().wstring())));
        ++restoringWorkspaceDocumentIndex_;
        QTimer::singleShot(0, this, &MainWindow::restoreNextWorkspaceDocument);
    }
}

void MainWindow::continueWorkspaceSessionRestore(
    const QString& identityKey, const bool success, const bool cancelled,
    const QString& message) {
    if (!restoringWorkspaceSession_ || !restoringWorkspaceSessionState_) return;
    const auto& documents = restoringWorkspaceSessionState_->documents;
    if (restoringWorkspaceDocumentIndex_ >= documents.size()) return;
    const auto& state = documents[restoringWorkspaceDocumentIndex_];
    if (QString::fromStdString(state.locator.identityKey()) != identityKey) return;
    if (success) {
        SctDocumentWidget* widget = nullptr;
        for (int index = 1; index < tabs_->count(); ++index) {
            auto* candidate = qobject_cast<SctDocumentWidget*>(tabs_->widget(index));
            if (candidate && candidate->locator() == state.locator) {
                widget = candidate;
                break;
            }
        }
        if (widget != nullptr) {
            if (state.selection && widget->containsTarget(*state.selection))
                widget->selectTarget(*state.selection, false);
            else if (state.selection)
                workspaceRestoreMessages_.push_back(tr("Stale selection in %1")
                    .arg(QString::fromStdWString(state.locator.path().wstring())));
            widget->setActiveView(state.view);
        }
    } else {
        workspaceRestoreMessages_.push_back(message);
    }
    ++restoringWorkspaceDocumentIndex_;
    if (cancelled) restoringWorkspaceDocumentIndex_ = documents.size();
    QTimer::singleShot(0, this, &MainWindow::restoreNextWorkspaceDocument);
}

void MainWindow::restoreProjectTreeState(
    const core::WorkspaceSessionState& state) {
    for (const auto& path : state.expandedProjectDirectories) {
        const auto index = workspaceModel_->indexForLogicalDirectory(path);
        if (index.isValid()) projectTree_->setExpanded(index, true);
    }
    if (state.selectedProjectAsset) {
        const auto index = workspaceModel_->indexForLocator(*state.selectedProjectAsset);
        if (index.isValid()) projectTree_->setCurrentIndex(index);
    }
}

void MainWindow::finishWorkspaceSessionRestore() {
    if (!restoringWorkspaceSessionState_) return;
    const auto state = std::move(*restoringWorkspaceSessionState_);
    restoringWorkspaceSessionState_.reset();
    restoreProjectTreeState(state);

    navigationHistory_.clear();
    std::size_t retainedThroughCurrent = 0;
    for (std::size_t index = 0; index < state.navigation.size(); ++index) {
        NavigationEntry entry{state.navigation[index].locator,
            state.navigation[index].target};
        if (!navigationEntryAvailable(entry)) continue;
        navigationHistory_.push_back(std::move(entry));
        if (index <= state.navigationIndex) retainedThroughCurrent = navigationHistory_.size();
    }
    navigationHistoryIndex_ = navigationHistory_.empty() || retainedThroughCurrent == 0u
        ? 0u : retainedThroughCurrent - 1u;

    bool activated = false;
    if (state.activeDocument) {
        for (int index = 1; index < tabs_->count(); ++index) {
            const auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index));
            if (widget && widget->locator() == *state.activeDocument) {
                tabs_->setCurrentIndex(index);
                activated = true;
                break;
            }
        }
    }
    if (!activated) tabs_->setCurrentIndex(0);
    replayingNavigation_ = false;
    restoringWorkspaceSession_ = false;
    restoringWorkspaceDocumentIndex_ = 0;
    syncNavigationActions();
    syncDiagnostics();
    syncSemanticNavigator();
    syncEditActions();
    if (workspaceRestoreMessages_.isEmpty())
        statusBar()->showMessage(tr("Workspace session restored."), 8000);
    else
        statusBar()->showMessage(tr("Workspace restored with %1 stale or unavailable item(s).")
            .arg(workspaceRestoreMessages_.size()), 12000);
    const auto restorationSummary = workspaceRestoreMessages_.isEmpty()
        ? tr("Workspace session restored.")
        : tr("Workspace restored with %1 stale or unavailable item(s).")
            .arg(workspaceRestoreMessages_.size());
    if (activeDatasetOperation_)
        activeDatasetOperation_->completeRestoration(
            workspaceRestoreMessages_.isEmpty() ? QString{} : restorationSummary);
    if (activeWorkspaceMaintenance_)
        activeWorkspaceMaintenance_->completeRestoration(restorationSummary);
    workspaceRestoreMessages_.clear();
    scheduleWorkspaceSessionSave();
}

void MainWindow::rebuildRecentMenu() {
    recentMenu_->clear();
    for (const auto& root : recentDatasets_) {
        auto* action = recentMenu_->addAction(root);
        action->setToolTip(root);
        connect(action, &QAction::triggered, this, [this, root]() {
            openDataset(root);
        });
    }
    const bool available = !controller_->busy() && !documentController_->busy()
        && !(exclusiveOperations_ && exclusiveOperations_->active())
        && !documentController_->isPublishing() && !recentDatasets_.isEmpty();
    applyActionAvailability(recentMenu_->menuAction(), {available,
        recentDatasets_.isEmpty() ? tr("No recent datasets are available.")
                                  : tr("Wait for the current operation to finish.")});
    recentMenu_->setEnabled(available);
}

void MainWindow::recordRecentDataset(const QString& canonicalRoot) {
    const auto normalizedRoot = normalizedRecentDatasetPath(canonicalRoot);
    if (normalizedRoot.isEmpty()) return;
    lastDataset_ = normalizedRoot;
    recentDatasets_.erase(
        std::remove_if(
            recentDatasets_.begin(),
            recentDatasets_.end(),
            [&normalizedRoot](const QString& existing) {
                return sameDatasetPath(existing, normalizedRoot);
            }),
        recentDatasets_.end());
    recentDatasets_.push_front(normalizedRoot);
    while (recentDatasets_.size() > MaximumRecentDatasets) {
        recentDatasets_.removeLast();
    }
    QSettings settings{};
    settings.setValue(QStringLiteral("workspace/recentDatasets"), recentDatasets_);
    settings.setValue(QStringLiteral("workspace/lastDataset"), lastDataset_);
    rebuildRecentMenu();
}

void MainWindow::attemptRestoreDataset() {
    if (lastDataset_.isEmpty() || controller_->busy() || controller_->hasDataset()) return;
    restoringDataset_ = true;
    auto operation = std::make_unique<WorkspaceOperationController>(
        controller_, WorkspaceController::Operation::Opening, lastDataset_);
    activeDatasetOperation_ = operation.get();
    if (!exclusiveOperations_->open(std::move(operation), this)) {
        activeDatasetOperation_.clear();
        restoringDataset_ = false;
    }
}

void MainWindow::restoreApplicationSettings() {
    QSettings settings{};
    const auto geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    const auto state = settings.value(QStringLiteral("window/state")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    if (!state.isEmpty()) {
        restoreState(state, SettingsStateVersion);
    }
    messageEditorDock_->hide();
    scptEditorDock_->hide();

    const auto stored = settings.value(QStringLiteral("workspace/recentDatasets")).toStringList();
    const auto storedLastDataset = settings.value(
        QStringLiteral("workspace/lastDataset"),
        stored.isEmpty() ? QString{} : stored.front()).toString();
    lastDataset_ = normalizedRecentDatasetPath(storedLastDataset);
    lastExportDirectory_ = normalizedRecentDatasetPath(
        settings.value(QStringLiteral("publication/lastDirectory")).toString());
    for (const auto& root : stored) {
        const auto normalizedRoot = normalizedRecentDatasetPath(root);
        if (!normalizedRoot.isEmpty() && QFileInfo(normalizedRoot).isDir()
            && !std::ranges::any_of(
                recentDatasets_,
                [&normalizedRoot](const QString& existing) {
                    return sameDatasetPath(existing, normalizedRoot);
                })) {
            recentDatasets_.push_back(normalizedRoot);
            if (recentDatasets_.size() == MaximumRecentDatasets) {
                break;
            }
        }
    }
    const auto storedAssociations = settings.value(
        QStringLiteral("workspace/patchAssociations")).toStringList();
    for (qsizetype index = 0; index + 1 < storedAssociations.size(); index += 2) {
        const auto datasetRoot = normalizedRecentDatasetPath(storedAssociations[index]);
        const auto workspaceRoot = normalizedRecentDatasetPath(storedAssociations[index + 1]);
        if (!datasetRoot.isEmpty() && !workspaceRoot.isEmpty()) {
            patchWorkspaceAssociations_.push_back(datasetRoot);
            patchWorkspaceAssociations_.push_back(workspaceRoot);
        }
    }
    settings.setValue(QStringLiteral("workspace/recentDatasets"), recentDatasets_);
    settings.setValue(QStringLiteral("workspace/lastDataset"), lastDataset_);
    settings.setValue(QStringLiteral("workspace/patchAssociations"),
        patchWorkspaceAssociations_);
    rebuildRecentMenu();
}

void MainWindow::saveApplicationSettings() const {
    QSettings settings{};
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState(SettingsStateVersion));
    settings.setValue(QStringLiteral("workspace/recentDatasets"), recentDatasets_);
    settings.setValue(QStringLiteral("workspace/lastDataset"), lastDataset_);
    settings.setValue(QStringLiteral("workspace/patchAssociations"),
        patchWorkspaceAssociations_);
    settings.setValue(QStringLiteral("publication/lastDirectory"),
        lastExportDirectory_);
}

void MainWindow::handleOperationCompleted(
    const WorkspaceController::Operation operation,
    const bool success,
    const bool cancelled,
    const QString& message) {
    const bool wasRestoreAttempt = restoringDataset_
        && operation == WorkspaceController::Operation::Opening;
    if (operation == WorkspaceController::Operation::Opening)
        restoringDataset_ = false;
    progressBar_->hide();
    cancelButton_->hide();
    statusBar()->showMessage(message, 8000);
    activityLogModel_->appendOperation(
        cancelled ? ActivityOutcome::Cancelled
            : success ? ActivityOutcome::Completed : ActivityOutcome::Failed,
        QStringLiteral("DatasetOperation"), message);
    syncActions();

    if (!success && !cancelled) {
        if (operation == WorkspaceController::Operation::Opening && !wasRestoreAttempt
            && !(exclusiveOperations_ && exclusiveOperations_->active())) {
            QMessageBox::warning(this, tr("Dataset could not be opened"), message);
        }
    }
    if (success && operation == WorkspaceController::Operation::Opening) {
        documentController_->closeAll();
        detachPatchWorkspace(false);
        restorePatchWorkspaceAssociation();
        QTimer::singleShot(0, this, [this]() {
            if (!restoringWorkspaceSession_ && activeDatasetOperation_)
                activeDatasetOperation_->completeRestoration();
        });
    }
    if (success && operation == WorkspaceController::Operation::Refreshing) {
        if (const auto* catalog = controller_->catalog())
            documentController_->synchronizeCatalog(*catalog);
    }
}

}  // namespace salsa::qt
