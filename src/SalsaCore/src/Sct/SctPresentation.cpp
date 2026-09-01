#include "SalsaCore/Sct/SctPresentation.h"

#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctOpcodeMetadata.h"
#include "SpiceSCT/SctTextCodec.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <type_traits>

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

[[nodiscard]] std::string expressionKindName(
    const spice::sct::SctCanonicalExpressionNodeKind kind) {
    using enum spice::sct::SctCanonicalExpressionNodeKind;
    switch (kind) {
    case NoLoopValue: return "No-loop value";
    case RawValue: return "Raw value";
    case FloatLiteral: return "Float literal";
    case DecimalLiteral: return "Decimal literal";
    case IntVariable: return "Integer variable";
    case FloatVariable: return "Float variable";
    case BitVariable: return "Bit variable";
    case ByteVariable: return "Byte variable";
    case SecondaryValue: return "Secondary value";
    case CompareOperator: return "Comparison";
    case ArithmeticOperator: return "Arithmetic";
    case AssignmentOperator: return "Assignment";
    case Stop: return "Stop";
    }
    return "Expression node";
}

[[nodiscard]] SctPropertyItem expressionNodeProperty(
    const spice::sct::SctCanonicalExpressionNode& node) {
    SctPropertyItem item{ expressionKindName(node.kind), hexValue(node.encodingCode),
        hexList(node.payloadWords), {} };
    for (const auto& child : node.children) item.children.push_back(expressionNodeProperty(child));
    return item;
}

[[nodiscard]] SctPropertyItem expressionProperty(
    const spice::sct::SctCanonicalExpression& expression,
    std::string name = "Expression") {
    SctPropertyItem item{ std::move(name),
        expression.termination == spice::sct::SctExpressionTermination::StopCode
            ? "stop-terminated" : "inline", {}, {} };
    std::visit([&item](const auto& root) {
        using T = std::decay_t<decltype(root)>;
        if constexpr (std::is_same_v<T, spice::sct::SctOpaqueExpression>) {
            item.children.push_back({ "Opaque words", hexList(root.words),
                "Typed structure was not reliable.", {} });
        } else {
            item.children.push_back(expressionNodeProperty(root));
        }
    }, expression.root);
    return item;
}

[[nodiscard]] std::string targetStorageName(
    const spice::sct::SctReferenceTargetStorage storage) {
    switch (storage) {
    case spice::sct::SctReferenceTargetStorage::Instruction: return "instruction";
    case spice::sct::SctReferenceTargetStorage::IndexedString: return "indexed string";
    case spice::sct::SctReferenceTargetStorage::FooterEntry: return "footer entry";
    }
    return "unknown";
}

[[nodiscard]] SctPropertyItem parameterProperty(
    const spice::sct::SctDocumentParameter& parameter) {
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
            children = expressionProperty(typed).children;
            value = typed.termination == spice::sct::SctExpressionTermination::StopCode
                ? "stop-terminated" : "inline";
        } else if constexpr (std::is_same_v<T, spice::sct::SctTerminatedWordSequenceValue>) {
            role = "Terminated words"; value = hexList(typed.words);
        } else if constexpr (std::is_same_v<T, spice::sct::SctInstructionReference>) {
            role = "Instruction reference"; value = "Instruction " + std::to_string(typed.target.value());
        } else if constexpr (std::is_same_v<T, spice::sct::SctStringReference>) {
            role = "String reference"; value = "String " + std::to_string(typed.target.value());
        } else if constexpr (std::is_same_v<T, spice::sct::SctFooterEntryReference>) {
            role = "Footer reference"; value = "Footer " + std::to_string(typed.target.value());
        } else if constexpr (std::is_same_v<T, spice::sct::SctUnresolvedReferenceValue>) {
            role = "Unresolved reference"; value = hexList(typed.encodedWords);
            notes = "Expected " + targetStorageName(typed.expectedTarget.storage);
        } else {
            role = "Opaque parameter"; value = hexList(typed.words);
            notes = "Retained without a claimed semantic interpretation.";
        }
    }, parameter.value);
    return { "Parameter " + std::to_string(parameter.schemaIndex), value,
        role + (notes.empty() ? "" : "; " + notes), std::move(children) };
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
        else if constexpr (std::is_same_v<T, spice::sct::SctLabelSectionContent>) return "Label";
        else return "Opaque";
    }, content);
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
    result.push_back({ "Document", snapshot.source.descriptor.locator.path().generic_string(),
        { SctNavigationKind::Document, 0 }, {} });
    for (std::size_t sectionIndex = 0; sectionIndex < snapshot.document->sections.size(); ++sectionIndex) {
        const auto& section = snapshot.document->sections[sectionIndex];
        SctOutlineItem item{
            "[" + std::to_string(sectionIndex) + "] " + escapedBytes(section.nameBytes),
            sectionKind(section.content), { SctNavigationKind::Section, section.id.value() }, {} };
        if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&section.content)) {
            for (const auto& instruction : script->instructions) {
                const auto* schema = spice::sct::findSctOpcodeSchema(instruction.opcode);
                const auto mnemonic = schema != nullptr && !schema->semantic.mnemonic.empty()
                    ? std::string(schema->semantic.mnemonic) : "Opcode";
                item.children.push_back({ mnemonic + " (" + std::to_string(instruction.opcode) + ")",
                    "Instruction " + std::to_string(instruction.id.value()),
                    { SctNavigationKind::Instruction, instruction.id.value() }, {} });
            }
        } else if (const auto* strings = std::get_if<spice::sct::SctStringSectionContent>(&section.content)) {
            item.children.push_back({ "Indexed string", textKindName(strings->string.kind),
                { SctNavigationKind::String, strings->string.id.value() }, {} });
        }
        result.push_back(std::move(item));
    }
    SctOutlineItem footer{ "Footer", std::to_string(snapshot.document->footerEntries.size()) + " entries",
        { SctNavigationKind::FooterGroup, 0 }, {} };
    for (std::size_t i = 0; i < snapshot.document->footerEntries.size(); ++i) {
        const auto& entry = snapshot.document->footerEntries[i];
        footer.children.push_back({ "[" + std::to_string(i) + "] " + textKindName(entry.kind), {},
            { SctNavigationKind::FooterEntry, entry.id.value() }, {} });
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

SctEntityPresentation SctPresentationService::describe(
    const SctDocumentSnapshot& snapshot,
    const SctNavigationTarget target) {
    const auto index = spice::sct::SctDocumentIndex::build(*snapshot.document);
    if (target.kind == SctNavigationKind::Document) {
        return { "SCT document", snapshot.source.descriptor.locator.path().generic_string(), {
            { "Source size", std::to_string(snapshot.source.descriptor.byteSize) + " bytes", {}, {} },
            { "Source revision", snapshot.source.descriptor.revision.digest.toHex(), {}, {} },
            { "Source byte order", std::to_string(static_cast<int>(snapshot.importReceipt.source.byteOrder)), {}, {} },
            { "Source wrapper", std::to_string(static_cast<int>(snapshot.importReceipt.source.wrapper)), {}, {} },
            { "Readiness", readinessName(snapshot.readiness), {}, {} },
            { "Text convention", snapshot.textConvention.has_value()
                ? std::string(sctTextConventionName(*snapshot.textConvention)) : "Unselected / opaque", {}, {} },
            { "Sections", std::to_string(snapshot.document->sections.size()), {}, {} },
            { "Footer entries", std::to_string(snapshot.document->footerEntries.size()), {}, {} },
            { "Opaque attachments", std::to_string(snapshot.document->opaqueAttachments.size()), {}, {} },
        }, {} };
    }
    if (target.kind == SctNavigationKind::Section) {
        if (const auto* section = index.find(spice::sct::SctSectionId(target.id))) {
            SctEntityPresentation result{ "Section " + escapedBytes(section->nameBytes), sectionKind(section->content), {
                { "Entity ID", std::to_string(section->id.value()), {}, {} },
                { "Name bytes", escapedBytes(section->nameBytes), {}, {} },
                { "Content", sectionKind(section->content), {}, {} },
            }, {} };
            if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&section->content))
                result.properties.push_back({ "Instructions", std::to_string(script->instructions.size()), {}, {} });
            if (const auto* string = std::get_if<spice::sct::SctStringSectionContent>(&section->content))
                result.properties.push_back({ "Preamble words", hexList(string->preambleWords), {}, {} });
            return result;
        }
    }
    if (target.kind == SctNavigationKind::Instruction) {
        if (const auto* instruction = index.find(spice::sct::SctInstructionId(target.id))) {
            const auto* schema = spice::sct::findSctOpcodeSchema(instruction->opcode);
            const auto mnemonic = schema != nullptr && !schema->semantic.mnemonic.empty()
                ? std::string(schema->semantic.mnemonic) : "Unknown opcode";
            SctEntityPresentation result{ mnemonic, "Opcode " + std::to_string(instruction->opcode), {
                { "Entity ID", std::to_string(instruction->id.value()), {}, {} },
                { "Opcode", std::to_string(instruction->opcode) + " (" + hexValue(instruction->opcode, 4) + ")", mnemonic, {} },
                { "Skip refresh", instruction->skipRefresh ? "Yes" : "No", {}, {} },
            }, {} };
            if (schema != nullptr && !schema->semantic.notes.empty())
                result.properties.push_back({ "Schema notes", std::string(schema->semantic.notes), {}, {} });
            if (instruction->scheduledExpression.has_value())
                result.properties.push_back(expressionProperty(*instruction->scheduledExpression, "Scheduled expression"));
            SctPropertyItem fixed{ "Fixed parameters", std::to_string(instruction->fixedParameters.size()), {}, {} };
            for (const auto& parameter : instruction->fixedParameters)
                fixed.children.push_back(parameterProperty(parameter));
            result.properties.push_back(std::move(fixed));
            SctPropertyItem repeated{ "Repeated groups", std::to_string(instruction->repeatedParameterGroups.size()), {}, {} };
            for (std::size_t groupIndex = 0; groupIndex < instruction->repeatedParameterGroups.size(); ++groupIndex) {
                SctPropertyItem group{ "Group " + std::to_string(groupIndex), {}, {}, {} };
                for (const auto& parameter : instruction->repeatedParameterGroups[groupIndex].parameters)
                    group.children.push_back(parameterProperty(parameter));
                repeated.children.push_back(std::move(group));
            }
            result.properties.push_back(std::move(repeated));
            return result;
        }
    }
    if (target.kind == SctNavigationKind::String) {
        if (const auto* string = index.find(spice::sct::SctStringId(target.id)))
            return describeText("Indexed string " + std::to_string(string->id.value()),
                "Section-backed text", string->kind,
                spice::sct::SctTextStorage::IndexedSection, string->value);
    }
    if (target.kind == SctNavigationKind::FooterEntry) {
        if (const auto* entry = index.find(spice::sct::SctFooterEntryId(target.id)))
            return describeText("Footer entry " + std::to_string(entry->id.value()),
                "Footer text", entry->kind, spice::sct::SctTextStorage::Footer, entry->value);
    }
    if (target.kind == SctNavigationKind::OpaqueAttachment) {
        if (const auto* attachment = index.find(spice::sct::SctOpaqueAttachmentId(target.id))) {
            return { "Opaque attachment " + std::to_string(attachment->id.value()),
                std::to_string(attachment->bytes.size()) + " bytes", {
                { "Bytes", hexList(attachment->bytes), {}, {} },
                { "Alignment", std::to_string(attachment->alignment), {}, {} },
                { "Fixed offset", attachment->fixedOffset.has_value()
                    ? hexValue(*attachment->fixedOffset) : "(none)", {}, {} },
                { "Placement", std::to_string(static_cast<int>(attachment->placement)), {}, {} },
                { "Relocation", std::to_string(static_cast<int>(attachment->relocation)), {}, {} },
                { "Reason", std::to_string(static_cast<int>(attachment->reason)), {}, {} },
            }, {} };
        }
    }
    if (target.kind == SctNavigationKind::FooterGroup)
        return { "Footer", "Physical footer entries", {{ "Entries", std::to_string(snapshot.document->footerEntries.size()), {}, {} }}, {} };
    if (target.kind == SctNavigationKind::OpaqueGroup)
        return { "Opaque attachments", "Preserved source data", {{ "Attachments", std::to_string(snapshot.document->opaqueAttachments.size()), {}, {} }}, {} };
    return { "Unavailable entity", "The selected entity is not present in this snapshot.", {}, {} };
}

}  // namespace salsa::core
