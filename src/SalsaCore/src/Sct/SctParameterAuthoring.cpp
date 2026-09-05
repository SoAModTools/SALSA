#include "SalsaCore/Sct/SctParameterAuthoring.h"
#include "SalsaCore/Sct/SctAuthoringCatalog.h"

#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include "SalsaCore/Sct/SctWorkingState.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include "SpiceSCT/SctOpcodeMetadata.h"
#include "SpiceSCT/SctOpcodeParameterFacts.h"
#include "SpiceSCT/SctScptEncoding.h"

#include <bit>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <ranges>
#include <sstream>
#include <type_traits>

namespace salsa::core {
namespace {

std::string hexWord(const std::uint32_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setfill('0')
        << std::setw(8) << value;
    return stream.str();
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1u);
}

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string preview(std::string text) {
    std::ranges::replace(text, '\n', ' ');
    constexpr std::size_t limit = 48u;
    if (text.size() > limit) text = text.substr(0, limit) + "...";
    return text;
}

std::string preview(const spice::sct::SctMessage& message) {
    if (message.headerUtf8 && !message.headerUtf8->empty())
        return preview(*message.headerUtf8);
    std::string text;
    for (const auto& element : message.body.elements) {
        if (const auto* chunk = std::get_if<spice::sct::SctTextChunk>(&element))
            text += chunk->utf8;
        if (text.size() > 48u) break;
    }
    return preview(text.empty() ? std::string{"message"} : std::move(text));
}

std::string confidenceName(const spice::sct::SctRuntimeFactConfidence value) {
    using enum spice::sct::SctRuntimeFactConfidence;
    switch (value) {
    case Low: return "low";
    case Medium: return "medium";
    case MediumHigh: return "medium-high";
    case High: return "high";
    default: return "unknown";
    }
}

std::optional<std::uint32_t> parseWord(const std::string& source) {
    auto text = trim(source);
    int base = 10;
    if (text.size() > 2u && text[0] == '0'
        && (text[1] == 'x' || text[1] == 'X')) {
        text.erase(0, 2);
        base = 16;
    }
    if (text.empty()) return std::nullopt;
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size()
        || value > (std::numeric_limits<std::uint32_t>::max)()) return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

std::string factNotes(const std::uint16_t opcode, const std::uint32_t schemaIndex) {
    const auto facts = spice::sct::SctOpcodeParameterFacts::query(opcode,
        schemaIndex, spice::sct::SctAllRegisteredRuntimeProfiles{});
    if (facts.status != spice::sct::SctRuntimeFactQueryStatus::Available) return {};
    std::string result = "Advisory runtime research: ";
    if (facts.agreement == spice::sct::SctRuntimeFactAgreement::Divergent)
        result += "profiles diverge";
    else if (facts.agreement == spice::sct::SctRuntimeFactAgreement::Uniform)
        result += "profiles agree";
    else
        result += "single-profile evidence";
    if (facts.coverage == spice::sct::SctRuntimeFactCoverage::Partial)
        result += "; incomplete profile coverage";
    spice::sct::SctRuntimeFactConfidence structural =
        spice::sct::SctRuntimeFactConfidence::High;
    spice::sct::SctRuntimeFactConfidence semantic =
        spice::sct::SctRuntimeFactConfidence::High;
    bool hasConfidence = false;
    for (const auto& record : facts.records) {
        for (const auto& behavior : record.behaviors) {
            for (const auto& evidence : behavior.evidence) {
                structural = std::min(structural, evidence.confidence.structural);
                semantic = std::min(semantic, evidence.confidence.semantic);
                hasConfidence = true;
            }
        }
    }
    if (hasConfidence) result += "; confidence structural "
        + confidenceName(structural) + ", semantic " + confidenceName(semantic);
    result += ".";
    return result;
}

std::string words(const std::vector<std::uint32_t>& values,
    const std::optional<std::uint32_t> managedTerminator = std::nullopt) {
    std::string result;
    auto size = values.size();
    if (managedTerminator && size != 0u && values.back() == *managedTerminator) --size;
    for (std::size_t i = 0; i < size; ++i) {
        if (!result.empty()) result += ' ';
        result += hexWord(values[i]);
    }
    return result;
}

const spice::sct::SctOpcodeParameterSchema* schemaFor(
    const spice::sct::SctDocumentInstruction& instruction,
    const std::uint32_t schemaIndex) {
    const auto* schema = spice::sct::findSctOpcodeSchema(instruction.opcode);
    return schema == nullptr ? nullptr
        : spice::sct::sctOpcodeParameterSchema(*schema, schemaIndex);
}

SctParameterRowPresentation projectParameter(const SctWorkingState& state,
    const spice::sct::SctDocumentInstruction& instruction,
    const spice::sct::SctDocumentParameter& parameter,
    const std::optional<std::uint32_t> group) {
    SctParameterRowPresentation result;
    result.site = {instruction.id, {parameter.schemaIndex, group}};
    const auto* schema = schemaFor(instruction, parameter.schemaIndex);
    const auto catalog = SctCatalogResolver::resolve(instruction.opcode);
    const auto* opcodeSchema = spice::sct::findSctOpcodeSchema(instruction.opcode);
    std::optional<std::size_t> position;
    if (opcodeSchema != nullptr) {
        const std::span parameters{opcodeSchema->parameterCatalog.data(),
            static_cast<std::size_t>(opcodeSchema->parameterCatalogCount)};
        const auto found = std::ranges::find(parameters, parameter.schemaIndex,
            &spice::sct::SctOpcodeParameterSchema::schemaIndex);
        if (found != parameters.end())
            position = static_cast<std::size_t>(found - parameters.begin());
    }
    result.parameter = position && *position < catalog.parameterLabels.size()
        && !catalog.parameterLabels[*position].empty()
        ? catalog.parameterLabels[*position]
        : schema != nullptr && !schema->role.empty()
            ? std::string(schema->role)
            : "Parameter " + std::to_string(parameter.schemaIndex);
    result.notes = factNotes(instruction.opcode, parameter.schemaIndex);
    const auto appendNote = [&](std::string note) {
        if (!result.notes.empty()) result.notes += ' ';
        result.notes += std::move(note);
    };
    std::visit([&](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctEncodedWordValue>) {
            result.value = schema != nullptr
                    && schema->scalarType == spice::sct::SctOpcodeScalarType::UnsignedInteger
                ? std::to_string(typed.value)
                : schema != nullptr
                    && schema->scalarType == spice::sct::SctOpcodeScalarType::SignedInteger
                ? std::to_string(static_cast<std::int32_t>(typed.value)) : hexWord(typed.value);
            result.editor = schema != nullptr
                    && schema->scalarType == spice::sct::SctOpcodeScalarType::UnsignedInteger
                ? SctInlineParameterEditorKind::EncodedUnsigned
                : schema != nullptr
                    && schema->scalarType == spice::sct::SctOpcodeScalarType::SignedInteger
                ? SctInlineParameterEditorKind::EncodedSigned
                : SctInlineParameterEditorKind::EncodedHex;
            appendNote("Raw " + hexWord(typed.value) + ".");
            if (schema != nullptr
                && schema->defaultKind == spice::sct::SctOpcodeDefaultKind::DerivedRepeatedGroupCount) {
                result.editor = SctInlineParameterEditorKind::ReadOnly;
                appendNote("Derived from repeated-group count.");
            }
        } else if constexpr (std::is_same_v<T, spice::sct::SctCanonicalExpression>) {
            const auto projection = SctExpressionLanguage::project(typed);
            const auto* program = std::get_if<spice::sct::SctTypedScptProgram>(&typed.body);
            if (projection.availability == SctExpressionTextAvailability::Editable) {
                result.value = projection.text;
                result.editor = SctInlineParameterEditorKind::ConventionalScpt;
                if (!projection.explanation.empty()) appendNote(projection.explanation);
            } else {
                result.value = "Advanced SCPT program — "
                    + std::to_string(program == nullptr ? 0u : program->operations.size())
                    + " operations";
                result.editor = SctInlineParameterEditorKind::AdvancedScpt;
                if (!projection.explanation.empty()) appendNote(projection.explanation);
            }
            appendNote("Ordered SCPT words: "
                + words(spice::sct::encodeSctCanonicalExpressionWords(typed)) + ".");
            appendNote(typed.termination == spice::sct::SctExpressionTermination::StopCode
                ? "Termination: stop code."
                : "Termination: inline value.");
            if (program != nullptr) {
                const auto analysis = spice::sct::analyzeSctScptProgram(*program);
                const auto* tree = analysis.conventionalTree
                    ? &*analysis.conventionalTree
                    : analysis.returnedExpression ? &*analysis.returnedExpression : nullptr;
                if (tree != nullptr) {
                    using enum spice::sct::SctScptComparisonMode;
                    switch (tree->comparisonMode) {
                    case Floating: appendNote("Comparison mode: floating."); break;
                    case Low16Integer: appendNote("Comparison mode: low-16 integer."); break;
                    case Unknown: appendNote("Comparison mode: unknown."); break;
                    case NotApplicable: break;
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, spice::sct::SctTerminatedWordSequenceValue>) {
            const auto terminator = schema == nullptr || !schema->terminator
                ? std::optional<std::uint32_t>{} : std::optional{schema->terminator->encodedWord};
            result.value = words(typed.words, terminator);
            result.editor = SctInlineParameterEditorKind::TerminatedWords;
            appendNote(terminator ? "Terminator is managed automatically." : "Exact encoded words.");
        } else if constexpr (std::is_same_v<T, spice::sct::SctInstructionReference>) {
            result.value = "Instruction " + std::to_string(typed.target.value());
            result.editor = SctInlineParameterEditorKind::Reference;
            result.navigation = SctNavigationTarget{SctNavigationKind::Instruction, typed.target.value()};
        } else if constexpr (std::is_same_v<T, spice::sct::SctStringReference>) {
            const auto* text = state.textValue(SctTextTarget{typed.target});
            const auto section = state.stringSectionName(typed.target);
            result.value = (section ? std::string(*section) : "Indexed string")
                + " [" + std::to_string(typed.target.value()) + "]";
            if (text != nullptr) {
                if (const auto* plain = std::get_if<spice::sct::SctPlainText>(text))
                    result.value += " — " + preview(plain->utf8);
                else if (const auto* message = std::get_if<spice::sct::SctMessage>(text))
                    result.value += " — " + preview(*message);
            }
            result.editor = SctInlineParameterEditorKind::Reference;
            result.navigation = SctNavigationTarget{SctNavigationKind::String, typed.target.value()};
        } else if constexpr (std::is_same_v<T, spice::sct::SctFooterEntryReference>) {
            result.value = "Footer entry " + std::to_string(typed.target.value());
            result.editor = SctInlineParameterEditorKind::Reference;
            result.navigation = SctNavigationTarget{SctNavigationKind::FooterEntry, typed.target.value()};
            const auto* entry = state.footerEntry(typed.target);
            const auto* plain = entry == nullptr ? nullptr
                : std::get_if<spice::sct::SctPlainText>(&entry->value);
            if (entry != nullptr && entry->kind == spice::sct::SctTextKind::PlainString
                && plain != nullptr && plain->utf8.find('\n') == std::string::npos) {
                result.value = plain->utf8;
                result.editor = SctInlineParameterEditorKind::PlainFooterText;
                result.inlineFooterText = typed.target;
                appendNote(state.referenceOccurrenceCount(
                    spice::sct::SctDocumentReferenceTarget{typed.target}) > 1u
                    ? "Shared footer text; editing creates a private copy."
                    : "Plain footer text.");
            } else if (entry != nullptr) {
                if (const auto* message = std::get_if<spice::sct::SctMessage>(&entry->value))
                    result.value += " — " + preview(*message);
            }
        } else if constexpr (std::is_same_v<T, spice::sct::SctUnresolvedReferenceValue>) {
            result.value = words(typed.encodedWords);
            result.editor = SctInlineParameterEditorKind::Reference;
            appendNote("Unresolved reference; choose Repair Reference.");
        } else {
            result.value = words(typed.words);
            result.editor = SctInlineParameterEditorKind::ReadOnly;
            if (schema != nullptr) {
                if (schema->referenceKind != spice::sct::SctOpcodeReferenceKind::None)
                    result.replacementEditor = SctInlineParameterEditorKind::Reference;
                else if (schema->encoding == spice::sct::SctOpcodeParameterEncoding::ScptExpression)
                    result.replacementEditor = SctInlineParameterEditorKind::AdvancedScpt;
                else if (schema->encoding
                    == spice::sct::SctOpcodeParameterEncoding::RawWordsUntilSentinel)
                    result.replacementEditor = SctInlineParameterEditorKind::TerminatedWords;
                else result.replacementEditor = SctInlineParameterEditorKind::EncodedHex;
            }
            appendNote("Opaque words are read-only; replace the complete value with a typed value.");
        }
    }, parameter.value);
    return result;
}

} // namespace

SctParameterTablePresentation SctParameterAuthoringService::project(
    const SctWorkingState& state, const spice::sct::SctInstructionId id) {
    SctParameterTablePresentation result;
    result.instruction = id;
    const auto* instruction = state.instruction(id);
    if (instruction == nullptr) return result;
    const auto* schema = spice::sct::findSctOpcodeSchema(instruction->opcode);
    const auto repeated = schema == nullptr
        ? std::nullopt : spice::sct::sctOpcodeRepeatedGroup(*schema);
    result.supportsRepeatedGroups = repeated.has_value();
    result.minimumRepeatedGroups = repeated && repeated->firstParameter < schema->parameters.paramCount
        ? 1u : 0u;
    result.repeatedGroupsManagedBySemanticEditor = schema != nullptr
        && schema->semantic.controlRole == spice::sct::SctOpcodeControlRole::Switch;
    for (const auto& parameter : instruction->fixedParameters)
        result.fixedParameters.push_back(projectParameter(state, *instruction, parameter, std::nullopt));
    for (std::uint32_t ordinal = 0;
        ordinal < instruction->repeatedParameterGroups.size(); ++ordinal) {
        SctRepeatedGroupPresentation group{ordinal, {}};
        for (const auto& parameter : instruction->repeatedParameterGroups[ordinal].parameters)
            group.parameters.push_back(projectParameter(state, *instruction, parameter, ordinal));
        result.repeatedGroups.push_back(std::move(group));
    }
    return result;
}

SctParameterParseResult SctParameterAuthoringService::parseInline(
    const spice::sct::SctDocumentInstruction& instruction,
    const spice::sct::SctParameterSite& site, std::string text) {
    SctParameterParseResult result;
    const auto* schema = schemaFor(instruction, site.parameter.schemaIndex);
    const auto findCurrent = [&]() -> const spice::sct::SctDocumentParameter* {
        if (!site.parameter.repeatedGroupOrdinal) {
            const auto found = std::ranges::find(instruction.fixedParameters,
                site.parameter.schemaIndex, &spice::sct::SctDocumentParameter::schemaIndex);
            return found == instruction.fixedParameters.end() ? nullptr : &*found;
        }
        if (*site.parameter.repeatedGroupOrdinal >= instruction.repeatedParameterGroups.size())
            return nullptr;
        const auto& parameters = instruction.repeatedParameterGroups[
            *site.parameter.repeatedGroupOrdinal].parameters;
        const auto found = std::ranges::find(parameters, site.parameter.schemaIndex,
            &spice::sct::SctDocumentParameter::schemaIndex);
        return found == parameters.end() ? nullptr : &*found;
    };
    const auto* current = findCurrent();
    if (schema == nullptr || current == nullptr) {
        result.error = "The parameter address is stale or outside the opcode schema.";
        return result;
    }
    if (const auto* word = std::get_if<spice::sct::SctEncodedWordValue>(&current->value)) {
        std::optional<std::uint32_t> parsed;
        if (schema->scalarType == spice::sct::SctOpcodeScalarType::SignedInteger) {
            const auto cleaned = trim(text);
            std::int64_t signedValue = 0;
            const auto [end, error] = std::from_chars(cleaned.data(),
                cleaned.data() + cleaned.size(), signedValue);
            if (error == std::errc{} && end == cleaned.data() + cleaned.size()
                && signedValue >= (std::numeric_limits<std::int32_t>::min)()
                && signedValue <= (std::numeric_limits<std::int32_t>::max)())
                parsed = static_cast<std::uint32_t>(static_cast<std::int32_t>(signedValue));
        } else parsed = parseWord(text);
        if (!parsed) { result.error = "Enter a valid 32-bit integer or hexadecimal word."; return result; }
        if (schema->bitContractConfidence == spice::sct::SctOpcodeContractConfidence::Confirmed
            && ((*parsed & ~schema->allowedBitMask) != 0u
                || (*parsed & schema->requiredBitValue) != schema->requiredBitValue)) {
            result.error = "The value violates the confirmed required-bit or allowed-bit mask.";
            return result;
        }
        if (schema->bitContractConfidence == spice::sct::SctOpcodeContractConfidence::Provisional
            && ((*parsed & ~schema->allowedBitMask) != 0u
                || (*parsed & schema->requiredBitValue) != schema->requiredBitValue))
            result.warnings.push_back("The value falls outside a provisional bit constraint.");
        result.value = spice::sct::SctEncodedWordValue{*parsed};
        return result;
    }
    if (const auto* expression = std::get_if<spice::sct::SctCanonicalExpression>(&current->value)) {
        if (SctExpressionLanguage::project(*expression).availability
            != SctExpressionTextAvailability::Editable) {
            result.error = "Open the Advanced SCPT Editor for this expression shape.";
            return result;
        }
        auto parsed = SctExpressionLanguage::parse(trim(text));
        if (!parsed.succeeded()) {
            result.error = parsed.issues.empty()
                ? "Enter a valid conventional SCPT expression."
                : parsed.issues.front().message;
            return result;
        }
        result.value = std::move(*parsed.expression);
        return result;
    }
    if (const auto* sequence = std::get_if<spice::sct::SctTerminatedWordSequenceValue>(&current->value)) {
        std::vector<std::uint32_t> parsed;
        std::istringstream stream(text);
        std::string token;
        while (stream >> token) {
            const auto word = parseWord(token);
            if (!word) { result.error = "Enter a whitespace-separated list of 32-bit words."; return result; }
            parsed.push_back(*word);
        }
        if (schema->terminator) parsed.push_back(schema->terminator->encodedWord);
        else if (!sequence->words.empty()) parsed.push_back(sequence->words.back());
        result.value = spice::sct::SctTerminatedWordSequenceValue{std::move(parsed)};
        return result;
    }
    if (std::holds_alternative<spice::sct::SctOpaqueParameterValue>(current->value)) {
        if (schema->referenceKind != spice::sct::SctOpcodeReferenceKind::None) {
            result.error = "Choose Replace With Typed Value and select a compatible reference target.";
            return result;
        }
        if (schema->encoding == spice::sct::SctOpcodeParameterEncoding::ScptExpression) {
            result.error = "Choose Replace With Typed Value to construct a typed SCPT program.";
            return result;
        }
        if (schema->encoding
            == spice::sct::SctOpcodeParameterEncoding::RawWordsUntilSentinel) {
            std::vector<std::uint32_t> parsed;
            std::istringstream stream(text);
            std::string token;
            while (stream >> token) {
                const auto word = parseWord(token);
                if (!word) {
                    result.error = "Enter a whitespace-separated list of 32-bit words.";
                    return result;
                }
                parsed.push_back(*word);
            }
            if (schema->terminator) parsed.push_back(schema->terminator->encodedWord);
            result.value = spice::sct::SctTerminatedWordSequenceValue{std::move(parsed)};
            return result;
        }
        const auto parsed = parseWord(text);
        if (!parsed) {
            result.error = "Enter a valid 32-bit integer or hexadecimal word.";
            return result;
        }
        result.value = spice::sct::SctEncodedWordValue{*parsed};
        return result;
    }
    result.error = "This value is edited through its typed reference or advanced authoring action.";
    return result;
}

SctParameterParseResult SctParameterAuthoringService::parseDraftValue(
    const std::uint16_t opcode, const spice::sct::SctParameterAddress& address,
    const spice::sct::SctDocumentParameterValue& current, std::string text) {
    spice::sct::SctDocumentInstruction instruction;
    instruction.id = spice::sct::SctInstructionId(1u);
    instruction.opcode = opcode;
    if (address.repeatedGroupOrdinal) {
        instruction.repeatedParameterGroups.resize(
            static_cast<std::size_t>(*address.repeatedGroupOrdinal) + 1u);
        instruction.repeatedParameterGroups[*address.repeatedGroupOrdinal]
            .parameters.push_back({address.schemaIndex, current});
    } else {
        instruction.fixedParameters.push_back({address.schemaIndex, current});
    }
    return parseInline(instruction,
        spice::sct::SctParameterSite{instruction.id, address}, std::move(text));
}

bool SctParameterAuthoringService::equivalent(
    const spice::sct::SctDocumentParameterValue& left,
    const spice::sct::SctDocumentParameterValue& right) {
    if (left.index() != right.index()) return false;
    return std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        const auto& other = std::get<T>(right);
        if constexpr (std::is_same_v<T, spice::sct::SctEncodedWordValue>)
            return value.value == other.value;
        else if constexpr (std::is_same_v<T, spice::sct::SctCanonicalExpression>)
            return value.termination == other.termination
                && spice::sct::encodeSctCanonicalExpressionWords(value)
                    == spice::sct::encodeSctCanonicalExpressionWords(other);
        else if constexpr (std::is_same_v<T, spice::sct::SctTerminatedWordSequenceValue>)
            return value.words == other.words;
        else if constexpr (std::is_same_v<T, spice::sct::SctInstructionReference>
            || std::is_same_v<T, spice::sct::SctStringReference>
            || std::is_same_v<T, spice::sct::SctFooterEntryReference>)
            return value.target == other.target;
        else if constexpr (std::is_same_v<T, spice::sct::SctUnresolvedReferenceValue>)
            return value.expectedTarget == other.expectedTarget
                && value.encodedWords == other.encodedWords;
        else return value.words == other.words;
    }, left);
}

std::vector<SctReferenceCandidate>
SctParameterAuthoringService::referenceCandidates(
    const SctWorkingState& state, const spice::sct::SctParameterSite& site) {
    const auto* instruction = state.instruction(site.instruction);
    return instruction == nullptr ? std::vector<SctReferenceCandidate>{}
        : referenceCandidates(state, instruction->opcode, site.parameter);
}

std::vector<SctReferenceCandidate>
SctParameterAuthoringService::referenceCandidates(
    const SctWorkingState& state, const std::uint16_t opcodeValue,
    const spice::sct::SctParameterAddress& address) {
    std::vector<SctReferenceCandidate> result;
    const auto* opcode = spice::sct::findSctOpcodeSchema(opcodeValue);
    const auto* schema = opcode == nullptr ? nullptr
        : spice::sct::sctOpcodeParameterSchema(*opcode, address.schemaIndex);
    if (schema == nullptr) return result;
    if (schema->referenceKind == spice::sct::SctOpcodeReferenceKind::Instruction) {
        for (const auto section : state.sectionOrder()) {
            for (const auto id : state.instructionOrder(section))
                result.push_back({id, "Instruction " + std::to_string(id.value())});
        }
    } else if (schema->referenceKind == spice::sct::SctOpcodeReferenceKind::Text
        && schema->textReference) {
        if (schema->textReference->storage == spice::sct::SctTextStorage::IndexedSection) {
            for (const auto sectionId : state.sectionOrder()) {
                const auto* section = state.section(sectionId);
                const auto* content = section == nullptr ? nullptr
                    : std::get_if<spice::sct::SctStringSectionContent>(&section->content);
                if (content != nullptr && content->string.kind == schema->textReference->kind)
                    result.push_back({content->string.id, section->nameBytes + " — indexed string "
                        + std::to_string(content->string.id.value())});
            }
        } else {
            for (const auto id : state.footerEntryOrder()) {
                const auto* entry = state.footerEntry(id);
                if (entry != nullptr && entry->kind == schema->textReference->kind)
                    result.push_back({id, "Footer entry " + std::to_string(id.value())});
            }
        }
    }
    return result;
}

} // namespace salsa::core
