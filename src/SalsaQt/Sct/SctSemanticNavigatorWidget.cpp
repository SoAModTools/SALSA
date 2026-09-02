#include "Sct/SctSemanticNavigatorWidget.h"

#include "SalsaCore/Sct/SctSemanticUsageIndex.h"

#include "SpiceSCT/SctDocumentIndex.h"
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
    const spice::sct::SctDocumentIndex& index,
    const spice::sct::SctInstructionId instruction) {
    const auto* value = index.find(instruction);
    return value == nullptr
        ? SctSemanticNavigatorWidget::tr("Instruction %1").arg(instruction.value())
        : QStringLiteral("%1 — %2")
            .arg(opcodeName(value->opcode))
            .arg(SctSemanticNavigatorWidget::tr("Instruction %1").arg(instruction.value()));
}

[[nodiscard]] QString instructionContext(
    const spice::sct::SctDocumentIndex& index,
    const spice::sct::SctInstructionId instruction) {
    const auto location = index.instructionLocation(instruction);
    const auto* section = location.has_value() ? index.find(location->sectionId) : nullptr;
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

[[nodiscard]] QString expressionName(const core::SctExpressionSite& site) {
    QString result;
    if (std::holds_alternative<core::SctScheduledExpressionSite>(site.owner)) {
        result = SctSemanticNavigatorWidget::tr("Scheduled expression");
    } else {
        result = parameterName(std::get<spice::sct::SctParameterAddress>(site.owner));
    }
    result += site.childPath.empty()
        ? SctSemanticNavigatorWidget::tr(" / root")
        : SctSemanticNavigatorWidget::tr(" / expression");
    for (const auto child : site.childPath) result += QStringLiteral("/%1").arg(child);
    return result;
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
        } else if constexpr (std::is_same_v<T, core::SctParameterSite>) {
            return QStringLiteral("parameter:%1:%2")
                .arg(typed.instruction.value()).arg(parameterKey(typed.parameter));
        } else {
            QString key = QStringLiteral("expression:%1:")
                .arg(typed.instruction.value());
            if (std::holds_alternative<core::SctScheduledExpressionSite>(typed.owner)) {
                key += QStringLiteral("scheduled");
            } else {
                key += QStringLiteral("parameter:")
                    + parameterKey(std::get<spice::sct::SctParameterAddress>(typed.owner));
            }
            key += QStringLiteral(":path");
            for (const auto child : typed.childPath) key += QStringLiteral(":%1").arg(child);
            return key;
        }
    }, location);
}

[[nodiscard]] QString variableKindName(const core::SctVariableKind kind) {
    switch (kind) {
    case core::SctVariableKind::Integer: return SctSemanticNavigatorWidget::tr("Integer");
    case core::SctVariableKind::Float: return SctSemanticNavigatorWidget::tr("Float");
    case core::SctVariableKind::Bit: return SctSemanticNavigatorWidget::tr("Bit");
    case core::SctVariableKind::Byte: return SctSemanticNavigatorWidget::tr("Byte");
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
    const spice::sct::SctDocumentIndex& index,
    const spice::sct::SctDocumentReferenceTarget& target) {
    return std::visit([&index](const auto typed) { return index.find(typed) != nullptr; }, target);
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
    populateTrees(*opcodes_, *references_, *variables_, *incomplete_, *snapshot.document);
    for (auto* tree : { opcodes_, references_, variables_, incomplete_ }) tree->collapseAll();
    if (sameDocument) {
        restoreState(*opcodes_, states[0]);
        restoreState(*references_, states[1]);
        restoreState(*variables_, states[2]);
        restoreState(*incomplete_, states[3]);
    }
    emptyLabel_->hide();
    tabs_->show();
}

bool SctSemanticNavigatorWidget::applyInstructionChanges(
    const core::AssetLocator& locator,
    const core::SctDocumentSnapshot& snapshot,
    const core::SctEditChangeSet& changes) {
    const auto nextIdentity = QString::fromStdString(locator.identityKey());
    const auto isInstruction = [](const core::SctNavigationTarget target) {
        return target.kind == core::SctNavigationKind::Instruction;
    };
    const bool hasStructuralChange = !changes.created.empty()
        || !changes.removed.empty() || !changes.moved.empty();
    if (snapshot.document == nullptr || identityKey_ != nextIdentity
        || !hasStructuralChange || !changes.modified.empty()
        || !std::ranges::all_of(changes.created, isInstruction)
        || !std::ranges::all_of(changes.removed, isInstruction)
        || !std::ranges::all_of(changes.moved, isInstruction)) return false;

    QTreeWidget desiredOpcodes;
    QTreeWidget desiredReferences;
    QTreeWidget desiredVariables;
    QTreeWidget desiredIncomplete;
    desiredOpcodes.setColumnCount(opcodes_->columnCount());
    desiredReferences.setColumnCount(references_->columnCount());
    desiredVariables.setColumnCount(variables_->columnCount());
    desiredIncomplete.setColumnCount(incomplete_->columnCount());
    populateTrees(desiredOpcodes, desiredReferences, desiredVariables,
        desiredIncomplete, *snapshot.document);

    reconcileTree(*opcodes_, desiredOpcodes);
    reconcileTree(*references_, desiredReferences);
    reconcileTree(*variables_, desiredVariables);
    reconcileTree(*incomplete_, desiredIncomplete);
    return true;
}

void SctSemanticNavigatorWidget::clear() {
    identityKey_.clear();
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
    const spice::sct::SctDocument& document) {
    const auto index = spice::sct::SctDocumentIndex::build(document);
    const auto usage = core::SctSemanticUsageIndex::build(document);
    buildOpcodes(opcodes, index, usage);
    buildReferences(references, index, usage);
    buildVariables(variables, index, usage);
    buildIncompleteEvidence(incomplete, document, index, usage);
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
    const spice::sct::SctDocumentIndex& index,
    const core::SctSemanticUsageIndex& usage) {
    std::map<std::uint16_t, std::vector<core::SctOpcodeUsage>> groups;
    for (const auto& occurrence : usage.opcodeUsages()) groups[occurrence.opcode].push_back(occurrence);
    for (const auto& [opcode, occurrences] : groups) {
        auto* group = new QTreeWidgetItem(&tree);
        group->setText(0, opcodeName(opcode));
        group->setText(1, tr("%1 occurrence(s)").arg(occurrences.size()));
        setKey(*group, QStringLiteral("opcode:%1").arg(opcode));
        for (const auto& occurrence : occurrences) {
            auto* item = new QTreeWidgetItem(group);
            item->setText(0, instructionName(index, occurrence.instruction));
            item->setText(1, instructionContext(index, occurrence.instruction));
            const core::SctInspectionLocation location{ core::SctNavigationTarget{
                core::SctNavigationKind::Instruction, occurrence.instruction.value() } };
            setKey(*item, QStringLiteral("opcode:%1:").arg(opcode) + inspectionKey(location));
            registerNavigation(item, -1, location);
        }
    }
}

void SctSemanticNavigatorWidget::buildReferences(
    QTreeWidget& tree,
    const spice::sct::SctDocumentIndex& index,
    const core::SctSemanticUsageIndex& usage) {
    auto* inbound = new QTreeWidgetItem(&tree);
    inbound->setText(0, tr("Inbound by target"));
    setKey(*inbound, QStringLiteral("references:inbound"));

    using TargetKey = std::pair<std::size_t, std::uint64_t>;
    std::map<TargetKey, std::vector<core::SctReferenceUsage>> inboundGroups;
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
        if (targetExists(index, target))
            registerNavigation(group, 0, core::SctInspectionLocation{ referenceTarget(target) });
        else
            group->setToolTip(0, tr("The target is not present in this document revision."));
        for (const auto& occurrence : occurrences) {
            auto* item = new QTreeWidgetItem(group);
            item->setText(0, parameterName(occurrence.source.parameter));
            item->setText(1, instructionName(index, occurrence.source.instruction));
            item->setText(2, referenceTargetName(occurrence.target));
            const core::SctInspectionLocation sourceLocation{ occurrence.source };
            setKey(*item, QStringLiteral("reference:in:%1:%2:")
                .arg(key.first).arg(key.second) + inspectionKey(sourceLocation));
            registerNavigation(item, 0, sourceLocation);
            registerNavigation(item, 1, sourceLocation);
            if (targetExists(index, occurrence.target))
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
        std::vector<core::SctReferenceUsage>>> outboundGroups;
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
        group->setText(0, instructionName(index, instruction));
        group->setText(1, tr("%1 outbound occurrence(s)").arg(occurrences.size()));
        setKey(*group, QStringLiteral("reference:source:%1").arg(instruction.value()));
        registerNavigation(group, 0, core::SctInspectionLocation{ core::SctNavigationTarget{
            core::SctNavigationKind::Instruction, instruction.value() } });
        for (const auto& occurrence : occurrences) {
            auto* item = new QTreeWidgetItem(group);
            item->setText(0, parameterName(occurrence.source.parameter));
            item->setText(1, instructionContext(index, occurrence.source.instruction));
            item->setText(2, referenceTargetName(occurrence.target));
            const core::SctInspectionLocation sourceLocation{ occurrence.source };
            setKey(*item, QStringLiteral("reference:out:") + inspectionKey(sourceLocation));
            registerNavigation(item, 0, sourceLocation);
            registerNavigation(item, 1, sourceLocation);
            if (targetExists(index, occurrence.target))
                registerNavigation(item, 2,
                    core::SctInspectionLocation{ referenceTarget(occurrence.target) });
            else
                item->setToolTip(2, tr("The target is not present in this document revision."));
        }
    }
}

void SctSemanticNavigatorWidget::buildVariables(
    QTreeWidget& tree,
    const spice::sct::SctDocumentIndex& index,
    const core::SctSemanticUsageIndex& usage) {
    for (const auto kind : { core::SctVariableKind::Integer, core::SctVariableKind::Float,
            core::SctVariableKind::Bit, core::SctVariableKind::Byte }) {
        auto* kindItem = new QTreeWidgetItem(&tree);
        kindItem->setText(0, variableKindName(kind));
        setKey(*kindItem, QStringLiteral("variable-kind:%1").arg(static_cast<int>(kind)));
        std::map<std::uint32_t, std::vector<core::SctVariableUsage>> groups;
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
                    .arg(instructionName(index, occurrence.source.instruction))
                    .arg(instructionContext(index, occurrence.source.instruction)));
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
    const core::SctSemanticUsageIndex& usage) {
    auto* unresolved = new QTreeWidgetItem(&tree);
    unresolved->setText(0, tr("Unresolved references"));
    unresolved->setText(2, tr("%1 occurrence(s)").arg(usage.unresolvedReferences().size()));
    setKey(*unresolved, QStringLiteral("incomplete:unresolved"));
    for (const auto& occurrence : usage.unresolvedReferences()) {
        auto* item = new QTreeWidgetItem(unresolved);
        item->setText(0, expectedTargetName(occurrence.expectedTarget));
        item->setText(1, QStringLiteral("%1 — %2")
            .arg(instructionName(index, occurrence.source.instruction))
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
            .arg(instructionName(index, occurrence.source.instruction))
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
            .arg(instructionName(index, occurrence.source.instruction))
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
