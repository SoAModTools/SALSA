#include "Sct/SctCatalogEditorDialog.h"

#include "SpiceSCT/SctOpcodeMetadata.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <charconv>
#include <ranges>

namespace salsa::qt {
namespace {
constexpr int OpcodeRole = Qt::UserRole;
constexpr int ParameterRole = Qt::UserRole + 1;

QString q(const std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString colorText(const std::optional<std::uint32_t> color) {
    return color ? QStringLiteral("#%1").arg(*color, 6, 16, QLatin1Char('0')).toUpper()
                 : QString{};
}

std::optional<std::uint32_t> parseWord(QString value, bool* valid) {
    value = value.trimmed();
    if (value.isEmpty()) { *valid = true; return std::nullopt; }
    bool converted = false;
    const auto result = value.toUInt(&converted, value.startsWith(QStringLiteral("0x"),
        Qt::CaseInsensitive) ? 16 : 10);
    *valid = converted;
    return converted ? std::optional<std::uint32_t>{result} : std::nullopt;
}

std::optional<std::uint32_t> parseColor(const QString& value, bool* valid) {
    auto text = value.trimmed();
    if (text.isEmpty()) { *valid = true; return std::nullopt; }
    if (text.startsWith(QLatin1Char('#'))) text.removeFirst();
    bool converted = false;
    const auto result = text.toUInt(&converted, 16);
    *valid = converted && text.size() == 6;
    return *valid ? std::optional<std::uint32_t>{result} : std::nullopt;
}

const core::SctCatalogOpcodeOverride* overrideFor(
    const core::SctPersonalCatalog& catalog, const std::uint16_t opcode) {
    const auto found = std::ranges::find(catalog.opcodes, opcode,
        &core::SctCatalogOpcodeOverride::opcode);
    return found == catalog.opcodes.end() ? nullptr : &*found;
}
} // namespace

SctCatalogEditorDialog::SctCatalogEditorDialog(
    core::SctPersonalCatalog catalog, QWidget* parent)
    : QDialog(parent), catalog_(std::move(catalog)) {
    setWindowTitle(tr("Instruction Catalog"));
    resize(1180, 720);
    auto* layout = new QVBoxLayout(this);
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(8);
    tree_->setHeaderLabels({tr("Kind"), tr("ID"), tr("Name / label"),
        tr("Description"), tr("Note"), tr("Default word"), tr("Color"),
        tr("Category")});
    tree_->setAlternatingRowColors(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree_->header()->resizeSection(0, 90);
    tree_->header()->resizeSection(1, 70);
    tree_->header()->resizeSection(2, 190);
    tree_->header()->resizeSection(3, 260);
    tree_->header()->resizeSection(4, 220);
    layout->addWidget(tree_, 1);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Save
        | QDialogButtonBox::Cancel, this);
    resetButton_ = buttons_->addButton(tr("Reset Selected"),
        QDialogButtonBox::ResetRole);
    importButton_ = buttons_->addButton(tr("Import Legacy..."),
        QDialogButtonBox::ActionRole);
    layout->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this,
        &SctCatalogEditorDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(resetButton_, &QPushButton::clicked, this,
        &SctCatalogEditorDialog::resetSelected);
    connect(importButton_, &QPushButton::clicked, this,
        &SctCatalogEditorDialog::importLegacy);
    populate();
}

const core::SctPersonalCatalog& SctCatalogEditorDialog::catalog() const noexcept {
    return catalog_;
}

void SctCatalogEditorDialog::populate() {
    tree_->clear();
    for (const auto& schema : spice::sct::sctOpcodeSchemas()) {
        const auto* overlay = overrideFor(catalog_, schema.opcode);
        auto* opcode = new QTreeWidgetItem(tree_);
        opcode->setData(0, OpcodeRole, schema.opcode);
        opcode->setText(0, tr("Opcode"));
        opcode->setText(1, QString::number(schema.opcode));
        opcode->setText(2, overlay && overlay->mnemonic
            ? QString::fromStdString(*overlay->mnemonic) : q(schema.semantic.mnemonic));
        opcode->setText(3, overlay && overlay->description
            ? QString::fromStdString(*overlay->description) : QString{});
        opcode->setText(4, overlay && overlay->note
            ? QString::fromStdString(*overlay->note) : q(schema.semantic.notes));
        opcode->setText(6, colorText(overlay ? overlay->colorRgb : std::nullopt));
        opcode->setText(7, overlay && overlay->category
            ? QString::fromStdString(*overlay->category) : QString{});
        opcode->setFlags(opcode->flags() | Qt::ItemIsEditable);
        for (std::size_t index = 0; index < schema.parameterCatalogCount; ++index) {
            const auto& parameter = schema.parameterCatalog[index];
            const core::SctCatalogParameterOverride* custom = nullptr;
            if (overlay) {
                const auto found = std::ranges::find(overlay->parameters,
                    parameter.schemaIndex,
                    &core::SctCatalogParameterOverride::schemaIndex);
                if (found != overlay->parameters.end()) custom = &*found;
            }
            auto* child = new QTreeWidgetItem(opcode);
            child->setData(0, OpcodeRole, schema.opcode);
            child->setData(0, ParameterRole, parameter.schemaIndex);
            child->setText(0, tr("Parameter"));
            child->setText(1, QString::number(parameter.schemaIndex));
            child->setText(2, custom && custom->label
                ? QString::fromStdString(*custom->label) : q(parameter.role));
            child->setText(5, custom && custom->creationDefaultWord
                ? QStringLiteral("0x%1").arg(*custom->creationDefaultWord,
                    8, 16, QLatin1Char('0')).toUpper() : QString{});
            child->setFlags(child->flags() | Qt::ItemIsEditable);
        }
    }
}

void SctCatalogEditorDialog::resetSelected() {
    for (auto* item : tree_->selectedItems()) {
        const auto opcode = static_cast<std::uint16_t>(
            item->data(0, OpcodeRole).toUInt());
        const auto* schema = spice::sct::findSctOpcodeSchema(opcode);
        if (!schema) continue;
        if (item->parent()) {
            const auto index = item->data(0, ParameterRole).toUInt();
            const auto* parameter = spice::sct::sctOpcodeParameterSchema(*schema, index);
            item->setText(2, parameter ? q(parameter->role) : QString{});
            item->setText(5, QString{});
        } else {
            item->setText(2, q(schema->semantic.mnemonic));
            item->setText(3, QString{});
            item->setText(4, q(schema->semantic.notes));
            item->setText(6, QString{});
            item->setText(7, QString{});
        }
    }
}

bool SctCatalogEditorDialog::collect(core::SctPersonalCatalog& result) {
    for (int row = 0; row < tree_->topLevelItemCount(); ++row) {
        auto* item = tree_->topLevelItem(row);
        const auto opcode = static_cast<std::uint16_t>(item->data(0, OpcodeRole).toUInt());
        const auto* schema = spice::sct::findSctOpcodeSchema(opcode);
        if (!schema) continue;
        core::SctCatalogOpcodeOverride entry{.opcode = opcode};
        const auto name = item->text(2).trimmed();
        if (name != q(schema->semantic.mnemonic)) entry.mnemonic = name.toStdString();
        if (!item->text(3).trimmed().isEmpty())
            entry.description = item->text(3).toStdString();
        if (item->text(4) != q(schema->semantic.notes))
            entry.note = item->text(4).toStdString();
        bool colorValid = false;
        entry.colorRgb = parseColor(item->text(6), &colorValid);
        if (!colorValid) {
            QMessageBox::warning(this, tr("Invalid catalog color"),
                tr("Opcode %1 has an invalid color. Use #RRGGBB.").arg(opcode));
            tree_->setCurrentItem(item, 6); return false;
        }
        if (!item->text(7).trimmed().isEmpty())
            entry.category = item->text(7).toStdString();
        for (int childRow = 0; childRow < item->childCount(); ++childRow) {
            auto* child = item->child(childRow);
            const auto schemaIndex = child->data(0, ParameterRole).toUInt();
            const auto* parameter = spice::sct::sctOpcodeParameterSchema(*schema, schemaIndex);
            core::SctCatalogParameterOverride custom{schemaIndex};
            if (!parameter || child->text(2) != q(parameter->role))
                custom.label = child->text(2).toStdString();
            bool defaultValid = false;
            custom.creationDefaultWord = parseWord(child->text(5), &defaultValid);
            if (!defaultValid) {
                QMessageBox::warning(this, tr("Invalid creation default"),
                    tr("Opcode %1 parameter %2 needs a decimal or 0x-prefixed word.")
                        .arg(opcode).arg(schemaIndex));
                tree_->setCurrentItem(child, 5); return false;
            }
            if (custom.label || custom.creationDefaultWord)
                entry.parameters.push_back(std::move(custom));
        }
        if (entry.mnemonic || entry.description || entry.note || !entry.parameters.empty()
            || entry.colorRgb || entry.category)
            result.opcodes.push_back(std::move(entry));
    }
    return true;
}

void SctCatalogEditorDialog::importLegacy() {
    const auto path = QFileDialog::getOpenFileName(this,
        tr("Import Legacy UserInstructions.json"), {}, tr("JSON files (*.json)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import failed"), file.errorString()); return;
    }
    const auto bytes = file.readAll();
    const auto preview = core::SctLegacyCatalogImporter::preview(
        std::as_bytes(std::span{bytes.constData(), static_cast<std::size_t>(bytes.size())}),
        catalog_);
    if (!preview) {
        QMessageBox::warning(this, tr("Import failed"),
            QString::fromStdString(preview.diagnostics().front().message)); return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Review Legacy Catalog Fields"));
    dialog.resize(760, 520);
    auto* layout = new QVBoxLayout(&dialog);
    auto* table = new QTableWidget(static_cast<int>(preview.value().changes.size()), 4,
        &dialog);
    table->setHorizontalHeaderLabels({tr("Import"), tr("Opcode"), tr("Field"),
        tr("Incoming value")});
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto& change = preview.value().changes[static_cast<std::size_t>(row)];
        auto* check = new QTableWidgetItem;
        check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        check->setCheckState(change.conflict ? Qt::Unchecked : Qt::Checked);
        if (change.conflict) check->setToolTip(tr("Conflict: keeping the current value is the default."));
        table->setItem(row, 0, check);
        table->setItem(row, 1, new QTableWidgetItem(QString::number(change.opcode)));
        table->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(change.field)));
        table->setItem(row, 3, new QTableWidgetItem(
            QString::fromStdString(change.incomingSummary)));
    }
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    layout->addWidget(table);
    if (!preview.value().ignoredLockedFields.empty())
        layout->addWidget(new QLabel(tr("%1 locked or unsupported legacy fields will be ignored.")
            .arg(preview.value().ignoredLockedFields.size()), &dialog));
    auto* reviewButtons = new QDialogButtonBox(QDialogButtonBox::Apply
        | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(reviewButtons);
    connect(reviewButtons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(reviewButtons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    std::vector<core::SctLegacyCatalogFieldChange> selected;
    for (int row = 0; row < table->rowCount(); ++row)
        if (table->item(row, 0)->checkState() == Qt::Checked)
            selected.push_back(preview.value().changes[static_cast<std::size_t>(row)]);
    auto merged = core::SctLegacyCatalogImporter::apply(
        preview.value(), catalog_, selected);
    if (!merged) {
        QMessageBox::warning(this, tr("Import failed"),
            QString::fromStdString(merged.diagnostics().front().message)); return;
    }
    catalog_ = std::move(merged).takeValue();
    populate();
}

void SctCatalogEditorDialog::accept() {
    core::SctPersonalCatalog result;
    if (!collect(result)) return;
    catalog_ = std::move(result);
    QDialog::accept();
}

} // namespace salsa::qt
