#include "Editor/OrganizationComponent.h"
#include <QAction>
#include <QCheckBox>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <set>
namespace salsa::qt {
namespace ed = core::editor;
namespace {
template <class W, class F> void background(QObject *o, W work, F done) {
    using R = decltype(work());
    auto *w = new QFutureWatcher<R>(o);
    QObject::connect(w, &QFutureWatcherBase::finished, o, [w, done = std::move(done)]() mutable {
        auto r = w->result();
        w->deleteLater();
        done(std::move(r));
    });
    w->setFuture(QtConcurrent::run(std::move(work)));
}
QString q(const std::string &s) { return QString::fromStdString(s); }
} // namespace
OrganizationComponent::OrganizationComponent(EditorSessionPresenter *s, QWidget *p)
    : QWidget(p), session_(s) {
    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    layout->addWidget(tabs);
    auto *page = new QWidget(tabs);
    auto *form = new QFormLayout(page);
    target_ = new QLabel(page);
    target_->setWordWrap(true);
    form->addRow(target_);
    alias_ = new QLineEdit(page);
    form->addRow(tr("Alias"), alias_);
    bookmark_ = new QLineEdit(page);
    form->addRow(tr("Bookmark"), bookmark_);
    note_ = new QPlainTextEdit(page);
    note_->setPlaceholderText(tr("Your notes about this item"));
    form->addRow(tr("Notes"), note_);
    color_ = new QPushButton(tr("Choose item color…"), page);
    form->addRow(color_);
    auto *clear = new QPushButton(tr("Use default color"), page);
    form->addRow(clear);
    apply_ = new QPushButton(tr("Apply"), page);
    auto *discard = new QPushButton(tr("Discard / load selection"), page);
    form->addRow(apply_, discard);
    tabs->addTab(page, tr("Item notes"));
    bookmarks_ = new QListWidget(tabs);
    tabs->addTab(bookmarks_, tr("Bookmarks"));
    auto dirty = [this] {
        if (!loading_) {
            dirty_ = true;
            apply_->setEnabled(draft_.has_value() && !session_->busy());
        }
    };
    connect(alias_, &QLineEdit::textChanged, this, dirty);
    connect(bookmark_, &QLineEdit::textChanged, this, dirty);
    connect(note_, &QPlainTextEdit::textChanged, this, dirty);
    connect(color_, &QPushButton::clicked, this, [this, dirty] {
        if (!draft_)
            return;
        auto c = QColorDialog::getColor(
            draft_->color ? QColor::fromRgb(*draft_->color) : palette().color(QPalette::Text), this);
        if (c.isValid()) {
            draft_->color = c.rgb() & 0xffffff;
            color_->setText(c.name());
            dirty();
        }
    });
    connect(clear, &QPushButton::clicked, this, [this, dirty] {
        if (draft_) {
            draft_->color.reset();
            color_->setText(tr("Default color"));
            dirty();
        }
    });
    connect(apply_, &QPushButton::clicked, this, [this] { apply(); });
    connect(discard, &QPushButton::clicked, this, [this] { refresh(true); });
    connect(bookmarks_, &QListWidget::itemActivated, this, [this] {
        auto i = bookmarks_->currentRow();
        if (i >= 0 && i < static_cast<int>(bookmarkTargets_.size()))
            session_->navigate(bookmarkTargets_[i]);
    });
    auto add = [&](QString name, std::function<void()> run) {
        auto *a = new QAction(name, this);
        connect(a, &QAction::triggered, this, std::move(run));
        actions_.push_back(a);
        return a;
    };
    add(tr("Item notes and bookmarks"), [this] {
        if (showRequested)
            showRequested();
        refresh();
    });
    add(tr("Add code section…"), [this] {
        if (!session_->inspection())
            return;
        auto script = session_->inspection()->script;
        auto r = session_->revision();
        bool ok = false;
        auto name = QInputDialog::getText(this, tr("New section"), tr("Name"), QLineEdit::Normal, {}, &ok);
        if (ok && !name.trimmed().isEmpty())
            session_->execute(tr("Add section"), ed::NewSection{script, name.toStdString()}, r);
    });
    add(tr("Rename section…"), [this] {
        if (!session_->selection())
            return;
        auto *id = std::get_if<core::program::SectionId>(&*session_->selection());
        if (!id) {
            emit session_->diagnostic(tr("Select a section to rename."));
            return;
        }
        auto section = *id;
        auto r = session_->revision();
        bool ok = false;
        auto name = QInputDialog::getText(this, tr("Rename section"), tr("Name"), QLineEdit::Normal, {}, &ok);
        if (ok && !name.trimmed().isEmpty())
            session_->execute(tr("Rename section"), ed::RenameCodeSection{section, name.toStdString()}, r);
    });
    add(tr("Organize sections and folders…"), [this] { folders(); });
    add(tr("Instruction catalog…"), [this] { catalog(); });
    connect(session_, &EditorSessionPresenter::inspectionChanged, this, [this] {
        if (isVisible())
            refresh();
    });
    connect(session_, &EditorSessionPresenter::changed, this, [this] {
        if (applying_) {
            applying_ = false;
            dirty_ = false;
        }
        if (isVisible())
            refresh();
    });
    connect(session_, &EditorSessionPresenter::sessionChanged, this, [this] {
        dirty_ = false;
        draft_.reset();
        ++generation_;
        bookmarks_->clear();
        bookmarkTargets_.clear();
        if (isVisible())
            refresh(true);
    });
    connect(session_, &EditorSessionPresenter::availabilityChanged, this, [this] {
        if (!session_->busy())
            applying_ = false;
        for (auto *a : actions_)
            a->setEnabled(session_->backend() && !session_->busy());
        apply_->setEnabled(draft_.has_value() && dirty_ && !session_->busy());
    });
    apply_->setEnabled(false);
}
void OrganizationComponent::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    refresh();
}
void OrganizationComponent::refresh(bool discard) {
    if (discard)
        dirty_ = false;
    if (dirty_) {
        target_->setText(tr("Unsaved notes retained for the previous selection. Apply or discard them before "
                            "loading another item."));
        return;
    }
    auto b = session_->backend();
    if (!b) {
        draft_.reset();
        return;
    }
    auto r = session_->revision();
    auto selected = session_->selection();
    auto generation = ++generation_;
    core::SctScriptId script;
    if (session_->inspection())
        script = session_->inspection()->script;
    background(
        this, [b, r, script] { return b->organization(r, script); },
        [this, r, selected, generation](auto result) {
            if (r != session_->revision() || generation != generation_ || dirty_)
                return;
            if (!result) {
                emit session_->diagnostic(q(result.diagnostics().front().message));
                return;
            }
            loading_ = true;
            bookmarks_->clear();
            bookmarkTargets_.clear();
            for (auto &n : result.value().notes)
                if (!n.bookmark.empty()) {
                    bookmarks_->addItem(q(n.bookmark));
                    bookmarkTargets_.push_back(n.subject);
                }
            draft_.reset();
            if (selected) {
                draft_ = ed::ItemNote{*selected};
                auto found = std::ranges::find(result.value().notes, *selected, &ed::ItemNote::subject);
                if (found != result.value().notes.end())
                    draft_ = *found;
                draftRevision_ = r;
            }
            target_->setText(
                draft_ ? tr("Item %1").arg(std::visit([](auto id) { return id.value; }, draft_->subject))
                       : tr("Select an item"));
            alias_->setText(draft_ ? q(draft_->alias) : QString{});
            note_->setPlainText(draft_ ? q(draft_->note) : QString{});
            bookmark_->setText(draft_ ? q(draft_->bookmark) : QString{});
            color_->setText(draft_ && draft_->color ? QColor::fromRgb(*draft_->color).name()
                                                    : tr("Default color"));
            loading_ = false;
            apply_->setEnabled(false);
        });
}
void OrganizationComponent::apply() {
    if (!draft_)
        return;
    draft_->alias = alias_->text().toStdString();
    draft_->note = note_->toPlainText().toStdString();
    draft_->bookmark = bookmark_->text().toStdString();
    applying_ = true;
    session_->execute(tr("Edit item notes"), ed::WriteNote{*draft_}, draftRevision_);
}
void OrganizationComponent::folders() {
    if (!session_->inspection())
        return;
    auto b = session_->backend();
    auto r = session_->revision();
    auto script = session_->inspection()->script;
    if (!script.valid())
        return;
    background(
        this, [b, r, script] { return b->organization(r, script); },
        [this, r, script](auto result) {
            if (r != session_->revision())
                return;
            if (!result) {
                emit session_->diagnostic(q(result.diagnostics().front().message));
                return;
            }
            auto model = std::move(result).takeValue();
            QDialog dialog(this);
            dialog.setWindowTitle(tr("Section organization"));
            dialog.resize(650, 550);
            auto *layout = new QVBoxLayout(&dialog);
            auto *tabs = new QTabWidget(&dialog);
            layout->addWidget(tabs);
            auto *order = new QListWidget(tabs);
            order->setDragDropMode(QAbstractItemView::InternalMove);
            order->setSelectionMode(QAbstractItemView::ExtendedSelection);
            for (auto &s : model.sections) {
                auto *item = new QListWidgetItem(q(s.label), order);
                item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(
                                                std::get<core::program::SectionId>(s.subject).value));
            }
            tabs->addTab(order, tr("Script section order"));
            auto *folderPage = new QWidget(tabs);
            auto *fl = new QVBoxLayout(folderPage);
            fl->addWidget(new QLabel(tr("Folders organize the editor. Drag sections between folders; this "
                                        "does not change script order."),
                                     folderPage));
            auto *tree = new QTreeWidget(folderPage);
            tree->setHeaderLabel(tr("Folders and sections"));
            tree->setDragDropMode(QAbstractItemView::InternalMove);
            tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
            fl->addWidget(tree);
            std::vector<QTreeWidgetItem *> folders;
            std::set<core::program::SectionId> assigned;
            auto addSection = [&](core::program::SectionId id, QTreeWidgetItem *parent) {
                auto found = std::ranges::find(model.sections, ed::Subject{id}, &ed::BrowseRow::subject);
                if (found == model.sections.end())
                    return;
                auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
                item->setText(0, q(found->label));
                item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id.value));
                item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
            };
            for (auto &f : model.folders) {
                auto *item =
                    f.parent ? new QTreeWidgetItem(folders.at(*f.parent)) : new QTreeWidgetItem(tree);
                item->setText(0, q(f.name));
                item->setFlags(item->flags() | Qt::ItemIsEditable);
                folders.push_back(item);
                for (auto id : f.sections) {
                    assigned.insert(id);
                    addSection(id, item);
                }
            }
            for (auto &s : model.sections) {
                auto id = std::get<core::program::SectionId>(s.subject);
                if (!assigned.contains(id))
                    addSection(id, nullptr);
            }
            tree->expandAll();
            auto *add = new QPushButton(tr("New folder"), folderPage);
            auto *remove = new QPushButton(tr("Remove folder (keep sections)"), folderPage);
            fl->addWidget(add);
            fl->addWidget(remove);
            tabs->addTab(folderPage, tr("Folders"));
            connect(add, &QPushButton::clicked, &dialog, [&] {
                auto *parent = tree->currentItem();
                if (parent && parent->data(0, Qt::UserRole).toULongLong())
                    parent = parent->parent();
                auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
                item->setText(0, tr("New folder"));
                item->setFlags(item->flags() | Qt::ItemIsEditable);
                tree->setCurrentItem(item);
                tree->editItem(item, 0);
            });
            connect(remove, &QPushButton::clicked, &dialog, [&] {
                auto *item = tree->currentItem();
                if (!item || item->data(0, Qt::UserRole).toULongLong())
                    return;
                while (item->childCount())
                    tree->addTopLevelItem(item->takeChild(0));
                delete item;
            });
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            layout->addWidget(buttons);
            connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            if (dialog.exec() != QDialog::Accepted)
                return;
            ed::WriteOrganization write;
            write.script = script;
            for (int i = 0; i < order->count(); ++i)
                write.order.push_back(
                    core::program::SectionId{order->item(i)->data(Qt::UserRole).toULongLong()});
            std::function<void(QTreeWidgetItem *, std::optional<std::uint32_t>)> collect =
                [&](QTreeWidgetItem *parent, std::optional<std::uint32_t> owner) {
                    for (int i = 0; i < parent->childCount(); ++i) {
                        auto *item = parent->child(i);
                        auto id = item->data(0, Qt::UserRole).toULongLong();
                        if (id) {
                            if (owner)
                                write.folders[*owner].sections.push_back(core::program::SectionId{id});
                        } else {
                            auto at = static_cast<std::uint32_t>(write.folders.size());
                            write.folders.push_back(
                                {script, item->text(0).trimmed().toStdString(), {}, owner});
                            collect(item, at);
                        }
                    }
                };
            collect(tree->invisibleRootItem(), {});
            session_->execute(tr("Organize sections"), std::move(write), r);
        });
}
void OrganizationComponent::catalog() {
    auto b = session_->backend();
    if (!b)
        return;
    auto r = session_->revision();
    background(
        this, [b, r] { return b->organization(r, {}); },
        [this, b, r](auto result) {
            if (r != session_->revision())
                return;
            if (!result) {
                emit session_->diagnostic(q(result.diagnostics().front().message));
                return;
            }
            auto options = b->opcodes();
            QStringList labels;
            for (auto &op : options)
                labels << QString::number(op.code) + " " + q(op.name);
            bool ok = false;
            auto selected =
                QInputDialog::getItem(this, tr("Instruction catalog"), tr("Opcode"), labels, 0, false, &ok);
            if (!ok)
                return;
            auto index = labels.indexOf(selected);
            auto code = options[index].code;
            ed::OpcodeCustomization draft;
            draft.opcode = code;
            auto found = std::ranges::find(result.value().opcodes, code, &ed::OpcodeCustomization::opcode);
            if (found != result.value().opcodes.end())
                draft = *found;
            QDialog dialog(this);
            dialog.setWindowTitle(tr("Opcode %1 appearance").arg(code));
            dialog.resize(550, 450);
            auto *form = new QFormLayout(&dialog);
            auto *name = new QLineEdit(q(draft.name), &dialog);
            name->setPlaceholderText(q(options[index].name));
            auto *description = new QPlainTextEdit(q(draft.description), &dialog);
            auto *color = new QPushButton(
                draft.color ? QColor::fromRgb(*draft.color).name() : tr("Default color"), &dialog);
            auto *clear = new QPushButton(tr("Reset color"), &dialog);
            auto *parameters = new QPlainTextEdit(&dialog);
            QStringList rows;
            for (auto &[id, label] : draft.parameters)
                rows << QString::number(id) + "=" + q(label);
            parameters->setPlainText(rows.join('\n'));
            parameters->setPlaceholderText(tr("Parameter labels: one schemaIndex=label per line"));
            form->addRow(tr("Display name"), name);
            form->addRow(tr("Description"), description);
            form->addRow(color, clear);
            form->addRow(tr("Parameter names"), parameters);
            auto *note = new QLineEdit(q(draft.note), &dialog);
            auto *category = new QLineEdit(q(draft.category), &dialog);
            auto *defaults = new QLineEdit(&dialog);
            QStringList defaultRows;
            for (auto [index, value] : draft.defaults)
                defaultRows << QString::number(index) + "=" + QString::number(value);
            defaults->setText(defaultRows.join(", "));
            defaults->setPlaceholderText(tr("schemaIndex=word, schemaIndex=word"));
            form->addRow(tr("Catalog note"), note);
            form->addRow(tr("Category"), category);
            form->addRow(tr("Creation defaults"), defaults);
            connect(color, &QPushButton::clicked, &dialog, [&] {
                auto c = QColorDialog::getColor(
                    draft.color ? QColor::fromRgb(*draft.color) : palette().color(QPalette::Text), &dialog);
                if (c.isValid()) {
                    draft.color = c.rgb() & 0xffffff;
                    color->setText(c.name());
                }
            });
            connect(clear, &QPushButton::clicked, &dialog, [&] {
                draft.color.reset();
                color->setText(tr("Default color"));
            });
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            form->addRow(buttons);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
                draft.defaults.clear();
                for (auto line : defaults->text().split(',', Qt::SkipEmptyParts)) {
                    auto parts = line.split('=');
                    bool a = false, b = false;
                    auto index = parts.value(0).trimmed().toUInt(&a);
                    auto value = parts.value(1).trimmed().toUInt(&b, 0);
                    if (parts.size() != 2 || !a || !b) {
                        QMessageBox::information(&dialog, tr("Creation default"),
                                                 tr("Use schemaIndex=word, separated by commas."));
                        return;
                    }
                    draft.defaults.push_back({index, value});
                }
                draft.parameters.clear();
                for (auto line : parameters->toPlainText().split('\n', Qt::SkipEmptyParts)) {
                    auto split = line.indexOf('=');
                    bool valid = false;
                    auto id = line.left(split).toUInt(&valid);
                    if (split < 1 || !valid) {
                        QMessageBox::information(&dialog, tr("Parameter label"),
                                                 tr("Use schemaIndex=label on each line."));
                        return;
                    }
                    draft.parameters.push_back({id, line.mid(split + 1).toStdString()});
                }
                dialog.accept();
            });
            if (dialog.exec() != QDialog::Accepted)
                return;
            draft.note = note->text().toStdString();
            draft.category = category->text().toStdString();
            draft.name = name->text().toStdString();
            draft.description = description->toPlainText().toStdString();
            session_->execute(tr("Customize instruction catalog"), ed::WriteOpcode{draft}, r);
        });
}
} // namespace salsa::qt
