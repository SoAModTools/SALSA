#include "Sct/ProgramValueDialog.h"
#include "Sct/SctScptEditorWidget.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QHeaderView>
#include <QFormLayout>
#include <QCheckBox>
#include <QLineEdit>
namespace salsa::qt {
namespace ir = core::program;
namespace {
QString kind(ir::ValueKind k) {
    static const char *names[] = {"Instruction",
                                  "Parameter",
                                  "Repeated group",
                                  "Word",
                                  "Expression",
                                  "Value operation",
                                  "Binary operation",
                                  "Stack operation",
                                  "Inert operation",
                                  "Word sequence",
                                  "Action target",
                                  "Indexed string",
                                  "Footer text",
                                  "Unresolved reference",
                                  "Unavailable content",
                                  "Plain text",
                                  "Message",
                                  "Text",
                                  "Message command",
                                  "Empty indexed string",
                                  "Indexed section",
                                  "String group"};
    auto n = static_cast<unsigned>(k);
    return n < std::size(names) ? QString::fromLatin1(names[n]) : QStringLiteral("Unknown value");
}
QString description(const ir::ValueBuffer &b, ir::ValueId id) {
    const auto &v = b.values.at(id.value);
    if (v.kind == ir::ValueKind::Expression) {
        auto e = ir::ProgramValues::expression(b, id);
        if (e)
            return QString::fromStdString(core::SctExpressionLanguage::project(e.value()).text);
    }
    if (v.kind == ir::ValueKind::PlainText || v.kind == ir::ValueKind::TextChunk)
        return QString::fromUtf8(b.characters.data() + v.data, v.detail);
    return QString::number(v.data);
}
} // namespace
ProgramValueDialog::ProgramValueDialog(ir::ValueBuffer b, ChooseTarget target, QWidget *parent)
    : QDialog(parent), buffer_(std::move(b)), target_(std::move(target)) {
    setWindowTitle(tr("Edit selected content"));
    resize(680, 480);
    auto *layout = new QVBoxLayout(this);
    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabels({tr("Field"), tr("Value")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    layout->addWidget(tree_);
    auto *editButton = new QPushButton(tr("Edit selected field…"), this);
    layout->addWidget(editButton);
    connect(editButton, &QPushButton::clicked, this, [this] { edit(); });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this] { edit(); });
    if (buffer_.values.at(buffer_.root.value).kind == ir::ValueKind::Instruction) {
        auto *groups = new QPushButton(tr("Repeated groups…"), this);
        layout->addWidget(groups);
        connect(groups, &QPushButton::clicked, this, [this] { this->groups(); });
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    rebuild();
}
void ProgramValueDialog::rebuild() {
    tree_->clear();
    std::function<void(ir::ValueId, QTreeWidgetItem *, unsigned)> add = [&](auto id, auto *parent, unsigned depth) {
        if (depth > 128 || id.value >= buffer_.values.size())
            return;
        const auto &v = buffer_.values[id.value];
        auto *row = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
        row->setText(0, v.kind == ir::ValueKind::Parameter ? tr("Parameter %1").arg(v.data) : kind(v.kind));
        row->setText(1, description(buffer_, id));
        row->setData(0, Qt::UserRole, id.value);
        if (v.kind != ir::ValueKind::Expression)
            for (auto child : std::span(buffer_.children).subspan(v.first, v.count))
                add(child, row, depth + 1);
        row->setExpanded(depth < 3);
    };
    add(buffer_.root, nullptr, 0);
}
void ProgramValueDialog::replace(ir::ValueId id, ir::ValueBuffer replacement) {
    auto r = ir::ProgramValues::replace(buffer_, buffer_.root, id, replacement);
    if (!r) {
        QMessageBox::warning(this, tr("Cannot edit"), QString::fromStdString(r.diagnostics().front().message));
        return;
    }
    buffer_ = std::move(r).takeValue();
    rebuild();
}
void ProgramValueDialog::edit() {
    auto *row = tree_->currentItem();
    if (!row)
        return;
    ir::ValueId id{row->data(0, Qt::UserRole).toUInt()};
    const auto v = buffer_.values.at(id.value);
    if (v.kind == ir::ValueKind::Parameter || v.kind == ir::ValueKind::RepeatedGroup ||
        v.kind == ir::ValueKind::Message) {
        if (row->childCount())
            tree_->setCurrentItem(row->child(0));
        return;
    }
    if (v.kind == ir::ValueKind::Instruction) {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Instruction timing"));
        auto *form = new QFormLayout(&dialog);
        auto *refresh = new QCheckBox(tr("Skip refresh"), &dialog);
        refresh->setChecked(v.detail & 1);
        form->addRow(refresh);
        auto *scheduled = new QCheckBox(tr("Scheduled"), &dialog);
        scheduled->setChecked(v.detail & 2);
        form->addRow(scheduled);
        auto *expression = new QLineEdit(QStringLiteral("0"), &dialog);
        auto children = std::span(buffer_.children).subspan(v.first, v.count);
        if ((v.detail & 2) && !children.empty())
            expression->setText(description(buffer_, children.front()));
        form->addRow(tr("Schedule expression"), expression);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        if (dialog.exec() != QDialog::Accepted)
            return;
        std::vector<ir::ValueBuffer> fields;
        if (scheduled->isChecked()) {
            // Preserve an unchanged nonconventional ordered program exactly.
            if ((v.detail & 2) && expression->text() == description(buffer_, children.front()))
                fields.push_back(ir::ProgramValues::slice(buffer_, children.front()).value());
            else {
                auto parsed = core::SctExpressionLanguage::parse(expression->text().toStdString());
                if (!parsed.succeeded()) {
                    QMessageBox::warning(this, tr("Invalid schedule"),
                                         tr("The schedule expression could not be parsed. No changes were applied."));
                    return;
                }
                fields.push_back(ir::ValueBuffer::expression(*parsed.expression).value());
            }
        }
        for (std::size_t i = (v.detail & 2) ? 1 : 0; i < children.size(); ++i)
            fields.push_back(ir::ProgramValues::slice(buffer_, children[i]).value());
        auto result = ir::ProgramValues::compose(
            v.kind, v.data, (refresh->isChecked() ? 1u : 0u) | (scheduled->isChecked() ? 2u : 0u), fields);
        if (result)
            replace(id, std::move(result).takeValue());
        return;
    }
    if (v.kind == ir::ValueKind::Expression) {
        auto e = ir::ProgramValues::expression(buffer_, id);
        if (!e)
            return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Expression"));
        dialog.resize(720, 520);
        auto *layout = new QVBoxLayout(&dialog);
        auto *editor = new SctScptEditorWidget(&dialog);
        layout->addWidget(editor);
        // The reused detached expression widget's address is presentation-only.
        // The commit closure captures the actual local value handle.
        auto locator = core::AssetLocator::fromRelativePath("selection.sct").value();
        editor->setCommitHandler([&](const auto &, const auto &, const auto &value) {
            auto encoded = ir::ValueBuffer::expression(value);
            if (!encoded)
                return false;
            replace(id, std::move(encoded).takeValue());
            dialog.accept();
            return true;
        });
        (void)editor->bind(locator, {spice::sct::SctInstructionId{1}, {0, {}}}, e.value());
        dialog.exec();
        return;
    }
    if (v.kind == ir::ValueKind::ActionReference || v.kind == ir::ValueKind::IndexedTextReference ||
        v.kind == ir::ValueKind::SupplementaryTextReference || v.kind == ir::ValueKind::Unresolved) {
        if (target_)
            if (auto chosen = target_(v.kind))
                replace(id, std::move(*chosen));
        return;
    }
    if (v.kind == ir::ValueKind::Unavailable) {
        bool ok = false;
        auto type = QInputDialog::getItem(
            this, tr("Supply authored value"), tr("Value type"),
            {tr("Expression"), tr("Word"), tr("Action target"), tr("Indexed string"), tr("Footer text")}, 0, false,
            &ok);
        if (!ok)
            return;
        ir::ValueBuffer b;
        if (type == tr("Expression"))
            b = ir::ValueBuffer::expression(spice::sct::SctExpressionFactory::encodedDecimalLiteral(0)).value();
        else if (type == tr("Word"))
            b.add(ir::ValueKind::Word, 0);
        else {
            auto k = type == tr("Action target")    ? ir::ValueKind::ActionReference
                     : type == tr("Indexed string") ? ir::ValueKind::IndexedTextReference
                                                    : ir::ValueKind::SupplementaryTextReference;
            if (!target_)
                return;
            auto chosen = target_(k);
            if (!chosen)
                return;
            b = std::move(*chosen);
        }
        replace(id, std::move(b));
        return;
    }
    if (v.kind == ir::ValueKind::PlainText || v.kind == ir::ValueKind::TextChunk) {
        bool ok = false;
        auto text = QInputDialog::getMultiLineText(this, tr("Text"), tr("Text"), description(buffer_, id), &ok);
        if (ok) {
            ir::ValueBuffer b;
            b.text(v.kind, text.toStdString());
            replace(id, std::move(b));
        }
        return;
    }
    if (v.kind == ir::ValueKind::Word || v.kind == ir::ValueKind::TextCommand) {
        bool ok = false;
        auto input = QInputDialog::getText(this, kind(v.kind), tr("Unsigned value (decimal or 0x hexadecimal)"),
                                           QLineEdit::Normal, QString::number(v.data), &ok);
        if (!ok)
            return;
        auto value = input.toULongLong(&ok, 0);
        if (!ok || value > UINT32_MAX) {
            QMessageBox::warning(this, tr("Invalid value"), tr("Enter a value from 0 to 4294967295."));
            return;
        }
        buffer_.values[id.value].data = value;
        rebuild();
    }
}

void ProgramValueDialog::groups() {
    const auto root = buffer_.values.at(buffer_.root.value);
    auto *schema = spice::sct::findSctOpcodeSchema(static_cast<std::uint16_t>(root.data));
    if (!schema || schema->parameters.loopStartParam < 0) {
        QMessageBox::information(this, tr("Repeated groups"), tr("This instruction has no repeated parameter groups."));
        return;
    }
    std::vector<ir::ValueBuffer> fixed, repeated;
    for (auto child : std::span(buffer_.children).subspan(root.first, root.count)) {
        auto value = ir::ProgramValues::slice(buffer_, child).value();
        (buffer_.values[child.value].kind == ir::ValueKind::RepeatedGroup ? repeated : fixed)
            .push_back(std::move(value));
    }
    bool ok;
    auto operation = QInputDialog::getItem(this, tr("Repeated groups"), tr("Operation"),
                                           {tr("Append group"), tr("Remove group"), tr("Move group")}, 0, false, &ok);
    if (!ok)
        return;
    auto before = repeated.size();
    if (operation == tr("Append group")) {
        spice::sct::SctInstructionFactoryRequest request;
        request.opcode = static_cast<std::uint16_t>(root.data);
        request.repeatedGroupCount = 1;
        auto draft = ir::ProgramValues::instruction(request);
        if (!draft)
            return;
        auto &b = draft.value();
        auto v = b.values[b.root.value];
        for (auto c : std::span(b.children).subspan(v.first, v.count))
            if (b.values[c.value].kind == ir::ValueKind::RepeatedGroup) {
                repeated.push_back(ir::ProgramValues::slice(b, c).value());
                break;
            }
    } else {
        if (repeated.empty())
            return;
        int from = QInputDialog::getInt(this, tr("Repeated group"), tr("Group ordinal (zero based)"), 0, 0,
                                        static_cast<int>(repeated.size() - 1), 1, &ok);
        if (!ok)
            return;
        if (operation == tr("Remove group"))
            repeated.erase(repeated.begin() + from);
        else {
            int to = QInputDialog::getInt(this, tr("Repeated group"), tr("New ordinal"), from, 0,
                                          static_cast<int>(repeated.size() - 1), 1, &ok);
            if (!ok)
                return;
            auto item = std::move(repeated[from]);
            repeated.erase(repeated.begin() + from);
            repeated.insert(repeated.begin() + to, std::move(item));
        }
    }
    if (before != repeated.size()) {
        // Ask the same opcode factory for its count encoding. Preserve every other fixed value and schedule.
        spice::sct::SctInstructionFactoryRequest request;
        request.opcode = static_cast<std::uint16_t>(root.data);
        request.repeatedGroupCount = static_cast<std::uint32_t>(repeated.size());
        auto draft = ir::ProgramValues::instruction(request);
        if (!draft)
            return;
        auto &b = draft.value();
        auto v = b.values[b.root.value];
        for (auto c : std::span(b.children).subspan(v.first, v.count)) {
            const auto &parameter = b.values[c.value];
            if (parameter.kind != ir::ValueKind::Parameter || parameter.data != schema->parameters.iterationCountParam)
                continue;
            for (auto &field : fixed)
                if (field.values[field.root.value].kind == ir::ValueKind::Parameter &&
                    field.values[field.root.value].data == parameter.data)
                    field = ir::ProgramValues::slice(b, c).value();
        }
    }
    fixed.insert(fixed.end(), std::make_move_iterator(repeated.begin()), std::make_move_iterator(repeated.end()));
    auto changed = ir::ProgramValues::compose(root.kind, root.data, root.detail, fixed);
    if (changed) {
        buffer_ = std::move(changed).takeValue();
        rebuild();
    }
}
} // namespace salsa::qt
