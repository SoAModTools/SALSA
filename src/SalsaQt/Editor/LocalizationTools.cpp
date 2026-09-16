#include "Editor/FeatureUi.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTableWidget>
#include <set>
namespace salsa::qt {
using namespace feature_ui;
namespace {
class LocalizationWindow final : public QDialog {
    EditorSessionPresenter *session;
    ed::LocalizationCatalog data;
    QTableWidget *table;
    QLabel *status;
    std::size_t page = 0;
    std::vector<ed::TranslationCell> draft;
    bool applyingText = false;
    bool pending() {
        if (!draft.empty())
            return true;
        for (int r = 0; r < table->rowCount(); ++r)
            for (int c = 0; c < table->columnCount(); ++c)
                if (auto *i = table->item(r, c); i && i->data(Qt::UserRole + 1).toBool())
                    return true;
        return false;
    }
    bool saved() {
        if (!pending())
            return true;
        QMessageBox::information(
            this, tr("Pending text edits"),
            tr("Apply text edits before using this action. Your edits are still in this table."));
        return false;
    }
    void reject() override {
        if (pending() && QMessageBox::question(
                             this, tr("Discard text edits?"), tr("Close without applying your text edits?"),
                             QMessageBox::Discard | QMessageBox::Cancel) != QMessageBox::Discard)
            return;
        QDialog::reject();
    }
    bool capture() {
        for (int r = 0; r < table->rowCount(); ++r)
            for (int c = 0; c < table->columnCount(); ++c) {
                auto *item = table->item(r, c);
                if (!item || !item->data(Qt::UserRole + 1).toBool())
                    continue;
                auto &message = data.messages[page * 100 + r];
                auto language = data.languages[c].name;
                auto value = item->text().toStdString();
                ed::TranslationCell edit{message.id, language, {}};
                if (!value.empty()) {
                    auto parsed = ed::LocalizationCsv::text(std::string_view(value));
                    if (!report(session, parsed))
                        return false;
                    edit.value = parsed.value();
                }
                auto found = std::ranges::find_if(draft, [&](auto &old) {
                    return old.message == edit.message && old.language == edit.language;
                });
                if (found == draft.end())
                    draft.push_back(std::move(edit));
                else
                    *found = std::move(edit);
                item->setData(Qt::UserRole + 1, false);
            }
        return true;
    }
    void draw() {
        QSignalBlocker block(table);
        table->clear();
        table->setColumnCount(static_cast<int>(data.languages.size()));
        QStringList labels;
        for (auto &l : data.languages)
            labels << q(l.name);
        table->setHorizontalHeaderLabels(labels);
        auto begin = std::min(page * 100, data.messages.size()),
             end = std::min(begin + 100, data.messages.size());
        table->setRowCount(static_cast<int>(end - begin));
        QStringList rows;
        for (auto n = begin; n < end; ++n) {
            auto &message = data.messages[n];
            rows << q(message.scriptName + ": " + message.name);
            for (std::size_t c = 0; c < data.languages.size(); ++c) {
                auto &language = data.languages[c].name;
                auto value =
                    std::ranges::find(message.translations, language, &ed::TranslationCell::language);
                auto changed = std::ranges::find_if(
                    draft, [&](auto &x) { return x.message == message.id && x.language == language; });
                const auto *cell = changed != draft.end()                ? &*changed
                                   : value != message.translations.end() ? &*value
                                                                         : nullptr;
                QString text;
                if (cell && cell->value) {
                    auto encoded = ed::LocalizationCsv::text(*cell->value);
                    if (encoded)
                        text = q(encoded.value());
                }
                auto *item = new QTableWidgetItem(text);
                item->setData(Qt::UserRole + 1, false);
                item->setToolTip(tr("Blank = missing translation. \\message = an empty message. Formatting "
                                    "commands are retained."));
                table->setItem(static_cast<int>(n - begin), static_cast<int>(c), item);
            }
        }
        table->setVerticalHeaderLabels(rows);
        status->setText(
            tr("Messages %1–%2 of %3. Changes apply as one undoable edit. %4 import/assignment issues.")
                .arg(begin)
                .arg(end)
                .arg(data.messages.size())
                .arg(data.issues.size()));
    }
    void apply(std::vector<ed::TranslationCell> edits) {
        if (edits.empty())
            return;
        ed::LocalizationChange change;
        change.translations = std::move(edits);
        if (session->busy())
            return;
        applyingText = true;
        setEnabled(false);
        session->execute(tr("Import / edit translations"), ed::FeatureEdit{std::move(change)}, data.revision);
    }
    void settings() {
        if (!saved())
            return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Languages and ByteVar[87]"));
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(
            tr("Export inclusion does not delete translations. Blank selector values remain draft settings."),
            &dialog));
        auto *grid = new QTableWidget(static_cast<int>(data.languages.size()), 3, &dialog);
        grid->setHorizontalHeaderLabels({tr("Language"), tr("ByteVar[87] value"), tr("Include in export")});
        for (int n = 0; n < grid->rowCount(); ++n) {
            auto &group = data.languages[n];
            auto found =
                std::ranges::find(data.settings.languages, group.id, &ed::LocalizationLanguage::language);
            auto *name = new QTableWidgetItem(q(group.name));
            name->setFlags(name->flags() & ~Qt::ItemIsEditable);
            grid->setItem(n, 0, name);
            grid->setItem(n, 1,
                          new QTableWidgetItem(found != data.settings.languages.end() && found->selector
                                                   ? QString::number(*found->selector)
                                                   : QString{}));
            auto *included = new QTableWidgetItem;
            included->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            included->setCheckState(
                found == data.settings.languages.end() || found->included ? Qt::Checked : Qt::Unchecked);
            grid->setItem(n, 2, included);
        }
        layout->addWidget(grid);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            std::vector<ed::LocalizationLanguage> rules;
            bool newValues = false;
            for (int n = 0; n < grid->rowCount(); ++n) {
                ed::LocalizationLanguage rule{
                    data.languages[n].id, {}, grid->item(n, 2)->checkState() == Qt::Checked};
                auto text = grid->item(n, 1)->text().trimmed();
                if (!text.isEmpty()) {
                    bool ok = false;
                    auto v = text.toUInt(&ok);
                    if (!ok || v > 255) {
                        QMessageBox::information(&dialog, tr("Selector value"),
                                                 tr("Enter a byte value from 0 to 255 or leave it blank."));
                        return;
                    }
                    rule.selector = v;
                }
                auto old = std::ranges::find(data.settings.languages, rule.language,
                                             &ed::LocalizationLanguage::language);
                newValues |=
                    rule.selector && (old == data.settings.languages.end() || old->selector != rule.selector);
                rules.push_back(rule);
            }
            if (newValues &&
                QMessageBox::warning(&dialog, tr("Game language selection"),
                                     tr("SALSA generates script-side language selection. New or remapped "
                                        "selector values require a matching game/title-screen selection "
                                        "mechanism, such as your extension. Continue?"),
                                     QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes)
                return;
            ed::LocalizationChange change;
            change.languages = std::move(rules);
            session->execute(tr("Set localization mappings and output languages"),
                             ed::FeatureEdit{std::move(change)}, data.revision);
            dialog.accept();
            accept();
        });
        dialog.exec();
    }
    void csvExport() {
        if (!saved())
            return;
        QStringList labels;
        for (auto &l : data.languages)
            labels << q(l.name);
        auto chosen = choose(this, tr("Languages to export as columns"), labels);
        if (!chosen || chosen->empty())
            return;
        std::vector<std::string> languages;
        for (auto n : *chosen)
            languages.push_back(data.languages[n].name);
        auto path = QFileDialog::getSaveFileName(this, tr("Export localization CSV"), {}, tr("CSV (*.csv)"));
        if (path.isEmpty())
            return;
        auto snapshot = data;
        session->runOperation(tr("Export localization CSV"), [snapshot, languages, path](auto stop,
                                                                                         const auto &) {
            if (stop.stop_requested())
                return core::Result<std::string>::failure(core::Diagnostic{
                    core::DiagnosticSeverity::Error, core::DiagnosticCode::Cancelled, "Cancelled"});
            auto encoded = ed::LocalizationCsv::encode(snapshot, languages);
            if (!encoded)
                return encoded;
            QSaveFile file(path);
            auto bytes = QByteArray::fromStdString(encoded.value());
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                return core::Result<std::string>::failure(
                    core::Diagnostic{core::DiagnosticSeverity::Error,
                                     core::DiagnosticCode::InvalidSctAuthoringProject, "Could not save CSV"});
            return core::Result<std::string>::success("Localization CSV exported.");
        });
    }
    void csvImport() {
        if (!saved())
            return;
        auto path = QFileDialog::getOpenFileName(this, tr("Import localization CSV"), {}, tr("CSV (*.csv)"));
        if (path.isEmpty())
            return;
        auto snapshot = data;
        auto result = std::make_shared<std::vector<ed::TranslationCell>>();
        session->runOperation(
            tr("Read localization CSV"),
            [snapshot, path, result](auto stop, const auto &) {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024)
                    return core::Result<std::string>::failure(core::Diagnostic{
                        core::DiagnosticSeverity::Error, core::DiagnosticCode::InvalidSctAuthoringProject,
                        "Unable to read CSV (limit 64 MiB)"});
                auto bytes = file.readAll();
                if (stop.stop_requested())
                    return core::Result<std::string>::failure(core::Diagnostic{
                        core::DiagnosticSeverity::Error, core::DiagnosticCode::Cancelled, "Cancelled"});
                auto decoded = ed::LocalizationCsv::decode(bytes.toStdString(), snapshot);
                if (!decoded)
                    return core::Result<std::string>::failure(decoded.diagnostics());
                *result = decoded.value();
                return core::Result<std::string>::success("CSV ready for review.");
            },
            [self = QPointer(this), result] {
                if (!self)
                    return;
                std::set<std::string> added;
                std::size_t missing = 0;
                for (auto &c : *result) {
                    missing += !c.value;
                    if (std::ranges::find(self->data.languages, c.language, &core::SctLanguageGroup::name) ==
                        self->data.languages.end())
                        added.insert(c.language);
                }
                QStringList names;
                for (auto &name : added)
                    names << q(name);
                if (QMessageBox::question(self, tr("Review CSV import"),
                                          tr("Apply %1 cells, including %2 missing translations? Unlisted "
                                             "language columns remain unchanged.\nNew language groups: "
                                             "%3\nNew groups need selector mappings before runtime export.")
                                              .arg(result->size())
                                              .arg(missing)
                                              .arg(names.join(", "))) == QMessageBox::Yes)
                    self->apply(*result);
            });
    }
    void indexedImport() {
        if (!saved())
            return;
        auto paths = QFileDialog::getOpenFileNames(this, tr("Import indexed translations only"), {},
                                                   tr("SCT scripts (*.sct)"));
        if (paths.empty())
            return;
        auto name =
            QInputDialog::getText(this, tr("Destination language"), tr("Language group (existing or new)"));
        if (name.trimmed().isEmpty())
            return;
        std::vector<std::filesystem::path> files;
        for (auto &path : paths)
            files.emplace_back(path.toStdWString());
        auto snapshot = data;
        auto result = std::make_shared<ed::IndexedTranslationReview>();
        session->runOperation(
            tr("Inspect indexed translations"),
            [files, snapshot, result](auto stop, const auto &progress) {
                auto read = ed::LocalizationSct::read(files, snapshot, stop, progress);
                if (!read)
                    return core::Result<std::string>::failure(read.diagnostics());
                *result = read.value();
                return core::Result<std::string>::success("Indexed translations ready for review.");
            },
            [self = QPointer(this), name, result] {
                if (!self)
                    return;
                QStringList labels;
                for (auto &row : result->rows)
                    labels << q(row.source) + " [" + q(row.suggestedLanguage) + "] " +
                                  (row.message.valid() ? QObject::tr("update") : QObject::tr("new message"));
                for (auto &issue : result->issues)
                    emit self->session->diagnostic(q(issue));
                auto selected =
                    choose(self,
                           QObject::tr("Select indexed strings for %1 (%2 unmatched/opaque issues)")
                               .arg(name)
                               .arg(result->issues.size()),
                           labels);
                if (!selected)
                    return;
                std::vector<ed::TranslationCell> cells;
                std::set<std::pair<core::SctScriptId, std::string>> seen;
                for (auto n : *selected) {
                    auto &r = result->rows[n];
                    if (!seen.emplace(r.script, r.message.valid() ? std::to_string(r.message.value) : r.key)
                             .second) {
                        QMessageBox::information(
                            self, QObject::tr("Ambiguous variants"),
                            QObject::tr("Choose only one source translation for each logical message."));
                        return;
                    }
                    cells.push_back({r.message, name.toStdString(), r.value, r.script, r.key});
                }
                self->apply(std::move(cells));
            });
    }

  public:
    LocalizationWindow(EditorSessionPresenter *s, QWidget *parent, ed::LocalizationCatalog c,
                       std::function<void()> groups)
        : QDialog(parent), session(s), data(std::move(c)) {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(tr("Localizations"));
        resize(1000, 650);
        auto *layout = new QVBoxLayout(this);
        auto *bar = new QToolBar(this);
        layout->addWidget(bar);
        bar->addAction(tr("Discover / link strings"), [this] {
            if (!saved())
                return;
            ed::LocalizationChange c;
            c.discover = true;
            session->execute(tr("Recognize logical messages"), ed::FeatureEdit{c}, data.revision);
            accept();
        });
        bar->addAction(tr("Recognize language branches…"), [this] {
            if (!saved())
                return;
            auto b = session->backend();
            auto r = data.revision;
            background(
                this, [=] { return b->localizations(r, true); },
                [this, r](auto result) {
                    if (r != session->revision() || !report(session, result))
                        return;
                    QStringList labels;
                    for (auto &c : result.value().candidates)
                        labels << tr("Action %1: %2").arg(c.first.value).arg(q(c.second));
                    auto chosen = choose(this, tr("Organize equivalent localized sequences"), labels);
                    if (!chosen || chosen->empty())
                        return;
                    ed::LocalizationChange edit;
                    std::set<core::SctActionId> unique;
                    for (int n : *chosen)
                        if (unique.insert(result.value().candidates[n].first).second)
                            edit.promote.push_back(result.value().candidates[n].first);
                    session->execute(tr("Organize localized sequences"), ed::FeatureEdit{edit}, r);
                    accept();
                });
        });
        bar->addAction(tr("Expand localization to instructions…"), [this] {
            if (!saved())
                return;
            QStringList labels;
            for (auto &block : data.settings.blocks)
                labels << tr("Localized action %1").arg(block.action.value);
            auto selected =
                choose(this, tr("Expand to explicit language branches (automatic localization routing ends)"),
                       labels);
            if (!selected || selected->empty())
                return;
            ed::LocalizationChange edit;
            for (int n : *selected)
                edit.expand.push_back(data.settings.blocks[n].action);
            session->execute(tr("Expand localized code"), ed::FeatureEdit{edit}, data.revision);
            accept();
        });
        bar->addAction(tr("Mappings / output languages…"), [this] { settings(); });
        bar->addAction(tr("Add language…"), [this] {
            if (!saved())
                return;
            auto name = QInputDialog::getText(this, tr("New language"), tr("Language name"));
            if (name.trimmed().isEmpty())
                return;
            session->execute(tr("Add language"),
                             ed::FeatureEdit{ed::WriteLanguage{{{}, name.toStdString(), {}}}}, data.revision);
            accept();
        });
        auto *files = new QToolBar(this);
        layout->addWidget(files);
        files->addAction(tr("Export CSV…"), [this] { csvExport(); });
        files->addAction(tr("Import CSV…"), [this] { csvImport(); });
        files->addAction(tr("Import indexed SCT strings…"), [this] { indexedImport(); });
        files->addAction(tr("String groups / assignments…"), [this, groups] {
            if (!saved())
                return;
            accept();
            groups();
        });
        table = new QTableWidget(this);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        layout->addWidget(table);
        connect(table, &QTableWidget::itemChanged, this, [](QTableWidgetItem *item) {
            QSignalBlocker block(item->tableWidget());
            item->setData(Qt::UserRole + 1, true);
        });
        status = new QLabel(this);
        layout->addWidget(status);
        auto *footer = new QToolBar(this);
        layout->addWidget(footer);
        footer->addAction(tr("Previous"), [this] {
            if (!capture())
                return;
            if (page)
                --page;
            draw();
        });
        footer->addAction(tr("Next"), [this] {
            if (!capture())
                return;
            if ((page + 1) * 100 < data.messages.size())
                ++page;
            draw();
        });
        footer->addAction(tr("Apply text edits"), [this] {
            if (capture())
                apply(draft);
        });
        connect(session, &EditorSessionPresenter::sessionChanged, this, &QDialog::reject);
        connect(session, &EditorSessionPresenter::changed, this, [this] {
            if (applyingText) {
                applyingText = false;
                accept();
            }
        });
        connect(session, &EditorSessionPresenter::availabilityChanged, this, [this] {
            if (applyingText && !session->busy()) {
                applyingText = false;
                setEnabled(true);
            }
        });
        draw();
    }
};
} // namespace
void LanguageTools::localizations() {
    if (!session_->backend() || session_->busy())
        return;
    auto b = session_->backend();
    auto r = session_->revision();
    background(
        this, [=] { return b->localizations(r); },
        [this, r](auto result) {
            if (r != session_->revision() || !report(session_, result))
                return;
            (new LocalizationWindow(session_, host_, std::move(result).takeValue(), [this] {
                open();
            }))->show();
        });
}
} // namespace salsa::qt
