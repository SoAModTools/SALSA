#include "Editor/FeatureUi.h"
#include "Sct/SctMessageEditorWidget.h"
#include <QSplitter>
#include <QSignalBlocker>
namespace salsa::qt {
using namespace feature_ui;
LanguageTools::LanguageTools(EditorSessionPresenter *s, QWidget *h) : QObject(h), session_(s), host_(h) {
    action = new QAction(tr("Localizations…"), this);
    connect(action, &QAction::triggered, this, [this] { localizations(); });
    auto available = [this] { action->setEnabled(session_->backend() && !session_->busy()); };
    connect(session_, &EditorSessionPresenter::sessionChanged, this, available);
    connect(session_, &EditorSessionPresenter::availabilityChanged, this, available);
    available();
}
void LanguageTools::open() {
    inventory(session_, this, [this](ed::FeatureInventory data) {
        QDialog dialog(host_);
        dialog.setWindowTitle(tr("Languages and text groups"));
        dialog.resize(780, 520);
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(
            tr("Language groups are user-defined. Text may belong to several groups or remain unassigned."),
            &dialog));
        auto *list = new QListWidget(&dialog);
        list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        for (const auto &t : data.texts) {
            QStringList groups;
            for (auto id : t.languages)
                for (const auto &g : data.languages)
                    if (g.id == id)
                        groups << q(g.name);
            list->addItem(q(t.name) + " — " + (groups.empty() ? tr("Unassigned") : groups.join(", ")) +
                          (t.footer ? tr(" [footer]") : QString{}));
        }
        layout->addWidget(list);
        auto selected = [&] {
            std::vector<core::SctTextResourceId> ids;
            for (auto *item : list->selectedItems())
                ids.push_back(data.texts[list->row(item)].id);
            return ids;
        };
        auto *menu = new QMenu(&dialog);
        auto *bar = new QToolBar(&dialog);
        bar->addAction(tr("Text and group actions"),
                       [=] { menu->exec(bar->mapToGlobal(QPoint(0, bar->height()))); });
        layout->addWidget(bar);
        menuAction(menu, tr("Edit selected texts as a batch…"), &dialog, [&] {
            auto ids = selected();
            if (!ids.empty()) {
                dialog.accept();
                batch(ids);
            }
        });
        menuAction(menu, tr("Open selected text / uses…"), &dialog, [&] {
            auto ids = selected();
            if (ids.empty())
                return;
            session_->navigate(ids.front());
            auto b = session_->backend();
            background(
                &dialog, [=] { return b->textUses(data.revision, ids.front()); },
                [&, r = data.revision](auto result) {
                    if (r != session_->revision() || !report(session_, result))
                        return;
                    QStringList labels;
                    for (auto site : result.value())
                        labels << tr("Action %1 parameter %2")
                                      .arg(site.action.value)
                                      .arg(site.operand.schemaIndex);
                    auto chosen = choose(&dialog, tr("Text uses"), labels, false);
                    if (chosen && !chosen->empty())
                        session_->navigate(result.value()[chosen->front()].action);
                });
        });
        menuAction(menu, tr("Create language group…"), &dialog, [&] {
            if (auto label = name(&dialog, tr("Language group")))
                commit(session_, dialog, data.revision, ed::WriteLanguage{{{}, *label, {}}},
                       tr("Create language group"));
        });
        auto language = [&]() -> std::optional<core::SctLanguageGroup> {
            QStringList labels;
            for (const auto &g : data.languages)
                labels << q(g.name);
            auto selected = choose(&dialog, tr("Choose language group"), labels, false);
            if (!selected || selected->empty())
                return {};
            return data.languages[selected->front()];
        };
        menuAction(menu, tr("Rename language group…"), &dialog, [&] {
            auto g = language();
            if (!g)
                return;
            if (auto label = name(&dialog, tr("Language name"), g->name)) {
                g->name = *label;
                commit(session_, dialog, data.revision, ed::WriteLanguage{*g}, tr("Rename language group"));
            }
        });
        menuAction(menu, tr("Remove unused language group…"), &dialog, [&] {
            if (auto g = language())
                commit(session_, dialog, data.revision, ed::RemoveLanguage{g->id},
                       tr("Remove language group"));
        });
        menuAction(menu, tr("Assign groups (none = unassigned)…"), &dialog, [&] {
            auto ids = selected();
            if (ids.empty())
                return;
            QStringList labels;
            for (const auto &g : data.languages)
                labels << q(g.name);
            auto chosen = choose(&dialog, tr("Select all memberships; select none to unassign"), labels);
            if (!chosen)
                return;
            std::vector<core::SctLanguageGroupId> groups;
            for (int i : *chosen)
                groups.push_back(data.languages[i].id);
            commit(session_, dialog, data.revision, ed::AssignTextGroups{ids, groups},
                   tr("Assign language groups"));
        });
        menuAction(menu, tr("Rescan naming suggestions…"), &dialog, [&] {
            auto b = session_->backend();
            background(
                &dialog, [=] { return b->suggestLanguages(data.revision, data.script); },
                [&](auto result) {
                    if (data.revision != session_->revision() || !report(session_, result))
                        return;
                    QStringList labels;
                    std::vector<core::SctTextResourceId> ids;
                    for (const auto &s : result.value())
                        if (!s.key.empty()) {
                            labels
                                << tr("Text %1 → %2: %3").arg(s.text.value).arg(q(s.key), q(s.explanation));
                            ids.push_back(s.text);
                        }
                    auto chosen = choose(
                        &dialog, tr("Apply selected suggestions (explicit assignments retained)"), labels);
                    if (!chosen || chosen->empty())
                        return;
                    std::vector<core::SctTextResourceId> apply;
                    for (int i : *chosen)
                        apply.push_back(ids[i]);
                    commit(session_, dialog, data.revision, ed::ApplyLanguageSuggestions{data.script, apply},
                           tr("Apply naming suggestions"));
                });
        });
        menuAction(menu, tr("Link selected variants as logical text…"), &dialog, [&] {
            auto ids = selected();
            if (ids.empty())
                return;
            if (auto label = name(&dialog, tr("Logical text")))
                commit(session_, dialog, data.revision,
                       ed::WriteLogicalText{{{}, data.script, *label, ids, {}}}, tr("Link logical text"));
        });
        menuAction(menu, tr("Manage logical text…"), &dialog, [&] {
            QStringList labels;
            for (const auto &l : data.logical)
                labels << q(l.name);
            auto chosen = choose(&dialog, tr("Logical text"), labels, false);
            if (!chosen || chosen->empty())
                return;
            auto logical = data.logical[chosen->front()];
            auto operation =
                choose(&dialog, tr("Logical text action"),
                       {tr("Rename / replace with selected variants"), tr("Remove link")}, false);
            if (!operation || operation->empty())
                return;
            if (operation->front() == 1) {
                commit(session_, dialog, data.revision, ed::RemoveLogicalText{logical.id},
                       tr("Remove logical link"));
                return;
            }
            auto label = name(&dialog, tr("Logical text"), logical.name);
            if (!label)
                return;
            logical.name = *label;
            auto ids = selected();
            if (!ids.empty())
                logical.variants = ids;
            commit(session_, dialog, data.revision, ed::WriteLogicalText{logical}, tr("Edit logical text"));
        });
        menuAction(menu, tr("Split selected shared text for a language…"), &dialog, [&] {
            auto ids = selected();
            if (ids.size() != 1)
                return;
            auto group = language();
            if (!group)
                return;
            auto label = name(&dialog, tr("New indexed string name"));
            if (!label)
                return;
            auto b = session_->backend();
            background(
                &dialog, [=] { return b->textUses(data.revision, ids.front()); },
                [&, ids, group, label](auto result) {
                    if (data.revision != session_->revision() || !report(session_, result))
                        return;
                    QStringList labels;
                    for (auto site : result.value())
                        labels << tr("Action %1 parameter %2")
                                      .arg(site.action.value)
                                      .arg(site.operand.schemaIndex);
                    auto chosen = choose(&dialog, tr("Uses to retarget to the new string"), labels);
                    if (!chosen || chosen->empty())
                        return;
                    std::vector<ed::FeatureSite> uses;
                    for (int i : *chosen)
                        uses.push_back(result.value()[i]);
                    commit(session_, dialog, data.revision,
                           ed::SplitText{ids.front(), group->id, *label, uses}, tr("Split shared text"));
                });
        });
        menuAction(menu, tr("Insert selected text using an existing instruction…"), &dialog, [&] {
            auto ids = selected();
            if (ids.size() != 1)
                return;
            auto prototype = site(session_, &dialog, tr("Choose the prototype instruction and text operand"));
            if (!prototype)
                return;
            auto before = feature_ui::action(session_, &dialog, tr("Insert before this action"));
            if (!before)
                return;
            std::optional<ed::FeatureSite> incoming;
            if (QMessageBox::question(&dialog, tr("Incoming edge"),
                                      tr("Retarget one incoming edge to the inserted instruction?")) ==
                QMessageBox::Yes) {
                incoming = site(session_, &dialog, tr("Choose the incoming target operand"));
                if (!incoming)
                    return;
            }
            commit(session_, dialog, data.revision,
                   ed::InsertTextUses{{{*prototype, *before, ids.front(), incoming}}}, tr("Insert text use"));
        });
        dialog.exec();
    });
}
void LanguageTools::batch(std::vector<core::SctTextResourceId> ids) {
    auto b = session_->backend();
    auto r = session_->revision();
    background(
        this,
        [=] {
            std::vector<ed::TextDraft> rows;
            for (auto id : ids) {
                auto result = b->text(r, id);
                if (!result)
                    return core::Result<std::vector<ed::TextDraft>>::failure(result.diagnostics());
                rows.push_back(std::move(result).takeValue());
            }
            return core::Result<std::vector<ed::TextDraft>>::success(std::move(rows));
        },
        [this, r](auto result) {
            if (r != session_->revision() || !report(session_, result))
                return;
            auto rows = std::move(result).takeValue();
            QDialog dialog(host_);
            dialog.setWindowTitle(tr("Batch text edit — one undo step"));
            dialog.resize(1000, 620);
            auto *layout = new QVBoxLayout(&dialog);
            auto *split = new QSplitter(&dialog);
            auto *list = new QListWidget(split);
            auto *editor = new SctMessageEditorWidget(split);
            split->addWidget(list);
            split->addWidget(editor);
            layout->addWidget(split);
            for (const auto &row : rows)
                list->addItem(q(row.name));
            int active = -1;
            connect(list, &QListWidget::currentRowChanged, &dialog, [&](int index) {
                if (!editor->flushPending()) {
                    QSignalBlocker block(list);
                    list->setCurrentRow(active);
                    return;
                }
                active = index;
                if (index >= 0)
                    editor->bindValue(rows[index].value, rows[index].kind,
                                      rows[index].footer ? spice::sct::SctTextStorage::Footer
                                                         : spice::sct::SctTextStorage::IndexedSection,
                                      [&, index](auto value) {
                                          rows[index].value = std::move(value);
                                          return true;
                                      });
            });
            list->setCurrentRow(0);
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
            layout->addWidget(buttons);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
                if (!editor->flushPending())
                    return;
                ed::WriteTextBatch edit;
                for (const auto &row : rows)
                    edit.edits.push_back({row.id, row.value});
                commit(session_, dialog, r, std::move(edit), tr("Edit text batch"));
            });
            dialog.exec();
        });
}
} // namespace salsa::qt
