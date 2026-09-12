#include "SctLanguageEditorDialog.h"
#include "SctMessageEditorWidget.h"
#include "SalsaCore/Authoring/SctTextAuthoring.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <map>
#include <set>

namespace salsa::qt {
namespace {
using namespace core;
namespace sct = spice::sct;
void failure(QWidget *p, const std::vector<Diagnostic> &ds) {
    QString text;
    for (const auto &d : ds)
        text += QString::fromStdString(d.message) + '\n';
    QMessageBox::warning(p, QObject::tr("Language groups"), text);
}
QString label(const SctLanguageGroup &g) {
    return QString::fromStdString(g.name) + QString(" [%1]").arg(g.id.value);
}
int pick(QWidget *p, const QString &title, const QString &prompt, const QStringList &options) {
    if (options.empty())
        return -1;
    bool ok = false;
    auto value = QInputDialog::getItem(p, title, prompt, options, 0, false, &ok);
    return ok ? options.indexOf(value) : -1;
}
} // namespace
void showSctLanguageEditor(SctDocumentController &controller, const AssetLocator &locator, QWidget *parent) {
    auto state = controller.authoringState();
    if (!state)
        return;
    SctScriptId script;
    for (const auto &s : state->project.scripts)
        if (state->project.find(s.baseline)->source.locator == locator)
            script = s.id;
    if (!script.valid())
        return;
    auto discovered = controller.executeAuthoringCommand(
        state->project.revision, "Discover language-name groups",
        [=](const SctAuthoringState &s) { return SctTextAuthoring::discover(s, script); });
    if (!discovered) {
        failure(parent, discovered.diagnostics());
        return;
    }
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Language Groups and Text"));
    dialog.resize(1180, 800);
    auto *layout = new QVBoxLayout(&dialog);
    auto *help = new QLabel(
        QObject::tr("Languages are your groups. Name patterns suggest initial assignments; explicit assignments "
                    "survive rescanning. Grouping changes neither encoding nor language-selector instructions."));
    help->setWordWrap(true);
    layout->addWidget(help);
    auto *module = new QComboBox;
    layout->addWidget(module);
    auto *logical = new QComboBox;
    layout->addWidget(logical);
    auto *groups = new QListWidget;
    groups->setSelectionMode(QAbstractItemView::ExtendedSelection);
    groups->setMaximumHeight(110);
    layout->addWidget(groups);
    auto *list = new QTreeWidget;
    list->setColumnCount(5);
    list->setHeaderLabels({QObject::tr("String / ID"), QObject::tr("Language groups"), QObject::tr("Source group"),
                           QObject::tr("Correspondence suggestion"), QObject::tr("Evidence")});
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layout->addWidget(list, 1);
    const auto addButton = [&](const QString &text) {
        auto *b = new QPushButton(text);
        layout->addWidget(b);
        return b;
    };
    auto *create = addButton(QObject::tr("Create language group…"));
    auto *rename = addButton(QObject::tr("Rename selected language group…"));
    auto *assign = addButton(QObject::tr("Assign selected strings to selected groups (none = unassigned)"));
    auto *rescan = addButton(QObject::tr("Rescan naming patterns"));
    auto *link = addButton(QObject::tr("Link selected strings as one logical text…"));
    auto *edit = addButton(QObject::tr("Edit selected texts together…"));
    auto *newText = addButton(QObject::tr("Create text resource…"));
    auto *split = addButton(QObject::tr("Split one language from shared text…"));
    auto *insert = addButton(QObject::tr("Insert selected texts at explicit action locations…"));
    const auto selectedTexts = [&] {
        std::vector<SctTextResourceId> ids;
        for (const auto *item : list->selectedItems())
            ids.push_back({item->data(0, Qt::UserRole).toULongLong()});
        return ids;
    };
    const auto selectedGroups = [&] {
        std::vector<SctLanguageGroupId> ids;
        for (const auto *item : groups->selectedItems())
            ids.push_back({item->data(Qt::UserRole).toULongLong()});
        return ids;
    };
    const auto encoding = [&]() -> std::optional<sct::SctTextEncoding> {
        const auto *b = state->project.find(state->project.find(script)->baseline);
        if (b->textConvention)
            return sct::sctTextEncodingFor(*b->textConvention);
        QStringList names;
        for (const auto &c : sct::kSctKnownTextConventions)
            names << QString::fromStdString(std::string(c.stableName));
        const auto at = pick(&dialog, QObject::tr("Text encoding"),
                             QObject::tr("Select the output text convention; independent of language groups"), names);
        return at < 0 ? std::nullopt : sct::sctTextEncodingFor(sct::kSctKnownTextConventions[at].convention);
    };
    const auto refresh = [&] {
        state = controller.authoringState();
        const auto selectedModule = module->currentData().toULongLong();
        {
            QSignalBlocker block(module);
            module->clear();
            module->addItem(QObject::tr("All script text"), qulonglong(0));
            for (const auto &m : state->project.modules)
                if (m.script == script)
                    module->addItem(QString::fromStdString(m.name), qulonglong(m.id.value));
            const auto i = module->findData(selectedModule);
            if (i >= 0)
                module->setCurrentIndex(i);
        }
        {
            QSignalBlocker block(logical);
            logical->clear();
            logical->addItem(QObject::tr("Select a logical text to select its variants"), qulonglong(0));
            for (const auto &t : state->project.logicalTexts)
                if (t.script == script && (!selectedModule || t.module == SctModuleId{selectedModule}))
                    logical->addItem(QString::fromStdString(t.name), qulonglong(t.id.value));
        }
        groups->clear();
        for (const auto &g : state->project.languageGroups) {
            auto *item = new QListWidgetItem(label(g), groups);
            item->setData(Qt::UserRole, qulonglong(g.id.value));
        }
        list->clear();
        auto working = SctAuthoringMaterializer::workingState(state->project, state->programs, script);
        if (!working)
            return;
        std::set<sct::SctTextEntityId> moduleTargets;
        if (selectedModule) {
            const auto *m = state->project.find(SctModuleId{selectedModule});
            for (const auto &section : working.value().document->sections)
                if (std::ranges::find(m->sections, section.id) != m->sections.end())
                    if (const auto *code = std::get_if<sct::SctScriptSectionContent>(&section.content))
                        for (const auto &action : code->instructions) {
                            const auto add = [&](const auto &parameters) {
                                for (const auto &p : parameters) {
                                    if (const auto *r = std::get_if<sct::SctStringReference>(&p.value))
                                        moduleTargets.insert(r->target);
                                    if (const auto *r = std::get_if<sct::SctSupplementaryTextReference>(&p.value))
                                        moduleTargets.insert(r->target);
                                }
                            };
                            add(action.fixedParameters);
                            for (const auto &g : action.repeatedParameterGroups)
                                add(g.parameters);
                        }
        }
        for (const auto &t : state->project.texts)
            if (t.script == script) {
                if (selectedModule) {
                    const auto *m = state->project.find(SctModuleId{selectedModule});
                    bool owned = moduleTargets.contains(t.target);
                    for (const auto &logical : state->project.logicalTexts)
                        if (logical.module == m->id &&
                            std::ranges::find(logical.variants, t.id) != logical.variants.end())
                            owned = true;
                    if (!owned)
                        continue;
                }
                QStringList languages;
                for (auto id : t.languages)
                    languages << label(*state->project.find(id));
                QString evidence =
                    t.explicitAssignment ? QObject::tr("Explicit assignment") : QObject::tr("Inferred / unassigned");
                for (const auto &e : t.evidence)
                    evidence += "; " + QString::fromStdString(e.description);
                auto *item =
                    new QTreeWidgetItem(list, {QString::fromStdString(t.sourceName) + QString(" [%1]").arg(t.id.value),
                                               languages.join(", "), QString::fromStdString(t.sourceGroup),
                                               QString::fromStdString(t.correspondenceKey), evidence});
                item->setData(0, Qt::UserRole, qulonglong(t.id.value));
            }
        for (int i = 0; i < 4; ++i)
            list->resizeColumnToContents(i);
    };
    const auto command = [&](std::string name, const SctAuthoringSession::Command &action) {
        auto result = controller.executeAuthoringCommand(state->project.revision, std::move(name), action);
        if (!result)
            failure(&dialog, result.diagnostics());
        else
            refresh();
    };
    QObject::connect(create, &QPushButton::clicked, &dialog, [&] {
        bool ok;
        const auto name = QInputDialog::getText(&dialog, QObject::tr("Language group"), QObject::tr("Your group name"),
                                                QLineEdit::Normal, {}, &ok);
        if (ok)
            command("Create language group", [&](const SctAuthoringState &s) {
                return SctTextAuthoring::createLanguage(s, name.toStdString());
            });
    });
    QObject::connect(rename, &QPushButton::clicked, &dialog, [&] {
        const auto ids = selectedGroups();
        if (ids.size() != 1)
            return;
        bool ok;
        const auto name =
            QInputDialog::getText(&dialog, QObject::tr("Language group"), QObject::tr("Group name"), QLineEdit::Normal,
                                  QString::fromStdString(state->project.find(ids.front())->name), &ok);
        if (ok)
            command("Rename language group", [&](const SctAuthoringState &s) {
                return SctTextAuthoring::renameLanguage(s, ids.front(), name.toStdString());
            });
    });
    QObject::connect(assign, &QPushButton::clicked, &dialog, [&] {
        const auto texts = selectedTexts();
        const auto languages = selectedGroups();
        if (texts.empty())
            return;
        command("Assign strings to language groups",
                [&](const SctAuthoringState &s) { return SctTextAuthoring::assign(s, texts, languages); });
    });
    QObject::connect(rescan, &QPushButton::clicked, &dialog, [&] {
        command("Rescan language-name groups",
                [&](const SctAuthoringState &s) { return SctTextAuthoring::discover(s, script); });
    });
    QObject::connect(link, &QPushButton::clicked, &dialog, [&] {
        auto texts = selectedTexts();
        if (texts.empty())
            return;
        bool ok;
        auto name = QInputDialog::getText(&dialog, QObject::tr("Logical text"),
                                          QObject::tr("Name for these explicitly corresponding texts"),
                                          QLineEdit::Normal, {}, &ok);
        if (!ok)
            return;
        auto owner = module->currentData().toULongLong();
        command("Link logical text", [&](const SctAuthoringState &s) {
            return SctTextAuthoring::link(s, script, name.toStdString(), texts,
                                          owner ? std::optional{SctModuleId{owner}} : std::nullopt);
        });
    });
    QObject::connect(logical, &QComboBox::currentIndexChanged, &dialog, [&] {
        const auto *selected = state->project.find(SctLogicalTextId{logical->currentData().toULongLong()});
        list->clearSelection();
        if (!selected)
            return;
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            auto *item = list->topLevelItem(i);
            item->setSelected(
                std::ranges::find(selected->variants, SctTextResourceId{item->data(0, Qt::UserRole).toULongLong()}) !=
                selected->variants.end());
        }
    });
    QObject::connect(module, &QComboBox::currentIndexChanged, &dialog, [&] { refresh(); });
    QObject::connect(edit, &QPushButton::clicked, &dialog, [&] {
        const auto texts = selectedTexts();
        if (texts.empty())
            return;
        const auto enc = encoding();
        if (!enc)
            return;
        QDialog batch(&dialog);
        batch.setWindowTitle(QObject::tr("Edit texts — apply as one command"));
        batch.resize(1000, 680);
        auto *l = new QVBoxLayout(&batch);
        auto *tabs = new QTabWidget;
        l->addWidget(tabs);
        std::map<SctTextResourceId, sct::SctTextValue> values;
        std::vector<SctMessageEditorWidget *> editors;
        for (auto id : texts) {
            const auto *t = state->project.find(id);
            auto v = SctTextAuthoring::inspect(*state, id);
            if (!v) {
                failure(&dialog, v.diagnostics());
                return;
            }
            values[id] = v.value().value;
            auto *editor = new SctMessageEditorWidget;
            editors.push_back(editor);
            tabs->addTab(editor, QString::fromStdString(t->sourceName));
            editor->setCommitHandler([&, id](const AssetLocator &, const SctMessageTarget &,
                                             const SctMessageDraft &draft, SctMessageEditKind) {
                auto result = SctMessageAuthoringProfile::materialize(draft);
                if (!result.succeeded())
                    return false;
                values[id] = *result.message;
                return true;
            });
            editor->setPlainTextCommitHandler([&, id](const AssetLocator &, const SctTextTarget &, std::string text) {
                values[id] = sct::SctPlainText{std::move(text)};
                return true;
            });
            editor->setTextValueCommitHandler([&, id](const AssetLocator &, const SctTextTarget &,
                                                      sct::SctTextValue value, std::string,
                                                      std::optional<SctTextRepairProvenance>) {
                values[id] = std::move(value);
                return true;
            });
            if (!editor->bindText(locator, t->target, v.value().value, v.value().kind, v.value().storage)) {
                QMessageBox::information(
                    &dialog, QObject::tr("Text"),
                    QObject::tr("This text needs repair in the ordinary text editor before compound editing."));
                return;
            }
        }
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        l->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &batch, [&] {
            for (auto *editor : editors)
                if (!editor->flushPending())
                    return;
            batch.accept();
        });
        QObject::connect(buttons, &QDialogButtonBox::rejected, &batch, &QDialog::reject);
        if (batch.exec() != QDialog::Accepted)
            return;
        std::vector<SctTextEdit> changes;
        for (auto &[id, value] : values)
            changes.push_back({id, std::move(value)});
        command("Edit logical text variants",
                [&](const SctAuthoringState &s) { return SctTextAuthoring::edit(s, changes, *enc); });
    });
    QObject::connect(newText, &QPushButton::clicked, &dialog, [&] {
        const auto enc = encoding();
        if (!enc)
            return;
        const auto kind =
            pick(&dialog, QObject::tr("New text"), QObject::tr("Storage and text kind"),
                 {QObject::tr("Indexed message"), QObject::tr("Footer message"), QObject::tr("Footer plain text")});
        if (kind < 0)
            return;
        bool ok;
        const auto name = QInputDialog::getText(&dialog, QObject::tr("New text"),
                                                QObject::tr("Section name (indexed) or descriptive name (footer)"),
                                                QLineEdit::Normal, {}, &ok);
        if (!ok)
            return;
        const auto text =
            QInputDialog::getMultiLineText(&dialog, QObject::tr("New text"), QObject::tr("Initial text"), {}, &ok);
        if (!ok)
            return;
        sct::SctTextValue value =
            kind == 2 ? sct::SctTextValue{sct::SctPlainText{text.toStdString()}}
                      : sct::SctTextValue{sct::SctMessage{{}, {{sct::SctTextChunk{text.toStdString()}}}}};
        const auto languages = selectedGroups();
        command("Create language text", [&](const SctAuthoringState &s) {
            return SctTextAuthoring::createText(
                s, script, name.toStdString(), kind == 2 ? sct::SctTextKind::PlainString : sct::SctTextKind::SctString,
                kind == 0 ? sct::SctTextStorage::IndexedSection : sct::SctTextStorage::Footer, value, languages, *enc);
        });
    });
    QObject::connect(split, &QPushButton::clicked, &dialog, [&] {
        const auto texts = selectedTexts();
        if (texts.size() != 1)
            return;
        const auto *t = state->project.find(texts.front());
        QStringList names;
        for (auto id : t->languages)
            names << label(*state->project.find(id));
        const auto selected = pick(&dialog, QObject::tr("Split shared text"),
                                   QObject::tr("Language that should receive its own resource"), names);
        if (selected < 0)
            return;
        bool ok;
        auto name = QInputDialog::getText(&dialog, QObject::tr("Split shared text"),
                                          QObject::tr("Unique indexed section name (ignored for footer)"),
                                          QLineEdit::Normal, {}, &ok);
        if (!ok)
            return;
        auto view = SctTextAuthoring::inspect(*state, t->id);
        if (!view)
            return;
        QDialog choices(&dialog);
        auto *l = new QVBoxLayout(&choices);
        l->addWidget(new QLabel(QObject::tr("Select the exact uses to retarget. None keeps the new variant unbound.")));
        auto *uses = new QListWidget;
        uses->setSelectionMode(QAbstractItemView::ExtendedSelection);
        l->addWidget(uses);
        for (const auto &site : view.value().uses)
            uses->addItem(QString("Instruction %1 / operand %2 / group %3")
                              .arg(site.instruction.value())
                              .arg(site.parameter.schemaIndex)
                              .arg(site.parameter.repeatedGroupOrdinal
                                       ? QString::number(*site.parameter.repeatedGroupOrdinal)
                                       : "none"));
        auto *b = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        l->addWidget(b);
        QObject::connect(b, &QDialogButtonBox::accepted, &choices, &QDialog::accept);
        QObject::connect(b, &QDialogButtonBox::rejected, &choices, &QDialog::reject);
        if (choices.exec() != QDialog::Accepted)
            return;
        std::vector<sct::SctParameterSite> sites;
        for (auto *item : uses->selectedItems())
            sites.push_back(view.value().uses[uses->row(item)]);
        const auto id = t->id;
        const auto language = t->languages[selected];
        command("Split shared language text", [&](const SctAuthoringState &s) {
            return SctTextAuthoring::split(s, id, language, name.toStdString(), sites);
        });
    });
    QObject::connect(insert, &QPushButton::clicked, &dialog, [&] {
        const auto texts = selectedTexts();
        if (texts.empty())
            return;
        const auto enc = encoding();
        if (!enc)
            return;
        auto working = SctAuthoringMaterializer::workingState(state->project, state->programs, script);
        if (!working) {
            failure(&dialog, working.diagnostics());
            return;
        }
        struct Action {
            sct::SctSectionId section;
            sct::SctDocumentInstruction instruction;
        };
        std::vector<Action> actions;
        QStringList anchors;
        for (const auto &section : working.value().document->sections)
            if (const auto *code = std::get_if<sct::SctScriptSectionContent>(&section.content))
                for (const auto &a : code->instructions) {
                    actions.push_back({section.id, a});
                    anchors << QString::fromStdString(section.nameBytes) +
                                   QString(" / instruction %1 / opcode %2").arg(a.id.value()).arg(a.opcode);
                }
        struct Placement {
            std::size_t anchor, prototype;
            sct::SctParameterAddress operand;
            SctTextResourceId text;
            std::optional<std::pair<std::size_t, sct::SctParameterAddress>> incoming;
        };
        std::vector<Placement> selections;
        for (auto id : texts) {
            const auto *t = state->project.find(id);
            QStringList templates;
            std::vector<std::pair<std::size_t, sct::SctParameterAddress>> operands;
            for (std::size_t i = 0; i < actions.size(); ++i) {
                const auto add = [&](const auto &ps, std::optional<std::uint32_t> group) {
                    for (const auto &p : ps)
                        if (std::holds_alternative<sct::SctStringId>(t->target)
                                ? std::holds_alternative<sct::SctStringReference>(p.value)
                                : std::holds_alternative<sct::SctSupplementaryTextReference>(p.value)) {
                            operands.push_back({i, {p.schemaIndex, group}});
                            templates << anchors[i] + QString(" / operand %1 / group %2")
                                                          .arg(p.schemaIndex)
                                                          .arg(group ? QString::number(*group) : "none");
                        }
                };
                add(actions[i].instruction.fixedParameters, {});
                for (std::uint32_t g = 0; g < actions[i].instruction.repeatedParameterGroups.size(); ++g)
                    add(actions[i].instruction.repeatedParameterGroups[g].parameters, g);
            }
            const auto source =
                pick(&dialog, QString::fromStdString(t->sourceName),
                     QObject::tr("Operation template; all other parameters and timing are copied"), templates);
            if (source < 0)
                return;
            const auto at =
                pick(&dialog, QString::fromStdString(t->sourceName),
                     QObject::tr("Insert before this instruction inside the intended language route"), anchors);
            if (at < 0)
                return;
            QStringList routes{QObject::tr("Physical position only; keep all incoming reference targets")};
            std::vector<std::pair<std::size_t, sct::SctParameterAddress>> incoming;
            for (std::size_t i = 0; i < actions.size(); ++i) {
                const auto add = [&](const auto &ps, std::optional<std::uint32_t> group) {
                    for (const auto &p : ps)
                        if (const auto *r = std::get_if<sct::SctInstructionReference>(&p.value);
                            r && r->target == actions[at].instruction.id) {
                            incoming.push_back({i, {p.schemaIndex, group}});
                            routes << anchors[i] + QString(" / operand %1 / group %2")
                                                       .arg(p.schemaIndex)
                                                       .arg(group ? QString::number(*group) : "none");
                        }
                };
                add(actions[i].instruction.fixedParameters, {});
                for (std::uint32_t g = 0; g < actions[i].instruction.repeatedParameterGroups.size(); ++g)
                    add(actions[i].instruction.repeatedParameterGroups[g].parameters, g);
            }
            const auto route = pick(&dialog, QString::fromStdString(t->sourceName),
                                    QObject::tr("Also redirect one incoming route to the inserted operation? Physical "
                                                "fallthrough still includes the insertion."),
                                    routes);
            if (route < 0)
                return;
            selections.push_back({static_cast<std::size_t>(at), operands[source].first, operands[source].second, id,
                                  route ? std::optional{incoming[route - 1]} : std::nullopt});
        }
        command("Insert logical text variants", [&](const SctAuthoringState &s) -> Result<SctAuthoringState> {
            auto next = s;
            std::set<sct::SctSectionId> sections;
            for (const auto &p : selections) {
                sections.insert(actions[p.anchor].section);
                sections.insert(actions[p.prototype].section);
                if (p.incoming)
                    sections.insert(actions[p.incoming->first].section);
            }
            for (auto section : sections)
                if (!std::ranges::any_of(next.project.sequences,
                                         [&](const auto &q) { return q.script == script && q.section == section; })) {
                    const auto found =
                        std::ranges::find(working.value().document->sections, section, &sct::SctDocumentSection::id);
                    auto promoted = SctSequenceAuthoring::promoteSequence(next, script, section, found->nameBytes);
                    if (!promoted)
                        return promoted;
                    next = std::move(promoted).takeValue();
                }
            const auto address = [&](std::size_t index) {
                const auto seq = std::ranges::find_if(next.project.sequences, [&](const auto &q) {
                    return q.script == script && q.section == actions[index].section;
                });
                return std::pair{seq->id, std::ranges::find(seq->actions, actions[index].instruction.id,
                                                            &SctSequenceAction::instruction)
                                              ->id};
            };
            std::vector<SctTextInsertion> placements;
            for (const auto &p : selections) {
                const auto anchor = address(p.anchor);
                placements.push_back({anchor.first, anchor.second, address(p.prototype).second, p.operand, p.text,
                                      p.incoming ? std::optional{SctTextIncomingEdge{address(p.incoming->first).second,
                                                                                     p.incoming->second}}
                                                 : std::nullopt});
            }
            return SctTextAuthoring::insert(next, placements, *enc);
        });
    });
    refresh();
    dialog.exec();
}
} // namespace salsa::qt
