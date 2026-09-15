#include "ProgramInternal.h"
#include "SalsaCore/Program/NativeImport.h"
#include "SpiceSCT/SctParser.h"
#include "SpiceSCT/SctDocumentImporter.h"
#include "SpiceSCT/SctTextEvidence.h"
#include <set>
#include <unordered_map>

namespace salsa::core::program {
namespace sct = spice::sct;
namespace {
// Compare SPICE's decoded values, not bytes or guessed language ranges.
bool sameSourceText(const sct::SctTextValue &a, const sct::SctTextValue &b) {
    if (a.index() != b.index()) return false;
    if (auto text = std::get_if<sct::SctPlainText>(&a)) return text->utf8 == std::get<sct::SctPlainText>(b).utf8;
    if (std::holds_alternative<sct::SctEmptyIndexedText>(a)) return true;
    auto left = std::get_if<sct::SctMessage>(&a);
    auto right = std::get_if<sct::SctMessage>(&b);
    if (!left || !right || left->headerUtf8 != right->headerUtf8 || left->body.elements.size() != right->body.elements.size()) return false;
    for (std::size_t i = 0; i < left->body.elements.size(); ++i) {
        const auto &x = left->body.elements[i], &y = right->body.elements[i];
        if (x.index() != y.index()) return false;
        if (auto chunk = std::get_if<sct::SctTextChunk>(&x)) {
            if (chunk->utf8 != std::get<sct::SctTextChunk>(y).utf8) return false;
        } else {
            const auto &command = std::get<sct::SctInlineCommand>(x), &other = std::get<sct::SctInlineCommand>(y);
            if (command.code != other.code || command.argument != other.argument) return false;
        }
    }
    return true;
}
bool equivalentInterpretations(const sct::SctSourceTextRecordAssessment &record) {
    const sct::SctTextValue *first = nullptr;
    for (const auto &interpretation : record.interpretations) {
        if (!interpretation.complete || !interpretation.semanticValue) continue;
        if (first && !sameSourceText(*first, *interpretation.semanticValue)) return false;
        first = &*interpretation.semanticValue;
    }
    return first != nullptr;
}
NativeTextEvidence indexedEvidence(const sct::SctSourceTextAssessment &assessment) {
    NativeTextEvidence out;out.automatic=true;out.recommendation=assessment.recommendation.convention;
    out.candidates=assessment.indexedSctStrings.viableConventions;
    for(auto ordinal:assessment.indexedSctStrings.recordOrdinals) {
        const auto &record=assessment.records[ordinal];
        if(record.viableConventions.size()==sct::kSctKnownTextConventions.size())continue;
        for(auto candidate:record.viableConventions)++out.support[static_cast<std::size_t>(candidate)];
    }
    return out;
}
const sct::SctTextInterpretation *footerInterpretation(const sct::SctSourceTextRecordAssessment &record) {
    // Footer debug text is independent of the main indexed-string convention.
    // Prefer the observed Shift-JIS debug convention in a tie, not the dataset
    // region. Complete alternatives remain SPICE's responsibility.
    for(auto convention:{sct::SctKnownTextConvention::ShiftJisByte7F,sct::SctKnownTextConvention::ShiftJis8140,sct::SctKnownTextConvention::Windows1252Byte7F})
        for(const auto &candidate:record.interpretations)
            if(candidate.knownConvention==convention && candidate.complete && candidate.semanticValue)return &candidate;
    return nullptr;
}
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
public:
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
sct::SctKnownTextConvention selectNativeConvention(std::span<const NativeTextEvidence> evidence) {
    using Score=std::array<std::uint64_t,3>;
    std::array<Score,sct::kSctKnownTextConventions.size()> scores{};
    for(const auto &file:evidence) if(file.automatic) {
        if(file.recommendation)++scores[static_cast<std::size_t>(*file.recommendation)][1];
        if(!file.candidates.empty() && file.candidates.size()<scores.size())
            for(auto c:file.candidates)scores[static_cast<std::size_t>(c)][0]+=6/file.candidates.size();
        for(std::size_t n=0;n<scores.size();++n)scores[n][2]+=file.support[n];
    }
    // SPICE exposes viable candidates, not a confidence ranking for tied cases.
    // Stable descriptor order is only our last-resort decoding assumption.
    std::size_t best=0;for(std::size_t n=1;n<scores.size();++n)if(scores[n]>scores[best])best=n;
    return sct::kSctKnownTextConventions[best].convention;
}
void resolveNativeText(ProgramData &d,std::span<const NativeTextEvidence> evidence,std::stop_token stop) {
    const auto main=selectNativeConvention(evidence);
    NativeNormalizer normalize(d);
    using TextKey=std::pair<SctScriptId,std::uint64_t>;
    std::vector<std::pair<TextKey,Text*>> indexed;
    if(std::ranges::any_of(evidence,[](const auto &file){return !file.pending.empty();})) {
        indexed.reserve(d.texts.size());
        for(auto &text:d.texts)if(!text.supplementary)indexed.push_back({{text.script,text.sourceText},&text});
        std::ranges::sort(indexed,{},[](const auto &item){return item.first;});
    }
    for(const auto &file:evidence) {
        require(!stop.stop_requested(),"Native text resolution cancelled");
        if(!file.automatic || file.recommendation)continue;
        auto *script=d.lookup(file.script.value,EntityKind::Script,d.scripts);
        require(script,"Missing pending text script");
        auto location=d.directory.find(script->context.baseline.value);
        require(location.kind==EntityKind::Baseline,"Missing imported baseline");
        auto *baseline=&d.baselines.at(location.index);
        const bool changed=baseline->textConvention!=main;
        baseline->textConvention=main;
        baseline->importEvidence.push_back({SctEvidenceKind::Inference,
            "Indexed text uses dataset default " + std::string(sct::findSctKnownTextConvention(main)->stableName) +
            "; selected by weighted indexed-string candidate support across files, with unique recommendations breaking ties. Ties use stable descriptor order. This is an editable import assumption."});
        if(!changed)continue;
        for(const auto &pending:file.pending) {
            require(!stop.stop_requested(),"Native text resolution cancelled");
            const TextKey key{file.script,pending.sourceText};
            auto entry=std::ranges::lower_bound(indexed,key,{},[](const auto &item){return item.first;});
            require(entry!=indexed.end() && entry->first==key,"Missing pending indexed text");
            auto *found=entry->second;
            auto decoded=sct::SctTextInspectionService::interpret(pending.bytes,pending.kind,sct::SctTextStorage::IndexedSection,*sct::sctTextEncodingFor(main));
            if(!decoded.complete || !decoded.semanticValue) {
                baseline->importEvidence.push_back({SctEvidenceKind::Inference,"Dataset convention could not completely decode indexed text " + std::to_string(pending.sourceText) + "; retained its local interpretation for correction."});
                continue;
            }
            const auto old=*d.values.find(found->value.value);
            if(old.kind==ValueKind::Unavailable && old.detail==3){--d.stats.unavailableItems;d.stats.excludedSourceBytes-=old.data;}
            found->value=normalize.text(*decoded.semanticValue);
        }
    }
}
Result<SctScriptId> ProgramBuilder::appendImported(const sct::SctDocument& document, SctAuthoringBaseline baseline, SctScriptContext context) {
    try {
        require(data_ != nullptr, "Builder already finished");
        return Result<SctScriptId>::success(NativeNormalizer(*data_).append(document, std::move(baseline), std::move(context)));
    } catch (const std::exception& e) {
        diagnostics_.push_back(failure(std::string("Native normalization: ") + e.what()));
        return Result<SctScriptId>::failure(diagnostics_.back());
    }
}
static Result<SctScriptId> appendNativeImpl(ProgramBuilder& builder, std::span<const std::uint8_t> bytes,
    SctAuthoringBaseline baseline, SctScriptContext context, const sct::SctDocumentImportOptions& options, std::stop_token stop, SctLoadingObserver progress, NativeTextEvidence* collectionEvidence, const CapturedSource* capture) {
    try {
        auto phase = [&](const char* name) { require(!stop.stop_requested(), "Native load cancelled"); if (progress) progress({name, 0, 0, context.name}); };
        phase("Checking source baseline");
        if (capture) {
            const auto& source = capture->descriptor();
            require(source.locator == baseline.source.locator && source.byteSize == baseline.source.byteSize &&
                    source.revision == baseline.source.revision, "Native capture differs from its imported baseline");
        } else {
            auto digest = sha256(std::as_bytes(bytes));
            if (!digest) return Result<SctScriptId>::failure(digest.diagnostics());
            require(bytes.size() == baseline.source.byteSize && digest.value() == baseline.source.revision.digest,
                    "Native input differs from its imported baseline");
        }
        phase("Parsing SCT");
        auto parsed = sct::SctParser{}.parse(bytes, {}, sct::SctParseOptions{true});
        require(parsed.parseOk, "SPICE could not parse native SCT");
        auto selected = options;
        if (!selected.declaredSourcePlatform && baseline.recipe.platform)
            selected.declaredSourcePlatform = *baseline.recipe.platform == GamePlatform::GameCube ? sct::SctPlatform::GameCube : sct::SctPlatform::Dreamcast;
        if (!selected.sourceTextEncoding && baseline.textConvention) selected.sourceTextEncoding = sct::sctTextEncodingFor(*baseline.textConvention);
        if (baseline.recipe.trustSelectedTextEncoding) selected.footerTextPromotion = sct::SctFooterTextPromotionPolicy::TrustSelectedEncoding;
        sct::SctSourceTextAssessment assessment;
        if(!selected.sourceTextEncoding || selected.footerTextPromotion!=sct::SctFooterTextPromotionPolicy::TrustSelectedEncoding) {
            phase("Assessing source text with SPICE");
            assessment=sct::SctSourceTextDetector::assess(parsed);
        }
        auto evidence=indexedEvidence(assessment);
        evidence.automatic=!selected.sourceTextEncoding;
        if(evidence.automatic) {
            const auto convention=selectNativeConvention(std::span(&evidence,1));
            selected.sourceTextEncoding=sct::sctTextEncodingFor(convention);
            baseline.importEvidence.push_back({evidence.recommendation?SctEvidenceKind::SourceObservation:SctEvidenceKind::Inference,
                evidence.recommendation?"Source text convention selected from SPICE's indexed-string recommendation.":"SPICE indexed text is ambiguous or insufficient; selected an editable decoding assumption. Collection import resolves its dataset default before installation."});
        } else baseline.importEvidence.push_back({SctEvidenceKind::UserAssertion,
            "Source text convention supplied by the import selection or retained baseline recipe."});
        // Persist the recipe actually used, including explicit import overrides.
        if (selected.declaredSourcePlatform) baseline.recipe.platform = *selected.declaredSourcePlatform == sct::SctPlatform::GameCube ? GamePlatform::GameCube : GamePlatform::Dreamcast;
        if (selected.sourceTextEncoding) baseline.textConvention = sct::findSctKnownTextConvention(*selected.sourceTextEncoding);
        baseline.recipe.trustSelectedTextEncoding = selected.footerTextPromotion == sct::SctFooterTextPromotionPolicy::TrustSelectedEncoding;
        phase("Importing typed SCT");
        auto imported = sct::SctDocumentImporter::import(parsed, selected);
        require(imported.document.has_value(), "SPICE could not import native SCT");
        std::vector<Diagnostic> diagnostics;
        for (const auto& item : imported.diagnostics) {
            diagnostics.push_back({DiagnosticSeverity::Warning, DiagnosticCode::SctImportFailed, "SPICE initial import observation (before SALSA text selection): " + item.message});
            baseline.importEvidence.push_back({SctEvidenceKind::SourceObservation, item.message});
        } // Imported drafts retain source diagnostics without retaining the native graph.
        const auto& source = imported.context.receipt().source;
        baseline.importEvidence.push_back({SctEvidenceKind::SourceObservation,
            std::string("SPICE source layout: ") + (source.byteOrder == sct::SctSourceByteOrder::BigEndian ? "big endian" : "little endian") +
            (source.wrapper == sct::SctSourceWrapper::Aklz ? ", AKLZ wrapper." : ", raw SCT.")});
        if (!context.outputProfile) context.outputProfile = SctAuthoredOutputProfile{
            source.byteOrder == sct::SctSourceByteOrder::BigEndian, source.wrapper == sct::SctSourceWrapper::Aklz, source.header.values};
        std::map<std::uint32_t,sct::SctSupplementaryTextId> footerIds;
        for(const auto &record:imported.context.receipt().sourceMap.records())
            if(record.role==sct::SctSourceSpanRole::FooterEntry && record.target)
                if(auto entity=std::get_if<sct::SctDocumentEntityId>(&*record.target))
                    if(auto id=std::get_if<sct::SctSupplementaryTextId>(entity))footerIds.emplace(record.span.offset,*id);
        for(const auto &record:assessment.records) {
            if(record.location.storage==sct::SctTextStorage::Footer) {
                // An explicit trusted-footer override still wins.
                if(selected.footerTextPromotion==sct::SctFooterTextPromotionPolicy::TrustSelectedEncoding)continue;
                auto mapped=footerIds.find(record.location.decodedPayloadOffset);if(mapped==footerIds.end())continue;
                auto text=std::ranges::find(imported.document->supplementaryText,mapped->second,&sct::SctDocumentSupplementaryText::id);
                if(text==imported.document->supplementaryText.end())continue;
                if(auto interpretation=footerInterpretation(record)) {
                    text->value=*interpretation->semanticValue;
                    baseline.importEvidence.push_back({SctEvidenceKind::Inference,
                        "Footer text " + std::to_string(mapped->second.value()) + " imported independently as " + std::string(sct::findSctKnownTextConvention(*interpretation->knownConvention)->stableName) +
                        (record.ambiguous?"; ambiguous complete interpretations prefer Shift-JIS debug text. Editable import assumption.":" from SPICE's complete interpretation.")});
                }
            } else if(record.location.physicalSectionIndex) {
                auto &section=imported.document->sections.at(*record.location.physicalSectionIndex);
                if(auto string=std::get_if<sct::SctStringSectionContent>(&section.content)) {
                    if(evidence.automatic && std::holds_alternative<sct::SctOpaqueText>(string->string.value)) {
                        for(const auto &candidate:record.interpretations)if(candidate.complete && candidate.semanticValue) {
                            string->string.value=*candidate.semanticValue;
                            baseline.importEvidence.push_back({SctEvidenceKind::Inference,"Indexed text " + std::to_string(string->string.id.value()) + " uses a complete local SPICE interpretation because the main convention could not decode it. Editable import assumption."});
                            break;
                        }
                    }
                    if(collectionEvidence && evidence.automatic && !evidence.recommendation && (!equivalentInterpretations(record) || record.viableConventions.size()!=sct::kSctKnownTextConventions.size()))
                        evidence.pending.push_back({string->string.id.value(),record.location.kind,record.bytes});
                }
            }
        }
        phase("Normalizing program");
        auto result = builder.appendImported(*imported.document, std::move(baseline), std::move(context));
        if (!result) return result;
        if(collectionEvidence){evidence.script=result.value();*collectionEvidence=std::move(evidence);}
        phase("Script normalized");
        return Result<SctScriptId>::success(result.value(), std::move(diagnostics));
    } catch (const std::exception& e) {
        return Result<SctScriptId>::failure({DiagnosticSeverity::Error, DiagnosticCode::SctImportFailed, e.what()});
    }
}
Result<SctScriptId> appendNative(ProgramBuilder& builder, std::span<const std::uint8_t> bytes,
    SctAuthoringBaseline baseline, SctScriptContext context, const sct::SctDocumentImportOptions& options,
    std::stop_token stop, SctLoadingObserver progress, NativeTextEvidence* evidence) {
    return appendNativeImpl(builder, bytes, std::move(baseline), std::move(context), options, stop,
                            std::move(progress), evidence, nullptr);
}
Result<SctScriptId> appendNative(ProgramBuilder& builder, const CapturedSource& source,
    SctAuthoringBaseline baseline, SctScriptContext context, const sct::SctDocumentImportOptions& options,
    std::stop_token stop, SctLoadingObserver progress, NativeTextEvidence* evidence) {
    const auto bytes = source.bytes();
    return appendNativeImpl(builder, {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()},
                            std::move(baseline), std::move(context), options, stop, std::move(progress), evidence, &source);
}
} // namespace salsa::core::program
