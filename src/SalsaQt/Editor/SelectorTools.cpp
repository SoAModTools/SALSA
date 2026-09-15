#include "Editor/FeatureUi.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QTableWidget>
namespace salsa::qt {
using namespace feature_ui;
SelectorTools::SelectorTools(EditorSessionPresenter *s, QWidget *h) : QObject(h), session_(s), host_(h) {
    action = new QAction(tr("Overworld selectors and initialization preview…"), this);
    connect(action, &QAction::triggered, this, [this] { open(); });
    availability(action, session_);
}
void SelectorTools::open() {
    if (!session_->backend() || !session_->inspection() || session_->busy())
        return;
    auto r = session_->revision();
    QDialog dialog(host_);
    dialog.setWindowTitle(tr("Overworld selectors"));
    dialog.resize(820, 610);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(
        tr("Ordered assignments select files for subsequent overworld initialization. Ordinary streaming "
           "reuses captured filenames. ByteVar[6] selects the overworld SCT/base MLD separately."),
        &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto *table = new QTableWidget(0, 3, &dialog);
    table->setHorizontalHeaderLabels({tr("ByteVar index"), tr("Expression (literal or variable copy)"),
                                      tr("Replace action (blank = insert)")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(table);
    auto *bar = new QToolBar(&dialog);
    layout->addWidget(bar);
    auto add = [&](QString index, QString expression, QString action) {
        int n = table->rowCount();
        table->insertRow(n);
        table->setItem(n, 0, new QTableWidgetItem(index));
        table->setItem(n, 1, new QTableWidgetItem(expression));
        table->setItem(n, 2, new QTableWidgetItem(action));
    };
    bar->addAction(tr("Add assignment"), [&] {
        QStringList labels{tr("6 — overworld script/base")};
        std::vector<unsigned> indices{6};
        for (const auto &f : smores::script::tileFamilies())
            if (f.byteVariable) {
                auto found = std::ranges::find(indices, *f.byteVariable);
                if (found == indices.end()) {
                    indices.push_back(*f.byteVariable);
                    labels << tr("%1 — tile %2").arg(*f.byteVariable).arg(q(f.key));
                }
            }
        auto selected = choose(&dialog, tr("Selector"), labels, false);
        if (selected && !selected->empty())
            add(QString::number(indices[selected->front()]), "0", {});
    });
    bar->addAction(tr("Remove row"), [&] {
        if (table->currentRow() >= 0)
            table->removeRow(table->currentRow());
    });
    bar->addAction(tr("Move row up"), [&] {
        int n = table->currentRow();
        if (n <= 0)
            return;
        for (int c = 0; c < 3; ++c) {
            auto *a = table->takeItem(n, c);
            auto *b = table->takeItem(n - 1, c);
            table->setItem(n - 1, c, a);
            table->setItem(n, c, b);
        }
        table->setCurrentCell(n - 1, 0);
    });
    auto *known = new QPlainTextEdit(&dialog);
    known->setPlaceholderText(tr("Transient known values, one per line: 8=2 or 8=?"));
    known->setMaximumHeight(80);
    layout->addWidget(known);
    auto *resultView = new QPlainTextEdit(&dialog);
    resultView->setReadOnly(true);
    layout->addWidget(resultView);
    bar->addAction(tr("Preview selected action range…"), [&] {
        auto first = feature_ui::action(session_, &dialog, tr("First action in the straight-line range"));
        if (!first)
            return;
        auto last = feature_ui::action(session_, &dialog, tr("Last action in the same body"));
        if (!last)
            return;
        smores::script::KnownBytes values;
        for (auto line : known->toPlainText().split('\n', Qt::SkipEmptyParts)) {
            auto pair = line.split('=');
            if (pair.size() != 2) {
                resultView->setPlainText(tr("Use index=value or index=?"));
                return;
            }
            bool ok;
            auto index = pair[0].trimmed().toUInt(&ok);
            if (!ok)
                return;
            if (pair[1].trimmed() == "?")
                values[index] = {};
            else {
                auto value = pair[1].trimmed().toUInt(&ok);
                if (!ok || value > 255) {
                    resultView->setPlainText(tr("Known byte values must be 0–255."));
                    return;
                }
                values[index] = static_cast<std::uint8_t>(value);
            }
        }
        auto b = session_->backend();
        background(
            &dialog, [=] { return b->previewSelectors(r, *first, *last, values); },
            [&, first](auto result) {
                if (r != session_->revision() || !report(session_, result))
                    return;
                QStringList lines{tr("Selection for subsequent overworld initialization"),
                                  tr("SCT: %1 | Base MLD: %2")
                                      .arg(q(result.value().overworldScript.filename),
                                           q(result.value().overworldBase.filename))};
                const auto &families = smores::script::tileFamilies();
                for (std::size_t i = 0; i < families.size(); ++i) {
                    const auto &f = families[i];
                    const auto &tile = result.value().tiles[i];
                    auto value = f.byteVariable ? result.value().values.find(*f.byteVariable)
                                                : result.value().values.end();
                    QString knownValue = value != result.value().values.end() && value->second
                                             ? QString::number(*value->second)
                                             : tr("unknown");
                    lines << tr("%1 | %2 | %3 | %4 | %5 retained resource matches")
                                 .arg(q(f.key),
                                      f.byteVariable ? tr("ByteVar[%1]").arg(*f.byteVariable) : tr("fixed a"),
                                      knownValue,
                                      tile.state == smores::script::SelectionState::Known ? q(tile.filename)
                                      : tile.state == smores::script::SelectionState::Unknown
                                          ? tr("unknown")
                                          : tr("unrepresentable"))
                                 .arg(tile.candidates.size());
                    for (const auto &candidate : tile.candidates)
                        lines << "    " + q(candidate.asset);
                }
                resultView->setPlainText(lines.join('\n'));
                if (table->rowCount() == 0)
                    for (const auto &row : result.value().assignments)
                        if (smores::script::isOverworldSelector(row.destination))
                            add(QString::number(row.destination),
                                q(core::SctExpressionLanguage::project(row.expression).text),
                                QString::number(row.action.value));
            });
    });
    bar->addAction(tr("Apply ordered assignments…"), [&] {
        if (table->rowCount() == 0)
            return;
        auto before =
            feature_ui::action(session_, &dialog, tr("Insertion anchor / existing body for replacements"));
        if (!before)
            return;
        ed::WriteSelectors edit{*before};
        for (int n = 0; n < table->rowCount(); ++n) {
            bool ok;
            auto index = table->item(n, 0)->text().toUInt(&ok);
            if (!ok)
                return;
            auto expression = core::SctExpressionLanguage::parse(table->item(n, 1)->text().toStdString());
            if (!expression.succeeded()) {
                resultView->setPlainText(tr("Invalid assignment expression."));
                return;
            }
            std::optional<core::SctActionId> replace;
            auto text = table->item(n, 2)->text().trimmed();
            if (!text.isEmpty()) {
                auto id = text.toULongLong(&ok);
                if (!ok || !id)
                    return;
                replace = core::SctActionId{id};
            }
            edit.edits.push_back({replace, index, *expression.expression});
        }
        commit(session_, dialog, r, edit, tr("Edit overworld selectors"));
    });
    dialog.exec();
}
} // namespace salsa::qt
