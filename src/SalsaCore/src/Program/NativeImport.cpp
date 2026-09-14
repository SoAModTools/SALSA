#include "ProgramInternal.h"
#include "SalsaCore/Program/NativeImport.h"
#include "SpiceSCT/SctParser.h"
#include "SpiceSCT/SctDocumentImporter.h"
#include <set>
#include <unordered_map>

namespace salsa::core::program {
namespace sct = spice::sct;
namespace {
struct ExpressionScratch { std::vector<ValueId> words, operations; };
ValueId normalizeExpression(ProgramData& d, const sct::SctCanonicalExpression& expression, ExpressionScratch& scratch) {
    const auto* program = std::get_if<sct::SctTypedScptProgram>(&expression.body);
    if (!program) {
        ++d.stats.unavailableItems;
        return d.add(ValueKind::Unavailable, std::get<sct::SctOpaqueExpression>(expression.body).words.size(), 1);
    }
    auto& words = scratch.words; auto& operations = scratch.operations;
    operations.clear(); operations.reserve(program->operations.size());
    for (const auto& operation : program->operations) operations.push_back(std::visit([&](const auto& op) {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, sct::SctScptValueOperation>) {
            words.clear(); words.reserve(op.payloadWords.size());
            for (auto word : op.payloadWords) words.push_back(d.add(ValueKind::Word, word));
            return d.add(ValueKind::ValueOperation, op.encodingWord, static_cast<std::uint32_t>(op.kind), words);
        } else if constexpr (std::is_same_v<T, sct::SctScptBinaryOperation>)
            return d.add(ValueKind::BinaryOperation, op.encodingWord, static_cast<std::uint32_t>(op.kind));
        else if constexpr (std::is_same_v<T, sct::SctScptInertOperation>) return d.add(ValueKind::InertOperation, op.encodingWord);
        else return d.add(ValueKind::StackOperation, op.encodingWord);
    }, operation));
    return d.add(ValueKind::Expression, static_cast<std::uint64_t>(expression.termination), 0, operations);
}
} // namespace
ValueId normalizeExpression(ProgramData& d, const sct::SctCanonicalExpression& expression) {
    ExpressionScratch scratch;
    return normalizeExpression(d, expression, scratch);
}
class NativeNormalizer {
    ProgramData& d;
    ExpressionScratch expressionScratch;
    std::unordered_map<std::uint64_t, SctActionId> actions;
    std::unordered_map<std::uint64_t, SctTextResourceId> indexed, supplementary;
    ValueId words(ValueKind kind, std::span<const std::uint32_t> words, std::uint64_t data = 0, std::uint32_t detail = 0) {
        std::vector<ValueId> children; children.reserve(words.size());
        for (auto word : words) children.push_back(d.add(ValueKind::Word, word));
        return d.add(kind, data, detail, children);
    }
    ValueId parameter(const sct::SctDocumentParameter& parameter) {
        const auto value = std::visit([&](const auto& v) -> ValueId {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, sct::SctEncodedWordValue>) return d.add(ValueKind::Word, v.value);
            else if constexpr (std::is_same_v<T, sct::SctCanonicalExpression>) return normalizeExpression(d, v, expressionScratch);
            else if constexpr (std::is_same_v<T, sct::SctTerminatedWordSequenceValue>) return words(ValueKind::Words, v.words);
            else if constexpr (std::is_same_v<T, sct::SctInstructionReference>) return d.add(ValueKind::ActionReference, actions.at(v.target.value()).value);
            else if constexpr (std::is_same_v<T, sct::SctStringReference>) return d.add(ValueKind::IndexedTextReference, indexed.at(v.target.value()).value);
            else if constexpr (std::is_same_v<T, sct::SctSupplementaryTextReference>) return d.add(ValueKind::SupplementaryTextReference, supplementary.at(v.target.value()).value);
            else if constexpr (std::is_same_v<T, sct::SctUnresolvedReferenceValue>) return words(ValueKind::Unresolved, v.encodedWords,
                static_cast<std::uint64_t>(v.expectedTarget.storage), v.expectedTarget.textKind ? 1 + static_cast<std::uint32_t>(*v.expectedTarget.textKind) : 0);
            else { ++d.stats.unavailableItems; return d.add(ValueKind::Unavailable, v.words.size(), 2); }
        }, parameter.value);
        return d.add(ValueKind::Parameter, parameter.schemaIndex, 0, std::span(&value, 1));
    }
    ValueId text(const sct::SctTextValue& text) {
        return std::visit([&](const auto& value) -> ValueId {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, sct::SctPlainText>) return d.string(ValueKind::PlainText, value.utf8);
            else if constexpr (std::is_same_v<T, sct::SctEmptyIndexedText>) return d.add(ValueKind::EmptyIndexedText);
            else if constexpr (std::is_same_v<T, sct::SctOpaqueText>) {
                d.stats.excludedSourceBytes += value.bytes.size(); ++d.stats.unavailableItems;
                return d.add(ValueKind::Unavailable, value.bytes.size(), 3);
            } else {
                std::vector<ValueId> elements;
                if (value.headerUtf8) elements.push_back(d.string(ValueKind::PlainText, *value.headerUtf8));
                for (const auto& element : value.body.elements) elements.push_back(std::visit([&](const auto& v) -> ValueId {
                    using E = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<E, sct::SctTextChunk>) return d.string(ValueKind::TextChunk, v.utf8);
                    else {
                        std::vector<ValueId> arguments;
                        std::visit([&](const auto& argument) {
                            using A = std::decay_t<decltype(argument)>;
                            if constexpr (std::is_same_v<A, sct::SctDecimalCommandArgument>) {
                                if (argument.value) arguments.push_back(d.add(ValueKind::Word, *argument.value));
                            } else if constexpr (std::is_same_v<A, sct::SctByteListCommandArgument>) {
                                for (auto byte : argument.values) arguments.push_back(d.add(ValueKind::Word, byte));
                            }
                        }, v.argument);
                        return d.add(ValueKind::TextCommand, static_cast<std::uint64_t>(v.code), static_cast<std::uint32_t>(v.argument.index()), arguments);
                    }
                }, element));
                return d.add(ValueKind::Message, value.headerUtf8.has_value(), 0, elements);
            }
        }, text);
    }
public:
    explicit NativeNormalizer(ProgramData& data) : d(data) {}
    SctScriptId append(const sct::SctDocument& document, SctAuthoringBaseline baseline, SctScriptContext context) {
        const auto script = d.beginScript(std::move(baseline), std::move(context));
        auto& output = d.scripts.back();
        // Bind every target before copying operands. Maps die with this import.
        for (const auto& section : document.sections) {
            if (const auto* code = std::get_if<sct::SctScriptSectionContent>(&section.content)) {
                for (const auto& instruction : code->instructions) {
                    require(instruction.id.value() != 0 && actions.emplace(instruction.id.value(), SctActionId{d.allocate()}).second, "Duplicate/zero native instruction ID");
                }
            } else if (const auto* value = std::get_if<sct::SctStringSectionContent>(&section.content))
                require(value->string.id.value() != 0 && indexed.emplace(value->string.id.value(), SctTextResourceId{d.allocate()}).second, "Duplicate/zero native text ID");
        }
        for (const auto& value : document.supplementaryText)
            require(value.id.value() != 0 && supplementary.emplace(value.id.value(), SctTextResourceId{d.allocate()}).second, "Duplicate/zero native supplementary text ID");
        std::set<std::uint64_t> sourceSections;
        for (const auto& source : document.sections) {
            require(source.id.value() != 0 && sourceSections.insert(source.id.value()).second, "Duplicate/zero native section ID");
            Section section{SectionId{d.claim(0, EntityKind::Section, count32(d.sections.size()))}, script, source.nameBytes, {}, {}, source.id.value()};
            if (const auto* code = std::get_if<sct::SctScriptSectionContent>(&source.content)) {
                section.body = d.makeSequence(script);
                std::vector<ValueId> parameters, items;
                for (const auto& instruction : code->instructions) {
                    parameters.clear();
                    if (instruction.scheduledExpression) parameters.push_back(normalizeExpression(d, *instruction.scheduledExpression, expressionScratch));
                    for (const auto& p : instruction.fixedParameters) parameters.push_back(parameter(p));
                    for (const auto& group : instruction.repeatedParameterGroups) {
                        items.clear(); for (const auto& p : group.parameters) items.push_back(parameter(p));
                        parameters.push_back(d.add(ValueKind::RepeatedGroup, 0, 0, items));
                    }
                    auto payload = d.add(ValueKind::Instruction, instruction.opcode,
                        (instruction.skipRefresh ? 1u : 0u) | (instruction.scheduledExpression ? 2u : 0u), parameters);
                    d.appendAction(section.body, payload, instruction.id.value(), actions.at(instruction.id.value()));
                }
                if (section.name == "init" || section.name == "loop") {
                    Entrypoint entry; entry.id = SctEntrypointId{d.allocate()}; entry.script = script;
                    entry.name = section.name; entry.kind = SctEntrypointKind::Lifecycle; entry.body = section.body;
                    const auto& order = d.pendingOrders[d.directory.find(section.body.value).index];
                    if (!order.empty()) entry.activation = SctNamedActivation{section.name, order.front()};
                    d.metadata.entrypoints.push_back(std::move(entry));
                }
            } else if (const auto* value = std::get_if<sct::SctStringSectionContent>(&source.content)) {
                const auto id = indexed.at(value->string.id.value());
                d.claim(id.value, EntityKind::Text, count32(d.texts.size()));
                d.texts.push_back({id, script, text(value->string.value), value->string.kind, false, value->string.id.value(), source.nameBytes});
                const auto preamble = words(ValueKind::Words, value->preambleWords);
                section.content = d.add(ValueKind::IndexedSection, id.value, 0, std::span(&preamble, 1));
            } else if (const auto* marker = std::get_if<sct::SctStringGroupMarkerSectionContent>(&source.content))
                section.content = words(ValueKind::StringGroup, marker->preambleWords);
            else { ++d.stats.unavailableItems; section.content = d.add(ValueKind::Unavailable, 0, 4); }
            output.sections.push_back(section.id); d.sections.push_back(std::move(section));
        }
        for (const auto& source : document.supplementaryText) {
            const auto id = supplementary.at(source.id.value());
            d.claim(id.value, EntityKind::Text, count32(d.texts.size()));
            d.texts.push_back({id, script, text(source.value), source.kind, true, source.id.value()});
            output.supplementaryText.push_back(id);
        }
        for (const auto& attachment : document.opaqueAttachments) d.stats.excludedSourceBytes += attachment.bytes.size();
        return script;
    }
};
Result<SctScriptId> ProgramBuilder::appendImported(const sct::SctDocument& document, SctAuthoringBaseline baseline, SctScriptContext context) {
    try {
        require(data_ != nullptr, "Builder already finished");
        return Result<SctScriptId>::success(NativeNormalizer(*data_).append(document, std::move(baseline), std::move(context)));
    } catch (const std::exception& e) {
        diagnostics_.push_back(failure(std::string("Native normalization: ") + e.what()));
        return Result<SctScriptId>::failure(diagnostics_.back());
    }
}
Result<SctScriptId> appendNative(ProgramBuilder& builder, std::span<const std::uint8_t> bytes,
    SctAuthoringBaseline baseline, SctScriptContext context, const sct::SctDocumentImportOptions& options) {
    try {
        auto digest = sha256(std::as_bytes(bytes));
        if (!digest) return Result<SctScriptId>::failure(digest.diagnostics());
        require(bytes.size() == baseline.source.byteSize && digest.value() == baseline.source.revision.digest, "Native input differs from its imported baseline");
        auto parsed = sct::SctParser{}.parse(bytes, {}, sct::SctParseOptions{true});
        require(parsed.parseOk, "SPICE could not parse native SCT");
        auto selected = options;
        if (!selected.declaredSourcePlatform && baseline.recipe.platform)
            selected.declaredSourcePlatform = *baseline.recipe.platform == GamePlatform::GameCube ? sct::SctPlatform::GameCube : sct::SctPlatform::Dreamcast;
        if (!selected.sourceTextEncoding && baseline.textConvention) selected.sourceTextEncoding = sct::sctTextEncodingFor(*baseline.textConvention);
        if (baseline.recipe.trustSelectedTextEncoding) selected.footerTextPromotion = sct::SctFooterTextPromotionPolicy::TrustSelectedEncoding;
        // Persist the recipe actually used, including explicit import overrides.
        if (selected.declaredSourcePlatform) baseline.recipe.platform = *selected.declaredSourcePlatform == sct::SctPlatform::GameCube ? GamePlatform::GameCube : GamePlatform::Dreamcast;
        if (selected.sourceTextEncoding) baseline.textConvention = sct::findSctKnownTextConvention(*selected.sourceTextEncoding);
        baseline.recipe.trustSelectedTextEncoding = selected.footerTextPromotion == sct::SctFooterTextPromotionPolicy::TrustSelectedEncoding;
        auto imported = sct::SctDocumentImporter::import(parsed, selected);
        require(imported.document.has_value(), "SPICE could not import native SCT");
        std::vector<Diagnostic> diagnostics;
        for (const auto& item : imported.diagnostics) {
            diagnostics.push_back({DiagnosticSeverity::Warning, DiagnosticCode::SctImportFailed, item.message});
            baseline.importEvidence.push_back({SctEvidenceKind::SourceObservation, item.message});
        } // Imported drafts retain source diagnostics without retaining the native graph.
        const auto& source = imported.context.receipt().source;
        if (!context.outputProfile) context.outputProfile = SctAuthoredOutputProfile{
            source.byteOrder == sct::SctSourceByteOrder::BigEndian, source.wrapper == sct::SctSourceWrapper::Aklz, source.header.values};
        auto result = builder.appendImported(*imported.document, std::move(baseline), std::move(context));
        if (!result) return result;
        return Result<SctScriptId>::success(result.value(), std::move(diagnostics));
    } catch (const std::exception& e) {
        return Result<SctScriptId>::failure({DiagnosticSeverity::Error, DiagnosticCode::SctImportFailed, e.what()});
    }
}
} // namespace salsa::core::program
