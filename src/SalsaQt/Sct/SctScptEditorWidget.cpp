#include "Sct/SctScptEditorWidget.h"

#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include "SpiceSCT/SctScptEncoding.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <charconv>
#include <array>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <type_traits>
#include <ranges>

namespace salsa::qt {
namespace {

QString hexWord(const std::uint32_t value) {
    return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
}

QString valueKindName(const spice::sct::SctScptValueKind kind) {
    using enum spice::sct::SctScptValueKind;
    switch (kind) {
    case InlineValue: return QObject::tr("Inline value");
    case FloatLiteral: return QObject::tr("Float literal");
    case DecimalLiteral: return QObject::tr("Fixed decimal");
    case ByteVariable: return QObject::tr("Byte variable");
    case BitVariable: return QObject::tr("Bit variable");
    case FloatVariable: return QObject::tr("Float variable");
    case DirectIntVariable: return QObject::tr("Integer variable");
    case NegatedIntVariable: return QObject::tr("Negated integer input");
    case NegatedIntVariableLow16Comparison: return QObject::tr("Low-16 comparison input");
    case SecondaryValue: return QObject::tr("Named runtime value");
    }
    return QObject::tr("Value");
}

QString operationText(const spice::sct::SctScptOperation& operation) {
    return std::visit([](const auto& value) -> QString {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, spice::sct::SctScptValueOperation>) {
            spice::sct::SctCanonicalExpression expression{
                spice::sct::SctTypedScptProgram{{value}},
                value.kind == spice::sct::SctScptValueKind::InlineValue
                    ? spice::sct::SctExpressionTermination::InlineValue
                    : spice::sct::SctExpressionTermination::StopCode};
            const auto projection = core::SctExpressionLanguage::project(expression);
            return projection.text.empty() ? valueKindName(value.kind)
                : QString::fromStdString(projection.text);
        }
        else if constexpr (std::is_same_v<T, spice::sct::SctScptBinaryOperation>) {
            const auto symbol = spice::sct::sctScptOperatorSymbol(value.encodingWord);
            return QObject::tr("Binary %1 — %2")
                .arg(value.kind == spice::sct::SctScptBinaryOperationKind::Comparison
                    ? QObject::tr("comparison") : QObject::tr("arithmetic"))
                .arg(QString::fromUtf8(symbol.data(),
                    static_cast<qsizetype>(symbol.size())));
        }
        else if constexpr (std::is_same_v<T,
                spice::sct::SctScptStackOverwritePreviousWithTopOperation>)
            return QObject::tr("Stack overwrite: previous = top");
        else return QObject::tr("Inert word (no known stack effect)");
    }, operation);
}

QString operationCategory(const spice::sct::SctScptOperation& operation) {
    if (std::holds_alternative<spice::sct::SctScptValueOperation>(operation))
        return QObject::tr("Push value");
    if (const auto* binary = std::get_if<spice::sct::SctScptBinaryOperation>(&operation))
        return binary->kind == spice::sct::SctScptBinaryOperationKind::Comparison
            ? QObject::tr("Comparison / logic") : QObject::tr("Arithmetic");
    if (std::holds_alternative<spice::sct::SctScptStackOverwritePreviousWithTopOperation>(operation))
        return QObject::tr("Stack operation");
    return QObject::tr("Preserved inert word");
}

QString operationWords(const spice::sct::SctScptOperation& operation) {
    return std::visit([](const auto& value) {
        QStringList result{hexWord(value.encodingWord)};
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>,
                spice::sct::SctScptValueOperation>)
            for (const auto payload : value.payloadWords) result.push_back(hexWord(payload));
        return result.join(QLatin1Char(' '));
    }, operation);
}

QString stackEffect(const spice::sct::SctScptOperation& operation) {
    if (std::holds_alternative<spice::sct::SctScptValueOperation>(operation)) return QStringLiteral("+1");
    if (std::holds_alternative<spice::sct::SctScptBinaryOperation>(operation)) return QStringLiteral("-1");
    return QStringLiteral("0");
}

} // namespace

SctScptEditorWidget::SctScptEditorWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    heading_ = new QLabel(tr("Select a typed SCPT parameter to begin."), this);
    heading_->setWordWrap(true);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    layout->addWidget(heading_);
    layout->addWidget(status_);

    auto* composer = new QHBoxLayout;
    expressionEdit_ = new QLineEdit(this);
    expressionEdit_->setPlaceholderText(
        tr("Conventional expression, for example ByteVar[87] == 3"));
    expressionEdit_->setToolTip(tr(
        "This replaces the ordered draft with the canonical postfix program for a conventional expression."));
    auto* replaceSimple = new QPushButton(
        tr("Convert to Conventional Expression"), this);
    replaceSimple->setToolTip(tr(
        "Explicitly discard nonconventional stack details and replace this draft with the entered expression."));
    composer->addWidget(expressionEdit_, 1);
    composer->addWidget(replaceSimple);
    layout->addLayout(composer);

    operationsTable_ = new QTableWidget(this);
    operationsTable_->setColumnCount(5);
    operationsTable_->setHorizontalHeaderLabels({tr("Step"), tr("Operation"),
        tr("Meaning / operand"), tr("Encoding"), tr("Stack")});
    operationsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    operationsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    operationsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    operationsTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    operationsTable_->horizontalHeader()->resizeSection(0, 55);
    operationsTable_->horizontalHeader()->resizeSection(1, 150);
    operationsTable_->horizontalHeader()->resizeSection(2, 260);
    operationsTable_->horizontalHeader()->resizeSection(3, 180);
    operationsTable_->horizontalHeader()->resizeSection(4, 55);
    layout->addWidget(new QLabel(tr("Advanced ordered operations (authoritative)"), this));
    layout->addWidget(operationsTable_, 1);
    auto* operations = new QHBoxLayout;
    auto* addValue = new QPushButton(tr("Add Value..."), this);
    auto* addBinary = new QPushButton(tr("Add Operator"), this);
    auto* addOverwrite = new QPushButton(tr("Add Stack Operation"), this);
    addOverwrite->setToolTip(tr("Insert overwrite-previous-with-top (0x0A): copy the top value into the preceding stack slot, clear the destination comparison flag, and keep the same stack depth."));
    auto* remove = new QPushButton(tr("Remove"), this);
    auto* edit = new QPushButton(tr("Edit"), this);
    auto* up = new QPushButton(tr("Up"), this);
    auto* down = new QPushButton(tr("Down"), this);
    for (auto* button : {addValue, addBinary, addOverwrite,
            edit, remove, up, down}) operations->addWidget(button);
    layout->addLayout(operations);
    preview_ = new QLabel(this);
    preview_->setWordWrap(true);
    layout->addWidget(preview_);
    auto* actions = new QDialogButtonBox(this);
    applyButton_ = actions->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
    revertButton_ = actions->addButton(tr("Revert"), QDialogButtonBox::ResetRole);
    layout->addWidget(actions);

    connect(replaceSimple, &QPushButton::clicked, this,
        &SctScptEditorWidget::convertToConventionalExpression);
    connect(addValue, &QPushButton::clicked, this, [this] { addOperation(0); });
    auto* operatorMenu = new QMenu(addBinary);
    const auto addOperatorGroup = [this, operatorMenu](const QString& name,
            const std::initializer_list<std::pair<const char*, std::uint32_t>> values) {
        auto* menu = operatorMenu->addMenu(name);
        for (const auto& [label, word] : values) {
            auto* action = menu->addAction(QString::fromUtf8(label));
            connect(action, &QAction::triggered, this,
                [this, word] { addOperator(word); });
        }
    };
    addOperatorGroup(tr("Comparisons"), {{"<", 0x00u}, {"<=", 0x01u},
        {">", 0x02u}, {">=", 0x03u}, {"==", 0x04u}, {"!=", 0x05u}});
    addOperatorGroup(tr("Arithmetic"), {{"*", 0x0bu}, {"/", 0x0cu},
        {"%", 0x0du}, {"+", 0x0eu}, {"-", 0x0fu}});
    addOperatorGroup(tr("Bitwise"), {{"&", 0x06u}, {"|", 0x07u}});
    addOperatorGroup(tr("Logical"), {{"&&", 0x08u}, {"||", 0x09u}});
    addBinary->setMenu(operatorMenu);
    connect(addOverwrite, &QPushButton::clicked, this, [this] { addOperation(2); });
    connect(edit, &QPushButton::clicked, this, &SctScptEditorWidget::editSelected);
    connect(remove, &QPushButton::clicked, this, &SctScptEditorWidget::removeSelected);
    connect(up, &QPushButton::clicked, this, [this] { moveSelected(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveSelected(1); });
    connect(applyButton_, &QPushButton::clicked, this, &SctScptEditorWidget::apply);
    connect(revertButton_, &QPushButton::clicked, this, &SctScptEditorWidget::revert);
    refresh();
}

void SctScptEditorWidget::setCommitHandler(CommitHandler handler) {
    commitHandler_ = std::move(handler);
}

bool SctScptEditorWidget::bind(const core::AssetLocator& locator,
    const spice::sct::SctParameterSite& site,
    const spice::sct::SctCanonicalExpression& expression) {
    if (dirty_ && locator_ && *locator_ == locator && site_ && *site_ == site)
        return true;
    if (dirty_ && (!locator_ || *locator_ != locator || !site_ || *site_ != site)) {
        const auto choice = QMessageBox::question(this, tr("Unsaved SCPT Draft"),
            tr("Apply the current SCPT draft before opening another parameter?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (choice == QMessageBox::Cancel) return false;
        if (choice == QMessageBox::Save) {
            apply();
            if (dirty_) return false;
        }
    }
    locator_ = locator;
    site_ = site;
    original_ = expression;
    termination_ = expression.termination;
    const auto* program = std::get_if<spice::sct::SctTypedScptProgram>(&expression.body);
    operations_ = program == nullptr ? std::vector<spice::sct::SctScptOperation>{}
        : program->operations;
    stale_ = false;
    setDirty(false);
    refresh();
    return true;
}

bool SctScptEditorWidget::prepareToClear() {
    if (!dirty_) return true;
    const auto choice = QMessageBox::question(this, tr("Unsaved SCPT Draft"),
        tr("Apply the current SCPT draft before closing its document?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (choice == QMessageBox::Cancel) return false;
    if (choice == QMessageBox::Save) {
        apply();
        return !dirty_;
    }
    revert();
    return true;
}

void SctScptEditorWidget::clear() {
    locator_.reset(); site_.reset(); original_.reset(); operations_.clear();
    stale_ = false; setDirty(false); refresh();
}

void SctScptEditorWidget::refreshOrMarkStale(
    const core::AssetLocator& locator, const core::SctEditChangeSet& changes,
    std::optional<spice::sct::SctCanonicalExpression> current) {
    if (committing_ || !locator_ || *locator_ != locator || !site_) return;
    const bool parameterAffected = std::ranges::any_of(
        changes.parameters, [this](const auto& change) {
            return change.site == *site_;
        });
    const bool groupAddressInvalidated = site_->parameter.repeatedGroupOrdinal
        && std::ranges::any_of(changes.repeatedGroups, [this](const auto& change) {
            return change.instruction == site_->instruction;
        });
    const bool instructionDeleted = std::ranges::any_of(
        changes.instructions, [this](const auto& change) {
            return change.instruction == site_->instruction && !change.after;
        });
    if (!parameterAffected && !groupAddressInvalidated && !instructionDeleted) return;
    if (dirty_) {
        stale_ = true;
        refresh();
        return;
    }
    if (!current || groupAddressInvalidated || instructionDeleted) {
        clear();
        return;
    }
    original_ = *current;
    termination_ = current->termination;
    const auto* program = std::get_if<spice::sct::SctTypedScptProgram>(&current->body);
    operations_ = program == nullptr ? std::vector<spice::sct::SctScptOperation>{}
        : program->operations;
    stale_ = false;
    refresh();
}

void SctScptEditorWidget::refresh() {
    operationsTable_->setRowCount(static_cast<int>(operations_.size()));
    for (int row = 0; row < static_cast<int>(operations_.size()); ++row) {
        const auto& operation = operations_[static_cast<std::size_t>(row)];
        const std::array values{QString::number(row + 1), operationCategory(operation),
            operationText(operation), operationWords(operation), stackEffect(operation)};
        for (int column = 0; column < static_cast<int>(values.size()); ++column) {
            auto* item = new QTableWidgetItem(values[static_cast<std::size_t>(column)]);
            item->setToolTip(values[static_cast<std::size_t>(column)]);
            operationsTable_->setItem(row, column, item);
        }
    }
    const spice::sct::SctCanonicalExpression expression{
        spice::sct::SctTypedScptProgram{operations_}, termination_};
    const auto projection = core::SctExpressionLanguage::project(expression);
    expressionEdit_->setText(projection.availability
            == core::SctExpressionTextAvailability::Editable
        ? QString::fromStdString(projection.text) : QString{});
    if (!projection.text.empty()) {
        preview_->setText(projection.availability
                == core::SctExpressionTextAvailability::Editable
            ? tr("Semantic result: %1").arg(QString::fromStdString(projection.text))
            : tr("Preview only — not an exact editable representation: %1")
                .arg(QString::fromStdString(projection.text)));
    } else {
        preview_->setText(tr("Semantic result unavailable: %1")
            .arg(QString::fromStdString(projection.explanation)));
    }
    const bool bound = locator_.has_value() && site_.has_value();
    if (bound) heading_->setText(tr("%1 — instruction %2, parameter %3")
        .arg(QString::fromStdWString(locator_->path().wstring()))
        .arg(site_->instruction.value()).arg(site_->parameter.schemaIndex));
    else heading_->setText(tr("Select a typed SCPT parameter to begin."));
    status_->setText(stale_ ? tr("This draft is stale because its parameter changed. Revert or discard it before applying.")
        : dirty_ ? tr("Detached draft has unapplied changes.") : QString{});
    applyButton_->setEnabled(bound && dirty_ && !stale_);
    revertButton_->setEnabled(bound && dirty_);
}

void SctScptEditorWidget::setDirty(const bool dirty) {
    dirty_ = dirty;
}

void SctScptEditorWidget::convertToConventionalExpression() {
    if (!site_) return;
    const auto parsed = core::SctExpressionLanguage::parse(
        expressionEdit_->text().trimmed().toStdString());
    if (!parsed.succeeded()) {
        status_->setText(parsed.issues.empty()
            ? tr("The conventional expression is invalid.")
            : QString::fromStdString(parsed.issues.front().message));
        return;
    }
    const auto* program = std::get_if<spice::sct::SctTypedScptProgram>(
        &parsed.expression->body);
    operations_ = program->operations;
    termination_ = parsed.expression->termination;
    setDirty(true); refresh();
}

void SctScptEditorWidget::editSelected() {
    const auto row = operationsTable_->currentRow();
    if (row < 0 || row >= static_cast<int>(operations_.size())) return;
    auto& operation = operations_[static_cast<std::size_t>(row)];
    if (auto* binary = std::get_if<spice::sct::SctScptBinaryOperation>(&operation)) {
        const QStringList labels{QStringLiteral("<  [0x00]"), QStringLiteral("<=  [0x01]"),
            QStringLiteral(">"), QStringLiteral(">="), QStringLiteral("=="),
            QStringLiteral("!="), QStringLiteral("&"), QStringLiteral("|"),
            QStringLiteral("&&"), QStringLiteral("||"), QStringLiteral("*"),
            QStringLiteral("/"), QStringLiteral("%"), QStringLiteral("+"),
            QStringLiteral("-"), QStringLiteral("&  [alternate 0x10]"),
            QStringLiteral("|  [alternate 0x11]"),
            QStringLiteral("*  [alternate 0x12]"),
            QStringLiteral("/  [alternate 0x13]"),
            QStringLiteral("%  [alternate 0x14]"),
            QStringLiteral("+  [alternate 0x15]"),
            QStringLiteral("-  [alternate 0x16]")};
        const std::array<std::uint32_t, 22> words{0x00u, 0x01u, 0x02u, 0x03u,
            0x04u, 0x05u, 0x06u, 0x07u, 0x08u, 0x09u, 0x0bu, 0x0cu,
            0x0du, 0x0eu, 0x0fu, 0x10u, 0x11u, 0x12u, 0x13u, 0x14u,
            0x15u, 0x16u};
        const auto current = static_cast<int>(std::distance(words.begin(),
            std::ranges::find(words, binary->encodingWord)));
        bool accepted = false;
        const auto selected = QInputDialog::getItem(this, tr("Edit Binary Operation"),
            tr("Operator:"), labels, current < labels.size() ? current : 0,
            false, &accepted);
        if (!accepted) return;
        const auto index = labels.indexOf(selected);
        if (index < 0) return;
        binary->encodingWord = words[static_cast<std::size_t>(index)];
        binary->kind = index < 10
            ? spice::sct::SctScptBinaryOperationKind::Comparison
            : spice::sct::SctScptBinaryOperationKind::Arithmetic;
        setDirty(true); refresh(); operationsTable_->selectRow(row);
        return;
    }
    if (const auto* value = std::get_if<spice::sct::SctScptValueOperation>(&operation)) {
        int kind = -1;
        using enum spice::sct::SctScptValueKind;
        if (value->kind == DecimalLiteral) kind = 0;
        else if (value->kind == FloatLiteral) kind = 1;
        else if (value->kind == DirectIntVariable) kind = 2;
        else if (value->kind == FloatVariable) kind = 3;
        else if (value->kind == BitVariable) kind = 4;
        else if (value->kind == ByteVariable) kind = 5;
        if (kind < 0) {
            status_->setText(tr("Use the semantic composer to replace this specialized value operation."));
            return;
        }
        bool accepted = false;
        const auto input = QInputDialog::getText(this, tr("Edit Value Operation"),
            tr("Typed value expression:"), QLineEdit::Normal,
            operationText(operation), &accepted);
        if (!accepted) return;
        const auto parsed = core::SctExpressionLanguage::parse(
            input.trimmed().toStdString());
        const auto* program = parsed.expression
            ? std::get_if<spice::sct::SctTypedScptProgram>(&parsed.expression->body)
            : nullptr;
        if (program == nullptr || program->operations.size() != 1u) {
            status_->setText(tr("The edited operation is outside its typed domain."));
            return;
        }
        operation = program->operations.front();
        setDirty(true); refresh(); operationsTable_->selectRow(row);
    }
}

void SctScptEditorWidget::addOperation(const int kind) {
    if (!site_) return;
    if (kind == 0) {
        const QStringList kinds{tr("Fixed decimal"), tr("Float literal"),
            tr("Integer input (factory-selected form)"), tr("Direct integer variable"),
            tr("Negated integer input"), tr("Low-16 comparison input"),
            tr("Float variable"), tr("Bit variable"), tr("Byte variable"),
            tr("Named runtime value"), tr("Inline constant")};
        const QStringList examples{QStringLiteral("0"), QStringLiteral("0f"),
            QStringLiteral("IntInput[24]"), QStringLiteral("IntVar[24]"),
            QStringLiteral("NegatedIntVar[87]"), QStringLiteral("Low16IntVar[15]"),
            QStringLiteral("FloatVar[0]"), QStringLiteral("BitVar[0]"),
            QStringLiteral("ByteVar[0]"), QStringLiteral("Gold"),
            QStringLiteral("InlineValue[0x7F7FFFFF]")};
        bool accepted = false;
        const auto selected = QInputDialog::getItem(this, tr("Add Value"),
            tr("Value kind:"), kinds, 0, false, &accepted);
        if (!accepted) return;
        const auto selectedKind = kinds.indexOf(selected);
        const auto input = QInputDialog::getText(this, tr("Add Value"),
            tr("Typed value expression:"), QLineEdit::Normal,
            examples.value(selectedKind), &accepted);
        if (!accepted) return;
        const auto parsed = core::SctExpressionLanguage::parse(
            input.trimmed().toStdString());
        const auto* program = parsed.expression
            ? std::get_if<spice::sct::SctTypedScptProgram>(&parsed.expression->body)
            : nullptr;
        if (program == nullptr || program->operations.size() != 1u) {
            status_->setText(tr("The value is invalid or outside its typed domain."));
            return;
        }
        operations_.push_back(program->operations.front());
    } else if (kind == 2) {
        operations_.push_back(
            spice::sct::SctExpressionFactory::stackOverwritePreviousWithTop());
    }
    setDirty(true); refresh();
}

void SctScptEditorWidget::addOperator(const std::uint32_t encodingWord) {
    if (!site_) return;
    const auto classification = spice::sct::classifySctScptWord(encodingWord);
    const auto kind = classification.kind == spice::sct::SctScptWordKind::CompareOperator
        ? spice::sct::SctScptBinaryOperationKind::Comparison
        : spice::sct::SctScptBinaryOperationKind::Arithmetic;
    operations_.push_back(spice::sct::SctScptBinaryOperation{kind, encodingWord});
    setDirty(true); refresh();
    operationsTable_->selectRow(static_cast<int>(operations_.size()) - 1);
}

void SctScptEditorWidget::removeSelected() {
    const auto row = operationsTable_->currentRow();
    if (row < 0 || row >= static_cast<int>(operations_.size())) return;
    operations_.erase(operations_.begin() + row);
    setDirty(true); refresh();
}

void SctScptEditorWidget::moveSelected(const int direction) {
    const auto row = operationsTable_->currentRow();
    const auto target = row + direction;
    if (row < 0 || target < 0 || row >= static_cast<int>(operations_.size())
        || target >= static_cast<int>(operations_.size())) return;
    std::swap(operations_[row], operations_[target]);
    setDirty(true); refresh();
    operationsTable_->selectRow(target);
}

void SctScptEditorWidget::apply() {
    if (!locator_ || !site_ || !commitHandler_ || stale_) return;
    const auto built = spice::sct::SctExpressionFactory::program(operations_, termination_);
    if (!built.expression) {
        status_->setText(tr("The ordered operations do not form one valid SCPT parameter. The draft was retained."));
        return;
    }
    committing_ = true;
    const auto committed = commitHandler_(*locator_, *site_, *built.expression);
    committing_ = false;
    if (!committed) {
        status_->setText(tr("The parameter edit was rejected. The draft was retained."));
        return;
    }
    original_ = *built.expression;
    setDirty(false); refresh();
    if (!built.diagnostics.empty())
        status_->setText(tr("Applied with %1 advisory warning(s).")
            .arg(built.diagnostics.size()));
}

void SctScptEditorWidget::revert() {
    if (stale_) {
        clear();
        return;
    }
    if (!original_) return;
    const auto* program = std::get_if<spice::sct::SctTypedScptProgram>(&original_->body);
    operations_ = program == nullptr ? std::vector<spice::sct::SctScptOperation>{}
        : program->operations;
    termination_ = original_->termination;
    stale_ = false; setDirty(false); refresh();
}

} // namespace salsa::qt
