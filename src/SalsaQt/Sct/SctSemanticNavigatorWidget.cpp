#include "Sct/SctSemanticNavigatorWidget.h"

#include "SpiceSCT/SctDocumentAnalysis.h"
#include "SpiceSCT/SctOpcodeMetadata.h"

#include <QHeaderView>
#include <QLabel>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringList>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

namespace salsa::qt {
namespace {

constexpr int StableKeyRole = Qt::UserRole;
constexpr int NavigationRole = Qt::UserRole + 1;
constexpr int FallbackNavigationRole = Qt::UserRole + 2;

struct TreeState final {
    QStringList expandedKeys;
    QString selectedKey;
    int scrollPosition = 0;
};

[[nodiscard]] QString escapedBytes(const std::string& bytes) {
    QString result;
    for (const auto byte : bytes) {
        const auto value = static_cast<unsigned char>(byte);
        if (value >= 0x20 && value < 0x7f && value != '\\') {
            result += QChar(value);
        } else if (value == '\\') {
            result += QStringLiteral("\\\\");
        } else {
            result += QStringLiteral("\\x%1").arg(value, 2, 16, QLatin1Char('0')).toUpper();
        }
    }
    return result.isEmpty() ? QStringLiteral("(empty)") : result;
}

[[nodiscard]] QString opcodeName(const std::uint16_t opcode) {
    const auto* schema = spice::sct::findSctOpcodeSchema(opcode);
    const auto mnemonic = schema != nullptr && !schema->semantic.mnemonic.empty()
        ? QString::fromUtf8(schema->semantic.mnemonic.data(),
            static_cast<qsizetype>(schema->semantic.mnemonic.size()))
        : SctSemanticNavigatorWidget::tr("Unknown opcode");
    return QStringLiteral("%1 (%2)").arg(mnemonic).arg(opcode);
}

[[nodiscard]] QString instructionName(
    const spice::sct::SctDocument& document,
    const spice::sct::SctDocumentIndex& index,
    const spice::sct::SctInstructionId instruction) {
    const auto* value = index.find(document, instruction);
    return value == nullptr
        ? SctSemanticNavigatorWidget::tr("Instruction %1").arg(instruction.value())
        : QStringLiteral("%1 — %2")
            .arg(opcodeName(value->opcode))
            .arg(SctSemanticNavigatorWidget::tr("Instruction %1").arg(instruction.value()));
}

[[nodiscard]] QString instructionContext(
    const spice::sct::SctDocument& document,
    const spice::sct::SctDocumentIndex& index,
    const spice::sct::SctInstructionId instruction) {
    const auto location = index.instructionLocation(instruction);
    const auto* section = location.has_value()
        ? index.find(document, location->sectionId) : nullptr;
    return section == nullptr
        ? SctSemanticNavigatorWidget::tr("Unknown section")
        : QStringLiteral("[%1] %2")
            .arg(location->sectionOrdinal)
            .arg(escapedBytes(section->nameBytes));
}

[[nodiscard]] QString parameterName(const spice::sct::SctParameterAddress& parameter) {
    if (parameter.repeatedGroupOrdinal.has_value()) {
        return SctSemanticNavigatorWidget::tr("Group %1 / Parameter %2")
            .arg(*parameter.repeatedGroupOrdinal)
            .arg(parameter.schemaIndex);
    }
    return SctSemanticNavigatorWidget::tr("Parameter %1").arg(parameter.schemaIndex);
}

[[nodiscard]] QString expressionName(const spice::sct::SctExpressionSite& site) {
    QString result;
    if (std::holds_alternative<spice::sct::SctScheduledExpressionSite>(site.owner)) {
        result = SctSemanticNavigatorWidget::tr("Scheduled expression");
    } else {
        result = parameterName(std::get<spice::sct::SctParameterAddress>(site.owner));
    }
    result += SctSemanticNavigatorWidget::tr(" / program");
    return result;
}

[[nodiscard]] QString expressionName(
    const spice::sct::SctExpressionOperationSite& site) {
    return expressionName(site.expression)
        + SctSemanticNavigatorWidget::tr(" / operation %1")
            .arg(site.operationOrdinal);
}

[[nodiscard]] QString parameterKey(const spice::sct::SctParameterAddress& parameter) {
    return QStringLiteral("%1:%2")
        .arg(parameter.schemaIndex)
        .arg(parameter.repeatedGroupOrdinal.has_value()
            ? QString::number(*parameter.repeatedGroupOrdinal)
            : QStringLiteral("fixed"));
}

[[nodiscard]] QString inspectionKey(const core::SctInspectionLocation& location) {
    return std::visit([](const auto& typed) -> QString {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, core::SctNavigationTarget>) {
            return QStringLiteral("entity:%1:%2")
                .arg(static_cast<int>(typed.kind)).arg(typed.id);
        } else if constexpr (std::is_same_v<T, spice::sct::SctParameterSite>) {
            return QStringLiteral("parameter:%1:%2")
                .arg(typed.instruction.value()).arg(parameterKey(typed.parameter));
        } else if constexpr (std::is_same_v<T, spice::sct::SctExpressionSite>) {
            QString key = QStringLiteral("expression:%1:")
                .arg(typed.instruction.value());
            if (std::holds_alternative<spice::sct::SctScheduledExpressionSite>(typed.owner)) {
                key += QStringLiteral("scheduled");
            } else {
                key += QStringLiteral("parameter:")
                    + parameterKey(std::get<spice::sct::SctParameterAddress>(typed.owner));
            }
            return key;
        } else {
            return inspectionKey(core::SctInspectionLocation{typed.expression})
                + QStringLiteral(":operation:%1").arg(typed.operationOrdinal);
        }
    }, location);
}

[[nodiscard]] QString variableKindName(const spice::sct::SctVariableKind kind) {
    switch (kind) {
    case spice::sct::SctVariableKind::Integer: return SctSemanticNavigatorWidget::tr("Integer");
    case spice::sct::SctVariableKind::Float: return SctSemanticNavigatorWidget::tr("Float");
    case spice::sct::SctVariableKind::Bit: return SctSemanticNavigatorWidget::tr("Bit");
    case spice::sct::SctVariableKind::Byte: return SctSemanticNavigatorWidget::tr("Byte");
    }
    return SctSemanticNavigatorWidget::tr("Unknown");
}

[[nodiscard]] QString expectedTargetName(
    const spice::sct::SctExpectedReferenceTarget& target) {
    switch (target.storage) {
    case spice::sct::SctReferenceTargetStorage::Instruction:
        return SctSemanticNavigatorWidget::tr("Expected instruction reference");
    case spice::sct::SctReferenceTargetStorage::IndexedString:
        return SctSemanticNavigatorWidget::tr("Expected indexed-string reference");
    case spice::sct::SctReferenceTargetStorage::FooterEntry:
        return SctSemanticNavigatorWidget::tr("Expected footer-entry reference");
    }
    return SctSemanticNavigatorWidget::tr("Expected reference");
}

[[nodiscard]] core::SctNavigationTarget referenceTarget(
    const spice::sct::SctDocumentReferenceTarget& target) {
    return std::visit([](const auto typed) -> core::SctNavigationTarget {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
            return { core::SctNavigationKind::Instruction, typed.value() };
        else if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return { core::SctNavigationKind::String, typed.value() };
        else
            return { core::SctNavigationKind::FooterEntry, typed.value() };
    }, target);
}

[[nodiscard]] QString referenceTargetName(
    const spice::sct::SctDocumentReferenceTarget& target) {
    return std::visit([](const auto typed) -> QString {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
            return SctSemanticNavigatorWidget::tr("Instruction %1").arg(typed.value());
        else if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return SctSemanticNavigatorWidget::tr("Indexed string %1").arg(typed.value());
        else
            return SctSemanticNavigatorWidget::tr("Footer entry %1").arg(typed.value());
    }, target);
}

[[nodiscard]] bool targetExists(
    const spice::sct::SctDocument& document,
    const spice::sct::SctDocumentIndex& index,
    const spice::sct::SctDocumentReferenceTarget& target) {
    return std::visit([&](const auto typed) {
        return index.find(document, typed) != nullptr;
    }, target);
}

[[nodiscard]] QString placementName(const spice::sct::SctOpaquePlacement placement) {
    switch (placement) {
    case spice::sct::SctOpaquePlacement::Before: return SctSemanticNavigatorWidget::tr("before");
    case spice::sct::SctOpaquePlacement::After: return SctSemanticNavigatorWidget::tr("after");
    case spice::sct::SctOpaquePlacement::FixedOffset: return SctSemanticNavigatorWidget::tr("fixed offset");
    }
    return SctSemanticNavigatorWidget::tr("unknown placement");
}

[[nodiscard]] QString opaqueReasonName(const spice::sct::SctOpaqueReason reason) {
    using enum spice::sct::SctOpaqueReason;
    switch (reason) {
    case Header: return SctSemanticNavigatorWidget::tr("Header");
    case Preamble: return SctSemanticNavigatorWidget::tr("Preamble");
    case Padding: return SctSemanticNavigatorWidget::tr("Padding");
    case Gap: return SctSemanticNavigatorWidget::tr("Gap");
    case Unreached: return SctSemanticNavigatorWidget::tr("Unreached");
    case UnknownEncoding: return SctSemanticNavigatorWidget::tr("Unknown encoding");
    case ContradictoryEvidence: return SctSemanticNavigatorWidget::tr("Contradictory evidence");
    }
    return SctSemanticNavigatorWidget::tr("Unknown");
}

[[nodiscard]] std::pair<QString, core::SctNavigationTarget> opaqueAnchor(
    const spice::sct::SctOpaqueAnchor& anchor) {
    const auto name = std::visit([](const auto typed) -> QString {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctDocumentAnchor>)
            return SctSemanticNavigatorWidget::tr("Document");
        else if constexpr (std::is_same_v<T, spice::sct::SctSectionId>)
            return SctSemanticNavigatorWidget::tr("Section %1").arg(typed.value());
        else if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
            return SctSemanticNavigatorWidget::tr("Instruction %1").arg(typed.value());
        else if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return SctSemanticNavigatorWidget::tr("Indexed string %1").arg(typed.value());
        else
            return SctSemanticNavigatorWidget::tr("Footer entry %1").arg(typed.value());
    }, anchor);
    return { name, core::navigationTargetForOpaqueAnchor(anchor) };
}

void configureTree(QTreeWidget& tree, const QStringList& headers) {
    tree.setHeaderLabels(headers);
    tree.setAlternatingRowColors(true);
    tree.setSelectionMode(QAbstractItemView::SingleSelection);
    tree.header()->setSectionResizeMode(QHeaderView::Interactive);
    tree.header()->setStretchLastSection(false);
    for (int column = 0; column < headers.size(); ++column)
        tree.header()->resizeSection(column, column == 0 ? 240 : 190);
}

void collectExpanded(const QTreeWidgetItem& item, QStringList& keys) {
    if (item.isExpanded() && !item.data(0, StableKeyRole).toString().isEmpty())
        keys.push_back(item.data(0, StableKeyRole).toString());
    for (int child = 0; child < item.childCount(); ++child)
        collectExpanded(*item.child(child), keys);
}

[[nodiscard]] QTreeWidgetItem* findKey(QTreeWidgetItem* item, const QString& key) {
    if (item->data(0, StableKeyRole).toString() == key) return item;
    for (int child = 0; child < item->childCount(); ++child)
        if (auto* found = findKey(item->child(child), key)) return found;
    return nullptr;
}

[[nodiscard]] QTreeWidgetItem* findKey(QTreeWidget& tree, const QString& key) {
    for (int row = 0; row < tree.topLevelItemCount(); ++row)
        if (auto* found = findKey(tree.topLevelItem(row), key)) return found;
    return nullptr;
}

[[nodiscard]] TreeState captureState(QTreeWidget& tree) {
    TreeState state;
    for (int row = 0; row < tree.topLevelItemCount(); ++row)
        collectExpanded(*tree.topLevelItem(row), state.expandedKeys);
    if (tree.currentItem() != nullptr)
        state.selectedKey = tree.currentItem()->data(0, StableKeyRole).toString();
    state.scrollPosition = tree.verticalScrollBar()->value();
    return state;
}

void restoreState(QTreeWidget& tree, const TreeState& state) {
    for (const auto& key : state.expandedKeys)
        if (auto* item = findKey(tree, key)) item->setExpanded(true);
    if (auto* item = findKey(tree, state.selectedKey)) tree.setCurrentItem(item);
    tree.doItemsLayout();
    tree.verticalScrollBar()->setValue(state.scrollPosition);
}

void setKey(QTreeWidgetItem& item, const QString& key) {
    item.setData(0, StableKeyRole, key);
}

[[nodiscard]] std::optional<spice::sct::SctInstructionId> sourceInstructionFor(
    const QTreeWidgetItem& item) {
    const auto key = item.data(0, StableKeyRole).toString();
    if (key.startsWith(QStringLiteral("reference:target:"))) return std::nullopt;
    auto location = item.data(0, FallbackNavigationRole);
    if (!location.isValid()) {
        for (int column = 0; column < item.columnCount(); ++column) {
            location = item.data(column, NavigationRole);
            if (location.isValid()) break;
        }
    }
    if (!location.isValid()) return std::nullopt;
    const auto target = core::owningNavigationTarget(
        location.value<core::SctInspectionLocation>());
    return target.kind == core::SctNavigationKind::Instruction
        ? std::optional{spice::sct::SctInstructionId(target.id)} : std::nullopt;
}

}  // namespace

SctSemanticNavigatorWidget::SctSemanticNavigatorWidget(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    emptyLabel_ = new QLabel(tr("No SCT document active."), this);
    emptyLabel_->setAlignment(Qt::AlignCenter);
    layout->addWidget(emptyLabel_, 1);

    tabs_ = new QTabWidget(this);
    opcodes_ = new QTreeWidget(tabs_);
    references_ = new QTreeWidget(tabs_);
    variables_ = new QTreeWidget(tabs_);
    incomplete_ = new QTreeWidget(tabs_);
    configureTree(*opcodes_, { tr("Opcode / occurrence"), tr("Count / location") });
    configureTree(*references_, { tr("Reference"), tr("Source"), tr("Target") });
    configureTree(*variables_, { tr("Variable / occurrence"), tr("Count / location") });
    configureTree(*incomplete_, { tr("Evidence"), tr("Location"), tr("Size"), tr("Anchor") });
    tabs_->addTab(opcodes_, tr("Opcodes"));
    tabs_->addTab(references_, tr("References"));
    tabs_->addTab(variables_, tr("Variables"));
    tabs_->addTab(incomplete_, tr("Incomplete Evidence"));
    layout->addWidget(tabs_, 1);
    tabs_->hide();

    for (auto* tree : { opcodes_, references_, variables_, incomplete_ }) {
        connect(tree, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* item, const int column) { activate(item, column); });
    }
}

void SctSemanticNavigatorWidget::setDocument(
    const core::AssetLocator& locator,
    const core::SctDocumentSnapshot& snapshot) {
    const auto nextIdentity = QString::fromStdString(locator.identityKey());
    const bool sameDocument = nextIdentity == identityKey_;
    std::array<TreeState, 4> states;
    if (sameDocument) {
        states = { captureState(*opcodes_), captureState(*references_),
            captureState(*variables_), captureState(*incomplete_) };
    }

    identityKey_ = nextIdentity;
    for (auto* tree : { opcodes_, references_, variables_, incomplete_ }) tree->clear();
    populateTrees(*opcodes_, *references_, *variables_, *incomplete_,
        *snapshot.document, *snapshot.analysis);
    resetIncrementalState(*snapshot.document, snapshot.analysis->entities);
    for (auto* tree : { opcodes_, references_, variables_, incomplete_ }) tree->collapseAll();
    if (sameDocument) {
        restoreState(*opcodes_, states[0]);
        restoreState(*references_, states[1]);
        restoreState(*variables_, states[2]);
        restoreState(*incomplete_, states[3]);
    }
    emptyLabel_->hide();
    tabs_->show();
    reconciliationPending_ = false;
}

void SctSemanticNavigatorWidget::installVerifiedDocument(
    const core::AssetLocator& locator,
    const core::SctDocumentSnapshot& snapshot) {
    const auto nextIdentity = QString::fromStdString(locator.identityKey());
    if (identityKey_ != nextIdentity || reconciliationPending_)
        setDocument(locator, snapshot);
}

bool SctSemanticNavigatorWidget::applyInstructionChanges(
    const core::AssetLocator& locator,
    const core::SctDocumentSnapshot& snapshot,
    const core::SctEditChangeSet& changes) {
    const auto nextIdentity = QString::fromStdString(locator.identityKey());
    Q_UNUSED(snapshot);
    if (identityKey_ != nextIdentity || changes.instructions.empty()
        || !changes.modified.empty()) {
        reconciliationPending_ = true;
        return false;
    }

    for (const auto& change : changes.instructions) {
        if (change.beforeValue.has_value() && !change.afterValue.has_value()) {
            removeContribution(change);
            applyInstructionOrder(change);
        } else if (!change.beforeValue.has_value() && change.afterValue.has_value()) {
            applyInstructionOrder(change);
            addContribution(change);
        } else if (change.before.has_value() && change.after.has_value()) {
            applyInstructionOrder(change);
            reorderInstructionOccurrences(change.instruction);
        } else {
            reconciliationPending_ = true;
            return false;
        }
    }
    return true;
}

void SctSemanticNavigatorWidget::resetIncrementalState(
    const spice::sct::SctDocument& document,
    const spice::sct::SctDocumentIndex& index) {
    sectionOrder_.clear();
    instructionOrder_.clear();
    instructionPresentation_.clear();
    for (std::size_t ordinal = 0; ordinal < document.sections.size(); ++ordinal) {
        const auto& section = document.sections[ordinal];
        const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
            &section.content);
        if (script == nullptr) continue;
        const auto sectionId = section.id.value();
        sectionOrder_.push_back(sectionId);
        auto& order = instructionOrder_[sectionId];
        order.reserve(script->instructions.size());
        for (const auto& instruction : script->instructions) {
            order.push_back(instruction.id.value());
            instructionPresentation_[instruction.id.value()] = InstructionPresentation{
                instruction.opcode,
                sectionId,
                instructionName(document, index, instruction.id),
                instructionContext(document, index, instruction.id),
            };
        }
    }
}

void SctSemanticNavigatorWidget::applyInstructionOrder(
    const core::SctInstructionStructuralChange& change) {
    if (change.before.has_value()) {
        auto found = instructionOrder_.find(change.before->section.value());
        if (found != instructionOrder_.end()) {
            std::erase(found->second, change.instruction.value());
        }
    }
    if (change.after.has_value()) {
        auto& order = instructionOrder_[change.after->section.value()];
        auto insertion = order.begin();
        if (change.after->after.has_value()) {
            const auto anchor = std::ranges::find(
                order, change.after->after->value());
            insertion = anchor == order.end() ? order.end() : std::next(anchor);
        }
        order.insert(insertion, change.instruction.value());
        if (change.afterValue.has_value()) {
            const auto sectionId = change.after->section.value();
            QString context = tr("Section %1").arg(sectionId);
            const auto sameSection = std::ranges::find_if(
                instructionPresentation_, [sectionId](const auto& entry) {
                    return entry.second.section == sectionId;
                });
            if (sameSection != instructionPresentation_.end())
                context = sameSection->second.context;
            instructionPresentation_[change.instruction.value()] = InstructionPresentation{
                change.afterValue->opcode,
                sectionId,
                QStringLiteral("%1 — %2")
                    .arg(opcodeName(change.afterValue->opcode))
                    .arg(tr("Instruction %1").arg(change.instruction.value())),
                std::move(context),
            };
        } else if (auto found = instructionPresentation_.find(
                change.instruction.value()); found != instructionPresentation_.end()) {
            found->second.section = change.after->section.value();
        }
    } else {
        instructionPresentation_.erase(change.instruction.value());
    }
}

bool SctSemanticNavigatorWidget::physicallyBefore(
    const spice::sct::SctInstructionId left,
    const spice::sct::SctInstructionId right) const {
    const auto leftInfo = instructionPresentation_.find(left.value());
    const auto rightInfo = instructionPresentation_.find(right.value());
    if (leftInfo == instructionPresentation_.end()
        || rightInfo == instructionPresentation_.end()) return left.value() < right.value();
    if (leftInfo->second.section != rightInfo->second.section) {
        const auto leftSection = std::ranges::find(sectionOrder_, leftInfo->second.section);
        const auto rightSection = std::ranges::find(sectionOrder_, rightInfo->second.section);
        return leftSection < rightSection;
    }
    const auto order = instructionOrder_.find(leftInfo->second.section);
    if (order == instructionOrder_.end()) return left.value() < right.value();
    return std::ranges::find(order->second, left.value())
        < std::ranges::find(order->second, right.value());
}

QString SctSemanticNavigatorWidget::incrementalInstructionName(
    const spice::sct::SctInstructionId instruction) const {
    const auto found = instructionPresentation_.find(instruction.value());
    return found == instructionPresentation_.end()
        ? tr("Instruction %1").arg(instruction.value()) : found->second.name;
}

QString SctSemanticNavigatorWidget::incrementalInstructionContext(
    const spice::sct::SctInstructionId instruction) const {
    const auto found = instructionPresentation_.find(instruction.value());
    return found == instructionPresentation_.end()
        ? tr("Pending verification") : found->second.context;
}

void SctSemanticNavigatorWidget::removeContribution(
    const core::SctInstructionStructuralChange& change) {
    const auto removeChild = [](QTreeWidgetItem& parent, const QString& key) {
        if (auto* item = findKey(&parent, key); item != nullptr && item->parent() != nullptr) {
            auto* owner = item->parent();
            delete owner->takeChild(owner->indexOfChild(item));
            return owner;
        }
        return static_cast<QTreeWidgetItem*>(nullptr);
    };
    const auto removeEmptyGroup = [](QTreeWidgetItem* group) {
        if (group == nullptr || group->childCount() != 0 || group->parent() == nullptr) return;
        auto* parent = group->parent();
        delete parent->takeChild(parent->indexOfChild(group));
    };
    const auto removeEmptyTopLevelGroup = [](QTreeWidget& tree, QTreeWidgetItem* group) {
        if (group == nullptr || group->childCount() != 0 || group->parent() != nullptr) return;
        delete tree.takeTopLevelItem(tree.indexOfTopLevelItem(group));
    };

    const auto instructionLocation = core::SctInspectionLocation{
        core::SctNavigationTarget{core::SctNavigationKind::Instruction,
            change.instruction.value()}};
    {
        const auto& usage = change.beforeSemantics.opcode;
        const auto key = QStringLiteral("opcode:%1:").arg(usage.opcode)
            + inspectionKey(instructionLocation);
        auto* occurrence = findKey(*opcodes_, key);
        auto* group = occurrence == nullptr ? nullptr : occurrence->parent();
        if (group != nullptr) {
            delete group->takeChild(group->indexOfChild(occurrence));
            group->setText(1, tr("%1 occurrence(s)").arg(group->childCount()));
            removeEmptyTopLevelGroup(*opcodes_, group);
        }
    }
    for (const auto& usage : change.beforeSemantics.references) {
        const auto targetId = std::visit(
            [](const auto id) { return id.value(); }, usage.target);
        const auto sourceLocation = core::SctInspectionLocation{usage.source};
        const auto inboundKey = QStringLiteral("reference:in:%1:%2:")
            .arg(usage.target.index()).arg(targetId) + inspectionKey(sourceLocation);
        if (auto* group = removeChild(*findKey(*references_,
                QStringLiteral("references:inbound")), inboundKey)) {
            group->setText(1, tr("%1 inbound occurrence(s)").arg(group->childCount()));
            removeEmptyGroup(group);
        }
        const auto outboundKey = QStringLiteral("reference:out:")
            + inspectionKey(sourceLocation);
        if (auto* group = removeChild(*findKey(*references_,
                QStringLiteral("references:outbound")), outboundKey)) {
            group->setText(1, tr("%1 outbound occurrence(s)").arg(group->childCount()));
            removeEmptyGroup(group);
        }
    }
    for (const auto& usage : change.beforeSemantics.variables) {
        const auto location = core::SctInspectionLocation{usage.source};
        const auto groupKey = QStringLiteral("variable:%1:%2")
            .arg(static_cast<int>(usage.variable.kind)).arg(usage.variable.index);
        auto* group = findKey(*variables_, groupKey);
        if (group == nullptr) continue;
        const auto occurrenceKey = groupKey + QLatin1Char(':') + inspectionKey(location);
        if (auto* item = findKey(group, occurrenceKey))
            delete group->takeChild(group->indexOfChild(item));
        group->setText(1, tr("%1 occurrence(s)").arg(group->childCount()));
        if (group->childCount() == 0 && group->parent() != nullptr) {
            auto* kind = group->parent();
            delete kind->takeChild(kind->indexOfChild(group));
            kind->setText(1, tr("%1 variable(s)").arg(kind->childCount()));
        }
    }
    const auto removeIncomplete = [&](const QString& rootKey, const QString& itemKey) {
        auto* root = findKey(*incomplete_, rootKey);
        if (root == nullptr) return;
        if (auto* item = findKey(root, itemKey))
            delete root->takeChild(root->indexOfChild(item));
        root->setText(2, tr("%1 occurrence(s)").arg(root->childCount()));
    };
    for (const auto& usage : change.beforeSemantics.unresolvedReferences)
        removeIncomplete(QStringLiteral("incomplete:unresolved"),
            QStringLiteral("unresolved:")
                + inspectionKey(core::SctInspectionLocation{usage.source}));
    for (const auto& usage : change.beforeSemantics.opaqueParameters)
        removeIncomplete(QStringLiteral("incomplete:opaque-parameters"),
            QStringLiteral("opaque-parameter:")
                + inspectionKey(core::SctInspectionLocation{usage.source}));
    for (const auto& usage : change.beforeSemantics.opaqueExpressions)
        removeIncomplete(QStringLiteral("incomplete:opaque-expressions"),
            QStringLiteral("opaque-expression:")
                + inspectionKey(core::SctInspectionLocation{usage.source}));
}

void SctSemanticNavigatorWidget::addContribution(
    const core::SctInstructionStructuralChange& change) {
    const auto insertPhysical = [this](QTreeWidgetItem& parent,
                                    QTreeWidgetItem* item,
                                    const spice::sct::SctInstructionId instruction) {
        int row = 0;
        while (row < parent.childCount()) {
            const auto other = sourceInstructionFor(*parent.child(row));
            if (other.has_value() && physicallyBefore(instruction, *other)) break;
            ++row;
        }
        parent.insertChild(row, item);
    };
    const auto instructionLocation = core::SctInspectionLocation{
        core::SctNavigationTarget{core::SctNavigationKind::Instruction,
            change.instruction.value()}};
    {
        const auto& usage = change.afterSemantics.opcode;
        const auto groupKey = QStringLiteral("opcode:%1").arg(usage.opcode);
        auto* group = findKey(*opcodes_, groupKey);
        if (group == nullptr) {
            group = new QTreeWidgetItem;
            group->setText(0, opcodeName(usage.opcode));
            setKey(*group, groupKey);
            int row = 0;
            while (row < opcodes_->topLevelItemCount()
                && opcodes_->topLevelItem(row)->text(0) < group->text(0)) ++row;
            opcodes_->insertTopLevelItem(row, group);
        }
        auto* item = new QTreeWidgetItem;
        item->setText(0, incrementalInstructionName(change.instruction));
        item->setText(1, incrementalInstructionContext(change.instruction));
        setKey(*item, groupKey + QLatin1Char(':') + inspectionKey(instructionLocation));
        registerNavigation(item, -1, instructionLocation);
        insertPhysical(*group, item, change.instruction);
        group->setText(1, tr("%1 occurrence(s)").arg(group->childCount()));
    }

    auto* inboundRoot = findKey(*references_, QStringLiteral("references:inbound"));
    auto* outboundRoot = findKey(*references_, QStringLiteral("references:outbound"));
    for (const auto& usage : change.afterSemantics.references) {
        const auto targetId = std::visit(
            [](const auto id) { return id.value(); }, usage.target);
        const auto sourceLocation = core::SctInspectionLocation{usage.source};
        const auto targetKey = QStringLiteral("reference:target:%1:%2")
            .arg(usage.target.index()).arg(targetId);
        auto* inboundGroup = findKey(inboundRoot, targetKey);
        if (inboundGroup == nullptr) {
            inboundGroup = new QTreeWidgetItem;
            inboundGroup->setText(0, referenceTargetName(usage.target));
            setKey(*inboundGroup, targetKey);
            registerNavigation(inboundGroup, 0,
                core::SctInspectionLocation{referenceTarget(usage.target)});
            inboundRoot->addChild(inboundGroup);
        }
        auto* inboundItem = new QTreeWidgetItem;
        inboundItem->setText(0, parameterName(usage.source.parameter));
        inboundItem->setText(1, incrementalInstructionName(change.instruction));
        inboundItem->setText(2, referenceTargetName(usage.target));
        setKey(*inboundItem, QStringLiteral("reference:in:%1:%2:")
            .arg(usage.target.index()).arg(targetId) + inspectionKey(sourceLocation));
        registerNavigation(inboundItem, 0, sourceLocation);
        registerNavigation(inboundItem, 1, sourceLocation);
        registerNavigation(inboundItem, 2,
            core::SctInspectionLocation{referenceTarget(usage.target)});
        insertPhysical(*inboundGroup, inboundItem, change.instruction);
        inboundGroup->setText(1,
            tr("%1 inbound occurrence(s)").arg(inboundGroup->childCount()));

        const auto sourceKey = QStringLiteral("reference:source:%1")
            .arg(change.instruction.value());
        auto* outboundGroup = findKey(outboundRoot, sourceKey);
        if (outboundGroup == nullptr) {
            outboundGroup = new QTreeWidgetItem;
            outboundGroup->setText(0, incrementalInstructionName(change.instruction));
            setKey(*outboundGroup, sourceKey);
            registerNavigation(outboundGroup, 0, instructionLocation);
            insertPhysical(*outboundRoot, outboundGroup, change.instruction);
        }
        auto* outboundItem = new QTreeWidgetItem;
        outboundItem->setText(0, parameterName(usage.source.parameter));
        outboundItem->setText(1, incrementalInstructionContext(change.instruction));
        outboundItem->setText(2, referenceTargetName(usage.target));
        setKey(*outboundItem, QStringLiteral("reference:out:")
            + inspectionKey(sourceLocation));
        registerNavigation(outboundItem, 0, sourceLocation);
        registerNavigation(outboundItem, 1, sourceLocation);
        registerNavigation(outboundItem, 2,
            core::SctInspectionLocation{referenceTarget(usage.target)});
        outboundGroup->addChild(outboundItem);
        outboundGroup->setText(1,
            tr("%1 outbound occurrence(s)").arg(outboundGroup->childCount()));
    }

    for (const auto& usage : change.afterSemantics.variables) {
        const auto kindKey = QStringLiteral("variable-kind:%1")
            .arg(static_cast<int>(usage.variable.kind));
        auto* kind = findKey(*variables_, kindKey);
        if (kind == nullptr) continue;
        const auto groupKey = QStringLiteral("variable:%1:%2")
            .arg(static_cast<int>(usage.variable.kind)).arg(usage.variable.index);
        auto* group = findKey(kind, groupKey);
        if (group == nullptr) {
            group = new QTreeWidgetItem;
            group->setText(0, tr("%1 %2")
                .arg(variableKindName(usage.variable.kind)).arg(usage.variable.index));
            setKey(*group, groupKey);
            int row = 0;
            while (row < kind->childCount()
                && kind->child(row)->text(0) < group->text(0)) ++row;
            kind->insertChild(row, group);
            kind->setText(1, tr("%1 variable(s)").arg(kind->childCount()));
        }
        auto* item = new QTreeWidgetItem;
        item->setText(0, expressionName(usage.source));
        item->setText(1, QStringLiteral("%1 — %2")
            .arg(incrementalInstructionName(change.instruction))
            .arg(incrementalInstructionContext(change.instruction)));
        const auto location = core::SctInspectionLocation{usage.source};
        setKey(*item, groupKey + QLatin1Char(':') + inspectionKey(location));
        registerNavigation(item, -1, location);
        insertPhysical(*group, item, change.instruction);
        group->setText(1, tr("%1 occurrence(s)").arg(group->childCount()));
    }

    const auto addIncomplete = [this, &insertPhysical, &change](
                                   const QString& rootKey,
                                   const QString& itemKey,
                                   QString title,
                                   QString locationText,
                                   QString size,
                                   core::SctInspectionLocation location) {
        auto* root = findKey(*incomplete_, rootKey);
        if (root == nullptr) return;
        auto* item = new QTreeWidgetItem;
        item->setText(0, std::move(title));
        item->setText(1, std::move(locationText));
        item->setText(2, std::move(size));
        setKey(*item, itemKey);
        registerNavigation(item, 0, location);
        registerNavigation(item, 1, location);
        insertPhysical(*root, item, change.instruction);
        root->setText(2, tr("%1 occurrence(s)").arg(root->childCount()));
    };
    for (const auto& usage : change.afterSemantics.unresolvedReferences) {
        const auto location = core::SctInspectionLocation{usage.source};
        addIncomplete(QStringLiteral("incomplete:unresolved"),
            QStringLiteral("unresolved:") + inspectionKey(location),
            expectedTargetName(usage.expectedTarget),
            QStringLiteral("%1 — %2")
                .arg(incrementalInstructionName(change.instruction))
                .arg(parameterName(usage.source.parameter)),
            tr("%1 word(s)").arg(usage.encodedWordCount), location);
    }
    for (const auto& usage : change.afterSemantics.opaqueParameters) {
        const auto location = core::SctInspectionLocation{usage.source};
        addIncomplete(QStringLiteral("incomplete:opaque-parameters"),
            QStringLiteral("opaque-parameter:") + inspectionKey(location),
            tr("Opaque parameter"),
            QStringLiteral("%1 — %2")
                .arg(incrementalInstructionName(change.instruction))
                .arg(parameterName(usage.source.parameter)),
            tr("%1 word(s)").arg(usage.wordCount), location);
    }
    for (const auto& usage : change.afterSemantics.opaqueExpressions) {
        const auto location = core::SctInspectionLocation{usage.source};
        addIncomplete(QStringLiteral("incomplete:opaque-expressions"),
            QStringLiteral("opaque-expression:") + inspectionKey(location),
            tr("Opaque expression"),
            QStringLiteral("%1 — %2")
                .arg(incrementalInstructionName(change.instruction))
                .arg(expressionName(usage.source)),
            tr("%1 word(s)").arg(usage.wordCount), location);
    }
}

void SctSemanticNavigatorWidget::reorderInstructionOccurrences(
    const spice::sct::SctInstructionId instruction) {
    const auto reorderTree = [this, instruction](QTreeWidget& tree) {
        std::vector<QTreeWidgetItem*> occurrences;
        std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* item) {
            if (sourceInstructionFor(*item) == instruction) {
                occurrences.push_back(item);
                return;
            }
            for (int row = 0; row < item->childCount(); ++row)
                collect(item->child(row));
        };
        for (int row = 0; row < tree.topLevelItemCount(); ++row)
            collect(tree.topLevelItem(row));
        for (auto* item : occurrences) {
            auto* parent = item->parent();
            if (parent == nullptr) continue;
            const auto oldRow = parent->indexOfChild(item);
            auto* retained = parent->takeChild(oldRow);
            int row = 0;
            while (row < parent->childCount()) {
                const auto other = sourceInstructionFor(*parent->child(row));
                if (other.has_value() && physicallyBefore(instruction, *other)) break;
                ++row;
            }
            parent->insertChild(row, retained);
        }
    };
    for (auto* tree : {opcodes_, references_, variables_, incomplete_})
        reorderTree(*tree);
}

void SctSemanticNavigatorWidget::clear() {
    identityKey_.clear();
    reconciliationPending_ = false;
    for (auto* tree : { opcodes_, references_, variables_, incomplete_ }) tree->clear();
    tabs_->hide();
    emptyLabel_->show();
}

void SctSemanticNavigatorWidget::activate(QTreeWidgetItem* item, const int column) {
    if (item == nullptr) return;
    auto navigation = item->data(column, NavigationRole);
    if (!navigation.isValid()) navigation = item->data(0, FallbackNavigationRole);
    if (navigation.isValid()) {
        emit navigationRequested(identityKey_, navigation.value<core::SctInspectionLocation>());
    } else if (!item->toolTip(column).isEmpty()) {
        emit statusMessageRequested(item->toolTip(column));
    } else if (item->childCount() != 0) {
        item->setExpanded(!item->isExpanded());
    }
}

void SctSemanticNavigatorWidget::registerNavigation(
    QTreeWidgetItem* item,
    const int column,
    core::SctInspectionLocation location) {
    if (column < 0)
        item->setData(0, FallbackNavigationRole, QVariant::fromValue(std::move(location)));
    else
        item->setData(column, NavigationRole, QVariant::fromValue(std::move(location)));
}

void SctSemanticNavigatorWidget::populateTrees(
    QTreeWidget& opcodes,
    QTreeWidget& references,
    QTreeWidget& variables,
    QTreeWidget& incomplete,
    const spice::sct::SctDocument& document,
    const spice::sct::SctDocumentAnalysis& analysis) {
    buildOpcodes(opcodes, document, analysis.entities, analysis.usage);
    buildReferences(references, document, analysis.entities, analysis.usage);
    buildVariables(variables, document, analysis.entities, analysis.usage);
    buildIncompleteEvidence(incomplete, document, analysis.entities, analysis.usage);
}

void SctSemanticNavigatorWidget::reconcileTree(
    QTreeWidget& current,
    QTreeWidget& desired) {
    const auto selectedKey = current.currentItem() == nullptr
        ? QString{} : current.currentItem()->data(0, StableKeyRole).toString();
    const auto scrollPosition = current.verticalScrollBar()->value();
    const QSignalBlocker blocker(&current);
    current.setUpdatesEnabled(false);

    std::function<void(QTreeWidgetItem*, QTreeWidgetItem*)> reconcileChildren;
    reconcileChildren = [&](QTreeWidgetItem* currentParent, QTreeWidgetItem* desiredParent) {
        const auto currentCount = [&]() {
            return currentParent == nullptr
                ? current.topLevelItemCount() : currentParent->childCount();
        };
        const auto desiredCount = [&]() {
            return desiredParent == nullptr
                ? desired.topLevelItemCount() : desiredParent->childCount();
        };
        const auto currentAt = [&](const int row) {
            return currentParent == nullptr
                ? current.topLevelItem(row) : currentParent->child(row);
        };
        const auto desiredAt = [&](const int row) {
            return desiredParent == nullptr
                ? desired.topLevelItem(row) : desiredParent->child(row);
        };
        const auto takeCurrent = [&](const int row) {
            return currentParent == nullptr
                ? current.takeTopLevelItem(row) : currentParent->takeChild(row);
        };
        const auto insertCurrent = [&](const int row, QTreeWidgetItem* item) {
            if (currentParent == nullptr) current.insertTopLevelItem(row, item);
            else currentParent->insertChild(row, item);
        };
        const auto desiredContains = [&](const int first, const QString& key) {
            for (int row = first; row < desiredCount(); ++row)
                if (desiredAt(row)->data(0, StableKeyRole).toString() == key) return true;
            return false;
        };

        int row = 0;
        while (row < desiredCount()) {
            auto* wanted = desiredAt(row);
            const auto wantedKey = wanted->data(0, StableKeyRole).toString();
            auto* item = row < currentCount() ? currentAt(row) : nullptr;
            if (item != nullptr
                && item->data(0, StableKeyRole).toString() != wantedKey) {
                const auto currentKey = item->data(0, StableKeyRole).toString();
                if (!desiredContains(row, currentKey)) {
                    delete takeCurrent(row);
                    continue;
                }
                int found = row + 1;
                while (found < currentCount()
                    && currentAt(found)->data(0, StableKeyRole).toString() != wantedKey)
                    ++found;
                item = found < currentCount() ? takeCurrent(found) : nullptr;
                if (item != nullptr) insertCurrent(row, item);
            }
            if (item == nullptr
                || item->data(0, StableKeyRole).toString() != wantedKey) {
                item = new QTreeWidgetItem;
                insertCurrent(row, item);
            }

            for (int column = 0; column < current.columnCount(); ++column) {
                if (item->text(column) != wanted->text(column))
                    item->setText(column, wanted->text(column));
                if (item->toolTip(column) != wanted->toolTip(column))
                    item->setToolTip(column, wanted->toolTip(column));
                item->setData(column, NavigationRole,
                    wanted->data(column, NavigationRole));
            }
            item->setData(0, StableKeyRole, wantedKey);
            item->setData(0, FallbackNavigationRole,
                wanted->data(0, FallbackNavigationRole));
            reconcileChildren(item, wanted);
            ++row;
        }
        while (currentCount() > row) delete takeCurrent(row);
    };

    reconcileChildren(nullptr, nullptr);
    if (!selectedKey.isEmpty()) {
        if (auto* selected = findKey(current, selectedKey))
            current.setCurrentItem(selected);
    }
    current.verticalScrollBar()->setValue(scrollPosition);
    current.setUpdatesEnabled(true);
    current.viewport()->update();
}

void SctSemanticNavigatorWidget::buildOpcodes(
    QTreeWidget& tree,
    const spice::sct::SctDocument& document,
    const spice::sct::SctDocumentIndex& index,
    const spice::sct::SctSemanticUsageIndex& usage) {
    std::map<std::uint16_t, std::vector<spice::sct::SctOpcodeUsage>> groups;
    for (const auto& occurrence : usage.opcodeUsages()) groups[occurrence.opcode].push_back(occurrence);
    for (const auto& [opcode, occurrences] : groups) {
        auto* group = new QTreeWidgetItem(&tree);
        group->setText(0, opcodeName(opcode));
        group->setText(1, tr("%1 occurrence(s)").arg(occurrences.size()));
        setKey(*group, QStringLiteral("opcode:%1").arg(opcode));
        for (const auto& occurrence : occurrences) {
            auto* item = new QTreeWidgetItem(group);
            item->setText(0, instructionName(document, index, occurrence.instruction));
            item->setText(1, instructionContext(document, index, occurrence.instruction));
            const core::SctInspectionLocation location{ core::SctNavigationTarget{
                core::SctNavigationKind::Instruction, occurrence.instruction.value() } };
            setKey(*item, QStringLiteral("opcode:%1:").arg(opcode) + inspectionKey(location));
            registerNavigation(item, -1, location);
        }
    }
}

void SctSemanticNavigatorWidget::buildReferences(
    QTreeWidget& tree,
    const spice::sct::SctDocument& document,
    const spice::sct::SctDocumentIndex& index,
    const spice::sct::SctSemanticUsageIndex& usage) {
    auto* inbound = new QTreeWidgetItem(&tree);
    inbound->setText(0, tr("Inbound by target"));
    setKey(*inbound, QStringLiteral("references:inbound"));

    using TargetKey = std::pair<std::size_t, std::uint64_t>;
    std::map<TargetKey, std::vector<spice::sct::SctReferenceUsage>> inboundGroups;
    for (const auto& reference : usage.referenceUsages()) {
        const auto id = std::visit([](const auto typed) { return typed.value(); }, reference.target);
        inboundGroups[{ reference.target.index(), id }].push_back(reference);
    }
    for (const auto& [key, occurrences] : inboundGroups) {
        const auto& target = occurrences.front().target;
        auto* group = new QTreeWidgetItem(inbound);
        group->setText(0, referenceTargetName(target));
        group->setText(1, tr("%1 inbound occurrence(s)").arg(occurrences.size()));
        setKey(*group, QStringLiteral("reference:target:%1:%2").arg(key.first).arg(key.second));
        if (targetExists(document, index, target))
            registerNavigation(group, 0, core::SctInspectionLocation{ referenceTarget(target) });
        else
            group->setToolTip(0, tr("The target is not present in this document revision."));
        for (const auto& occurrence : occurrences) {
            auto* item = new QTreeWidgetItem(group);
            item->setText(0, parameterName(occurrence.source.parameter));
            item->setText(1, instructionName(document, index, occurrence.source.instruction));
            item->setText(2, referenceTargetName(occurrence.target));
            const core::SctInspectionLocation sourceLocation{ occurrence.source };
            setKey(*item, QStringLiteral("reference:in:%1:%2:")
                .arg(key.first).arg(key.second) + inspectionKey(sourceLocation));
            registerNavigation(item, 0, sourceLocation);
            registerNavigation(item, 1, sourceLocation);
            if (targetExists(document, index, occurrence.target))
                registerNavigation(item, 2,
                    core::SctInspectionLocation{ referenceTarget(occurrence.target) });
            else
                item->setToolTip(2, tr("The target is not present in this document revision."));
        }
    }

    auto* outbound = new QTreeWidgetItem(&tree);
    outbound->setText(0, tr("Outbound by instruction"));
    setKey(*outbound, QStringLiteral("references:outbound"));
    std::vector<std::pair<spice::sct::SctInstructionId,
        std::vector<spice::sct::SctReferenceUsage>>> outboundGroups;
    for (const auto& reference : usage.referenceUsages()) {
        auto found = std::ranges::find_if(outboundGroups, [&reference](const auto& group) {
            return group.first == reference.source.instruction;
        });
        if (found == outboundGroups.end()) {
            outboundGroups.push_back({ reference.source.instruction, { reference } });
        } else {
            found->second.push_back(reference);
        }
    }
    for (const auto& [instruction, occurrences] : outboundGroups) {
        auto* group = new QTreeWidgetItem(outbound);
        group->setText(0, instructionName(document, index, instruction));
        group->setText(1, tr("%1 outbound occurrence(s)").arg(occurrences.size()));
        setKey(*group, QStringLiteral("reference:source:%1").arg(instruction.value()));
        registerNavigation(group, 0, core::SctInspectionLocation{ core::SctNavigationTarget{
            core::SctNavigationKind::Instruction, instruction.value() } });
        for (const auto& occurrence : occurrences) {
            auto* item = new QTreeWidgetItem(group);
            item->setText(0, parameterName(occurrence.source.parameter));
            item->setText(1, instructionContext(document, index, occurrence.source.instruction));
            item->setText(2, referenceTargetName(occurrence.target));
            const core::SctInspectionLocation sourceLocation{ occurrence.source };
            setKey(*item, QStringLiteral("reference:out:") + inspectionKey(sourceLocation));
            registerNavigation(item, 0, sourceLocation);
            registerNavigation(item, 1, sourceLocation);
            if (targetExists(document, index, occurrence.target))
                registerNavigation(item, 2,
                    core::SctInspectionLocation{ referenceTarget(occurrence.target) });
            else
                item->setToolTip(2, tr("The target is not present in this document revision."));
        }
    }
}

void SctSemanticNavigatorWidget::buildVariables(
    QTreeWidget& tree,
    const spice::sct::SctDocument& document,
    const spice::sct::SctDocumentIndex& index,
    const spice::sct::SctSemanticUsageIndex& usage) {
    for (const auto kind : { spice::sct::SctVariableKind::Integer, spice::sct::SctVariableKind::Float,
            spice::sct::SctVariableKind::Bit, spice::sct::SctVariableKind::Byte }) {
        auto* kindItem = new QTreeWidgetItem(&tree);
        kindItem->setText(0, variableKindName(kind));
        setKey(*kindItem, QStringLiteral("variable-kind:%1").arg(static_cast<int>(kind)));
        std::map<std::uint32_t, std::vector<spice::sct::SctVariableUsage>> groups;
        for (const auto& occurrence : usage.variableUsages())
            if (occurrence.variable.kind == kind)
                groups[occurrence.variable.index].push_back(occurrence);
        kindItem->setText(1, tr("%1 variable(s)").arg(groups.size()));
        for (const auto& [variable, occurrences] : groups) {
            auto* group = new QTreeWidgetItem(kindItem);
            group->setText(0, tr("%1 %2").arg(variableKindName(kind)).arg(variable));
            group->setText(1, tr("%1 occurrence(s)").arg(occurrences.size()));
            setKey(*group, QStringLiteral("variable:%1:%2")
                .arg(static_cast<int>(kind)).arg(variable));
            for (const auto& occurrence : occurrences) {
                auto* item = new QTreeWidgetItem(group);
                item->setText(0, expressionName(occurrence.source));
                item->setText(1, QStringLiteral("%1 — %2")
                    .arg(instructionName(document, index,
                        occurrence.source.expression.instruction))
                    .arg(instructionContext(document, index,
                        occurrence.source.expression.instruction)));
                const core::SctInspectionLocation location{ occurrence.source };
                setKey(*item, QStringLiteral("variable:%1:%2:")
                    .arg(static_cast<int>(kind)).arg(variable) + inspectionKey(location));
                registerNavigation(item, -1,
                    location);
            }
        }
    }
}

void SctSemanticNavigatorWidget::buildIncompleteEvidence(
    QTreeWidget& tree,
    const spice::sct::SctDocument& document,
    const spice::sct::SctDocumentIndex& index,
    const spice::sct::SctSemanticUsageIndex& usage) {
    auto* unresolved = new QTreeWidgetItem(&tree);
    unresolved->setText(0, tr("Unresolved references"));
    unresolved->setText(2, tr("%1 occurrence(s)").arg(usage.unresolvedReferences().size()));
    setKey(*unresolved, QStringLiteral("incomplete:unresolved"));
    for (const auto& occurrence : usage.unresolvedReferences()) {
        auto* item = new QTreeWidgetItem(unresolved);
        item->setText(0, expectedTargetName(occurrence.expectedTarget));
        item->setText(1, QStringLiteral("%1 — %2")
            .arg(instructionName(document, index, occurrence.source.instruction))
            .arg(parameterName(occurrence.source.parameter)));
        item->setText(2, tr("%1 word(s)").arg(occurrence.encodedWordCount));
        const core::SctInspectionLocation location{ occurrence.source };
        setKey(*item, QStringLiteral("unresolved:") + inspectionKey(location));
        registerNavigation(item, 0, location);
        registerNavigation(item, 1, location);
    }

    auto* opaqueParameters = new QTreeWidgetItem(&tree);
    opaqueParameters->setText(0, tr("Opaque parameters"));
    opaqueParameters->setText(2, tr("%1 occurrence(s)").arg(usage.opaqueParameters().size()));
    setKey(*opaqueParameters, QStringLiteral("incomplete:opaque-parameters"));
    for (const auto& occurrence : usage.opaqueParameters()) {
        auto* item = new QTreeWidgetItem(opaqueParameters);
        item->setText(0, tr("Opaque parameter"));
        item->setText(1, QStringLiteral("%1 — %2")
            .arg(instructionName(document, index, occurrence.source.instruction))
            .arg(parameterName(occurrence.source.parameter)));
        item->setText(2, tr("%1 word(s)").arg(occurrence.wordCount));
        const core::SctInspectionLocation location{ occurrence.source };
        setKey(*item, QStringLiteral("opaque-parameter:") + inspectionKey(location));
        registerNavigation(item, 0, location);
        registerNavigation(item, 1, location);
    }

    auto* opaqueExpressions = new QTreeWidgetItem(&tree);
    opaqueExpressions->setText(0, tr("Opaque expressions"));
    opaqueExpressions->setText(2, tr("%1 occurrence(s)").arg(usage.opaqueExpressions().size()));
    setKey(*opaqueExpressions, QStringLiteral("incomplete:opaque-expressions"));
    for (const auto& occurrence : usage.opaqueExpressions()) {
        auto* item = new QTreeWidgetItem(opaqueExpressions);
        item->setText(0, tr("Opaque expression"));
        item->setText(1, QStringLiteral("%1 — %2")
            .arg(instructionName(document, index, occurrence.source.instruction))
            .arg(expressionName(occurrence.source)));
        item->setText(2, tr("%1 word(s)").arg(occurrence.wordCount));
        const core::SctInspectionLocation location{ occurrence.source };
        setKey(*item, QStringLiteral("opaque-expression:") + inspectionKey(location));
        registerNavigation(item, 0, location);
        registerNavigation(item, 1, location);
    }

    auto* attachments = new QTreeWidgetItem(&tree);
    attachments->setText(0, tr("Opaque attachments"));
    attachments->setText(2, tr("%1 attachment(s)").arg(document.opaqueAttachments.size()));
    setKey(*attachments, QStringLiteral("incomplete:attachments"));
    for (const auto& attachment : document.opaqueAttachments) {
        const auto [anchorName, anchorTarget] = opaqueAnchor(attachment.anchor);
        auto* item = new QTreeWidgetItem(attachments);
        item->setText(0, tr("Attachment %1 — %2")
            .arg(attachment.id.value()).arg(opaqueReasonName(attachment.reason)));
        item->setText(1, placementName(attachment.placement));
        item->setText(2, tr("%1 byte(s)").arg(attachment.bytes.size()));
        item->setText(3, anchorName);
        setKey(*item, QStringLiteral("attachment:%1").arg(attachment.id.value()));
        const core::SctInspectionLocation attachmentLocation{ core::SctNavigationTarget{
            core::SctNavigationKind::OpaqueAttachment, attachment.id.value() } };
        registerNavigation(item, 0, attachmentLocation);
        registerNavigation(item, 1, attachmentLocation);
        registerNavigation(item, 3, core::SctInspectionLocation{ anchorTarget });
    }
}

}  // namespace salsa::qt
