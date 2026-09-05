#include "SalsaCore/Sct/SctPresentation.h"
#include "SalsaCore/Sct/SctAuthoringCatalog.h"

#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctOpcodeMetadata.h"
#include "SpiceSCT/SctScptEncoding.h"
#include "SpiceSCT/SctTextCodec.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <functional>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace salsa::core {
namespace {

[[nodiscard]] std::string hexValue(const std::uint64_t value, const int width = 8) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setfill('0')
           << std::setw(width) << value;
    return stream.str();
}

template <typename Range>
[[nodiscard]] std::string hexList(const Range& values) {
    std::string text;
    for (const auto value : values) {
        if (!text.empty()) text += ' ';
        text += hexValue(static_cast<std::uint64_t>(value), sizeof(value) == 1 ? 2 : 8);
    }
    return text.empty() ? "(empty)" : text;
}

[[nodiscard]] std::string escapedBytes(const std::string& bytes) {
    std::string text;
    for (const auto byte : bytes) {
        const auto value = static_cast<unsigned char>(byte);
        if (value >= 0x20 && value < 0x7f && value != '\\') text.push_back(byte);
        else if (value == '\\') text += "\\\\";
        else text += "\\x" + hexValue(value, 2).substr(2);
    }
    return text.empty() ? "(empty)" : text;
}

[[nodiscard]] std::string textKindName(const spice::sct::SctTextKind kind) {
    return kind == spice::sct::SctTextKind::SctString ? "SCT message" : "plain string";
}

[[nodiscard]] std::string readinessName(const spice::sct::SctDocumentReadiness readiness) {
    switch (readiness) {
    case spice::sct::SctDocumentReadiness::Unavailable: return "Unavailable";
    case spice::sct::SctDocumentReadiness::Inspectable: return "Inspectable";
    case spice::sct::SctDocumentReadiness::StructurallyValid: return "Structurally valid";
    case spice::sct::SctDocumentReadiness::ExportReady: return "Export ready";
    }
    return "Unknown";
}

[[nodiscard]] std::string expressionKindName(const spice::sct::SctScptValueKind kind) {
    using enum spice::sct::SctScptValueKind;
    switch (kind) {
    case InlineValue: return "Inline value";
    case FloatLiteral: return "Float literal";
    case DecimalLiteral: return "Decimal literal";
    case FloatBackedIntegerVariable: return "Float-backed integer variable";
    case IntegerVariable: return "Integer variable";
    case IntegerVariableLow16Comparison: return "Integer variable (low-16 comparison)";
    case FloatVariable: return "Float variable";
    case BitVariable: return "Bit variable";
    case ByteVariable: return "Byte variable";
    case SecondaryValue: return "Secondary value";
    }
    return "SCPT value";
}

[[nodiscard]] std::string numericValue(const double value, const int precision) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return std::signbit(value) ? "-Infinity" : "Infinity";
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(precision) << value;
    return stream.str();
}

[[nodiscard]] std::optional<std::string> operatorValue(const std::uint32_t code) {
    switch (code) {
    case 0x00u: return "<";
    case 0x01u: return "<=";
    case 0x02u: return ">";
    case 0x03u: return ">=";
    case 0x04u: return "==";
    case 0x06u: return "bitwise AND";
    case 0x07u: return "bitwise OR";
    case 0x08u: return "logical AND";
    case 0x09u: return "logical OR";
    case 0x0au: return "=";
    case 0x0bu: return "*";
    case 0x0cu: return "/";
    case 0x0du: return "%";
    case 0x0eu: return "+";
    case 0x0fu: return "-";
    default: return std::nullopt;
    }
}

[[nodiscard]] std::string expressionEncodingNotes(
    const std::uint32_t encodingWord,
    const std::vector<std::uint32_t>& payloadWords = {}) {
    auto notes = "Encoding " + hexValue(encodingWord);
    if (!payloadWords.empty()) notes += "; payload " + hexList(payloadWords);
    return notes;
}

[[nodiscard]] SctPropertyItem expressionOperationProperty(
    const spice::sct::SctScptOperation& operation,
    const spice::sct::SctExpressionSite& expressionSite,
    const std::uint32_t ordinal,
    std::string prefix = "Operation ") {
    const auto location = SctInspectionLocation{spice::sct::SctExpressionOperationSite{
        expressionSite, ordinal}};
    return std::visit([&](const auto& typed) -> SctPropertyItem {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctScptValueOperation>) {
            std::string value = hexValue(typed.encodingWord);
            auto notes = expressionEncodingNotes(typed.encodingWord, typed.payloadWords);
            using enum spice::sct::SctScptValueKind;
            switch (typed.kind) {
            case FloatLiteral:
                value = typed.payloadWords.size() == 1u
                    ? numericValue(static_cast<double>(std::bit_cast<float>(
                        typed.payloadWords.front())), std::numeric_limits<float>::max_digits10)
                    : "(invalid float payload)";
                break;
            case DecimalLiteral: {
                const auto whole = static_cast<std::int16_t>(
                    (typed.encodingWord >> 8u) & 0xffffu);
                const auto fraction = typed.encodingWord & 0xffu;
                value = numericValue(static_cast<double>(whole)
                    + static_cast<double>(fraction) / 256.0,
                    std::numeric_limits<double>::max_digits10);
                break;
            }
            case FloatBackedIntegerVariable:
            case IntegerVariable:
            case IntegerVariableLow16Comparison:
            case FloatVariable:
            case BitVariable:
            case ByteVariable:
                value = std::to_string(typed.encodingWord & 0x00ffffffu);
                break;
            case SecondaryValue: {
                const auto name = spice::sct::sctScptSecondaryValueName(
                    typed.encodingWord & 0x00ffffffu);
                value = name.empty() ? std::to_string(typed.encodingWord & 0x00ffffffu)
                    : std::string(name);
                break;
            }
            case InlineValue:
                break;
            }
            return {prefix + std::to_string(ordinal) + ": "
                    + expressionKindName(typed.kind), std::move(value),
                std::move(notes), {}, location};
        } else if constexpr (std::is_same_v<T, spice::sct::SctScptBinaryOperation>) {
            const auto symbol = spice::sct::sctScptOperatorSymbol(typed.encodingWord);
            const auto value = symbol.empty()
                ? operatorValue(typed.encodingWord).value_or(
                    "Operator " + hexValue(typed.encodingWord))
                : std::string(symbol);
            return {prefix + std::to_string(ordinal) + ": "
                    + (typed.kind == spice::sct::SctScptBinaryOperationKind::Comparison
                        ? "Comparison" : "Arithmetic"), value,
                expressionEncodingNotes(typed.encodingWord), {}, location};
        } else if constexpr (std::is_same_v<T,
                spice::sct::SctScptStackOverwritePreviousWithTopOperation>) {
            return {prefix + std::to_string(ordinal) + ": Stack overwrite",
                "previous = top", expressionEncodingNotes(typed.encodingWord), {}, location};
        } else {
            return {prefix + std::to_string(ordinal) + ": Inert operation",
                "No stack effect", expressionEncodingNotes(typed.encodingWord), {}, location};
        }
    }, operation);
}

[[nodiscard]] SctPropertyItem derivedExpressionProperty(
    const spice::sct::SctScptDerivedExpressionNode& node,
    const spice::sct::SctTypedScptProgram& program,
    const spice::sct::SctExpressionSite& site) {
    if (node.operationOrdinal >= program.operations.size()) {
        return {"Derived operation", "(invalid ordinal)",
            std::to_string(node.operationOrdinal), {}, SctInspectionLocation{site}};
    }
    auto item = expressionOperationProperty(program.operations[node.operationOrdinal],
        site, node.operationOrdinal, "Result operation ");
    for (const auto& child : node.children)
        item.children.push_back(derivedExpressionProperty(child, program, site));
    return item;
}

[[nodiscard]] SctPropertyItem expressionProperty(
    const spice::sct::SctCanonicalExpression& expression,
    const spice::sct::SctExpressionSite& site,
    std::string name = "Expression") {
    SctPropertyItem item{ std::move(name),
        expression.termination == spice::sct::SctExpressionTermination::StopCode
            ? "stop-terminated" : "inline", {}, {} };
    item.location = SctInspectionLocation{site};
    std::visit([&item, &site](const auto& body) {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, spice::sct::SctOpaqueExpression>) {
            item.children.push_back({ "Opaque words", hexList(body.words),
                "Typed structure was not reliable.", {}, SctInspectionLocation{ site } });
        } else {
            SctPropertyItem operations{"Ordered operations",
                std::to_string(body.operations.size()), "Authoritative SCPT program", {}};
            for (std::uint32_t ordinal = 0; ordinal < body.operations.size(); ++ordinal)
                operations.children.push_back(expressionOperationProperty(
                    body.operations[ordinal], site, ordinal));
            item.children.push_back(std::move(operations));
            const auto analysis = spice::sct::analyzeSctScptProgram(body);
            if (analysis.conventionalTree) {
                SctPropertyItem result{"Conventional result", "Derived",
                    "Read-only symbolic projection", {}};
                result.children.push_back(derivedExpressionProperty(
                    *analysis.conventionalTree, body, site));
                item.children.push_back(std::move(result));
            }
        }
    }, expression.body);
    return item;
}

[[nodiscard]] std::string targetStorageName(
    const spice::sct::SctReferenceTargetStorage storage) {
    switch (storage) {
    case spice::sct::SctReferenceTargetStorage::Instruction: return "instruction";
    case spice::sct::SctReferenceTargetStorage::IndexedString: return "indexed string";
    case spice::sct::SctReferenceTargetStorage::SupplementaryText: return "supplementary text";
    }
    return "unknown";
}

[[nodiscard]] SctPropertyItem parameterProperty(
    const spice::sct::SctInstructionId instruction,
    const spice::sct::SctDocumentParameter& parameter,
    const std::optional<std::uint32_t> repeatedGroupOrdinal) {
    const spice::sct::SctParameterAddress address{
        parameter.schemaIndex, repeatedGroupOrdinal };
    const spice::sct::SctParameterSite parameterSite{ instruction, address };
    std::string role;
    std::string notes;
    std::string value;
    std::vector<SctPropertyItem> children;
    std::visit([&](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctEncodedWordValue>) {
            role = "Encoded word"; value = hexValue(typed.value);
        } else if constexpr (std::is_same_v<T, spice::sct::SctCanonicalExpression>) {
            role = "SCPT expression";
            children = expressionProperty(typed,
                spice::sct::SctExpressionSite{ instruction, address }).children;
            value = typed.termination == spice::sct::SctExpressionTermination::StopCode
                ? "stop-terminated" : "inline";
        } else if constexpr (std::is_same_v<T, spice::sct::SctTerminatedWordSequenceValue>) {
            role = "Terminated words"; value = hexList(typed.words);
        } else if constexpr (std::is_same_v<T, spice::sct::SctInstructionReference>) {
            role = "Instruction reference"; value = "Instruction " + std::to_string(typed.target.value());
        } else if constexpr (std::is_same_v<T, spice::sct::SctStringReference>) {
            role = "String reference"; value = "String " + std::to_string(typed.target.value());
        } else if constexpr (std::is_same_v<T, spice::sct::SctSupplementaryTextReference>) {
            role = "Supplementary-text reference";
            value = "Supplementary text " + std::to_string(typed.target.value());
        } else if constexpr (std::is_same_v<T, spice::sct::SctUnresolvedReferenceValue>) {
            role = "Unresolved reference"; value = hexList(typed.encodedWords);
            notes = "Expected " + targetStorageName(typed.expectedTarget.storage);
        } else {
            role = "Opaque parameter"; value = hexList(typed.words);
            notes = "Retained without a claimed semantic interpretation.";
        }
    }, parameter.value);
    return { "Parameter " + std::to_string(parameter.schemaIndex), value,
        role + (notes.empty() ? "" : "; " + notes), std::move(children),
        SctInspectionLocation{ parameterSite } };
}

[[nodiscard]] std::string commandName(const spice::sct::SctMessageCommandCode code) {
    using enum spice::sct::SctMessageCommandCode;
    switch (code) {
    case A: return "/a"; case B: return "/b"; case C: return "/c";
    case D: return "/d"; case E: return "/e"; case P: return "/p";
    case R: return "/r"; case S: return "/s"; case U: return "/u";
    case X: return "/x"; case Wc: return "/wc"; case Wo: return "/wo";
    }
    return "/?";
}

[[nodiscard]] std::string commandArgument(const spice::sct::SctMessageCommandArgument& argument) {
    return std::visit([](const auto& typed) -> std::string {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctNoCommandArgument>) return {};
        else if constexpr (std::is_same_v<T, spice::sct::SctDecimalCommandArgument>)
            return typed.value.has_value() ? std::to_string(*typed.value) : "(missing)";
        else return hexList(typed.values);
    }, argument);
}

void appendTextProperties(std::vector<SctPropertyItem>& properties,
    std::vector<SctTextPreviewRun>& preview,
    const spice::sct::SctTextValue& value) {
    std::visit([&](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctPlainText>) {
            properties.push_back({ "Text", typed.utf8, {}, {} });
            preview.push_back({ typed.utf8, false, std::nullopt });
        } else if constexpr (std::is_same_v<T, spice::sct::SctMessage>) {
            properties.push_back({ "Header", typed.headerUtf8.value_or("(none)"), {}, {} });
            SctPropertyItem elements{ "Ordered body elements", std::to_string(typed.body.elements.size()), {}, {} };
            bool bold = false;
            std::optional<std::uint32_t> color;
            for (std::size_t i = 0; i < typed.body.elements.size(); ++i) {
                std::visit([&](const auto& element) {
                    using E = std::decay_t<decltype(element)>;
                    if constexpr (std::is_same_v<E, spice::sct::SctTextChunk>) {
                        elements.children.push_back({ "Text " + std::to_string(i), element.utf8, {}, {} });
                        preview.push_back({ element.utf8, bold, color });
                    } else {
                        const auto argument = commandArgument(element.argument);
                        elements.children.push_back({ "Command " + std::to_string(i),
                            commandName(element.code), argument, {} });
                        if (element.code == spice::sct::SctMessageCommandCode::B) bold = !bold;
                        if (element.code == spice::sct::SctMessageCommandCode::R) {
                            bold = false; color.reset();
                        }
                        if (element.code == spice::sct::SctMessageCommandCode::P) {
                            if (const auto* bytes = std::get_if<spice::sct::SctByteListCommandArgument>(
                                    &element.argument); bytes != nullptr && bytes->values.size() >= 3) {
                                color = (static_cast<std::uint32_t>(bytes->values[0]) << 16)
                                    | (static_cast<std::uint32_t>(bytes->values[1]) << 8)
                                    | bytes->values[2];
                            }
                        }
                    }
                }, typed.body.elements[i]);
            }
            properties.push_back(std::move(elements));
        } else if constexpr (std::is_same_v<T, spice::sct::SctOpaqueText>) {
            properties.push_back({ "Opaque bytes", hexList(typed.bytes),
                "No convention was selected or the record could not be decoded.", {} });
        } else {
            properties.push_back({ "Value", "(empty indexed text)", {}, {} });
        }
    }, value);
}

[[nodiscard]] std::string sectionKind(const spice::sct::SctDocumentSectionContent& content) {
    return std::visit([](const auto& typed) -> std::string {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctScriptSectionContent>) return "Script";
        else if constexpr (std::is_same_v<T, spice::sct::SctStringSectionContent>) return "Indexed string";
        else if constexpr (std::is_same_v<T, spice::sct::SctStringGroupMarkerSectionContent>)
            return "String group marker";
        else return "Opaque";
    }, content);
}

[[nodiscard]] std::string stringGroupBasisName(
    const spice::sct::SctIndexedStringGroupBasis basis) {
    return basis == spice::sct::SctIndexedStringGroupBasis::ExplicitMarker
        ? "explicit marker" : "unmarked contiguous run";
}

[[nodiscard]] std::string semanticConfidenceName(
    const spice::sct::SctSemanticConfidence confidence) {
    using enum spice::sct::SctSemanticConfidence;
    switch (confidence) {
    case Known: return "known";
    case Partial: return "partial";
    case Heuristic: return "heuristic";
    case Unknown: return "unknown";
    }
    return "unknown";
}

[[nodiscard]] std::string controlFlowKindName(
    const spice::sct::SctControlFlowKind kind) {
    using enum spice::sct::SctControlFlowKind;
    switch (kind) {
    case Fallthrough: return "fallthrough";
    case BranchTrue: return "true branch";
    case BranchFalse: return "false branch";
    case SwitchCase: return "switch case";
    case Jump: return "jump";
    case Call: return "call";
    case Return: return "return";
    }
    return "control flow";
}

[[nodiscard]] std::string structuredRegionName(
    const spice::sct::SctStructuredRegionKind kind) {
    using enum spice::sct::SctStructuredRegionKind;
    switch (kind) {
    case If: return "if";
    case IfElse: return "if / else";
    case While: return "while";
    case NaturalLoop: return "loop";
    case Switch: return "switch";
    }
    return "structured region";
}

[[nodiscard]] std::string rejectionReasonName(
    const spice::sct::SctStructuredRejectionReason reason) {
    using enum spice::sct::SctStructuredRejectionReason;
    switch (reason) {
    case MissingTarget: return "missing target";
    case ExternalEntry: return "external entry";
    case UnsupportedExitShape: return "unsupported exit shape";
    case NonContiguousCandidate: return "non-contiguous candidate";
    case OverlappingArms: return "overlapping arms";
    case AmbiguousSwitchCases: return "ambiguous switch cases";
    case ClosedComponentWithoutExit: return "closed component without exit";
    case HistoricalConflict: return "conflicts with current control flow";
    case UnsupportedHistoricalShape: return "unsupported imported shape";
    }
    return "unclassified";
}

[[nodiscard]] std::string structureEvidenceKindName(
    const spice::sct::SctStructureEvidenceKind kind) {
    using enum spice::sct::SctStructureEvidenceKind;
    switch (kind) {
    case CurrentControlFlow: return "current control flow";
    case ConditionalFalseTarget: return "conditional false target";
    case PreTargetJump: return "jump before false target";
    case BackwardTerminatorJump: return "backward terminator jump";
    case CommonForwardExit: return "common forward exit";
    case PhysicalCaseBoundary: return "physical case boundary";
    case SharedCaseTarget: return "shared case target";
    case CaseFallthrough: return "case fallthrough";
    case ImportedControlFlow: return "imported control flow";
    case ImportedOpaqueControlFlowGap: return "imported opaque control-flow gap";
    }
    return "structure evidence";
}

[[nodiscard]] SctInspectionLocation instructionLocation(
    const spice::sct::SctInstructionId instruction) {
    return SctNavigationTarget{SctNavigationKind::Instruction, instruction.value()};
}

[[nodiscard]] SctPropertyItem historicalEvidenceProperty(
    const spice::sct::SctHistoricalStructureCandidate& candidate,
    const std::size_t ordinal) {
    SctPropertyItem result{
        "Evidence " + std::to_string(ordinal + 1u),
        candidate.suggestedKind
            ? structuredRegionName(*candidate.suggestedKind)
            : "control-flow observation",
        "Imported evidence is explanatory only; current document semantics remain authoritative.",
        {}};
    result.children.push_back({"Source instruction",
        std::to_string(candidate.sourceInstruction.value()), {}, {},
        instructionLocation(candidate.sourceInstruction)});
    if (candidate.targetInstruction) {
        result.children.push_back({"Resolved imported target",
            std::to_string(candidate.targetInstruction->value()), {}, {},
            instructionLocation(*candidate.targetInstruction)});
    } else {
        result.children.push_back({"Resolved imported target", "(none)", {}, {}});
    }
    result.children.push_back({"Unresolved target payload offset",
        candidate.unresolvedTargetPayloadOffset
            ? hexValue(*candidate.unresolvedTargetPayloadOffset)
            : "(none)", {}, {}});
    result.children.push_back({"Suggested kind",
        candidate.suggestedKind
            ? structuredRegionName(*candidate.suggestedKind) : "(none)", {}, {}});
    if (candidate.suggestedController) {
        result.children.push_back({"Suggested controller",
            std::to_string(candidate.suggestedController->value()), {}, {},
            instructionLocation(*candidate.suggestedController)});
    } else {
        result.children.push_back({"Suggested controller", "(none)", {}, {}});
    }
    if (candidate.suggestedJoin) {
        result.children.push_back({"Suggested join",
            std::to_string(candidate.suggestedJoin->value()), {}, {},
            instructionLocation(*candidate.suggestedJoin)});
    } else {
        result.children.push_back({"Suggested join", "(none)", {}, {}});
    }
    result.children.push_back({"Confidence",
        semanticConfidenceName(candidate.evidenceConfidence), {}, {}});
    result.children.push_back({"Rejection reason",
        candidate.rejectionReason
            ? rejectionReasonName(*candidate.rejectionReason) : "(none)", {}, {}});

    SctPropertyItem evidence{"Opaque and structural evidence",
        std::to_string(candidate.evidence.size()), {}, {}};
    for (std::size_t index = 0; index < candidate.evidence.size(); ++index) {
        const auto& source = candidate.evidence[index];
        SctPropertyItem item{"Evidence record " + std::to_string(index + 1u),
            structureEvidenceKindName(source.kind),
            semanticConfidenceName(source.confidence) + " confidence", {}};
        if (source.edgeKind)
            item.children.push_back({"Edge kind", controlFlowKindName(*source.edgeKind), {}, {}});
        if (source.source) {
            item.children.push_back({"Source instruction",
                std::to_string(source.source->value()), {}, {},
                instructionLocation(*source.source)});
        }
        if (source.target) {
            item.children.push_back({"Target instruction",
                std::to_string(source.target->value()), {}, {},
                instructionLocation(*source.target)});
        }
        for (const auto attachment : source.opaqueAttachments) {
            item.children.push_back({"Opaque attachment",
                std::to_string(attachment.value()), "Imported gap evidence", {},
                SctInspectionLocation{SctNavigationTarget{
                    SctNavigationKind::OpaqueAttachment, attachment.value()}}});
        }
        evidence.children.push_back(std::move(item));
    }
    result.children.push_back(std::move(evidence));
    return result;
}

void appendHistoricalEvidence(SctEntityPresentation& presentation,
    const spice::sct::SctSectionStructure& section,
    const std::optional<spice::sct::SctInstructionId> sourceInstruction) {
    SctPropertyItem group{"Imported control-flow evidence", "0",
        "Import-only evidence; it does not alter current structure.", {}};
    for (const auto& candidate : section.historicalCandidates) {
        if (sourceInstruction && candidate.sourceInstruction != *sourceInstruction) continue;
        group.children.push_back(historicalEvidenceProperty(
            candidate, group.children.size()));
    }
    if (group.children.empty()) return;
    group.value = std::to_string(group.children.size());
    presentation.properties.push_back(std::move(group));
}

void markImportedEvidence(SctOutlineItem& item, const std::size_t count) {
    item.importedEvidenceCount = count;
    if (count != 0u)
        item.secondary += " | Imported evidence: " + std::to_string(count);
}

[[nodiscard]] std::string opaqueAnchorName(const spice::sct::SctOpaqueAnchor& anchor) {
    return std::visit([](const auto& typed) -> std::string {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctDocumentAnchor>)
            return "Document";
        else if constexpr (std::is_same_v<T, spice::sct::SctSectionId>)
            return "Section " + std::to_string(typed.value());
        else if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
            return "Instruction " + std::to_string(typed.value());
        else if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return "Indexed string " + std::to_string(typed.value());
        else
            return "Supplementary text " + std::to_string(typed.value());
    }, anchor);
}

[[nodiscard]] std::string sourceRegionName(const spice::sct::SctSourceRegion region) {
    using enum spice::sct::SctSourceRegion;
    switch (region) {
    case Header: return "header";
    case SectionIndex: return "section index";
    case SectionPayload: return "section payload";
    case Footer: return "footer";
    }
    return "unknown";
}

[[nodiscard]] std::string sourceRoleName(const spice::sct::SctSourceSpanRole role) {
    using enum spice::sct::SctSourceSpanRole;
    switch (role) {
    case Header: return "header"; case SectionCount: return "section count";
    case SectionIndexRow: return "section index row";
    case SectionOffsetField: return "section offset"; case SectionName: return "section name";
    case SectionNamePadding: return "section name padding"; case SectionPayload: return "section payload";
    case Instruction: return "instruction"; case InstructionModifier: return "instruction modifier";
    case InstructionOpcode: return "instruction opcode"; case InstructionParameter: return "instruction parameter";
    case Expression: return "expression"; case ExpressionOperation: return "expression operation";
    case ExpressionPayload: return "expression payload";
    case ExpressionTerminator: return "expression terminator";
    case TextElement: return "text element";
    case TextTerminator: return "text terminator";
    case IndexedStringGroupMarkerPreamble: return "indexed-string group marker preamble";
    case IndexedStringPreamble: return "indexed-string preamble";
    case IndexedStringRecord: return "indexed-string record"; case FooterRegion: return "footer region";
    case FooterEntry: return "footer entry"; case DerivedPadding: return "derived padding";
    case OpaqueAttachment: return "opaque attachment";
    }
    return "source record";
}

[[nodiscard]] std::optional<SctInspectionLocation> inspectionForImportedTarget(
    const spice::sct::SctImportedSourceTarget& target) {
    return std::visit([](const auto& typed) -> std::optional<SctInspectionLocation> {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctDocumentEntityId>) {
            return std::visit([](const auto& id) -> std::optional<SctInspectionLocation> {
                using Id = std::decay_t<decltype(id)>;
                if constexpr (std::is_same_v<Id, std::monostate>) return std::nullopt;
                else if constexpr (std::is_same_v<Id, spice::sct::SctSectionId>)
                    return SctInspectionLocation{SctNavigationTarget{SctNavigationKind::Section, id.value()}};
                else if constexpr (std::is_same_v<Id, spice::sct::SctInstructionId>)
                    return SctInspectionLocation{SctNavigationTarget{SctNavigationKind::Instruction, id.value()}};
                else if constexpr (std::is_same_v<Id, spice::sct::SctStringId>)
                    return SctInspectionLocation{SctNavigationTarget{SctNavigationKind::String, id.value()}};
                else if constexpr (std::is_same_v<Id, spice::sct::SctSupplementaryTextId>)
                    return SctInspectionLocation{SctNavigationTarget{SctNavigationKind::SupplementaryText, id.value()}};
                else return SctInspectionLocation{SctNavigationTarget{
                    SctNavigationKind::OpaqueAttachment, id.value()}};
            }, typed);
        } else if constexpr (std::is_same_v<T, spice::sct::SctParameterSite>
            || std::is_same_v<T, spice::sct::SctExpressionSite>
            || std::is_same_v<T, spice::sct::SctExpressionOperationSite>) {
            return SctInspectionLocation{typed};
        } else if constexpr (std::is_same_v<T, spice::sct::SctTextSite>) {
            return std::visit([](const auto& id) -> SctInspectionLocation {
                using Id = std::decay_t<decltype(id)>;
                if constexpr (std::is_same_v<Id, spice::sct::SctStringId>)
                    return SctNavigationTarget{SctNavigationKind::String, id.value()};
                else return SctNavigationTarget{SctNavigationKind::SupplementaryText, id.value()};
            }, typed.text);
        } else {
            return std::nullopt;
        }
    }, target);
}

[[nodiscard]] std::string addressabilityName(
    const spice::sct::SctImportedSiteAddressability value) {
    using enum spice::sct::SctImportedSiteAddressability;
    switch (value) {
    case ExactSite: return "exact site remains addressable";
    case ParentSiteOnly: return "only the parent site remains addressable";
    case OwningEntityOnly: return "only the owning entity remains addressable";
    case MissingEntity: return "the historical entity is no longer addressable";
    }
    return "addressability unknown";
}

[[nodiscard]] SctEntityPresentation describeText(
    std::string title, std::string subtitle, spice::sct::SctTextKind kind,
    spice::sct::SctTextStorage storage,
    const spice::sct::SctTextValue& value) {
    SctEntityPresentation result{ std::move(title), std::move(subtitle), {}, {} };
    result.properties.push_back({ "Kind", textKindName(kind), {}, {} });
    appendTextProperties(result.properties, result.preview, value);
    if (const auto* opaque = std::get_if<spice::sct::SctOpaqueText>(&value)) {
        const auto inspection = spice::sct::SctTextInspectionService::inspectKnownConventions(
            *opaque, kind, storage);
        SctPropertyItem interpretations{ "Candidate interpretations",
            std::to_string(inspection.interpretations.size()),
            inspection.ambiguous ? std::string("Multiple interpretations remain possible.")
                                 : std::string{}, {} };
        for (const auto& candidate : inspection.interpretations) {
            const auto name = candidate.knownConvention.has_value()
                ? std::string(sctTextConventionName(*candidate.knownConvention)) : "custom";
            interpretations.children.push_back({ name,
                candidate.complete ? "complete" : "incomplete",
                std::to_string(candidate.issues.size()) + " issue(s)", {} });
        }
        result.properties.push_back(std::move(interpretations));
    }
    return result;
}

}  // namespace

std::vector<SctOutlineItem> SctPresentationService::outline(
    const SctDocumentSnapshot& snapshot) {
    std::vector<SctOutlineItem> result;
    std::unordered_map<std::uint64_t, std::size_t> sectionEvidenceCounts;
    std::unordered_map<std::uint64_t, std::size_t> instructionEvidenceCounts;
    for (const auto& structure : snapshot.analysis->structuredControlFlow.sections()) {
        sectionEvidenceCounts[structure.section.value()] =
            structure.historicalCandidates.size();
        for (const auto& candidate : structure.historicalCandidates)
            ++instructionEvidenceCounts[candidate.sourceInstruction.value()];
    }
    result.push_back({ "Document", snapshot.provenance->source().descriptor.locator.path().generic_string(),
        { SctNavigationKind::Document, 0 }, {} });
    for (std::size_t sectionIndex = 0; sectionIndex < snapshot.document->sections.size(); ++sectionIndex) {
        const auto& section = snapshot.document->sections[sectionIndex];
        SctOutlineItem item{
            "[" + std::to_string(sectionIndex) + "] " + escapedBytes(section.nameBytes),
            sectionKind(section.content), { SctNavigationKind::Section, section.id.value() }, {} };
        markImportedEvidence(item, sectionEvidenceCounts[section.id.value()]);
        if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&section.content)) {
            for (const auto& instruction : script->instructions) {
                auto mnemonic = SctCatalogResolver::resolve(instruction.opcode).mnemonic;
                if (mnemonic.empty()) mnemonic = "Opcode";
                SctOutlineItem instructionItem{ mnemonic + " (" + std::to_string(instruction.opcode) + ")",
                    "Instruction " + std::to_string(instruction.id.value()),
                    { SctNavigationKind::Instruction, instruction.id.value() }, {} };
                markImportedEvidence(instructionItem,
                    instructionEvidenceCounts[instruction.id.value()]);
                item.children.push_back(std::move(instructionItem));
            }
        } else if (const auto* strings = std::get_if<spice::sct::SctStringSectionContent>(&section.content)) {
            item.children.push_back({ "Indexed string", textKindName(strings->string.kind),
                { SctNavigationKind::String, strings->string.id.value() }, {} });
        }
        result.push_back(std::move(item));
    }
    SctOutlineItem footer{ "Footer", std::to_string(snapshot.document->supplementaryText.size()) + " entries",
        { SctNavigationKind::FooterGroup, 0 }, {} };
    for (std::size_t i = 0; i < snapshot.document->supplementaryText.size(); ++i) {
        const auto& entry = snapshot.document->supplementaryText[i];
        footer.children.push_back({ "[" + std::to_string(i) + "] " + textKindName(entry.kind), {},
            { SctNavigationKind::SupplementaryText, entry.id.value() }, {} });
    }
    result.push_back(std::move(footer));
    SctOutlineItem opaque{ "Opaque attachments",
        std::to_string(snapshot.document->opaqueAttachments.size()) + " attachments",
        { SctNavigationKind::OpaqueGroup, 0 }, {} };
    for (const auto& attachment : snapshot.document->opaqueAttachments) {
        opaque.children.push_back({ "Attachment " + std::to_string(attachment.id.value()),
            std::to_string(attachment.bytes.size()) + " bytes",
            { SctNavigationKind::OpaqueAttachment, attachment.id.value() }, {} });
    }
    result.push_back(std::move(opaque));
    return result;
}

std::vector<SctOutlineItem> SctPresentationService::outline(
    const SctDocumentSnapshot& snapshot,
    const std::span<const SctSectionFolder> folders) {
    auto flat = outline(snapshot);
    if (folders.empty()) return flat;
    std::unordered_map<std::uint64_t, SctOutlineItem> sectionItems;
    std::vector<SctOutlineItem> prefix;
    std::vector<SctOutlineItem> suffix;
    bool sawSection = false;
    for (auto& item : flat) {
        if (item.target.kind == SctNavigationKind::Section) {
            sawSection = true;
            sectionItems.emplace(item.target.id, std::move(item));
        } else if (!sawSection) {
            prefix.push_back(std::move(item));
        } else {
            suffix.push_back(std::move(item));
        }
    }
    std::unordered_map<std::uint64_t, const SctSectionFolder*> byId;
    std::unordered_map<std::uint64_t, std::uint64_t> directFolder;
    for (const auto& folder : folders) {
        byId.emplace(folder.id.value, &folder);
        for (const auto section : folder.sections)
            directFolder.emplace(section.value(), folder.id.value);
    }
    std::function<SctOutlineItem(const SctSectionFolder&)> makeFolder;
    makeFolder = [&](const SctSectionFolder& folder) {
        SctOutlineItem result{folder.name, "Section folder",
            {SctNavigationKind::SectionFolder, folder.id.value}, {}};
        const auto coveredByChild = [&](const auto section) {
            return std::ranges::any_of(folders, [&](const auto& child) {
                return child.parent == folder.id
                    && std::ranges::find(child.sections, section) != child.sections.end();
            });
        };
        for (const auto section : folder.sections)
            if (const auto found = sectionItems.find(section.value());
                found != sectionItems.end() && !coveredByChild(section))
                result.children.push_back(found->second);
        for (const auto& child : folders)
            if (child.parent == folder.id)
                result.children.push_back(makeFolder(child));
        const auto position = [&](const SctOutlineItem& item) {
            if (item.target.kind == SctNavigationKind::Section) {
                const auto found = std::ranges::find_if(snapshot.document->sections,
                    [&](const auto& section) { return section.id.value() == item.target.id; });
                return static_cast<std::size_t>(found - snapshot.document->sections.begin());
            }
            const auto* nested = byId.at(item.target.id);
            std::size_t first = snapshot.document->sections.size();
            for (const auto section : nested->sections) {
                const auto found = std::ranges::find(snapshot.document->sections,
                    section, &spice::sct::SctDocumentSection::id);
                first = std::min(first, static_cast<std::size_t>(
                    found - snapshot.document->sections.begin()));
            }
            return first;
        };
        std::ranges::stable_sort(result.children, {}, position);
        return result;
    };
    std::unordered_set<std::uint64_t> emittedFolders;
    for (const auto& section : snapshot.document->sections) {
        const auto grouped = directFolder.find(section.id.value());
        if (grouped == directFolder.end()) {
            if (const auto found = sectionItems.find(section.id.value());
                found != sectionItems.end())
                prefix.push_back(found->second);
            continue;
        }
        auto folderId = grouped->second;
        while (byId.at(folderId)->parent) folderId = byId.at(folderId)->parent->value;
        if (emittedFolders.insert(folderId).second)
            prefix.push_back(makeFolder(*byId.at(folderId)));
    }
    prefix.insert(prefix.end(), std::make_move_iterator(suffix.begin()),
        std::make_move_iterator(suffix.end()));
    return prefix;
}

SctEntityPresentation SctPresentationService::describe(
    const SctDocumentSnapshot& snapshot,
    const SctNavigationTarget target) {
    return describe(snapshot, target, snapshot.analysis->entities);
}

SctEntityPresentation SctPresentationService::describe(
    const SctDocumentSnapshot& snapshot,
    const SctNavigationTarget target,
    const spice::sct::SctDocumentIndex& index) {
    if (target.kind == SctNavigationKind::Document) {
        return { "SCT document", snapshot.provenance->source().descriptor.locator.path().generic_string(), {
            { "Source size", std::to_string(snapshot.provenance->source().descriptor.byteSize) + " bytes", {}, {} },
            { "Source revision", snapshot.provenance->source().descriptor.revision.digest.toHex(), {}, {} },
            { "Source byte order", snapshot.provenance->importReceipt()
                ? std::to_string(static_cast<int>(snapshot.provenance->importReceipt()->source.byteOrder)) : "Unknown", {}, {} },
            { "Source wrapper", snapshot.provenance->importReceipt()
                ? std::to_string(static_cast<int>(snapshot.provenance->importReceipt()->source.wrapper)) : "Unknown", {}, {} },
            { "Readiness", readinessName(snapshot.readiness), {}, {} },
            { "Text convention", snapshot.provenance->textConvention.has_value()
                ? std::string(sctTextConventionName(*snapshot.provenance->textConvention)) : "Unselected / opaque", {}, {} },
            { "Sections", std::to_string(snapshot.document->sections.size()), {}, {} },
            { "Supplementary text", std::to_string(snapshot.document->supplementaryText.size()), {}, {} },
            { "Opaque attachments", std::to_string(snapshot.document->opaqueAttachments.size()), {}, {} },
        }, {} };
    }
    if (target.kind == SctNavigationKind::Section) {
        if (const auto* section = index.find(*snapshot.document,
                spice::sct::SctSectionId(target.id))) {
            SctEntityPresentation result{ "Section " + escapedBytes(section->nameBytes), sectionKind(section->content), {
                { "Entity ID", std::to_string(section->id.value()), {}, {} },
                { "Name bytes", escapedBytes(section->nameBytes), {}, {} },
                { "Content", sectionKind(section->content), {}, {} },
            }, {} };
            if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&section->content))
                result.properties.push_back({ "Instructions", std::to_string(script->instructions.size()), {}, {} });
            if (const auto* string = std::get_if<spice::sct::SctStringSectionContent>(&section->content))
                result.properties.push_back({ "Preamble words", hexList(string->preambleWords), {}, {} });
            if (const auto* marker = std::get_if<spice::sct::SctStringGroupMarkerSectionContent>(
                    &section->content)) {
                result.properties.push_back({"Preamble words",
                    hexList(marker->preambleWords), "String-group marker evidence", {}});
            }

            const auto* currentGroup = snapshot.analysis->stringGroups.currentByMarker(section->id);
            if (currentGroup == nullptr)
                currentGroup = snapshot.analysis->stringGroups.currentContainingSection(section->id);
            if (currentGroup != nullptr) {
                SctPropertyItem group{"Current indexed-string group",
                    std::to_string(currentGroup->ordinal),
                    stringGroupBasisName(currentGroup->basis), {}};
                group.children.push_back({"Member sections",
                    std::to_string(currentGroup->memberSections.size()), {}, {}});
                group.children.push_back({"Strings",
                    std::to_string(currentGroup->strings.size()), {}, {}});
                if (currentGroup->markerSection) {
                    group.children.push_back({"Marker section",
                        std::to_string(currentGroup->markerSection->value()), {}, {},
                        SctInspectionLocation{SctNavigationTarget{SctNavigationKind::Section,
                            currentGroup->markerSection->value()}}});
                }
                result.properties.push_back(std::move(group));
            }

            const auto* importedGroup = snapshot.analysis->stringGroups.importedByMarker(section->id);
            if (importedGroup == nullptr)
                importedGroup = snapshot.analysis->stringGroups.importedContainingSection(section->id);
            if (importedGroup != nullptr) {
                result.properties.push_back({"Imported group observation",
                    std::to_string(importedGroup->ordinal),
                    stringGroupBasisName(importedGroup->basis) + "; "
                        + semanticConfidenceName(importedGroup->confidence) + " confidence",
                    {}});
            }
            const auto ambiguityCount = std::ranges::count_if(
                snapshot.analysis->stringGroups.importedAmbiguities(),
                [sectionId = section->id](const auto& ambiguity) {
                    return std::visit([sectionId](const auto id) {
                        using Id = std::decay_t<decltype(id)>;
                        if constexpr (std::is_same_v<Id, spice::sct::SctSectionId>)
                            return id == sectionId;
                        else return false;
                    }, ambiguity.target);
                });
            if (ambiguityCount != 0) {
                result.properties.push_back({"Imported group ambiguity",
                    std::to_string(ambiguityCount),
                    "Historical group membership is not singular.", {}});
            }
            if (const auto* structure =
                    snapshot.analysis->structuredControlFlow.findSection(section->id))
                appendHistoricalEvidence(result, *structure, std::nullopt);
            return result;
        }
    }
    if (target.kind == SctNavigationKind::Instruction) {
        if (const auto* instruction = index.find(*snapshot.document,
                spice::sct::SctInstructionId(target.id))) {
            const auto* schema = spice::sct::findSctOpcodeSchema(instruction->opcode);
            auto catalog = SctCatalogResolver::resolve(instruction->opcode);
            const auto mnemonic = catalog.mnemonic.empty()
                ? std::string("Unknown opcode") : catalog.mnemonic;
            SctEntityPresentation result{ mnemonic, "Opcode " + std::to_string(instruction->opcode), {
                { "Entity ID", std::to_string(instruction->id.value()), {}, {} },
                { "Opcode", std::to_string(instruction->opcode) + " (" + hexValue(instruction->opcode, 4) + ")", mnemonic, {} },
                { "Skip refresh", instruction->skipRefresh ? "Yes" : "No", {}, {} },
            }, {} };
            if (!catalog.description.empty())
                result.properties.push_back({"Description", catalog.description, {}, {}});
            if (!catalog.note.empty())
                result.properties.push_back({"Catalog notes", catalog.note, {}, {}});
            if (instruction->scheduledExpression.has_value())
                result.properties.push_back(expressionProperty(*instruction->scheduledExpression,
                    spice::sct::SctExpressionSite{ instruction->id,
                        spice::sct::SctScheduledExpressionSite{} },
                    "Scheduled expression"));
            SctPropertyItem fixed{ "Fixed parameters", std::to_string(instruction->fixedParameters.size()), {}, {} };
            for (const auto& parameter : instruction->fixedParameters)
                fixed.children.push_back(parameterProperty(instruction->id, parameter, std::nullopt));
            result.properties.push_back(std::move(fixed));
            SctPropertyItem repeated{ "Repeated groups", std::to_string(instruction->repeatedParameterGroups.size()), {}, {} };
            for (std::size_t groupIndex = 0; groupIndex < instruction->repeatedParameterGroups.size(); ++groupIndex) {
                SctPropertyItem group{ "Group " + std::to_string(groupIndex), {}, {}, {} };
                for (const auto& parameter : instruction->repeatedParameterGroups[groupIndex].parameters)
                    group.children.push_back(parameterProperty(instruction->id, parameter,
                        static_cast<std::uint32_t>(groupIndex)));
                repeated.children.push_back(std::move(group));
            }
            result.properties.push_back(std::move(repeated));
            if (const auto* owner = index.owningSection(
                    *snapshot.document, instruction->id)) {
                if (const auto* structure = snapshot.analysis->structuredControlFlow
                        .findSection(owner->id))
                    appendHistoricalEvidence(result, *structure, instruction->id);
            }
            return result;
        }
    }
    if (target.kind == SctNavigationKind::String) {
        if (const auto* string = index.find(*snapshot.document,
                spice::sct::SctStringId(target.id)))
            return describeText("Indexed string " + std::to_string(string->id.value()),
                "Section-backed text", string->kind,
                spice::sct::SctTextStorage::IndexedSection, string->value);
    }
    if (target.kind == SctNavigationKind::SupplementaryText) {
        if (const auto* entry = index.find(*snapshot.document,
                spice::sct::SctSupplementaryTextId(target.id)))
            return describeText("Supplementary text " + std::to_string(entry->id.value()),
                "Supplementary text", entry->kind, spice::sct::SctTextStorage::Footer, entry->value);
    }
    if (target.kind == SctNavigationKind::OpaqueAttachment) {
        if (const auto* attachment = index.find(*snapshot.document,
                spice::sct::SctOpaqueAttachmentId(target.id))) {
            const auto anchorTarget = navigationTargetForOpaqueAnchor(attachment->anchor);
            SctEntityPresentation result{ "Opaque attachment " + std::to_string(attachment->id.value()),
                std::to_string(attachment->bytes.size()) + " bytes", {
                { "Bytes", hexList(attachment->bytes), {}, {} },
                { "Anchor", opaqueAnchorName(attachment->anchor),
                    "Activate the attachment in the physical outline to follow this anchor.",
                    {}, SctInspectionLocation{ anchorTarget } },
                { "Alignment", std::to_string(attachment->alignment), {}, {} },
                { "Fixed offset", attachment->fixedOffset.has_value()
                    ? hexValue(*attachment->fixedOffset) : "(none)", {}, {} },
                { "Placement", std::to_string(static_cast<int>(attachment->placement)), {}, {} },
                { "Relocation", std::to_string(static_cast<int>(attachment->relocation)), {}, {} },
                { "Reason", std::to_string(static_cast<int>(attachment->reason)), {}, {} },
            }, {} };
            const auto* context = snapshot.analysis->opaqueContext.find(attachment->id);
            if (context != nullptr) {
                result.properties.push_back({"Decoded source span",
                    hexValue(context->sourceSpan.offset) + " - "
                        + hexValue(context->sourceSpan.endOffset()),
                    std::to_string(context->sourceSpan.size) + " bytes", {}});
                result.properties.push_back({"Source region",
                    sourceRegionName(context->region), {}, {}});
                if (context->containingSection) {
                    result.properties.push_back({"Containing section",
                        std::to_string(context->containingSection->value()), {}, {},
                        SctInspectionLocation{SctNavigationTarget{SctNavigationKind::Section,
                            context->containingSection->value()}}});
                }
                if (const auto* receipt = snapshot.provenance->importReceipt()) {
                    const auto records = receipt->sourceMap.recordsFor(
                        spice::sct::SctDocumentEntityId{attachment->id});
                    const auto record = std::ranges::find_if(records, [&](const auto& candidate) {
                        return candidate.span == context->sourceSpan;
                    });
                    if (record != records.end() && record->sectionRelativeOffset) {
                        result.properties.push_back({"Section-relative offset",
                            hexValue(*record->sectionRelativeOffset), {}, {}});
                    }
                }
                const auto addEntity = [&](std::string name,
                        const std::optional<spice::sct::SctDocumentEntityId>& entity) {
                    if (!entity) return;
                    auto imported = spice::sct::SctImportedSourceTarget{*entity};
                    std::string addressability;
                    bool navigable = true;
                    if (snapshot.analysis->importedSites) {
                        if (const auto* status = snapshot.analysis->importedSites->find(imported)) {
                            addressability = addressabilityName(status->addressability);
                            navigable = status->addressability
                                != spice::sct::SctImportedSiteAddressability::MissingEntity;
                        }
                    }
                    result.properties.push_back({std::move(name), "Semantic entity",
                        std::move(addressability), {}, navigable
                            ? inspectionForImportedTarget(imported) : std::nullopt});
                };
                addEntity("Previous semantic entity", context->previousSemanticEntity);
                addEntity("Next semantic entity", context->nextSemanticEntity);
                const auto addNeighborhood = [&](std::string name,
                        const std::optional<spice::sct::SctSourceRecordSummary>& record) {
                    if (!record) return;
                    std::optional<SctInspectionLocation> location;
                    std::string addressability;
                    if (record->target) {
                        location = inspectionForImportedTarget(*record->target);
                        if (snapshot.analysis->importedSites) {
                            if (const auto* status = snapshot.analysis->importedSites->find(*record->target)) {
                                addressability = addressabilityName(status->addressability);
                                if (status->addressability
                                    == spice::sct::SctImportedSiteAddressability::MissingEntity)
                                    location.reset();
                            }
                        }
                    }
                    result.properties.push_back({std::move(name), sourceRoleName(record->role),
                        hexValue(record->span.offset) + " + " + std::to_string(record->span.size)
                            + " bytes; " + addressability, {}, std::move(location)});
                };
                addNeighborhood("Preceding targeted source leaf",
                    context->sourceNeighborhood.precedingTargetedLeaf);
                addNeighborhood("Following targeted source leaf",
                    context->sourceNeighborhood.followingTargetedLeaf);
                SctPropertyItem envelopes{"Containing source envelopes",
                    std::to_string(context->sourceNeighborhood.containingTargetedEnvelopes.size()), {}, {}};
                for (const auto& envelope : context->sourceNeighborhood.containingTargetedEnvelopes) {
                    std::optional<SctInspectionLocation> location;
                    std::string addressability;
                    if (envelope.target) {
                        location = inspectionForImportedTarget(*envelope.target);
                        if (snapshot.analysis->importedSites) {
                            if (const auto* status = snapshot.analysis->importedSites->find(*envelope.target)) {
                                addressability = addressabilityName(status->addressability);
                                if (status->addressability
                                    == spice::sct::SctImportedSiteAddressability::MissingEntity)
                                    location.reset();
                            }
                        }
                    }
                    envelopes.children.push_back({sourceRoleName(envelope.role),
                        hexValue(envelope.span.offset) + " + " + std::to_string(envelope.span.size),
                        std::move(addressability), {}, std::move(location)});
                }
                result.properties.push_back(std::move(envelopes));
                SctPropertyItem edges{"Crossing imported control-flow edges",
                    std::to_string(context->crossingImportedEdges.size()), {}, {}};
                for (const auto& edge : context->crossingImportedEdges) {
                    std::optional<SctInspectionLocation> sourceLocation;
                    std::string addressability = "Imported evidence; kind "
                        + std::to_string(static_cast<int>(edge.kind));
                    if (index.find(*snapshot.document, edge.sourceInstruction) != nullptr) {
                        sourceLocation = SctNavigationTarget{SctNavigationKind::Instruction,
                            edge.sourceInstruction.value()};
                    } else {
                        addressability += "; source instruction is no longer addressable";
                    }
                    edges.children.push_back({"From instruction "
                            + std::to_string(edge.sourceInstruction.value()),
                        edge.targetInstruction ? "to instruction "
                            + std::to_string(edge.targetInstruction->value()) : "unresolved target",
                        std::move(addressability), {}, std::move(sourceLocation)});
                }
                result.properties.push_back(std::move(edges));
                SctPropertyItem interpretations{"Evidence-qualified interpretations",
                    std::to_string(context->interpretations.size()), {}, {}};
                for (const auto& interpretation : context->interpretations) {
                    interpretations.children.push_back({
                        interpretation.kind == spice::sct::SctOpaqueInterpretationKind::ControlFlowGap
                            ? "Control-flow gap" : "Switch-dispatch gap",
                        "confidence " + std::to_string(static_cast<int>(interpretation.confidence)),
                        "Advisory interpretation derived from import evidence.", {}});
                }
                result.properties.push_back(std::move(interpretations));
            }
            return result;
        }
    }
    if (target.kind == SctNavigationKind::FooterGroup)
        return { "Footer", "Physical footer entries", {{ "Entries", std::to_string(snapshot.document->supplementaryText.size()), {}, {} }}, {} };
    if (target.kind == SctNavigationKind::OpaqueGroup)
        return { "Opaque attachments", "Preserved source data", {{ "Attachments", std::to_string(snapshot.document->opaqueAttachments.size()), {}, {} }}, {} };
    return { "Unavailable entity", "The selected entity is not present in this snapshot.", {}, {} };
}

}  // namespace salsa::core
